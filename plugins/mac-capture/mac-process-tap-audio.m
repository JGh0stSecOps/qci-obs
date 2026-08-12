/*
 * mac-process-tap-audio.m -- capture one application's audio with a Core Audio process tap.
 *
 * WHY THIS EXISTS
 *
 * Capturing a single app's audio on this rig was previously done OUTSIDE OBS, by a CLI that
 * created a tap, wrapped it in a PUBLIC aggregate device, and left the device on the machine for
 * OBS to bind by UID. That works, and it taught us everything below, but it has three costs that
 * exist only because the tap lives in another process:
 *
 *   1. TCC attributes a tap to the RESPONSIBLE process, not the accessor. So the CLI worked when
 *      launched from one shell and returned pure digital silence from another, with noErr
 *      everywhere and no dialog. Inside OBS the responsible process IS OBS, which already holds
 *      Screen & System Audio Recording -- and CGPreflightScreenCaptureAccess() can therefore
 *      answer the actual question before we create anything.
 *   2. The aggregate had to be public (another process had to see it), so it outlived its creator
 *      and could be left behind as a husk: present, right UID, 48 kHz float32, clocking, and
 *      carrying nothing. In here the aggregate is PRIVATE, which means it dies with OBS. There is
 *      nothing to leak and nothing to sweep.
 *   3. The CLI had a `stop`/`sweep` surface that could, and did, destroy a live tap as collateral
 *      damage from a mistyped --uid: a silent music bed, mid-stream, exit 0. There is no such
 *      surface here. Every destroy in this file names an object ID this process was handed.
 *
 * GROUND TRUTH, MEASURED ON THIS MACHINE (tools/apptap/probe in the rig repo). Not re-derived:
 *
 *   - Tap format is 48000 Hz, 2 ch, 32-bit, 'lpcm', flags 0x9 (float|packed), bytesPerFrame 8.
 *     0x9 has kAudioFormatFlagIsNonInterleaved CLEAR, so it is interleaved float32 in one buffer.
 *     We still read kAudioTapPropertyFormat back and translate it rather than assuming this.
 *   - A tap-backed aggregate with an EMPTY subdevice list presents in=2 out=0 and clocks.
 *   - private=1 plus an IOProc delivered REAL AUDIO: peak 0.1519, rms -29.9 dBFS, 280 callbacks
 *     in 3 s. That measured-good run bound the tap by PROCESS OBJECT, not by bundle ID -- the
 *     probe's --by-bundle flag defaults to false. See the note in app_tap_start_locked.
 *   - There is NO kAudioHardwarePropertyTranslateBundleIDToProcessObject. Walk 'prs#' and compare
 *     'pbid'. Several 'prs#' entries have EMPTY bundle IDs and must be filtered.
 *   - The TCC gate is kTCCServiceScreenCapture, NOT kTCCServiceAudioCapture. AudioCapture was
 *     observed DENIED (reason 8) while audio flowed.
 *   - Ungranted looks like: noErr from every call, IOProc firing at the correct rate, in=2,
 *     48 kHz float32, and peak 0.000000 forever. No error, no prompt, no log line. That is the
 *     single most important fact in this file and it is why the watchdog exists.
 *
 * WHAT THIS SOURCE REFUSES TO DO: guess. When it cannot prove it is carrying audio it says so,
 * in the properties dialog, in the words "these two causes are indistinguishable".
 */

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/HostTime.h>
#import <CoreGraphics/CoreGraphics.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/apple/cfstring-utils.h>

#include "mac-audio-format.h"

#define TAP_LOG(level, format, ...) blog(level, "[app-audio-tap] " format, ##__VA_ARGS__)

/* Every tap we create is named "<TAP_NAME_PREFIX><pid>] <bundle id>". The pid is not decoration:
 * it is what makes the module-load orphan sweep in app_tap_sweep_orphans() provably safe -- a
 * live instance's tap can never be mistaken for an orphan, because its pid is alive. */
#define TAP_NAME_PREFIX "QCi-OBS["
#define AGG_UID_PREFIX "QCiOBSTap-"

/* How long the device may clock with every sample zero before we say something. Long enough that
 * a track change or a paused player does not cry wolf. */
#define SILENCE_ALARM_NS (4ULL * 1000000000ULL)
/* How long after AudioDeviceStart a total absence of callbacks means "not clocking". */
#define NO_CLOCK_ALARM_NS (2ULL * 1000000000ULL)

void app_tap_sweep_orphans(void) API_AVAILABLE(macos(14.2));

struct app_tap {
	obs_source_t *source;

	/* settings */
	char *bundle_id;
	bool mono_mixdown;
	bool mute_app;

	/* HAL objects. Guarded by `mutex`. Destroyed BY OBJECT ID, never by UID lookup -- the
	 * external tool's worst incident was a rollback that resolved "its" tap by UUID and
	 * destroyed a concurrently created one, because the HAL does not enforce tap UUID
	 * uniqueness. We only ever unwind what we were handed. */
	pthread_mutex_t mutex;
	AudioObjectID tap;
	AudioObjectID device;
	AudioDeviceIOProcID proc_id;
	bool running;
	bool failed;
	bool healthy;
	unsigned healthy_polls;
	uint64_t start_ns;
	int rebuilds;

	/* cached format. Read once from kAudioTapPropertyFormat; NEVER recomputed per callback. */
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bytes_per_frame;
	enum audio_format format;
	enum speaker_layout speakers;
	bool planar;

	/* IOProc -> watchdog. Atomics only; the IOProc is a realtime thread and must not allocate,
	 * lock, or blog. All interpretation happens off-thread. */
	volatile long cb_count;
	volatile long last_audio_ns;
	/* Rolling peak since the watchdog last looked, as amplitude * 1e6 so it fits an integer
	 * atomic. A LEVEL, not just a yes/no: "clocking but silent" and "clocking at -60 dBFS" are
	 * different faults and the operator should not have to tell them apart by ear -- which is
	 * exactly how the 16 kHz mono regression was found. */
	volatile long peak_micro;
	volatile bool saw_audio;
	volatile bool flag_hosttime_invalid;
	volatile bool flag_output_buffers;

	/* status shown in the properties dialog. Guarded by `status_mutex`. */
	pthread_mutex_t status_mutex;
	char status[768];
	enum obs_text_info_type status_level;

	bool ready; /* create() has returned; safe to push property updates */

	pthread_t watchdog;
	bool watchdog_active;
	os_event_t *exit_event;
};

/* ------------------------------------------------------------------------------------------ */
/* small HAL helpers                                                                            */

static const AudioObjectPropertyAddress kProcListAddr = {kAudioHardwarePropertyProcessObjectList,
							 kAudioObjectPropertyScopeGlobal,
							 kAudioObjectPropertyElementMain};

static void osstatus_str(OSStatus st, char *buf, size_t len)
{
	/* Print BOTH forms. The numeric value is what turns up in a crash report; the fourCC is
	 * what is greppable in Apple's headers. */
	char cc[5] = {(char)((st >> 24) & 0xff), (char)((st >> 16) & 0xff), (char)((st >> 8) & 0xff), (char)(st & 0xff),
		      0};
	bool printable = true;

	for (int i = 0; i < 4; i++) {
		if (!isprint((unsigned char)cc[i]))
			printable = false;
	}

	if (printable)
		snprintf(buf, len, "%d ('%s')", (int)st, cc);
	else
		snprintf(buf, len, "%d", (int)st);
}

static char *ca_string_prop(AudioObjectID obj, AudioObjectPropertySelector sel)
{
	AudioObjectPropertyAddress address = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	CFStringRef cf = NULL;
	UInt32 size = sizeof(cf);

	if (AudioObjectGetPropertyData(obj, &address, 0, NULL, &size, &cf) != noErr || !cf)
		return NULL;

	char *out = cfstr_copy_cstr(cf, kCFStringEncodingUTF8);
	CFRelease(cf);
	return out;
}

static UInt32 ca_uint32_prop(AudioObjectID obj, AudioObjectPropertySelector sel, UInt32 fallback)
{
	AudioObjectPropertyAddress address = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	UInt32 value = 0;
	UInt32 size = sizeof(value);

	if (AudioObjectGetPropertyData(obj, &address, 0, NULL, &size, &value) != noErr)
		return fallback;
	return value;
}

static UInt32 ca_channel_count(AudioObjectID dev, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress address = {kAudioDevicePropertyStreamConfiguration, scope,
					      kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	UInt32 channels = 0;

	if (AudioObjectGetPropertyDataSize(dev, &address, 0, NULL, &size) != noErr || !size)
		return 0;

	AudioBufferList *list = bmalloc(size);
	if (AudioObjectGetPropertyData(dev, &address, 0, NULL, &size, list) == noErr) {
		for (UInt32 i = 0; i < list->mNumberBuffers; i++)
			channels += list->mBuffers[i].mNumberChannels;
	}
	bfree(list);
	return channels;
}

/* 'grup' / kAudioAggregateDevicePropertyFullSubDeviceList is a CFArray of UID STRINGS.
 * 'agrp' / ...ActiveSubDeviceList is an AudioObjectID array. Confusing the two reads garbage
 * rather than erroring, so they get separate accessors. */
static CFArrayRef ca_cfarray_prop(AudioObjectID obj, AudioObjectPropertySelector sel)
{
	AudioObjectPropertyAddress address = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	CFArrayRef array = NULL;
	UInt32 size = sizeof(array);

	if (AudioObjectGetPropertyData(obj, &address, 0, NULL, &size, &array) != noErr)
		return NULL;
	return array;
}

static UInt32 ca_objectid_array_count(AudioObjectID obj, AudioObjectPropertySelector sel)
{
	AudioObjectPropertyAddress address = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	UInt32 size = 0;

	if (AudioObjectGetPropertyDataSize(obj, &address, 0, NULL, &size) != noErr)
		return 0;
	return size / (UInt32)sizeof(AudioObjectID);
}

/* ------------------------------------------------------------------------------------------ */
/* the BlackHole interlock                                                                      */
/*
 * On this rig BlackHole 2ch is NOT a scratch loopback: it is the Android device's MICROPHONE, the
 * literal path into the TikTok broadcast (qci-rig/README.md:215 -- "mic -> bleeper (1.5s censor
 * buffer) -> BlackHole 2ch -> device mic"). README.md:367 states the fail-closed invariant: "the
 * bleeper is the only writer to BlackHole, so if it dies TikTok gets silence, never raw audio".
 *
 * A tap CANNOT write anywhere -- see the five structural arguments above
 * app_tap_assert_capture_only() -- so this check is not preventing a leak. It prevents something
 * quieter: a tap on the BlackHole driver process would be metering the CENSORED MICROPHONE and
 * labelling it as an application's audio. Everything in this rig that was merely documented has
 * eventually been violated by a tool that did not know better; this is that lesson applied in
 * advance.
 *
 * Matched case-, space-, hyphen- and underscore-insensitively, because a near-miss spelling that
 * slipped through would defeat the whole point. */
static bool mentions_blackhole(const char *s)
{
	static const char *const needles[] = {"blackhole", "existential.audio"};
	char norm[512];
	size_t n = 0;

	if (!s || !*s)
		return false;

	for (const char *p = s; *p && n < sizeof(norm) - 1; p++) {
		char c = *p;
		if (c == ' ' || c == '-' || c == '_')
			continue;
		norm[n++] = (char)tolower((unsigned char)c);
	}
	norm[n] = 0;

	for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
		if (strstr(norm, needles[i]))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------------------------ */
/* status reporting                                                                             */

static void app_tap_status_v(struct app_tap *t, enum obs_text_info_type level, bool allow_log, const char *format,
			     va_list args)
{
	char buf[sizeof(t->status)];
	bool changed;

	vsnprintf(buf, sizeof(buf), format, args);

	pthread_mutex_lock(&t->status_mutex);
	changed = strcmp(buf, t->status) != 0 || level != t->status_level;
	if (changed) {
		strncpy(t->status, buf, sizeof(t->status) - 1);
		t->status[sizeof(t->status) - 1] = 0;
		t->status_level = level;
	}
	pthread_mutex_unlock(&t->status_mutex);

	if (!changed)
		return;

	/* Log ONCE PER TRANSITION, not per poll -- a watchdog that logs every second is a watchdog
	 * nobody reads. The healthy line carries a live peak level, so its text changes every poll
	 * and it passes allow_log = false; the caller logs that one on entry and then periodically,
	 * which is what keeps this from drowning the log it is supposed to make readable. */
	if (allow_log)
		TAP_LOG(level == OBS_TEXT_INFO_ERROR ? LOG_ERROR
						    : (level == OBS_TEXT_INFO_WARNING ? LOG_WARNING : LOG_INFO),
			"%s", buf);

	/* Never while holding status_mutex, and never before create() has returned. */
	if (t->ready)
		obs_source_update_properties(t->source);
}

static void app_tap_set_status(struct app_tap *t, enum obs_text_info_type level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	app_tap_status_v(t, level, true, format, args);
	va_end(args);
}

static void app_tap_set_status_quiet(struct app_tap *t, enum obs_text_info_type level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	app_tap_status_v(t, level, false, format, args);
	va_end(args);
}

/* ------------------------------------------------------------------------------------------ */
/* process object enumeration ('prs#')                                                          */

static AudioObjectID find_process_object(const char *bundle_id)
{
	AudioObjectPropertyAddress address = kProcListAddr;
	AudioObjectID found = kAudioObjectUnknown;
	UInt32 size = 0;

	if (!bundle_id || !*bundle_id)
		return kAudioObjectUnknown;

	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr || !size)
		return kAudioObjectUnknown;

	AudioObjectID *objects = bmalloc(size);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, objects) == noErr) {
		UInt32 count = size / (UInt32)sizeof(AudioObjectID);
		for (UInt32 i = 0; i < count && found == kAudioObjectUnknown; i++) {
			char *bid = ca_string_prop(objects[i], kAudioProcessPropertyBundleID);
			/* Several 'prs#' entries have EMPTY bundle ids (measured: objects 178, 179
			 * and 188 on this machine). An empty-string compare would match an empty
			 * setting, so filter before comparing. */
			if (bid && *bid && strcmp(bid, bundle_id) == 0)
				found = objects[i];
			bfree(bid);
		}
	}
	bfree(objects);
	return found;
}

static bool process_is_running_output(AudioObjectID proc)
{
	return ca_uint32_prop(proc, kAudioProcessPropertyIsRunningOutput, 0) != 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the safety assertion                                                                         */
/*
 * "Music never touches BlackHole" was true BY ARCHITECTURE. This makes it true by architecture
 * AND BY ASSERTION, which is the difference between a claim and a check. Five independent reasons
 * this source cannot put audio anywhere; the checkable ones are checked on every start, and a
 * failure tears the device down rather than continuing with a warning.
 *
 *   1. A tap is capture-only. Its entire property surface is 'tuid'/'tdsc'/'tfmt'
 *      (AudioHardware.h:2025-2027). There is no render or write path in the API.
 *   2. The aggregate is composed with an EMPTY "subdevices" list, so it owns no hardware and has
 *      no output stream.                                            <- ASSERTED HERE (out == 0)
 *   3. The aggregate is private=1: it is not in kAudioHardwarePropertyDevices for any other
 *      process, so the set of writers to BlackHole cannot change -- nothing outside OBS can even
 *      open it. This is strictly stronger than the external tool's public aggregate.
 *   4. The IOProc never writes outOutputData.                       <- ASSERTED in the IOProc
 *   5. A bundle id naming BlackHole is refused outright (mentions_blackhole above).
 *
 * out == 0 is the load-bearing one: a device with zero output channels cannot be selected as an
 * output anywhere in macOS, so audio physically cannot leave through it.
 */
static bool app_tap_assert_capture_only(struct app_tap *t, AudioObjectID dev)
{
	UInt32 out_channels = ca_channel_count(dev, kAudioObjectPropertyScopeOutput);
	UInt32 active;
	bool ok = true;

	if (out_channels != 0) {
		app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
				   "REFUSING: the aggregate came back with %u output channel(s); it must have 0. "
				   "An output-capable device is a device audio can be routed OUT of, which is "
				   "exactly the property that must not exist here. Tearing it down.",
				   (unsigned)out_channels);
		return false;
	}

	CFArrayRef subs = ca_cfarray_prop(dev, kAudioAggregateDevicePropertyFullSubDeviceList);
	if (subs) {
		CFIndex count = CFArrayGetCount(subs);
		for (CFIndex i = 0; i < count && ok; i++) {
			CFStringRef uid = CFArrayGetValueAtIndex(subs, i);
			char *cstr = uid ? cfstr_copy_cstr(uid, kCFStringEncodingUTF8) : NULL;
			if (cstr && mentions_blackhole(cstr)) {
				app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
						   "REFUSING: the aggregate names sub-device '%s'. BlackHole 2ch is "
						   "the Android device's microphone -- the path INTO the TikTok "
						   "broadcast. Tearing it down.",
						   cstr);
				ok = false;
			}
			bfree(cstr);
		}
		if (ok && count != 0) {
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
					   "REFUSING: the aggregate reports %ld sub-device(s); it was composed with "
					   "none. Tearing it down.",
					   (long)count);
			ok = false;
		}
		CFRelease(subs);
	}

	if (ok) {
		active = ca_objectid_array_count(dev, kAudioAggregateDevicePropertyActiveSubDeviceList);
		if (active != 0) {
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
					   "REFUSING: the aggregate reports %u active sub-device(s); it was composed "
					   "with none. Tearing it down.",
					   (unsigned)active);
			ok = false;
		}
	}

	return ok;
}

/* ------------------------------------------------------------------------------------------ */
/* the IOProc -- CoreAudio realtime thread                                                      */

/* One linear scan of the buffer, giving both answers at once: "is there anything here" and "how
 * much". Roughly 4 KB per callback at the measured 512-frame stereo buffer size -- cheap enough
 * for the realtime thread, and it is the ONLY way to know the tap is carrying audio rather than
 * carrying zeros with every other light green. */
static inline float buffer_peak(const AudioBuffer *b)
{
	const float *p = (const float *)b->mData;
	UInt32 n = b->mDataByteSize / (UInt32)sizeof(float);
	float peak = 0.0f;

	for (UInt32 i = 0; i < n; i++) {
		float a = fabsf(p[i]);
		if (a > peak)
			peak = a;
	}
	return peak;
}

static OSStatus app_tap_ioproc(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *in_data,
			       const AudioTimeStamp *in_time, AudioBufferList *out_data,
			       const AudioTimeStamp *out_time, void *client_data)
{
	struct app_tap *t = client_data;
	struct obs_source_audio audio = {0};

	/* No allocation, no locks, no blog on this thread. Diagnosis is a pair of atomics; the
	 * watchdog does every bit of the interpreting. */

	/* Safety assertion 4: we are handed an output buffer list and we never write to it. Record
	 * that we were handed one, so the claim is checked rather than merely asserted in a
	 * comment. */
	if (out_data && out_data->mNumberBuffers > 0 && !t->flag_output_buffers)
		os_atomic_set_bool(&t->flag_output_buffers, true);

	if (!in_data || in_data->mNumberBuffers == 0 || !in_data->mBuffers[0].mData)
		return noErr;

	if (t->planar) {
		UInt32 planes = in_data->mNumberBuffers;
		if (planes > MAX_AV_PLANES)
			planes = MAX_AV_PLANES;
		for (UInt32 i = 0; i < planes; i++)
			audio.data[i] = in_data->mBuffers[i].mData;
	} else {
		audio.data[0] = in_data->mBuffers[0].mData;
	}

	audio.frames = in_data->mBuffers[0].mDataByteSize / t->bytes_per_frame;
	audio.speakers = t->speakers;
	audio.format = t->format;
	audio.samples_per_sec = t->sample_rate;

	/* A tap-backed aggregate owns no hardware, so whether mHostTime is populated is not
	 * something we get to assume. When it is, AudioConvertHostTimeToNanos shares its epoch and
	 * its units with os_gettime_ns() (CLOCK_UPTIME_RAW -- libobs/util/platform-cocoa.m:41),
	 * which is why mac-audio.c's input_callback can hand OBS mHostTime directly and
	 * handle_ts_jump never fires on it. The fallback is therefore interchangeable, not a
	 * fudge. It is still surfaced by the watchdog, so an unexpected fallback is visible rather
	 * than silently absorbed. */
	if (in_time && (in_time->mFlags & kAudioTimeStampHostTimeValid)) {
		audio.timestamp = AudioConvertHostTimeToNanos(in_time->mHostTime);
	} else {
		audio.timestamp = os_gettime_ns();
		if (!t->flag_hosttime_invalid)
			os_atomic_set_bool(&t->flag_hosttime_invalid, true);
	}

	os_atomic_inc_long(&t->cb_count);
	{
		float peak = buffer_peak(&in_data->mBuffers[0]);
		if (peak > 0.0f) {
			long micro = (long)(peak * 1000000.0f);
			os_atomic_set_long(&t->last_audio_ns, (long)os_gettime_ns());
			if (micro > os_atomic_load_long(&t->peak_micro))
				os_atomic_set_long(&t->peak_micro, micro);
			if (!t->saw_audio)
				os_atomic_set_bool(&t->saw_audio, true);
		}
	}

	obs_source_output_audio(t->source, &audio);

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(now);
	UNUSED_PARAMETER(out_time);
	return noErr;
}

/* ------------------------------------------------------------------------------------------ */
/* start / stop                                                                                 */

API_AVAILABLE(macos(14.2))
static void app_tap_stop_locked(struct app_tap *t)
{
	char buf[32];

	/* Strict order. A leaked IOProc keeps the device busy and the next start cannot destroy
	 * it; an aggregate destroyed before its IOProc is torn down is the same bug wearing a
	 * different hat. No step here has an "and also tidy up while I am here" clause -- that
	 * clause is what silenced a live rig. */
	if (t->device != kAudioObjectUnknown && t->proc_id) {
		AudioDeviceStop(t->device, t->proc_id);
		AudioDeviceDestroyIOProcID(t->device, t->proc_id);
	}
	t->proc_id = NULL;

	if (t->device != kAudioObjectUnknown) {
		OSStatus st = AudioHardwareDestroyAggregateDevice(t->device);
		if (st != noErr) {
			osstatus_str(st, buf, sizeof(buf));
			TAP_LOG(LOG_WARNING, "AudioHardwareDestroyAggregateDevice(id=%u) -> %s", (unsigned)t->device,
				buf);
		}
	}
	t->device = kAudioObjectUnknown;

	if (t->tap != kAudioObjectUnknown) {
		/* BY OBJECT ID. Never by UUID lookup: the HAL does not enforce tap UUID uniqueness,
		 * and a rollback that resolved "its" tap by UUID is how the external tool's loser
		 * of a start race destroyed the winner's tap. */
		OSStatus st = AudioHardwareDestroyProcessTap(t->tap);
		if (st != noErr) {
			osstatus_str(st, buf, sizeof(buf));
			TAP_LOG(LOG_WARNING, "AudioHardwareDestroyProcessTap(id=%u) -> %s", (unsigned)t->tap, buf);
		}
	}
	t->tap = kAudioObjectUnknown;

	t->running = false;
	os_atomic_set_long(&t->cb_count, 0);
	os_atomic_set_bool(&t->saw_audio, false);
}

API_AVAILABLE(macos(14.2))
static bool app_tap_start_locked(struct app_tap *t)
{
	bool result = false;

	/* This runs on the OBS source thread and on the watchdog thread, neither of which has an
	 * autorelease pool of its own. */
	@autoreleasepool {
		CATapDescription *desc = nil;
		AudioObjectID tap = kAudioObjectUnknown;
		AudioObjectID dev = kAudioObjectUnknown;
		CFStringRef tap_uid = NULL;
		AudioStreamBasicDescription asbd = {0};
		AudioObjectPropertyAddress address = {0, kAudioObjectPropertyScopeGlobal,
						      kAudioObjectPropertyElementMain};
		char status_buf[32];
		bool screen_capture_granted;
		bool bound_by_bundle = false;
		AudioObjectID proc;
		UInt32 size;
		OSStatus st;

		/* --- gate 0: nothing reaches CoreAudio until all of this passes ------------- */

		if (!t->bundle_id || !*t->bundle_id) {
			/* Empty is the DEFAULT and it must be inert, not "tap whatever came first in
			 * the list". The sck_audio_capture segfault this fork already fixed came
			 * from a source that trusted an unvalidated settings value; every value read
			 * from obs_data here is treated as hostile, because obs-websocket can set
			 * any of them without going near the properties dialog. */
			app_tap_set_status(t, OBS_TEXT_INFO_NORMAL,
					   "Idle: no application selected. Pick one, or type its bundle ID.");
			goto done;
		}

		if (mentions_blackhole(t->bundle_id)) {
			app_tap_set_status(
				t, OBS_TEXT_INFO_ERROR,
				"REFUSED: '%s' names BlackHole. BlackHole 2ch is the Android device's microphone "
				"-- the path INTO the TikTok broadcast (rig README:215, \"mic -> bleeper -> "
				"BlackHole 2ch -> device mic\"). Tapping it would meter the censored mic feed and "
				"label it as an application. Nothing was created.",
				t->bundle_id);
			t->failed = true;
			goto done;
		}

		CFStringRef self_cf = CFBundleGetIdentifier(CFBundleGetMainBundle());
		if (self_cf) {
			char *self = cfstr_copy_cstr(self_cf, kCFStringEncodingUTF8);
			bool is_self = self && strcmp(self, t->bundle_id) == 0;
			bfree(self);
			if (is_self) {
				app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
						   "REFUSED: '%s' is OBS itself. Tapping our own output is a "
						   "monitoring feedback path. Nothing was created.",
						   t->bundle_id);
				t->failed = true;
				goto done;
			}
		}

		/* Non-prompting, and this is the capability the external CLI could not have: in here
		 * the accessor IS the responsible process, so this answers the actual question.
		 * Recorded, not fatal -- we still try, because being wrong about the gate and
		 * refusing to run would be its own silent failure. */
		screen_capture_granted = CGPreflightScreenCaptureAccess();

		/* --- 1: target resolution --------------------------------------------------- */

		proc = find_process_object(t->bundle_id);

		if (@available(macOS 26.0, *)) {
			/* bundleIDs + processRestoreEnabled binds the tap to an IDENTITY rather than
			 * to a live process object, so it survives the app quitting and relaunching.
			 * The process-object route only ever sees apps that are already running and
			 * producing audio, and goes deaf the moment the app quits.
			 *
			 * NOTE, because the record is thinner than it looks: the run that MEASURED
			 * real audio (probe/capture-private.txt) used the PROCESS-OBJECT route --
			 * probe's --by-bundle flag defaults to false, and the probe's `bundletap`
			 * subcommand never had its output saved. So we hand the HAL BOTH: the
			 * process object when one exists, plus the bundle binding for restore. If
			 * the bundle binding is ignored we still have the proven path; if it is
			 * honoured we also survive a relaunch. */
			NSArray<NSNumber *> *procs = (proc != kAudioObjectUnknown) ? @[ @(proc) ] : @[];
			desc = t->mono_mixdown ? [[CATapDescription alloc] initMonoMixdownOfProcesses:procs]
					       : [[CATapDescription alloc] initStereoMixdownOfProcesses:procs];
			desc.bundleIDs = @[ [NSString stringWithUTF8String:t->bundle_id] ];
			desc.processRestoreEnabled = YES;
			bound_by_bundle = true;
		} else {
			if (proc == kAudioObjectUnknown) {
				/* Not an error. The app simply is not running, or has not produced
				 * audio yet -- which is what puts it in 'prs#'. The watchdog retries;
				 * t->failed stays false. */
				app_tap_set_status(t, OBS_TEXT_INFO_NORMAL,
						   "Waiting for '%s' to start producing audio. macOS below 26 can "
						   "only tap a process that is already running.",
						   t->bundle_id);
				goto done;
			}
			NSArray<NSNumber *> *procs = @[ @(proc) ];
			desc = t->mono_mixdown ? [[CATapDescription alloc] initMonoMixdownOfProcesses:procs]
					       : [[CATapDescription alloc] initStereoMixdownOfProcesses:procs];
		}

		/* pid-stamped so app_tap_sweep_orphans() can attribute a tap leaked by a crash --
		 * and so a LIVE instance's tap can never be mistaken for one. */
		desc.name = [NSString stringWithFormat:@"%s%d] %s", TAP_NAME_PREFIX, (int)getpid(), t->bundle_id];
		/* Unmuted by default: the operator keeps hearing the app in their own ears, and OBS's
		 * fader is a second, independent level on the same playback. */
		desc.muteBehavior = t->mute_app ? CATapMuted : CATapUnmuted;
		/* Public, deliberately. A PRIVATE tap is invisible to other processes, so a tap
		 * leaked by an OBS crash could never be found and swept. Public plus the pid stamp
		 * makes recovery provable. This is also what the measured-good run used. */
		desc.privateTap = NO;
		/* desc.UUID is deliberately NOT set. We hold the object ID for our whole lifetime
		 * and never resolve a tap by UUID, so the duplicate-UUID race that needed a
		 * machine-wide lock in the external tool cannot occur here at all. */

		/* --- 2: create the tap ------------------------------------------------------ */

		st = AudioHardwareCreateProcessTap(desc, &tap);
		[desc release];
		desc = nil;

		if (st != noErr || tap == kAudioObjectUnknown) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "AudioHardwareCreateProcessTap('%s') failed: %s%s",
					   t->bundle_id, status_buf,
					   (st == noErr) ? " (noErr, but it handed back kAudioObjectUnknown)" : "");
			goto done;
		}

		/* --- 3: read the format back; never guess it -------------------------------- */

		address.mSelector = kAudioTapPropertyFormat;
		size = sizeof(asbd);
		st = AudioObjectGetPropertyData(tap, &address, 0, NULL, &size, &asbd);
		if (st != noErr) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "could not read kAudioTapPropertyFormat: %s",
					   status_buf);
			goto unwind;
		}

		if (asbd.mFormatID != kAudioFormatLinearPCM) {
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "tap format is not linear PCM (mFormatID = 0x%x)",
					   (unsigned)asbd.mFormatID);
			t->failed = true;
			goto unwind;
		}

		t->format = convert_ca_format(asbd.mFormatFlags, asbd.mBitsPerChannel);
		if (t->format == AUDIO_FORMAT_UNKNOWN) {
			/* Quote the exact flags and bit depth, mac-audio.c:337-343 style. Never
			 * guess a format. */
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "unknown tap format: flags 0x%x, %u bits, %u ch",
					   (unsigned)asbd.mFormatFlags, (unsigned)asbd.mBitsPerChannel,
					   (unsigned)asbd.mChannelsPerFrame);
			t->failed = true;
			goto unwind;
		}

		t->planar = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
		t->sample_rate = (uint32_t)asbd.mSampleRate;
		t->channels = asbd.mChannelsPerFrame;
		t->bytes_per_frame = asbd.mBytesPerFrame;
		t->speakers = convert_ca_speaker_layout(asbd.mChannelsPerFrame);

		if (!t->bytes_per_frame || !t->sample_rate || t->speakers == SPEAKERS_UNKNOWN) {
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
					   "the tap reported an unusable format: %u Hz, %u ch, bytesPerFrame %u",
					   (unsigned)t->sample_rate, (unsigned)t->channels,
					   (unsigned)t->bytes_per_frame);
			t->failed = true;
			goto unwind;
		}

		/* --- 4: the private aggregate ----------------------------------------------- */

		address.mSelector = kAudioTapPropertyUID;
		size = sizeof(tap_uid);
		st = AudioObjectGetPropertyData(tap, &address, 0, NULL, &size, &tap_uid);
		if (st != noErr || !tap_uid) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "could not read kAudioTapPropertyUID: %s",
					   status_buf);
			goto unwind;
		}

		{
			const char *uuid = obs_source_get_uuid(t->source);
			NSString *agg_uid = [NSString
				stringWithFormat:@"%s%d-%s", AGG_UID_PREFIX, (int)getpid(), uuid ? uuid : "0"];
			NSString *agg_name = [NSString stringWithFormat:@"QCi-OBS App Audio (%s)", t->bundle_id];

			/* The sub-tap UID comes from the object the HAL just created. Never from a
			 * UUID we invented. */
			NSDictionary *composition = @{
				@(kAudioAggregateDeviceUIDKey) : agg_uid,
				@(kAudioAggregateDeviceNameKey) : agg_name,
				/* private=1 is THE in-process win. The device dies with OBS: there
				 * is no husk for a later run to adopt, nothing to sweep, and it is
				 * invisible to every other process -- including the operator's
				 * production OBS, whose device picker it must never appear in. It
				 * is also exactly the configuration the probe measured delivering
				 * real audio. */
				@(kAudioAggregateDeviceIsPrivateKey) : @1,
				@(kAudioAggregateDeviceIsStackedKey) : @0,
				/* 0, even though private=1 now permits 1. AudioHardware.h:1638-1641
				 * says autostart makes AudioDeviceStart "wait until a tapped
				 * process begins receiving its first audio"; whether that BLOCKS on
				 * a silent target was never measured, and this function runs on the
				 * OBS source thread. A hang there is worse than the
				 * clocking-but-silent state, which the watchdog has to detect
				 * anyway. */
				@(kAudioAggregateDeviceTapAutoStartKey) : @0,
				/* EMPTY. No hardware sub-device, therefore no output path,
				 * therefore no way for this device to reach BlackHole or anything
				 * else. app_tap_assert_capture_only() verifies the consequence. */
				@(kAudioAggregateDeviceSubDeviceListKey) : @[],
				@(kAudioAggregateDeviceTapListKey) : @[ @{
					@(kAudioSubTapUIDKey) : (NSString *)tap_uid,
					@(kAudioSubTapDriftCompensationKey) : @1,
				} ],
			};

			st = AudioHardwareCreateAggregateDevice((CFDictionaryRef)composition, &dev);
		}

		if (st != noErr || dev == kAudioObjectUnknown) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "AudioHardwareCreateAggregateDevice failed: %s",
					   status_buf);
			goto unwind;
		}

		t->tap = tap;
		t->device = dev;

		/* --- 5: prove it is capture-only before a single frame moves ---------------- */

		if (!app_tap_assert_capture_only(t, dev)) {
			t->failed = true;
			goto unwind_owned;
		}

		/* --- 6: IOProc -------------------------------------------------------------- */

		st = AudioDeviceCreateIOProcID(dev, app_tap_ioproc, t, &t->proc_id);
		if (st != noErr || !t->proc_id) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "AudioDeviceCreateIOProcID failed: %s", status_buf);
			goto unwind_owned;
		}

		os_atomic_set_long(&t->cb_count, 0);
		os_atomic_set_long(&t->last_audio_ns, 0);
		os_atomic_set_long(&t->peak_micro, 0);
		os_atomic_set_bool(&t->saw_audio, false);
		t->healthy = false;
		t->healthy_polls = 0;
		os_atomic_set_bool(&t->flag_hosttime_invalid, false);
		os_atomic_set_bool(&t->flag_output_buffers, false);

		st = AudioDeviceStart(dev, t->proc_id);
		if (st != noErr) {
			osstatus_str(st, status_buf, sizeof(status_buf));
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR, "AudioDeviceStart failed: %s", status_buf);
			goto unwind_owned;
		}

		t->running = true;
		t->start_ns = os_gettime_ns();

		TAP_LOG(LOG_INFO,
			"capturing '%s' (%s): tap id=%u, private aggregate id=%u, %u Hz %u ch %s, in=%u out=0, "
			"Screen & System Audio Recording preflight=%s",
			t->bundle_id, bound_by_bundle ? "bundle-bound" : "process-bound", (unsigned)tap, (unsigned)dev,
			(unsigned)t->sample_rate, (unsigned)t->channels,
			t->planar ? "planar float" : "interleaved float",
			(unsigned)ca_channel_count(dev, kAudioObjectPropertyScopeInput),
			screen_capture_granted ? "granted" : "NOT GRANTED");

		if (!screen_capture_granted) {
			/* The one failure mode that looks exactly like success. Say it before the
			 * operator has any chance to trust a green light. */
			app_tap_set_status(
				t, OBS_TEXT_INFO_ERROR,
				"OBS does not hold Screen & System Audio Recording. Process taps return SUCCESS "
				"and deliver PURE SILENCE without it -- no error, no prompt, no log line. Grant "
				"it in System Settings > Privacy & Security > Screen & System Audio Recording, "
				"then press Restart capture.");
		} else {
			app_tap_set_status(t, OBS_TEXT_INFO_NORMAL, "Started. Waiting for the first audio from '%s'.",
					   t->bundle_id);
		}

		result = true;
		goto done;

	unwind_owned:
		/* The pair is already recorded on `t`; unwind through the one ordered teardown. */
		app_tap_stop_locked(t);
		goto done;

	unwind:
		/* Not yet recorded on `t`. Unwind by the object IDs we were handed, same order. */
		if (dev != kAudioObjectUnknown)
			AudioHardwareDestroyAggregateDevice(dev);
		if (tap != kAudioObjectUnknown)
			AudioHardwareDestroyProcessTap(tap);
		t->tap = kAudioObjectUnknown;
		t->device = kAudioObjectUnknown;
		t->running = false;

	done:
		if (tap_uid)
			CFRelease(tap_uid);
		[desc release];
	}

	return result;
}

/* ------------------------------------------------------------------------------------------ */
/* watchdog -- owns ALL interpretation and ALL logging                                          */

API_AVAILABLE(macos(14.2))
static void *app_tap_watchdog(void *param)
{
	struct app_tap *t = param;

	os_set_thread_name("app-audio-tap: watchdog");

	while (os_event_timedwait(t->exit_event, 1000) == ETIMEDOUT) {
		uint64_t now;
		uint64_t last_audio;
		long callbacks;
		bool saw_audio;
		bool silent_now;
		AudioObjectID dev;
		AudioObjectID proc;

		pthread_mutex_lock(&t->mutex);

		if (!t->running) {
			/* Retry unless we failed for a reason retrying cannot fix (a refusal, an
			 * unusable format). Those set t->failed and stay put until the operator
			 * changes a setting or presses Restart capture. */
			if (!t->failed && t->bundle_id && *t->bundle_id)
				app_tap_start_locked(t);
			pthread_mutex_unlock(&t->mutex);
			continue;
		}

		now = os_gettime_ns();
		callbacks = os_atomic_load_long(&t->cb_count);
		saw_audio = os_atomic_load_bool(&t->saw_audio);
		dev = t->device;

		if (os_atomic_load_bool(&t->flag_hosttime_invalid)) {
			os_atomic_set_bool(&t->flag_hosttime_invalid, false);
			TAP_LOG(LOG_WARNING,
				"the tap-backed aggregate is not supplying a valid host time; falling back to "
				"os_gettime_ns(). Same clock (CLOCK_UPTIME_RAW), so timestamps stay coherent with "
				"the rest of OBS.");
		}
		if (os_atomic_load_bool(&t->flag_output_buffers)) {
			os_atomic_set_bool(&t->flag_output_buffers, false);
			TAP_LOG(LOG_INFO,
				"the IOProc is handed a non-empty output buffer list; it does not and will not "
				"write to it (safety assertion 4).");
		}

		/* the device vanished underneath us */
		if (ca_uint32_prop(dev, kAudioDevicePropertyDeviceIsAlive, 1) == 0) {
			app_tap_set_status(t, OBS_TEXT_INFO_WARNING, "the capture device disappeared; rebuilding.");
			app_tap_stop_locked(t);
			pthread_mutex_unlock(&t->mutex);
			continue;
		}

		if (callbacks == 0) {
			if (now - t->start_ns > NO_CLOCK_ALARM_NS) {
				if (t->rebuilds < 1) {
					t->rebuilds++;
					app_tap_set_status(t, OBS_TEXT_INFO_WARNING,
							   "present but NOT CLOCKING -- the device started and no audio "
							   "callback has ever fired. Rebuilding once.");
				} else {
					t->failed = true;
					app_tap_set_status(
						t, OBS_TEXT_INFO_ERROR,
						"present but NOT CLOCKING after a rebuild. The aggregate exists and "
						"AudioDeviceStart succeeded, but no audio callback has ever fired. "
						"This source is NOT carrying audio. Press Restart capture to try "
						"again.");
				}
				app_tap_stop_locked(t);
			}
			pthread_mutex_unlock(&t->mutex);
			continue;
		}

		/* Clocking. Now the only question that matters: is there anything in it? */
		last_audio = (uint64_t)os_atomic_load_long(&t->last_audio_ns);
		silent_now = !saw_audio || (now - last_audio) > SILENCE_ALARM_NS;

		if (!silent_now) {
			/* Consume the rolling peak so the next line reports the NEXT interval, not the
			 * loudest thing since the source was created. */
			long peak_micro = os_atomic_set_long(&t->peak_micro, 0);
			double peak_db = peak_micro > 0 ? 20.0 * log10((double)peak_micro / 1000000.0) : -INFINITY;

			app_tap_set_status_quiet(t, OBS_TEXT_INFO_NORMAL,
						 "Capturing '%s' -- %u Hz, %u ch, peak %.1f dBFS, %ld callbacks.",
						 t->bundle_id, (unsigned)t->sample_rate, (unsigned)t->channels,
						 peak_db, callbacks);

			/* Log on entry to the healthy state, then once a minute as a heartbeat that
			 * carries a real measured level rather than "still running". */
			if (!t->healthy || t->healthy_polls % 60 == 0)
				TAP_LOG(LOG_INFO, "capturing '%s': %u Hz, %u ch, peak %.1f dBFS, %ld callbacks",
					t->bundle_id, (unsigned)t->sample_rate, (unsigned)t->channels, peak_db,
					callbacks);
			t->healthy = true;
			t->healthy_polls++;

			pthread_mutex_unlock(&t->mutex);
			continue;
		}

		t->healthy = false;

		if (now - t->start_ns < SILENCE_ALARM_NS) {
			pthread_mutex_unlock(&t->mutex);
			continue;
		}

		/* Silent while clocking. Consult the target's own state before saying anything --
		 * and then REFUSE TO GUESS between the two remaining causes, because they are
		 * genuinely indistinguishable from this API and guessing sends the operator hunting
		 * the wrong fault mid-stream. */
		proc = find_process_object(t->bundle_id);

		if (proc == kAudioObjectUnknown) {
			const char *note = "";
			if (@available(macOS 26.0, *))
				note = " The tap is bound by bundle ID and stays armed for its relaunch.";
			app_tap_set_status(t, OBS_TEXT_INFO_NORMAL,
					   "Clocking, no audio: '%s' is not producing any right now.%s", t->bundle_id,
					   note);
		} else if (!process_is_running_output(proc)) {
			app_tap_set_status(t, OBS_TEXT_INFO_NORMAL,
					   "Clocking, no audio: '%s' is running but is not playing anything.",
					   t->bundle_id);
		} else if (!CGPreflightScreenCaptureAccess()) {
			app_tap_set_status(t, OBS_TEXT_INFO_ERROR,
					   "SILENT: '%s' IS producing audio and OBS does NOT hold Screen & System "
					   "Audio Recording. That permission is the gate for process taps; without "
					   "it the API returns success and delivers pure zeros. Grant it in System "
					   "Settings > Privacy & Security.",
					   t->bundle_id);
		} else {
			app_tap_set_status(
				t, OBS_TEXT_INFO_WARNING,
				"SILENT: the device is clocking and every sample is zero, while '%s' reports that "
				"it IS producing audio. This is EITHER the application being muted or at zero "
				"volume, OR the Screen & System Audio Recording grant not being in effect for "
				"this build. Those two are indistinguishable from the CoreAudio API and this "
				"source will not guess which one it is.",
				t->bundle_id);
		}

		pthread_mutex_unlock(&t->mutex);
	}

	return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* orphan sweep -- module load only                                                             */
/*
 * A private aggregate cannot leak: it dies with the process. A tap CAN, because taps outlive
 * their creator (measured). If OBS is SIGKILLed or crashes, its tap stays behind.
 *
 * The test here is strictly stronger than a name-prefix sweep: the tap's own name carries the pid
 * that created it, so "orphan" means "the process that made this is gone", which is checkable and
 * cannot produce a false positive against a live instance. That matters -- the external tool's
 * prefix-only sweep destroyed a live music tap as collateral, mid-stream, and reported success.
 *
 * Reachable ONLY from obs_module_load. Never from source destroy. A global operation bolted onto
 * a targeted one is exactly what made that CLI's `stop` a rig-silencing command.
 */
void app_tap_sweep_orphans(void)
{
	@autoreleasepool {
		AudioObjectPropertyAddress address = {kAudioHardwarePropertyTapList, kAudioObjectPropertyScopeGlobal,
						      kAudioObjectPropertyElementMain};
		UInt32 size = 0;

		if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr ||
		    !size)
			return;

		AudioObjectID *taps = bmalloc(size);
		if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, taps) != noErr) {
			bfree(taps);
			return;
		}

		UInt32 count = size / (UInt32)sizeof(AudioObjectID);
		for (UInt32 i = 0; i < count; i++) {
			AudioObjectPropertyAddress desc_addr = {kAudioTapPropertyDescription,
								kAudioObjectPropertyScopeGlobal,
								kAudioObjectPropertyElementMain};
			CATapDescription *desc = nil;
			UInt32 desc_size = sizeof(desc);
			char buf[32];

			if (AudioObjectGetPropertyData(taps[i], &desc_addr, 0, NULL, &desc_size, &desc) != noErr ||
			    !desc)
				continue;

			const char *name = desc.name.UTF8String;
			if (name && strncmp(name, TAP_NAME_PREFIX, strlen(TAP_NAME_PREFIX)) == 0) {
				int pid = atoi(name + strlen(TAP_NAME_PREFIX));
				/* kill(pid, 0) == -1 with ESRCH is the ONLY condition under which we
				 * touch anything. A live instance -- including this one -- can never
				 * match, because its pid is alive. */
				if (pid > 0 && kill((pid_t)pid, 0) == -1 && errno == ESRCH) {
					OSStatus st = AudioHardwareDestroyProcessTap(taps[i]);
					osstatus_str(st, buf, sizeof(buf));
					TAP_LOG(LOG_INFO,
						"swept orphaned tap \"%s\" (id=%u, creator pid %d is gone) -> %s",
						name, (unsigned)taps[i], pid, buf);
				}
			}

			[desc release];
		}

		bfree(taps);
	}
}

/* ------------------------------------------------------------------------------------------ */
/* obs_source_info                                                                              */

static const char *app_tap_getname(void *unused __unused)
{
	return obs_module_text("ProcessTap.Name");
}

API_AVAILABLE(macos(14.2))
static void app_tap_destroy(void *data)
{
	struct app_tap *t = data;

	if (!t)
		return;

	/* Join the watchdog FIRST: it is the other caller of app_tap_start_locked, and a rebuild
	 * racing a teardown is how objects get left behind. */
	if (t->watchdog_active) {
		os_event_signal(t->exit_event);
		pthread_join(t->watchdog, NULL);
		t->watchdog_active = false;
	}

	pthread_mutex_lock(&t->mutex);
	app_tap_stop_locked(t);
	pthread_mutex_unlock(&t->mutex);

	os_event_destroy(t->exit_event);
	pthread_mutex_destroy(&t->mutex);
	pthread_mutex_destroy(&t->status_mutex);
	bfree(t->bundle_id);
	bfree(t);
}

static void app_tap_apply_settings(struct app_tap *t, obs_data_t *settings)
{
	const char *bundle_id = obs_data_get_string(settings, "bundle_id");

	bfree(t->bundle_id);
	t->bundle_id = bstrdup(bundle_id ? bundle_id : "");
	t->mono_mixdown = obs_data_get_bool(settings, "mono_mixdown");
	t->mute_app = obs_data_get_bool(settings, "mute_app");
	t->rebuilds = 0;
	t->failed = false;
}

API_AVAILABLE(macos(14.2))
static void app_tap_update(void *data, obs_data_t *settings)
{
	struct app_tap *t = data;

	pthread_mutex_lock(&t->mutex);
	app_tap_stop_locked(t);
	app_tap_apply_settings(t, settings);
	app_tap_start_locked(t);
	pthread_mutex_unlock(&t->mutex);
}

API_AVAILABLE(macos(14.2))
static void *app_tap_create(obs_data_t *settings, obs_source_t *source)
{
	struct app_tap *t = bzalloc(sizeof(struct app_tap));

	t->source = source;
	t->tap = kAudioObjectUnknown;
	t->device = kAudioObjectUnknown;
	t->status_level = OBS_TEXT_INFO_NORMAL;
	pthread_mutex_init(&t->mutex, NULL);
	pthread_mutex_init(&t->status_mutex, NULL);

	if (os_event_init(&t->exit_event, OS_EVENT_TYPE_MANUAL) != 0) {
		TAP_LOG(LOG_ERROR, "failed to create the exit event: %d", errno);
		pthread_mutex_destroy(&t->mutex);
		pthread_mutex_destroy(&t->status_mutex);
		bfree(t);
		return NULL;
	}

	pthread_mutex_lock(&t->mutex);
	app_tap_apply_settings(t, settings);
	app_tap_start_locked(t);
	pthread_mutex_unlock(&t->mutex);

	/* The watchdog runs whether or not the start succeeded: "waiting for the app to launch" is
	 * a state it recovers from, not a failure. It is also the ONLY thing that can tell the
	 * operator this source is carrying silence, which is the whole point. */
	if (pthread_create(&t->watchdog, NULL, app_tap_watchdog, t) == 0)
		t->watchdog_active = true;
	else
		TAP_LOG(LOG_WARNING, "could not start the watchdog thread; silence will NOT be reported");

	t->ready = true;
	return t;
}

static void app_tap_defaults(obs_data_t *settings)
{
	/* Empty is inert, deliberately. Never "the first application in the list". */
	obs_data_set_default_string(settings, "bundle_id", "");
	obs_data_set_default_bool(settings, "mono_mixdown", false);
	obs_data_set_default_bool(settings, "mute_app", false);
}

API_AVAILABLE(macos(14.2))
static bool app_tap_restart_clicked(obs_properties_t *props __unused, obs_property_t *property __unused, void *data)
{
	struct app_tap *t = data;

	if (!t)
		return false;

	pthread_mutex_lock(&t->mutex);
	app_tap_stop_locked(t);
	t->rebuilds = 0;
	t->failed = false;
	app_tap_start_locked(t);
	pthread_mutex_unlock(&t->mutex);
	return true;
}

static void app_tap_fill_application_list(obs_property_t *list, const char *self_bundle)
{
	AudioObjectPropertyAddress address = kProcListAddr;
	UInt32 size = 0;

	obs_property_list_clear(list);
	/* Null entry at the top, for the same reason mac-sck-common.m:184 has one: opening the
	 * properties window must not inadvertently select the first enumerated application. */
	obs_property_list_add_string(list, " ", "");

	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr || !size)
		return;

	AudioObjectID *objects = bmalloc(size);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, objects) == noErr) {
		UInt32 count = size / (UInt32)sizeof(AudioObjectID);
		for (UInt32 i = 0; i < count; i++) {
			struct dstr label = {0};
			char *name;
			char *bid = ca_string_prop(objects[i], kAudioProcessPropertyBundleID);

			/* empty bundle ids: measured, must be filtered. */
			if (!bid || !*bid || mentions_blackhole(bid) ||
			    (self_bundle && strcmp(bid, self_bundle) == 0)) {
				bfree(bid);
				continue;
			}

			name = ca_string_prop(objects[i], kAudioObjectPropertyName);
			if (name && *name)
				dstr_printf(&label, "%s (%s)", name, bid);
			else
				dstr_copy(&label, bid);
			dstr_cat(&label, process_is_running_output(objects[i]) ? " \xe2\x80\x94 playing"
									       : " \xe2\x80\x94 idle");

			obs_property_list_add_string(list, label.array, bid);
			dstr_free(&label);
			bfree(name);
			bfree(bid);
		}
	}
	bfree(objects);
}

static obs_properties_t *app_tap_properties(void *data)
{
	struct app_tap *t = data;
	obs_properties_t *props = obs_properties_create();
	CFStringRef self_cf = CFBundleGetIdentifier(CFBundleGetMainBundle());
	char *self = self_cf ? cfstr_copy_cstr(self_cf, kCFStringEncodingUTF8) : NULL;
	obs_property_t *safety;
	obs_property_t *list;

	/* EDITABLE, deliberately. 'prs#' only lists applications that have already produced audio
	 * -- 36 entries out of everything running, on this machine. With a bundle-bound tap the
	 * operator must be able to TYPE a bundle id for an app that is not running yet; a pure
	 * LIST combo makes "not running" unrepresentable. */
	list = obs_properties_add_list(props, "bundle_id", obs_module_text("ProcessTap.Application"),
				       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	app_tap_fill_application_list(list, self);
	bfree(self);

	obs_properties_add_bool(props, "mono_mixdown", obs_module_text("ProcessTap.MonoMixdown"));
	obs_properties_add_bool(props, "mute_app", obs_module_text("ProcessTap.MuteApp"));

	if (t) {
		char status[sizeof(t->status)];
		enum obs_text_info_type level;

		pthread_mutex_lock(&t->status_mutex);
		memcpy(status, t->status, sizeof(status));
		level = t->status_level;
		pthread_mutex_unlock(&t->status_mutex);

		if (status[0]) {
			obs_property_t *info = obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
			obs_property_text_set_info_type(info, level);
		}
	}

	safety = obs_properties_add_text(props, "safety", obs_module_text("ProcessTap.Safety"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(safety, OBS_TEXT_INFO_NORMAL);

	if (__builtin_available(macOS 14.2, *))
		obs_properties_add_button2(props, "restart", obs_module_text("ProcessTap.Restart"),
					   app_tap_restart_clicked, t);

	return props;
}

API_AVAILABLE(macos(14.2))
struct obs_source_info coreaudio_app_audio_capture_info = {
	.id = "coreaudio_app_audio_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = app_tap_getname,
	.create = app_tap_create,
	.destroy = app_tap_destroy,
	.update = app_tap_update,
	.get_defaults = app_tap_defaults,
	.get_properties = app_tap_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};
