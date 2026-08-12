#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <errno.h>

#include <obs-module.h>
#include <mach/mach_time.h>
#include <util/threading.h>
#include <util/c99defs.h>
#include <util/apple/cfstring-utils.h>

#include "audio-device-enum.h"

#define PROPERTY_DEFAULT_DEVICE kAudioHardwarePropertyDefaultInputDevice
#define PROPERTY_FORMATS kAudioStreamPropertyAvailablePhysicalFormats

#define SCOPE_OUTPUT kAudioUnitScope_Output
#define SCOPE_INPUT kAudioUnitScope_Input
#define SCOPE_GLOBAL kAudioUnitScope_Global

#define BUS_OUTPUT 0
#define BUS_INPUT 1

#define set_property AudioUnitSetProperty
#define get_property AudioUnitGetProperty

#define TEXT_AUDIO_INPUT obs_module_text("CoreAudio.InputCapture");
#define TEXT_AUDIO_OUTPUT obs_module_text("CoreAudio.OutputCapture");
#define TEXT_DEVICE obs_module_text("CoreAudio.Device")
#define TEXT_DEVICE_DEFAULT obs_module_text("CoreAudio.Device.Default")

/*
 * The ceiling of HFP wideband (mSBC).  Narrowband CVSD is 8 kHz.  There is no Bluetooth
 * audio profile that legitimately delivers 16 kHz, so a Bluetooth input at or below this
 * rate IS the telephony profile -- it is a measurement, not a guess.  Deliberately NOT
 * "< 44100": 22.05 and 24 kHz are legitimate choices and flagging them is the alarmist
 * version of this feature.
 */
#define CA_NARROWBAND_MAX_HZ 16000

/*
 * QCi: honest device diagnostics.
 *
 * WHY THIS EXISTS.  The operator streams with a Shokz bone-conduction headset.  Opening its
 * microphone forces macOS into HFP/SCO, which collapses the whole Bluetooth link to 16 kHz
 * mono in BOTH directions -- so their music went tinny and their voice went to stream as
 * wideband telephony.  That profile switch happens in bluetoothd/CoreAudio, far below OBS,
 * and is NOT fixable from here; nothing in this file tries to.  What was fixable is that
 * OBS knew the device was at 16 kHz and said so exactly once, in a LOG_INFO line at startup
 * that nobody reads, while every meter and every encoder downstream cheerfully reported the
 * 48 kHz mix rate because obs_source_output_audio() had already resampled it.  The operator
 * found the fault by ear.  Refusing to hide that is the whole feature -- so this reports what
 * IS, never a guess at intent, and never "fixes" it by resampling and pretending.
 */
struct coreaudio_diagnosis {
	/* Rule A: a Bluetooth input whose best available rate is telephony-band. */
	bool narrowband;
	/* Rule B: Rule A holds AND the headset's other half is this Mac's default output
	 * and can do better -- i.e. using this mic costs the operator their playback. */
	bool profile_collapse;
	/* Rule C: mono input where the paired endpoint proves the hardware can do stereo. */
	bool mono_where_stereo;
	/* Rule B, confirmed live rather than predicted: the output half is sitting AT the
	 * narrowband rate right now even though it advertises better. */
	bool collapsed_now;

	uint32_t transport;
	uint32_t max_rate;
	uint32_t input_channels;

	uint32_t peer_max_rate;
	uint32_t peer_nominal_rate;
	uint32_t peer_out_channels;
	bool peer_is_default_output;
};

struct coreaudio_data {
	char *device_name;
	char *device_uid;
	AudioUnit unit;
	AudioDeviceID device_id;
	AudioBufferList *buf_list;
	bool au_initialized;
	bool active;
	bool default_device;
	bool input;
	bool no_devices;

	uint32_t available_channels;
	char **channel_names;
	int32_t *channel_map;

	uint32_t sample_rate;
	enum audio_format format;
	enum speaker_layout speakers;
	bool enable_downmix;

	struct coreaudio_diagnosis diag;

	pthread_t reconnect_thread;
	os_event_t *exit_event;
	volatile bool reconnecting;
	unsigned long retry_time;

	obs_source_t *source;
};

static bool get_default_output_device(struct coreaudio_data *ca)
{
	struct device_list list;

	memset(&list, 0, sizeof(struct device_list));
	coreaudio_enum_devices(&list, false);

	if (!list.items.num)
		return false;

	bfree(ca->device_uid);
	ca->device_uid = bstrdup(list.items.array[0].value.array);

	device_list_free(&list);
	return true;
}

static bool find_device_id_by_uid(struct coreaudio_data *ca)
{
	UInt32 size = sizeof(AudioDeviceID);
	CFStringRef cf_uid = NULL;
	CFStringRef qual = NULL;
	UInt32 qual_size = 0;
	OSStatus stat;
	bool success;

	AudioObjectPropertyAddress addr = {.mScope = kAudioObjectPropertyScopeGlobal,
					   .mElement = kAudioObjectPropertyElementMain};

	if (!ca->device_uid)
		ca->device_uid = bstrdup("default");

	ca->default_device = false;
	ca->no_devices = false;

	/* have to do this because mac output devices don't actually exist */
	if (astrcmpi(ca->device_uid, "default") == 0) {
		if (ca->input) {
			ca->default_device = true;
		} else {
			if (!get_default_output_device(ca)) {
				ca->no_devices = true;
				return false;
			}
		}
	}

	cf_uid = CFStringCreateWithCString(NULL, ca->device_uid, kCFStringEncodingUTF8);

	if (ca->default_device) {
		addr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
		stat = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, qual_size, &qual, &size,
						  &ca->device_id);
		success = (stat == noErr);
	} else {
		success = coreaudio_get_device_id(cf_uid, &ca->device_id);
	}

	if (cf_uid)
		CFRelease(cf_uid);

	return success;
}

static inline void ca_warn(struct coreaudio_data *ca, const char *func, const char *format, ...)
{
	va_list args;
	struct dstr str = {0};

	va_start(args, format);

	dstr_printf(&str, "[%s]:[device '%s'] ", func, ca->device_name);
	dstr_vcatf(&str, format, args);
	blog(LOG_WARNING, "%s", str.array);
	dstr_free(&str);

	va_end(args);
}

static inline bool ca_success(OSStatus stat, struct coreaudio_data *ca, const char *func, const char *action)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "[%s]:[device '%s'] %s failed: %d", func, ca->device_name, action, (int)stat);
		return false;
	}

	return true;
}

enum coreaudio_io_type {
	IO_TYPE_INPUT,
	IO_TYPE_OUTPUT,
};

static inline bool enable_io(struct coreaudio_data *ca, enum coreaudio_io_type type, bool enable)
{
	UInt32 enable_int = enable;
	return set_property(ca->unit, kAudioOutputUnitProperty_EnableIO,
			    (type == IO_TYPE_INPUT) ? SCOPE_INPUT : SCOPE_OUTPUT,
			    (type == IO_TYPE_INPUT) ? BUS_INPUT : BUS_OUTPUT, &enable_int, sizeof(enable_int));
}

static inline enum speaker_layout convert_ca_speaker_layout(UInt32 channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	}
	return SPEAKERS_UNKNOWN;
}

static inline enum audio_format convert_ca_format(UInt32 format_flags, UInt32 bits)
{
	bool planar = (format_flags & kAudioFormatFlagIsNonInterleaved) != 0;

	if (format_flags & kAudioFormatFlagIsFloat)
		return planar ? AUDIO_FORMAT_FLOAT_PLANAR : AUDIO_FORMAT_FLOAT;

	if (!(format_flags & kAudioFormatFlagIsSignedInteger) && bits == 8)
		return planar ? AUDIO_FORMAT_U8BIT_PLANAR : AUDIO_FORMAT_U8BIT;

	/* not float?  not signed int?  no clue, fail */
	if ((format_flags & kAudioFormatFlagIsSignedInteger) == 0)
		return AUDIO_FORMAT_UNKNOWN;

	if (bits == 16)
		return planar ? AUDIO_FORMAT_16BIT_PLANAR : AUDIO_FORMAT_16BIT;
	else if (bits == 32)
		return planar ? AUDIO_FORMAT_32BIT_PLANAR : AUDIO_FORMAT_32BIT;

	return AUDIO_FORMAT_UNKNOWN;
}

static char *sanitize_device_name(char *name)
{
	const size_t max_len = 64;
	size_t len = strlen(name);
	char buf[64];
	size_t out_idx = 0;

	for (size_t i = len > max_len ? len - max_len : 0; i < len; i++) {
		char c = name[i];
		if (isalnum(c)) {
			buf[out_idx++] = name[i];
		}
		if (c == '-' || c == ' ' || c == '_' || c == ':') {
			buf[out_idx++] = '_';
		}
	}
	return bstrdup_n(buf, out_idx);
}

static char **coreaudio_get_channel_names(struct coreaudio_data *ca)
{
	char **channel_names = bzalloc(sizeof(char *) * ca->available_channels);

	for (uint32_t i = 0; i < ca->available_channels; i++) {
		CFStringRef cf_chan_name = NULL;
		UInt32 dataSize = sizeof(cf_chan_name);
		AudioObjectPropertyAddress pa;
		pa.mSelector = kAudioObjectPropertyElementName;
		pa.mScope = kAudioDevicePropertyScopeInput;
		pa.mElement = i + 1;
		OSStatus stat = AudioObjectGetPropertyData(ca->device_id, &pa, 0, NULL, &dataSize, &cf_chan_name);

		struct dstr name;
		dstr_init(&name);
		if (ca_success(stat, ca, "coreaudio_init_format", "get channel names") &&
		    CFStringGetLength(cf_chan_name)) {

			char *channelName = cfstr_copy_cstr(cf_chan_name, kCFStringEncodingUTF8);

			dstr_printf(&name, "%s", channelName);

			if (channelName) {
				bfree(channelName);
			}
		} else {
			dstr_printf(&name, "%s %d", obs_module_text("CoreAudio.Channel.Device"), i + 1);
		}
		channel_names[i] = bstrdup_n(name.array, name.len);
		dstr_free(&name);

		if (cf_chan_name) {
			CFRelease(cf_chan_name);
		}
	}
	return channel_names;
}

/* ---------------------- QCi: honest device diagnostics ---------------------- */

static uint32_t ca_transport_type(AudioDeviceID id)
{
	AudioObjectPropertyAddress addr = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	UInt32 transport = 0;
	UInt32 size = sizeof(transport);

	if (AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, &transport) != noErr)
		return 0;

	return (uint32_t)transport;
}

/*
 * Read the device's best AVAILABLE rate, not its current nominal rate.
 *
 * The Shokz input's only available rate is 16000, so the verdict is permanently true and
 * the warning is permanently correct.  Reading kAudioDevicePropertyNominalSampleRate instead
 * would make it flicker every time the link renegotiates, which trains the operator to
 * ignore it.  Measured on this machine: the property returns the same list at input and at
 * output scope even on an input-only device, so global scope is right -- and it matches what
 * coreaudio_init_buffer() already does for NominalSampleRate.
 */
static uint32_t ca_max_available_rate(AudioDeviceID id)
{
	AudioObjectPropertyAddress addr = {kAudioDevicePropertyAvailableNominalSampleRates,
					   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	uint32_t best = 0;

	if (AudioObjectGetPropertyDataSize(id, &addr, 0, NULL, &size) != noErr || !size)
		return 0;

	AudioValueRange *ranges = bmalloc(size);
	if (AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, ranges) == noErr) {
		size_t count = size / sizeof(AudioValueRange);
		for (size_t i = 0; i < count; i++) {
			if (ranges[i].mMaximum > (Float64)best)
				best = (uint32_t)ranges[i].mMaximum;
		}
	}
	bfree(ranges);

	return best;
}

static uint32_t ca_nominal_rate(AudioDeviceID id)
{
	AudioObjectPropertyAddress addr = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	Float64 rate = 0.0;
	UInt32 size = sizeof(rate);

	if (AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, &rate) != noErr)
		return 0;

	return (uint32_t)rate;
}

static uint32_t ca_channel_count(AudioDeviceID id, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamConfiguration, scope,
					   kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	uint32_t channels = 0;

	if (AudioObjectGetPropertyDataSize(id, &addr, 0, NULL, &size) != noErr || !size)
		return 0;

	AudioBufferList *bl = bmalloc(size);
	if (AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, bl) == noErr) {
		for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
			channels += bl->mBuffers[i].mNumberChannels;
	}
	bfree(bl);

	return channels;
}

static char *ca_copy_string_prop(AudioDeviceID id, AudioObjectPropertySelector selector)
{
	AudioObjectPropertyAddress addr = {selector, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	CFStringRef cf_str = NULL;
	UInt32 size = sizeof(cf_str);
	char *str;

	if (AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, &cf_str) != noErr || !cf_str)
		return NULL;

	str = cfstr_copy_cstr(cf_str, kCFStringEncodingUTF8);
	CFRelease(cf_str);

	return str;
}

static AudioDeviceID ca_default_output_device(void)
{
	AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	AudioDeviceID id = kAudioObjectUnknown;
	UInt32 size = sizeof(id);

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &id) != noErr)
		return kAudioObjectUnknown;

	return id;
}

/*
 * Do two device UIDs name the two halves of one Bluetooth endpoint?
 *
 * Measured on this machine, the Shokz is TWO AudioDeviceIDs with different UIDs:
 *   145 "B8-84-11-20-2B-4E:input"   16 kHz, 1 in, 0 out
 *   139 "B8-84-11-20-2B-4E:output"  44.1 kHz, 0 in, 2 out
 * They are joined only by the UID text before the final ':'.
 */
static bool ca_uid_endpoints_match(const char *a, const char *b)
{
	const char *sep_a = strrchr(a, ':');
	const char *sep_b = strrchr(b, ':');
	size_t len_a, len_b;

	if (!sep_a || !sep_b)
		return false;

	len_a = (size_t)(sep_a - a);
	len_b = (size_t)(sep_b - b);

	return len_a && len_a == len_b && strncmp(a, b, len_a) == 0;
}

/*
 * Find the output half of the Bluetooth headset this input belongs to.
 *
 * THE TRAP THIS AVOIDS.  The obvious test is `ca->device_id == default_output_device`.  That
 * is NEVER true for a Bluetooth headset -- the two halves are separate AudioObjects -- so a
 * guard written that way reads perfectly correct and silently never fires.
 * kAudioDevicePropertyModelUID is no help either: it is the string "0 0" on BOTH halves.
 * So join by name, with the UID prefix as a second opinion.  There is in-tree precedent:
 * devices_match() in libobs/audio-monitoring/osx/coreaudio-enum-devices.c compares devices by
 * kAudioDevicePropertyDeviceNameCFString and not by UID, and that name-join is exactly why
 * the OBS_SOURCE_DO_NOT_SELF_MONITOR guard works on Bluetooth headsets at all.
 */
static bool ca_find_bluetooth_peer(AudioDeviceID input_id, struct coreaudio_diagnosis *diag)
{
	AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	AudioDeviceID *ids;
	AudioDeviceID default_out;
	char *self_name, *self_uid;
	size_t count;
	bool found = false;

	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr || !size)
		return false;

	self_name = ca_copy_string_prop(input_id, kAudioDevicePropertyDeviceNameCFString);
	self_uid = ca_copy_string_prop(input_id, kAudioDevicePropertyDeviceUID);
	if (!self_name && !self_uid) {
		bfree(self_name);
		bfree(self_uid);
		return false;
	}

	default_out = ca_default_output_device();

	ids = bmalloc(size);
	count = size / sizeof(AudioDeviceID);

	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids) == noErr) {
		for (size_t i = 0; i < count && !found; i++) {
			char *name, *uid;
			bool match;

			if (ids[i] == input_id)
				continue;
			/* Only ever pair with another Bluetooth endpoint.  This is also what keeps
			 * unrelated devices whose UIDs happen to contain ':' (AirPlay, for one) out. */
			if (ca_transport_type(ids[i]) != kAudioDeviceTransportTypeBluetooth)
				continue;
			if (!ca_channel_count(ids[i], kAudioDevicePropertyScopeOutput))
				continue;

			name = ca_copy_string_prop(ids[i], kAudioDevicePropertyDeviceNameCFString);
			uid = ca_copy_string_prop(ids[i], kAudioDevicePropertyDeviceUID);

			match = (self_name && name && strcmp(self_name, name) == 0) ||
				(self_uid && uid && ca_uid_endpoints_match(self_uid, uid));

			if (match) {
				diag->peer_out_channels = ca_channel_count(ids[i], kAudioDevicePropertyScopeOutput);
				diag->peer_max_rate = ca_max_available_rate(ids[i]);
				diag->peer_nominal_rate = ca_nominal_rate(ids[i]);
				diag->peer_is_default_output = (ids[i] == default_out);
				found = true;
			}

			bfree(name);
			bfree(uid);
		}
	}

	bfree(ids);
	bfree(self_name);
	bfree(self_uid);

	return found;
}

static void coreaudio_diagnose(struct coreaudio_data *ca)
{
	struct coreaudio_diagnosis *diag = &ca->diag;

	memset(diag, 0, sizeof(*diag));

	/* Only inputs can force a profile switch; an output capture is already the sink. */
	if (!ca->input)
		return;

	diag->transport = ca_transport_type(ca->device_id);
	diag->max_rate = ca_max_available_rate(ca->device_id);
	diag->input_channels = ca->available_channels;

	/* Rule A -- NARROWBAND.  A wired 16 kHz device is a legitimate choice, so this is
	 * gated on Bluetooth: over Bluetooth, 16 kHz is not a choice, it is HFP. */
	if (diag->transport != kAudioDeviceTransportTypeBluetooth)
		return;
	if (!diag->max_rate || diag->max_rate > CA_NARROWBAND_MAX_HZ)
		return;

	diag->narrowband = true;

	if (!ca_find_bluetooth_peer(ca->device_id, diag))
		return;

	/* Rule B -- PROFILE COLLAPSE.  The headset's own other half is this Mac's default
	 * output AND can do better than telephony band, so opening this mic costs the operator
	 * their playback quality.  Both halves of that are measured, neither is assumed. */
	diag->profile_collapse = diag->peer_is_default_output && diag->peer_max_rate > CA_NARROWBAND_MAX_HZ;

	/* ...and is it already collapsed, or merely about to be?  Say which. */
	diag->collapsed_now = diag->profile_collapse && diag->peer_nominal_rate &&
			      diag->peer_nominal_rate <= CA_NARROWBAND_MAX_HZ;

	/* Rule C -- MONO WHERE STEREO EXISTS.  Read available_channels, NOT
	 * obs_source_get_speaker_layout(): when enable_downmix is false,
	 * coreaudio_init_buffer() overwrites mChannelsPerFrame with the OBS mix width, so a
	 * mono device is presented to libobs as fake stereo with a -1 channel map.  The layout
	 * would say "stereo" and be wrong.  Sample rate has no such distortion. */
	diag->mono_where_stereo = diag->input_channels == 1 && diag->peer_out_channels >= 2;
}

/* 44100 must read "44.1 kHz", not "44 kHz".  The whole point of this feature is that the
 * numbers are trustworthy, and a rate that quietly rounds is a number nobody can check. */
static const char *ca_khz(uint32_t hz, char buf[16])
{
	if (hz % 1000 == 0)
		snprintf(buf, 16, "%" PRIu32, hz / 1000);
	else
		snprintf(buf, 16, "%.1f", (double)hz / 1000.0);

	return buf;
}

/*
 * Compose the operator-facing sentence.  Returns false when there is nothing to say.
 *
 * House rule: state what IS, in measured numbers, and never guess at intent.  Rule B is
 * phrased as what WILL happen when the peer is still at full rate, because the warning fires
 * when the source is created -- before the collapse -- which is more useful than after.
 */
static bool coreaudio_format_notice(const struct coreaudio_data *ca, struct dstr *out)
{
	const struct coreaudio_diagnosis *diag = &ca->diag;
	char khz[16];

	if (!diag->narrowband)
		return false;

	dstr_init(out);
	dstr_printf(out, "%s: %s kHz", ca->device_name ? ca->device_name : "device", ca_khz(diag->max_rate, khz));

	if (diag->mono_where_stereo)
		dstr_cat(out, " mono");

	/* Name the actual codec. 16 kHz is HFP wideband (mSBC); 8 kHz is narrowband (CVSD).
	 * Calling 8 kHz "wideband" would be the kind of confidently-wrong detail that makes an
	 * operator stop believing the rest of the message. */
	dstr_catf(out,
		  ", Bluetooth. This is HFP %s telephony -- the only profile that carries a Bluetooth "
		  "microphone, so it is the ceiling for this device, not a setting.",
		  diag->max_rate > 8000 ? "wideband (mSBC)" : "narrowband (CVSD)");

	if (diag->profile_collapse) {
		char peer_now[16], peer_best[16];

		if (diag->collapsed_now) {
			dstr_catf(out,
				  " This headset is also this Mac's default output, and its playback side is at "
				  "%s kHz right now although it can do %s kHz -- the link has already collapsed.",
				  ca_khz(diag->peer_nominal_rate, peer_now), ca_khz(diag->peer_max_rate, peer_best));
		} else {
			dstr_catf(out,
				  " This headset is also this Mac's default output and can do %s kHz, so playback "
				  "drops to telephony band while this microphone is open.",
				  ca_khz(diag->peer_max_rate, peer_best));
		}
		dstr_cat(out, " That is a Bluetooth profile switch below CoreAudio; OBS cannot undo it. "
			      "Use a different microphone to keep full-rate playback.");
	}

	return true;
}

/* -------------------------------------------------------------------------- */

static bool coreaudio_init_format(struct coreaudio_data *ca)
{
	AudioStreamBasicDescription desc;
	AudioStreamBasicDescription inputDescription;
	OSStatus stat;
	UInt32 size;
	struct obs_audio_info aoi;
	if (!obs_get_audio_info(&aoi)) {
		blog(LOG_WARNING, "No active audio");
		return false;
	}
	ca->speakers = aoi.speakers;
	uint32_t channels = get_audio_channels(ca->speakers);

	size = sizeof(inputDescription);
	stat = get_property(ca->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &inputDescription,
			    &size);

	if (!ca_success(stat, ca, "coreaudio_init_format", "get input device format"))
		return false;

	stat = get_property(ca->unit, kAudioUnitProperty_StreamFormat, SCOPE_OUTPUT, BUS_INPUT, &desc, &size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "get input format"))
		return false;

	ca->available_channels = inputDescription.mChannelsPerFrame;
	if (ca->available_channels > MAX_DEVICE_INPUT_CHANNELS) {
		ca->available_channels = MAX_DEVICE_INPUT_CHANNELS;
	}

	ca->channel_names = coreaudio_get_channel_names(ca);

	if (ca->enable_downmix) {
		blog(LOG_INFO, "Downmix enabled: %d to %d channels.", ca->available_channels, channels);
		desc.mChannelsPerFrame = ca->available_channels;
	} else {
		// Mute any channels mapped in config that we don't really have
		char *sep = "";
		struct dstr cm_str;
		dstr_init(&cm_str);
		for (size_t i = 0; i < channels; i++) {
			dstr_cat(&cm_str, sep);
			if (ca->channel_map[i] >= (int32_t)ca->available_channels) {
				ca->channel_map[i] = -1;
			}
			dstr_catf(&cm_str, "%d", ca->channel_map[i]);
			sep = ",";
		}
		blog(LOG_INFO, "Channel map enabled: [%s] (%d channels available)", cm_str.array,
		     ca->available_channels);
		dstr_free(&cm_str);

		stat = set_property(ca->unit, kAudioOutputUnitProperty_ChannelMap, SCOPE_OUTPUT, BUS_INPUT,
				    ca->channel_map, sizeof(SInt32) * channels);
		if (!ca_success(stat, ca, "coreaudio_init_format", "set channel map")) {
			return false;
		}

		desc.mChannelsPerFrame = channels;
	}

	desc.mSampleRate = inputDescription.mSampleRate;

	stat = set_property(ca->unit, kAudioUnitProperty_StreamFormat, SCOPE_OUTPUT, BUS_INPUT, &desc, size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "set output format"))
		return false;

	if (desc.mFormatID != kAudioFormatLinearPCM) {
		ca_warn(ca, "coreaudio_init_format", "format is not PCM");
		return false;
	}

	ca->format = convert_ca_format(desc.mFormatFlags, desc.mBitsPerChannel);
	if (ca->format == AUDIO_FORMAT_UNKNOWN) {
		ca_warn(ca, "coreaudio_init_format",
			"unknown format flags: "
			"%u, bits: %u",
			(unsigned int)desc.mFormatFlags, (unsigned int)desc.mBitsPerChannel);
		return false;
	}

	ca->sample_rate = (uint32_t)desc.mSampleRate;

	coreaudio_diagnose(ca);

	return true;
}

static bool coreaudio_init_buffer(struct coreaudio_data *ca)
{
	UInt32 bufferSizeFrames;
	UInt32 bufferSizeBytes;
	UInt32 propertySize;
	OSStatus err = noErr;

	propertySize = sizeof(bufferSizeFrames);
	err = AudioUnitGetProperty(ca->unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
				   &bufferSizeFrames, &propertySize);

	if (!ca_success(err, ca, "coreaudio_init_buffer", "get buffer frame size")) {
		return false;
	}

	bufferSizeBytes = bufferSizeFrames * sizeof(Float32);

	AudioStreamBasicDescription streamDescription;
	propertySize = sizeof(streamDescription);
	err = AudioUnitGetProperty(ca->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
				   &streamDescription, &propertySize);

	if (!ca_success(err, ca, "coreaudio_init_buffer", "get stream format")) {
		return false;
	}

	if (!ca->enable_downmix) {
		streamDescription.mChannelsPerFrame = get_audio_channels(ca->speakers);
	}

	Float64 rate = 0.0;
	propertySize = sizeof(Float64);
	AudioObjectPropertyAddress propertyAddress = {kAudioDevicePropertyNominalSampleRate,
						      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

	err = AudioObjectGetPropertyData(ca->device_id, &propertyAddress, 0, NULL, &propertySize, &rate);

	if (!ca_success(err, ca, "coreaudio_init_buffer", "get input sample rate")) {
		return false;
	}

	streamDescription.mSampleRate = rate;

	int bufferPropertySize =
		offsetof(AudioBufferList, mBuffers[0]) + (sizeof(AudioBuffer) * streamDescription.mChannelsPerFrame);

	AudioBufferList *inputBuffer = (AudioBufferList *)bmalloc(bufferPropertySize);
	inputBuffer->mNumberBuffers = streamDescription.mChannelsPerFrame;

	for (UInt32 i = 0; i < inputBuffer->mNumberBuffers; i++) {
		inputBuffer->mBuffers[i].mNumberChannels = 1;
		inputBuffer->mBuffers[i].mDataByteSize = bufferSizeBytes;
		inputBuffer->mBuffers[i].mData = bmalloc(bufferSizeBytes);
	}

	ca->buf_list = inputBuffer;
	return true;
}

static void buf_list_free(AudioBufferList *buf_list)
{
	if (buf_list) {
		for (UInt32 i = 0; i < buf_list->mNumberBuffers; i++)
			bfree(buf_list->mBuffers[i].mData);

		bfree(buf_list);
	}
}

static OSStatus input_callback(void *data, AudioUnitRenderActionFlags *action_flags, const AudioTimeStamp *ts_data,
			       UInt32 bus_num, UInt32 frames, AudioBufferList *ignored_buffers)
{
	struct coreaudio_data *ca = data;
	OSStatus stat;
	struct obs_source_audio audio;

	stat = AudioUnitRender(ca->unit, action_flags, ts_data, bus_num, frames, ca->buf_list);
	if (!ca_success(stat, ca, "input_callback", "audio retrieval"))
		return noErr;

	for (UInt32 i = 0; i < ca->buf_list->mNumberBuffers; i++) {
		if (i < MAX_AUDIO_CHANNELS) {
			audio.data[i] = ca->buf_list->mBuffers[i].mData;
		}
	}

	audio.frames = frames;
	audio.speakers = (ca->buf_list->mNumberBuffers > MAX_AUDIO_CHANNELS) ? MAX_AUDIO_CHANNELS
									     : ca->buf_list->mNumberBuffers;
	audio.format = ca->format;
	audio.samples_per_sec = ca->sample_rate;
	audio.timestamp = AudioConvertHostTimeToNanos(ts_data->mHostTime);

	obs_source_output_audio(ca->source, &audio);

	UNUSED_PARAMETER(ignored_buffers);
	return noErr;
}

static void coreaudio_stop(struct coreaudio_data *ca);
static bool coreaudio_init(struct coreaudio_data *ca);
static void coreaudio_uninit(struct coreaudio_data *ca);

static void *reconnect_thread(void *param)
{
	struct coreaudio_data *ca = param;

	ca->reconnecting = true;

	while (os_event_timedwait(ca->exit_event, ca->retry_time) == ETIMEDOUT) {
		if (coreaudio_init(ca))
			break;
	}

	blog(LOG_DEBUG, "coreaudio: exit the reconnect thread");
	ca->reconnecting = false;
	return NULL;
}

static void coreaudio_begin_reconnect(struct coreaudio_data *ca)
{
	int ret;

	if (ca->reconnecting)
		return;

	ret = pthread_create(&ca->reconnect_thread, NULL, reconnect_thread, ca);
	if (ret != 0)
		blog(LOG_WARNING,
		     "[coreaudio_begin_reconnect] failed to "
		     "create thread, error code: %d",
		     ret);
}

static OSStatus notification_callback(AudioObjectID id, UInt32 num_addresses,
				      const AudioObjectPropertyAddress addresses[], void *data)
{
	struct coreaudio_data *ca = data;

	coreaudio_stop(ca);
	coreaudio_uninit(ca);

	if (addresses[0].mSelector == PROPERTY_DEFAULT_DEVICE)
		ca->retry_time = 300;
	else
		ca->retry_time = 2000;

	blog(LOG_INFO,
	     "coreaudio: device '%s' disconnected or changed.  "
	     "attempting to reconnect",
	     ca->device_name);

	coreaudio_begin_reconnect(ca);

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(num_addresses);

	return noErr;
}

static OSStatus add_listener(struct coreaudio_data *ca, UInt32 property)
{
	AudioObjectPropertyAddress addr = {property, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

	return AudioObjectAddPropertyListener(ca->device_id, &addr, notification_callback, ca);
}

static bool coreaudio_init_hooks(struct coreaudio_data *ca)
{
	OSStatus stat;
	AURenderCallbackStruct callback_info = {.inputProc = input_callback, .inputProcRefCon = ca};

	stat = add_listener(ca, kAudioDevicePropertyDeviceIsAlive);
	if (!ca_success(stat, ca, "coreaudio_init_hooks", "set disconnect callback"))
		return false;

	stat = add_listener(ca, PROPERTY_FORMATS);
	if (!ca_success(stat, ca, "coreaudio_init_hooks", "set format change callback"))
		return false;

	if (ca->default_device) {
		AudioObjectPropertyAddress addr = {PROPERTY_DEFAULT_DEVICE, kAudioObjectPropertyScopeGlobal,
						   kAudioObjectPropertyElementMain};

		stat = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, notification_callback, ca);
		if (!ca_success(stat, ca, "coreaudio_init_hooks", "set device change callback"))
			return false;
	}

	stat = set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback, SCOPE_GLOBAL, 0, &callback_info,
			    sizeof(callback_info));
	if (!ca_success(stat, ca, "coreaudio_init_hooks", "set input callback"))
		return false;

	return true;
}

static void coreaudio_remove_hooks(struct coreaudio_data *ca)
{
	AURenderCallbackStruct callback_info = {.inputProc = NULL, .inputProcRefCon = NULL};

	AudioObjectPropertyAddress addr = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMain};

	AudioObjectRemovePropertyListener(ca->device_id, &addr, notification_callback, ca);

	addr.mSelector = PROPERTY_FORMATS;
	AudioObjectRemovePropertyListener(ca->device_id, &addr, notification_callback, ca);

	if (ca->default_device) {
		addr.mSelector = PROPERTY_DEFAULT_DEVICE;
		AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr, notification_callback, ca);
	}

	set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback, SCOPE_GLOBAL, 0, &callback_info,
		     sizeof(callback_info));
}

static bool coreaudio_get_device_name(struct coreaudio_data *ca)
{
	CFStringRef cf_name = NULL;
	UInt32 size = sizeof(CFStringRef);
	char *name = NULL;

	const AudioObjectPropertyAddress addr = {kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeInput,
						 kAudioObjectPropertyElementMain};

	OSStatus stat = AudioObjectGetPropertyData(ca->device_id, &addr, 0, NULL, &size, &cf_name);
	if (stat != noErr) {
		blog(LOG_WARNING,
		     "[coreaudio_get_device_name] failed to "
		     "get name: %d",
		     (int)stat);
		return false;
	}

	name = cfstr_copy_cstr(cf_name, kCFStringEncodingUTF8);
	if (!name) {
		blog(LOG_WARNING, "[coreaudio_get_device_name] failed to "
				  "convert name to cstr for some reason");
		return false;
	}

	bfree(ca->device_name);
	ca->device_name = name;

	if (cf_name)
		CFRelease(cf_name);

	return true;
}

static bool coreaudio_start(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->active)
		return true;

	stat = AudioOutputUnitStart(ca->unit);
	return ca_success(stat, ca, "coreaudio_start", "start audio");
}

static void coreaudio_stop(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (!ca->active)
		return;

	ca->active = false;

	stat = AudioOutputUnitStop(ca->unit);
	ca_success(stat, ca, "coreaudio_stop", "stop audio");
}

static bool coreaudio_init_unit(struct coreaudio_data *ca)
{
	AudioComponentDescription desc = {.componentType = kAudioUnitType_Output,
					  .componentSubType = kAudioUnitSubType_HALOutput};

	AudioComponent component = AudioComponentFindNext(NULL, &desc);
	if (!component) {
		ca_warn(ca, "coreaudio_init_unit", "find component failed");
		return false;
	}

	OSStatus stat = AudioComponentInstanceNew(component, &ca->unit);
	if (!ca_success(stat, ca, "coreaudio_init_unit", "instance unit"))
		return false;

	ca->au_initialized = true;
	return true;
}

static bool coreaudio_init(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->au_initialized)
		return true;

	if (!find_device_id_by_uid(ca))
		return false;
	if (!coreaudio_get_device_name(ca))
		return false;
	if (!coreaudio_init_unit(ca))
		return false;

	stat = enable_io(ca, IO_TYPE_INPUT, true);
	if (!ca_success(stat, ca, "coreaudio_init", "enable input io"))
		goto fail;

	stat = enable_io(ca, IO_TYPE_OUTPUT, false);
	if (!ca_success(stat, ca, "coreaudio_init", "disable output io"))
		goto fail;

	stat = set_property(ca->unit, kAudioOutputUnitProperty_CurrentDevice, SCOPE_GLOBAL, 0, &ca->device_id,
			    sizeof(ca->device_id));
	if (!ca_success(stat, ca, "coreaudio_init", "set current device"))
		goto fail;

	if (!coreaudio_init_format(ca))
		goto fail;
	if (!coreaudio_init_buffer(ca))
		goto fail;
	if (!coreaudio_init_hooks(ca))
		goto fail;

	stat = AudioUnitInitialize(ca->unit);
	if (!ca_success(stat, ca, "coreaudio_initialize", "initialize"))
		goto fail;

	if (!coreaudio_start(ca))
		goto fail;

	blog(LOG_INFO, "coreaudio: Device '%s' [%" PRIu32 " Hz, %" PRIu32 " ch] initialized", ca->device_name,
	     ca->sample_rate, ca->available_channels);

	/* This used to be the ONE place OBS mentioned the device's real rate, at LOG_INFO, once,
	 * at startup.  It is now the third copy -- the mixer strip and the properties dialog both
	 * carry it too -- and it is a warning, because a silent degradation being silent is the
	 * bug we are fixing. */
	struct dstr notice;
	if (coreaudio_format_notice(ca, &notice)) {
		blog(LOG_WARNING, "coreaudio: DEGRADED INPUT FORMAT -- %s", notice.array);
		dstr_free(&notice);
	}

	return ca->au_initialized;

fail:
	coreaudio_uninit(ca);
	return false;
}

static void coreaudio_try_init(struct coreaudio_data *ca)
{
	if (!coreaudio_init(ca)) {
		blog(LOG_INFO,
		     "coreaudio: failed to find device "
		     "uid: %s, waiting for connection",
		     ca->device_uid);

		ca->retry_time = 2000;

		if (ca->no_devices)
			blog(LOG_INFO, "coreaudio: no device found");
		else
			coreaudio_begin_reconnect(ca);
	}
}

static void coreaudio_uninit(struct coreaudio_data *ca)
{
	if (!ca->au_initialized)
		return;

	if (ca->unit) {
		coreaudio_stop(ca);

		OSStatus stat = AudioUnitUninitialize(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "uninitialize");

		coreaudio_remove_hooks(ca);

		stat = AudioComponentInstanceDispose(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "dispose");

		ca->unit = NULL;
	}

	ca->au_initialized = false;

	buf_list_free(ca->buf_list);
	ca->buf_list = NULL;

	if (ca->channel_names) {
		for (uint32_t i = 0; i < ca->available_channels; i++) {
			bfree(ca->channel_names[i]);
		}
		bfree(ca->channel_names);
		ca->channel_names = NULL;
	}
}

/* ------------------------------------------------------------------------- */

static const char *coreaudio_input_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_AUDIO_INPUT;
}

static const char *coreaudio_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_AUDIO_OUTPUT;
}

static void coreaudio_shutdown(struct coreaudio_data *ca)
{
	if (ca->reconnecting) {
		os_event_signal(ca->exit_event);
		pthread_join(ca->reconnect_thread, NULL);
		os_event_reset(ca->exit_event);
	}

	coreaudio_uninit(ca);

	if (ca->unit)
		AudioComponentInstanceDispose(ca->unit);
}

static void coreaudio_destroy(void *data)
{
	struct coreaudio_data *ca = data;

	if (ca) {
		coreaudio_shutdown(ca);
		/* If the device is also used for monitoring, a cleanup is needed. */
		if (!ca->input)
			obs_source_audio_output_capture_device_changed(ca->source, NULL);

		os_event_destroy(ca->exit_event);

		if (ca->channel_map) {
			bfree(ca->channel_map);
			ca->channel_map = NULL;
		}

		bfree(ca->device_name);
		bfree(ca->device_uid);
		bfree(ca);
	}
}

static void coreaudio_set_channels(struct coreaudio_data *ca, obs_data_t *settings)
{
	ca->channel_map = bzalloc(sizeof(SInt32) * MAX_AUDIO_CHANNELS);

	char *device_config_name = sanitize_device_name(ca->device_uid);
	for (uint8_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		char setting_name[128];
		snprintf(setting_name, 128, "output-%s-%i", device_config_name, i + 1);
		int64_t found = obs_data_has_user_value(settings, setting_name)
					? obs_data_get_int(settings, setting_name)
					: -1L;
		int64_t adjusted = found > 0 ? found - 1 : -1;
		ca->channel_map[i] = (int32_t)adjusted;
	}
	bfree(device_config_name);
}

static void coreaudio_update(void *data, obs_data_t *settings)
{
	struct coreaudio_data *ca = data;
	const char *new_id = obs_data_get_string(settings, "device_id");

	if (!ca->input && strcmp(new_id, ca->device_uid) != 0)
		obs_source_audio_output_capture_device_changed(ca->source, new_id);

	coreaudio_shutdown(ca);

	bfree(ca->device_uid);
	ca->device_uid = bstrdup(new_id);

	ca->enable_downmix = obs_data_get_bool(settings, "enable_downmix");

	if (!ca->enable_downmix) {
		coreaudio_set_channels(ca, settings);
	}

	coreaudio_try_init(ca);
}

static void coreaudio_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device_id", "default");
	obs_data_set_default_bool(settings, "enable_downmix", true);
}

static void *coreaudio_create(obs_data_t *settings, obs_source_t *source, bool input)
{
	struct coreaudio_data *ca = bzalloc(sizeof(struct coreaudio_data));

	if (os_event_init(&ca->exit_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR,
		     "[coreaudio_create] failed to create "
		     "semephore: %d",
		     errno);
		bfree(ca);
		return NULL;
	}

	ca->device_uid = bstrdup(obs_data_get_string(settings, "device_id"));
	ca->source = source;
	ca->input = input;
	ca->enable_downmix = obs_data_get_bool(settings, "enable_downmix");

	if (!ca->enable_downmix) {
		coreaudio_set_channels(ca, settings);
	}

	if (!ca->device_uid)
		ca->device_uid = bstrdup("default");

	coreaudio_try_init(ca);
	if (!ca->input)
		obs_source_audio_output_capture_device_changed(source, ca->device_uid);

	return ca;
}

static void *coreaudio_create_input_capture(obs_data_t *settings, obs_source_t *source)
{
	return coreaudio_create(settings, source, true);
}

static void *coreaudio_create_output_capture(obs_data_t *settings, obs_source_t *source)
{
	return coreaudio_create(settings, source, false);
}

static void coreaudio_fill_combo_with_inputs(const struct coreaudio_data *ca, obs_property_t *input_combo,
					     uint32_t output_channel)
{
	bool hasMutedChannel = false;
	obs_property_list_clear(input_combo);

	if (output_channel < ca->available_channels) {
		obs_property_list_add_int(input_combo, ca->channel_names[output_channel], output_channel + 1);
	} else {
		obs_property_list_add_int(input_combo, obs_module_text("CoreAudio.None"), -1);
		hasMutedChannel = true;
	}

	for (uint32_t input_chan = 0; input_chan < ca->available_channels; input_chan++) {

		if (input_chan != output_channel) {
			obs_property_list_add_int(input_combo, ca->channel_names[input_chan], input_chan + 1);
		}
	}

	if (!hasMutedChannel) {
		obs_property_list_add_int(input_combo, obs_module_text("CoreAudio.None"), -1);
	}
}

static void ensure_output_channel_prop(const struct coreaudio_data *ca, obs_properties_t *props,
				       const char *device_config_name, uint32_t out_chan)
{
	struct dstr name;
	dstr_init(&name);
	dstr_printf(&name, "output-%s-%d", device_config_name, out_chan + 1);

	obs_property_t *prop = obs_properties_get(props, name.array);

	if (prop) {
		obs_property_set_visible(prop, true);
	} else {
		struct dstr label;
		dstr_init(&label);
		dstr_printf(&label, "%s %i", obs_module_text("CoreAudio.Channel"), out_chan + 1);
		obs_property_t *input_combo = obs_properties_add_list(props, name.array, label.array,
								      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		dstr_free(&label);
		coreaudio_fill_combo_with_inputs(ca, input_combo, out_chan);
	}
	dstr_free(&name);
}

static void ensure_output_channels_visible(obs_properties_t *props, const struct coreaudio_data *ca, uint32_t channels)
{
	char *device_config_name = sanitize_device_name(ca->device_uid);
	for (uint32_t out_chan = 0; out_chan < channels; out_chan++) {
		ensure_output_channel_prop(ca, props, device_config_name, out_chan);
	}
	bfree(device_config_name);
}

static void hide_all_output_channels(obs_properties_t *props)
{
	for (obs_property_t *prop = obs_properties_first(props); prop != NULL; obs_property_next(&prop)) {
		const char *prop_name = obs_property_name(prop);
		if (strncmp("output-", prop_name, 7) == 0) {
			obs_property_set_visible(prop, false);
		}
	}
}

static bool coreaudio_device_changed(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct coreaudio_data *ca = data;
	if (ca != NULL) {
		hide_all_output_channels(props);

		if (!ca->enable_downmix) {
			uint32_t channels = get_audio_channels(ca->speakers);
			ensure_output_channels_visible(props, ca, channels);
		}
	}
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(settings);
	return true;
}

static bool coreaudio_downmix_changed(void *data, obs_properties_t *props, obs_property_t *p __unused,
				      obs_data_t *settings)
{
	struct coreaudio_data *ca = data;
	if (ca != NULL) {
		bool enable_downmix = obs_data_get_bool(settings, "enable_downmix");
		ca->enable_downmix = enable_downmix;

		hide_all_output_channels(props);

		if (!ca->enable_downmix) {
			uint32_t channels = get_audio_channels(ca->speakers);
			ensure_output_channels_visible(props, ca, channels);
		}
	}

	return true;
}

static obs_properties_t *coreaudio_properties(bool input, void *data)
{
	struct coreaudio_data *ca = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *property;
	struct device_list devices;

	memset(&devices, 0, sizeof(struct device_list));

	property =
		obs_properties_add_list(props, "device_id", TEXT_DEVICE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	coreaudio_enum_devices(&devices, input);

	if (devices.items.num)
		obs_property_list_add_string(property, TEXT_DEVICE_DEFAULT, "default");

	for (size_t i = 0; i < devices.items.num; i++) {
		struct device_item *item = devices.items.array + i;
		obs_property_list_add_string(property, item->name.array, item->value.array);
	}

	obs_property_set_modified_callback2(property, coreaudio_device_changed, ca);

	/* Sits directly under the device combo, so it appears at the moment the operator picks
	 * the device -- which is when the decision is actually being made.  The mixer strip
	 * carries the two-word version for the rest of the session; this is where there is room
	 * to explain why.  Left with an empty settings value and no long description on purpose:
	 * properties-view then spans it full width and styles it "text-warning". */
	if (ca != NULL && ca->au_initialized) {
		struct dstr notice;

		if (coreaudio_format_notice(ca, &notice)) {
			obs_property_t *warn = obs_properties_add_text(props, "qci_format_notice", notice.array,
								      OBS_TEXT_INFO);
			obs_property_text_set_info_type(warn, OBS_TEXT_INFO_WARNING);
			obs_property_text_set_info_word_wrap(warn, true);
			dstr_free(&notice);
		}
	}

	property = obs_properties_add_bool(props, "enable_downmix", obs_module_text("CoreAudio.Downmix"));
	obs_property_set_modified_callback2(property, coreaudio_downmix_changed, ca);

	if (ca != NULL && ca->au_initialized) {
		uint32_t channels = get_audio_channels(ca->speakers);
		ensure_output_channels_visible(props, ca, channels);

		if (ca->enable_downmix) {
			hide_all_output_channels(props);
		}
	}

	device_list_free(&devices);
	return props;
}

static obs_properties_t *coreaudio_input_properties(void *data)
{
	return coreaudio_properties(true, data);
}

static obs_properties_t *coreaudio_output_properties(void *data)
{
	return coreaudio_properties(false, data);
}

struct obs_source_info coreaudio_input_capture_info = {
	.id = "coreaudio_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = coreaudio_input_getname,
	.create = coreaudio_create_input_capture,
	.destroy = coreaudio_destroy,
	.update = coreaudio_update,
	.get_defaults = coreaudio_defaults,
	.get_properties = coreaudio_input_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};

struct obs_source_info coreaudio_output_capture_info = {
	.id = "coreaudio_output_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR,
	.get_name = coreaudio_output_getname,
	.create = coreaudio_create_output_capture,
	.destroy = coreaudio_destroy,
	.update = coreaudio_update,
	.get_defaults = coreaudio_defaults,
	.get_properties = coreaudio_output_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};
