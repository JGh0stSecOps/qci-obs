// qci-audio-unit — host an AudioUnit v2 effect as an OBS audio filter.
//
// WHY THIS EXISTS. AudioUnit is the macOS-native effect format: it ships in every macOS, it is
// what the host OS itself uses, and it is the format the mic-chain effects the operator wants
// are distributed in. This module makes those reachable from an OBS filter.
//
// ⚠️ IT IS NOT A REPLACEMENT FOR obs-vst, and the brief that requested it said otherwise. The
// premise handed to this work was that "VST2 hosting is inert on Apple Silicon", so obs-vst
// enumerates nothing here. That was checked against the machine and it is NOT true:
//
//     /Library/Audio/Plug-Ins/VST/AmpliTube 5.vst              x86_64 arm64
//     /Library/Audio/Plug-Ins/VST/Auburn Sounds Graillon 3.vst x86_64 arm64
//
// Both are arm64-native, both sit in a directory obs-vst scans (obs-vst.cpp fill_out_plugins),
// and obs-vst loads them through CFBundleGetFunctionPointerForName("VSTPluginMain"), which is
// architecture-agnostic. obs-vst is still built for macOS in this tree and should still work.
// AU is an ADDITION to the operator's options, not a rescue from a broken path.
//
// Also worth knowing before anyone tries this out: none of the four effects this was requested
// for (TDR Nova, TDR VOS SlickEQ, TDR Kotelnikov, LoudMax) are installed on this machine in any
// format — not AU, not VST2, not VST3. The only third-party unit present is AmpliTube 5, and it
// registers as 'aumf' rather than 'aufx' (see qci_au_types below). Apple's own 'aufx' units
// (AUNBandEQ, AUDynamicsProcessor, AUPeakLimiter) are what this can be smoke-tested against
// today.
//
// SCOPE — AudioUnit v2 ONLY (kAudioUnitType_Effect 'aufx' and kAudioUnitType_MusicEffect
// 'aumf', instantiated through AudioComponentInstanceNew). AUv3 / out-of-process hosting is
// DELIBERATELY OUT OF SCOPE: it is a different API surface (AUAudioUnit
// instantiateWithComponentDescription:options:completionHandler:), a different render path (an
// AUInternalRenderBlock rather than AudioUnitRender), asynchronous instantiation that does not
// fit obs_source_info::update, and XPC plumbing that needs app-group entitlements the plugin
// bundle does not carry. Note that macOS bridges most AUv3 effects back into the v2 component
// namespace, so many of them are reachable through this host anyway.
//
// THREADING. filter_audio runs on OBS's audio thread. Everything it touches is preallocated and
// lock-free: the active engine is swapped with an atomic store and the retired engine is freed
// only after the audio thread is observed to be outside a render. There is no allocation, no
// mutex and no Objective-C message send below render_engine().
//
// engine_mutex serialises the NON-realtime users of the engine (update, save, the properties
// pane, the editor window) against each other. The audio thread NEVER takes it. Without it,
// obs-websocket calling obs_source_update on its own thread while the UI thread is inside
// obs_source_save is a use-after-free on the AudioUnit: save reads e->unit while update is
// disposing it.

#include <obs-module.h>
#include <util/platform.h>
#include <media-io/audio-io.h>

#import <AudioToolbox/AudioToolbox.h>
// AUCocoaUIBase — the protocol a unit's view factory conforms to. NOT reachable through the
// AudioToolbox umbrella header (it is guarded on __OBJC__ && TARGET_OS_OSX), so it has to be
// imported by name or the @protocol reference below fails to compile.
#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <Cocoa/Cocoa.h>

#include <pthread.h>
#include <stdatomic.h>

#include "qci-au-format.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("qci-audio-unit", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "AudioUnit (v2) effect host";
}

#define S_COMPONENT "component"      // qci_au_key_format() identity of the chosen effect
#define S_STATE "state"              // kAudioUnitProperty_ClassInfo, as an XML plist
#define S_STATE_FOR "state_for"      // which component S_STATE was captured from
#define S_OPEN_UI "open_ui"

#define do_log(level, format, ...) \
	blog(level, "[qci-audio-unit: '%s'] " format, obs_source_get_name(f->context), ##__VA_ARGS__)

// Ceiling on a single AudioUnitRender call. OBS normally hands filters AUDIO_OUTPUT_FRAMES
// (1024) but an async source can deliver whatever its device produced, so anything larger is
// sliced rather than assumed impossible.
#define QCI_AU_MAX_SLICE 4096

// ── engine: everything the audio thread touches ────────────────────────────────────────────

struct au_engine {
	AudioUnit unit;
	struct qci_au_channel_plan plan;
	uint64_t latency_ns;

	// The mix this engine was BUILT for. Kept so update() can tell a settings change that
	// changes nothing material (re-open the properties pane, hit OK) from one that genuinely
	// invalidates the unit. Rebuilding needlessly is not free: it discards every parameter the
	// operator has tweaked in the plug-in's own window since the last scene save.
	uint32_t sample_rate;
	bool source_is_mono;

	// Preallocated. AudioBufferList is a variable-length struct, so it is sized for
	// plan.au_channels at construction and never resized.
	AudioBufferList *out_abl;
	float *scratch;        // au_channels * QCI_AU_MAX_SLICE, the AU's output
	Float64 sample_time;   // monotonic, fed to AudioUnitRender

	// Handoff to the input render callback, written immediately before AudioUnitRender and
	// read only from inside it, on the same thread.
	float *in_planes[MAX_AUDIO_CHANNELS];
	UInt32 in_offset;
};

struct qci_au_filter {
	obs_source_t *context;

	// Read by the audio thread, written by whichever thread runs update().
	_Atomic(struct au_engine *) engine;
	atomic_bool in_render;

	// Serialises the non-realtime users of `engine` against each other. Never taken on the
	// audio thread — see the THREADING note at the top of this file.
	pthread_mutex_t engine_mutex;

	// Returned from filter_audio so the timestamp can carry the latency compensation without
	// mutating the source's own obs_audio_data.
	struct obs_audio_data out;

	// Main-thread only. Strong under ARC; the struct is bzalloc'd, so `editor` starts nil and
	// must be cleared through close_editor() rather than by bfree(), which does not run ARC's
	// release for a field inside malloc'd memory.
	NSWindow *editor;

	char component_key[QCI_AU_KEY_SIZE];
};

// Defined with the editor code further down; update() has to close the window before it disposes
// the AudioUnit that window is drawing.
static void close_editor(struct qci_au_filter *f);

// ── discovery ──────────────────────────────────────────────────────────────────────────────

// 'aufx' is the effect type the brief names. 'aumf' is included because a music effect is an
// effect that additionally accepts MIDI — it renders audio through exactly the same
// AudioUnitRender path — and because on this machine the ONLY third-party audio unit installed
// (IK Multimedia's AmpliTube 5) registers as 'aumf'. Scanning 'aufx' alone would present the
// operator an all-Apple list and make the feature look broken.
static const OSType qci_au_types[] = {kAudioUnitType_Effect, kAudioUnitType_MusicEffect};

static void fill_component_list(obs_property_t *list)
{
	obs_property_list_add_string(list, obs_module_text("AudioUnit.None"), "");

	for (size_t t = 0; t < sizeof(qci_au_types) / sizeof(qci_au_types[0]); t++) {
		AudioComponentDescription search = {0};
		search.componentType = qci_au_types[t];
		// Zero subtype/manufacturer are wildcards for AudioComponentFindNext.

		AudioComponent comp = NULL;
		while ((comp = AudioComponentFindNext(comp, &search)) != NULL) {
			AudioComponentDescription desc = {0};
			if (AudioComponentGetDescription(comp, &desc) != noErr)
				continue;

			CFStringRef cf_name = NULL;
			if (AudioComponentCopyName(comp, &cf_name) != noErr || !cf_name)
				continue;

			// AudioComponentCopyName returns "Manufacturer: Effect Name" already, which
			// is exactly the manufacturer + name pairing the operator needs to tell two
			// similarly named effects apart.
			char name[512];
			if (CFStringGetCString(cf_name, name, sizeof(name), kCFStringEncodingUTF8)) {
				char key[QCI_AU_KEY_SIZE];
				qci_au_key_format(key, desc.componentType, desc.componentSubType,
						  desc.componentManufacturer);
				obs_property_list_add_string(list, name, key);
			}
			CFRelease(cf_name);
		}
	}
}

// ── engine construction (never on the audio thread) ────────────────────────────────────────

static OSStatus au_input_callback(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
				  UInt32 bus, UInt32 frames, AudioBufferList *io)
{
	UNUSED_PARAMETER(flags);
	UNUSED_PARAMETER(ts);
	UNUSED_PARAMETER(bus);

	struct au_engine *e = ref;

	// Zero-copy: hand the unit pointers into the buffer OBS already gave us. Legal — the
	// callback is allowed to replace the mData pointers rather than fill the supplied ones —
	// and it is what keeps this path allocation-free.
	for (UInt32 c = 0; c < io->mNumberBuffers; c++) {
		float *plane = (c < e->plan.au_channels) ? e->in_planes[c] : NULL;

		io->mBuffers[c].mNumberChannels = 1;
		io->mBuffers[c].mDataByteSize = frames * (UInt32)sizeof(float);
		io->mBuffers[c].mData = plane ? (plane + e->in_offset) : NULL;
	}

	return noErr;
}

static void engine_destroy(struct au_engine *e)
{
	if (!e)
		return;

	if (e->unit) {
		AudioUnitUninitialize(e->unit);
		AudioComponentInstanceDispose(e->unit);
	}

	bfree(e->out_abl);
	bfree(e->scratch);
	bfree(e);
}

// Reads kAudioUnitProperty_SupportedNumChannels and decides how to drive the unit.
static struct qci_au_channel_plan read_channel_plan(AudioUnit unit, uint32_t obs_planes, bool source_is_mono)
{
	UInt32 size = 0;
	AUChannelInfo *infos = NULL;
	size_t count = 0;

	if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0,
				     &size, NULL) == noErr &&
	    size >= sizeof(AUChannelInfo)) {
		infos = bmalloc(size);
		if (AudioUnitGetProperty(unit, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0,
					 infos, &size) == noErr)
			count = size / sizeof(AUChannelInfo);
	}

	// count == 0 is meaningful, not an error: it means the unit does not publish the property
	// and handles any matched configuration. qci_au_plan_channels encodes that.
	struct qci_au_channel_plan plan = qci_au_plan_channels(infos, count, obs_planes, source_is_mono);
	bfree(infos);
	return plan;
}

static struct au_engine *engine_create(struct qci_au_filter *f, const char *key, uint32_t obs_planes,
				       uint32_t sample_rate, bool source_is_mono)
{
	uint32_t type = 0, subtype = 0, manufacturer = 0;
	if (!qci_au_key_parse(key, &type, &subtype, &manufacturer)) {
		do_log(LOG_WARNING, "unparsable component key '%s'", key);
		return NULL;
	}

	AudioComponentDescription desc = {0};
	desc.componentType = type;
	desc.componentSubType = subtype;
	desc.componentManufacturer = manufacturer;

	AudioComponent comp = AudioComponentFindNext(NULL, &desc);
	if (!comp) {
		do_log(LOG_WARNING, "audio unit %s is not installed", key);
		return NULL;
	}

	AudioUnit unit = NULL;
	OSStatus err = AudioComponentInstanceNew(comp, &unit);
	if (err != noErr || !unit) {
		do_log(LOG_WARNING, "AudioComponentInstanceNew failed (%d)", (int)err);
		return NULL;
	}

	struct qci_au_channel_plan plan = read_channel_plan(unit, obs_planes, source_is_mono);
	if (plan.mode == QCI_AU_CHAN_UNSUPPORTED) {
		do_log(LOG_WARNING,
		       "audio unit cannot run at %u channel(s) in and out; an OBS filter may not "
		       "change the channel count, so it is bypassed",
		       obs_planes);
		AudioComponentInstanceDispose(unit);
		return NULL;
	}

	// MaximumFramesPerSlice and the stream formats must all be set BEFORE
	// AudioUnitInitialize — that is when the unit sizes its internal buffers.
	UInt32 max_frames = QCI_AU_MAX_SLICE;
	AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames,
			     sizeof(max_frames));

	AudioStreamBasicDescription asbd;
	qci_au_make_asbd(&asbd, (double)sample_rate, plan.au_channels);

	err = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd,
				   sizeof(asbd));
	if (err == noErr)
		err = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &asbd,
					   sizeof(asbd));
	if (err != noErr) {
		do_log(LOG_WARNING, "unit rejected %u-channel non-interleaved float at %u Hz (%d)",
		       plan.au_channels, sample_rate, (int)err);
		AudioComponentInstanceDispose(unit);
		return NULL;
	}

	struct au_engine *e = bzalloc(sizeof(*e));
	e->unit = unit;
	e->plan = plan;
	e->sample_rate = sample_rate;
	e->source_is_mono = source_is_mono;

	AURenderCallbackStruct cb = {.inputProc = au_input_callback, .inputProcRefCon = e};
	AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));

	err = AudioUnitInitialize(unit);
	if (err != noErr) {
		do_log(LOG_WARNING, "AudioUnitInitialize failed (%d)", (int)err);
		e->unit = NULL;
		AudioComponentInstanceDispose(unit);
		engine_destroy(e);
		return NULL;
	}

	// LATENCY. kAudioUnitProperty_Latency is Float64 seconds, global scope, read-only. It is
	// read AFTER initialisation because a unit's reported latency can depend on the stream
	// format it was configured with (lookahead expressed in samples).
	Float64 latency_s = 0.0;
	UInt32 latency_size = sizeof(latency_s);
	if (AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency_s,
				 &latency_size) == noErr)
		e->latency_ns = qci_au_latency_ns(latency_s);

	const size_t abl_size = offsetof(AudioBufferList, mBuffers) + plan.au_channels * sizeof(AudioBuffer);
	e->out_abl = bzalloc(abl_size);
	e->out_abl->mNumberBuffers = plan.au_channels;
	e->scratch = bmalloc(sizeof(float) * plan.au_channels * QCI_AU_MAX_SLICE);

	do_log(LOG_INFO, "loaded %s: %u channel(s), %s, latency %.2f ms", key, plan.au_channels,
	       plan.mode == QCI_AU_CHAN_MONO ? "mono source (plane 0, mirrored)" : "direct",
	       (double)e->latency_ns / 1.0e6);

	return e;
}

// Swaps in a new engine and disposes the old one only once the audio thread is known not to be
// inside a render that could still hold it.
//
// No mutex is taken on the audio thread. The audio thread sets in_render BEFORE loading the
// engine pointer and clears it after, both seq_cst, so once the store below is visible any
// render observed as not-in-flight will load the NEW pointer when it starts.
static void engine_swap(struct qci_au_filter *f, struct au_engine *next)
{
	struct au_engine *prev = atomic_exchange(&f->engine, next);
	if (!prev)
		return;

	// Bounded: if the audio thread never reports quiescent we leak one engine rather than
	// free memory it may still be rendering through. A leak is recoverable; a use-after-free
	// on the audio thread of a live stream is not.
	for (int i = 0; i < 200; i++) {
		if (!atomic_load(&f->in_render)) {
			engine_destroy(prev);
			return;
		}
		os_sleep_ms(1);
	}

	blog(LOG_WARNING, "[qci-audio-unit] audio thread never went quiescent; leaking one engine");
}

// ── render ─────────────────────────────────────────────────────────────────────────────────
//
// Realtime path. No allocation, no locks, no Objective-C.

static void render_engine(struct au_engine *e, struct obs_audio_data *audio, uint32_t planes)
{
	const uint32_t chans = e->plan.au_channels;
	uint32_t done = 0;

	while (done < audio->frames) {
		const UInt32 slice = (UInt32)((audio->frames - done > QCI_AU_MAX_SLICE)
						      ? QCI_AU_MAX_SLICE
						      : audio->frames - done);

		for (uint32_t c = 0; c < chans; c++) {
			e->in_planes[c] = (float *)audio->data[c];
			e->out_abl->mBuffers[c].mNumberChannels = 1;
			e->out_abl->mBuffers[c].mDataByteSize = slice * (UInt32)sizeof(float);
			// Rendered into scratch rather than back over the input: the unit pulls its
			// input through au_input_callback while it writes, and a unit that is not
			// in-place capable would otherwise read samples it has already overwritten.
			e->out_abl->mBuffers[c].mData = e->scratch + (size_t)c * QCI_AU_MAX_SLICE;
		}
		e->in_offset = done;

		AudioUnitRenderActionFlags flags = 0;
		AudioTimeStamp ts = {0};
		ts.mFlags = kAudioTimeStampSampleTimeValid;
		ts.mSampleTime = e->sample_time;

		if (AudioUnitRender(e->unit, &flags, &ts, 0, slice, e->out_abl) != noErr)
			return; // leave this slice as-is rather than emit silence

		for (uint32_t c = 0; c < chans; c++)
			memcpy((float *)audio->data[c] + done, e->scratch + (size_t)c * QCI_AU_MAX_SLICE,
			       slice * sizeof(float));

		// Mono parent: the unit ran once over plane 0, so mirror it back across the planes
		// libobs duplicated the signal into.
		if (e->plan.mode == QCI_AU_CHAN_MONO)
			for (uint32_t c = 1; c < planes; c++)
				if (audio->data[c])
					memcpy((float *)audio->data[c] + done,
					       (float *)audio->data[0] + done, slice * sizeof(float));

		e->sample_time += slice;
		done += slice;
	}
}

static struct obs_audio_data *qci_au_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct qci_au_filter *f = data;

	// Set BEFORE loading the engine pointer: engine_swap() relies on that order to know a
	// render it cannot see has not yet captured the old pointer.
	atomic_store(&f->in_render, true);

	struct au_engine *e = atomic_load(&f->engine);

	if (e && audio->frames && audio->data[0]) {
		const uint32_t planes = (uint32_t)audio_output_get_channels(obs_get_audio());

		// The plan was made for a particular parent layout. If the parent's layout has
		// changed underneath us (a device swap), mono mirroring would smear one channel
		// across a genuine stereo signal, so bypass until update() rebuilds the engine.
		const bool mono_now =
			obs_source_get_speaker_layout(obs_filter_get_parent(f->context)) == SPEAKERS_MONO;
		// libobs NULLs the planes past the active channel count (obs-source.c
		// obs_source_output_audio does it explicitly, because filters were found checking
		// the pointer rather than the count). Every plane the unit will be handed must be
		// real memory before we let it write there.
		bool planes_present = true;
		for (uint32_t c = 0; c < e->plan.au_channels; c++)
			if (!audio->data[c])
				planes_present = false;

		const bool plan_still_valid = planes_present &&
					      (e->plan.mode != QCI_AU_CHAN_MONO || mono_now) &&
					      planes == e->plan.obs_planes;

		if (plan_still_valid) {
			render_engine(e, audio, planes);

			// LATENCY COMPENSATION. libobs has no filter-latency API — there is no field
			// on obs_source_info and no obs_source_* entry point for it (the only
			// related call, obs_source_set_sync_offset, is a per-source manual A/V
			// nudge the operator owns). The idiom filters actually use is to back-date
			// the outgoing timestamp by the delay they introduce; see
			// plugins/obs-filters/noise-suppress-filter.c, which does exactly this with
			// its own 10 ms buffering latency. We follow it.
			f->out = *audio;
			// Guarded subtraction: obs timestamps are unsigned nanoseconds, and a source
			// whose first buffers land with a timestamp smaller than the unit's reported
			// latency would otherwise wrap to something near UINT64_MAX and throw the
			// mixer's sync tracking a few hundred years into the future.
			f->out.timestamp = (audio->timestamp > e->latency_ns)
						   ? audio->timestamp - e->latency_ns
						   : 0;
			atomic_store(&f->in_render, false);
			return &f->out;
		}
	}

	atomic_store(&f->in_render, false);
	return audio;
}

// ── settings ───────────────────────────────────────────────────────────────────────────────

static void apply_saved_state(struct qci_au_filter *f, struct au_engine *e, obs_data_t *settings)
{
	const char *state = obs_data_get_string(settings, S_STATE);
	const char *state_for = obs_data_get_string(settings, S_STATE_FOR);

	if (!state || !*state)
		return;

	// The same guard obs-vst applies with its chunk_hash: state belonging to a different
	// effect must never be pushed into this one. obs-vst compares an MD5 of the plug-in file;
	// the component triple is the equivalent identity here, and a more exact one.
	if (!state_for || strcmp(state_for, f->component_key) != 0) {
		do_log(LOG_INFO, "saved state belongs to a different effect; not restoring");
		return;
	}

	CFPropertyListRef plist = qci_au_classinfo_from_string(state);
	if (!plist) {
		do_log(LOG_WARNING, "saved state is not a usable ClassInfo dictionary");
		return;
	}

	OSStatus err = AudioUnitSetProperty(e->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
					    &plist, sizeof(plist));
	CFRelease(plist);

	if (err != noErr) {
		do_log(LOG_WARNING, "unit rejected saved ClassInfo (%d)", (int)err);
		return;
	}

	// Required by the kAudioUnitProperty_ClassInfo contract: after a host sets it, parameter
	// listeners must be told, or an open editor keeps drawing the previous values.
	AudioUnitParameter changed = {.mAudioUnit = e->unit, .mParameterID = kAUParameterListener_AnyParameter};
	AUParameterListenerNotify(NULL, NULL, &changed);
}

static void qci_au_update(void *data, obs_data_t *settings)
{
	struct qci_au_filter *f = data;
	const char *key = obs_data_get_string(settings, S_COMPONENT);

	// BEFORE the mutex, and before anything is disposed. The editor window draws an NSView that
	// belongs to the plug-in and is bound to a live AudioUnit; disposing that unit underneath an
	// open window leaves the view messaging freed memory the next time the operator touches a
	// knob. Closing first is also why this is not called with engine_mutex held: close_editor may
	// dispatch_sync to the main thread, which could itself be waiting on the mutex inside
	// open_editor, and that pair deadlocks.
	close_editor(f);

	pthread_mutex_lock(&f->engine_mutex);

	if (!key || !*key) {
		f->component_key[0] = '\0';
		engine_swap(f, NULL);
		pthread_mutex_unlock(&f->engine_mutex);
		return;
	}

	const struct qci_au_mix now = {
		.planes = (uint32_t)audio_output_get_channels(obs_get_audio()),
		.sample_rate = audio_output_get_sample_rate(obs_get_audio()),
		// NULL parent at create() time — a filter is constructed before obs_source_filter_add
		// binds it to a source — and SPEAKERS_UNKNOWN until the parent has produced its first
		// buffer, so this is false on a cold scene load and may become true later.
		// filter_add() re-runs this function for exactly that reason.
		.source_is_mono = obs_source_get_speaker_layout(obs_filter_get_parent(f->context)) == SPEAKERS_MONO,
	};

	// Nothing material changed: same effect, same mix, same parent layout. Rebuilding here would
	// throw away every parameter the operator has adjusted in the plug-in's own window since the
	// last scene save — obs_source_update is called for any settings write, including ones this
	// filter does not care about. The predicate is pure and unit-tested; see qci-au-format.c.
	struct au_engine *cur = atomic_load(&f->engine);
	if (cur) {
		const struct qci_au_mix built = {cur->plan.obs_planes, cur->sample_rate, cur->source_is_mono};

		if (qci_au_engine_still_valid(f->component_key, built, key, now)) {
			pthread_mutex_unlock(&f->engine_mutex);
			return;
		}
	}

	snprintf(f->component_key, sizeof(f->component_key), "%s", key);

	struct au_engine *e = engine_create(f, key, now.planes, now.sample_rate, now.source_is_mono);
	if (e)
		apply_saved_state(f, e, settings);

	engine_swap(f, e);
	pthread_mutex_unlock(&f->engine_mutex);
}

// Re-runs update() now that the filter is bound to a parent. libobs calls this from
// obs_source_filter_add AFTER filter_parent is set (obs-source.c), which is the first moment
// obs_source_get_speaker_layout can answer — at create() time the parent is still NULL, so a
// mono mic would otherwise be processed as if it were stereo for the lifetime of the filter.
// update() short-circuits when nothing material changed, so this is free in the common case.
static void qci_au_filter_add(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);

	struct qci_au_filter *f = data;
	obs_data_t *settings = obs_source_get_settings(f->context);

	qci_au_update(f, settings);
	obs_data_release(settings);
}

static void qci_au_save(void *data, obs_data_t *settings)
{
	struct qci_au_filter *f = data;

	// Held across the AudioUnitGetProperty: without it, an obs_source_update arriving on another
	// thread (obs-websocket does exactly this) can dispose e->unit between the load and the call.
	pthread_mutex_lock(&f->engine_mutex);

	struct au_engine *e = atomic_load(&f->engine);
	if (!e) {
		// Leave any previously saved S_STATE in `settings` untouched. A unit that failed to
		// instantiate this session — the operator uninstalled it, or it is still scanning —
		// must not cause the scene collection to forget a preset it can no longer read.
		pthread_mutex_unlock(&f->engine_mutex);
		return;
	}

	CFPropertyListRef plist = NULL;
	UInt32 size = sizeof(plist);
	OSStatus err = AudioUnitGetProperty(e->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist,
					    &size);

	pthread_mutex_unlock(&f->engine_mutex);

	if (err != noErr || !plist)
		return;

	char *xml = qci_au_classinfo_to_string(plist);
	CFRelease(plist);

	if (xml) {
		obs_data_set_string(settings, S_STATE, xml);
		obs_data_set_string(settings, S_STATE_FOR, f->component_key);
		free(xml);
	}
}

// ── editor window ──────────────────────────────────────────────────────────────────────────
//
// Follows the shape of plugins/obs-vst/mac/EditorWidget-osx.mm — ask the plug-in for its own
// view, size the container to it — but hosts it in a plain NSWindow rather than a QWidget.
// obs-vst needs createWindowContainer because a VST2 editor is opened by handing the plug-in a
// raw NSView to draw into; an AudioUnit hands back a finished NSView, so there is nothing for
// Qt to wrap and no reason to pull Qt into this module.

static NSView *copy_au_view(AudioUnit unit)
{
	UInt32 size = 0;
	Boolean writable = false;

	if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size,
				     &writable) == noErr &&
	    size >= sizeof(AudioUnitCocoaViewInfo)) {
		AudioUnitCocoaViewInfo *info = bmalloc(size);

		if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size) ==
		    noErr) {
			NSURL *url = (__bridge NSURL *)info->mCocoaAUViewBundleLocation;
			NSString *class_name = (__bridge NSString *)info->mCocoaAUViewClass[0];
			NSView *view = nil;

			// Third-party code signed by someone other than this app's team, loaded into
			// this process — so it needs com.apple.security.cs.disable-library-validation,
			// same as the AudioComponentInstanceNew above (a v2 AudioUnit is loaded
			// in-process too; this view bundle is simply the second such load).
			//
			// VERIFIED PRESENT, not assumed, and not added by this module: the entitlement
			// is in frontend/cmake/macos/entitlements.plist and comes out in the app's
			// generated Entitlements.plist. Library validation is a process-wide property
			// taken from the MAIN EXECUTABLE's entitlements, so nothing a plugin bundle
			// declares would change it either way.
			NSBundle *bundle = url ? [NSBundle bundleWithURL:url] : nil;
			if (bundle && [bundle load]) {
				Class factory_class = [bundle classNamed:class_name];
				if ([factory_class conformsToProtocol:@protocol(AUCocoaUIBase)]) {
					id<AUCocoaUIBase> factory = [[factory_class alloc] init];
					view = [factory uiViewForAudioUnit:unit withSize:NSZeroSize];
				}
			}

			// The host owns references to CF properties it retrieves.
			if (info->mCocoaAUViewBundleLocation)
				CFRelease(info->mCocoaAUViewBundleLocation);
			for (UInt32 i = 0; i < (size - sizeof(CFURLRef)) / sizeof(CFStringRef); i++)
				if (info->mCocoaAUViewClass[i])
					CFRelease(info->mCocoaAUViewClass[i]);

			bfree(info);

			if (view)
				return view;
		} else {
			bfree(info);
		}
	}

	// Every unit gets a usable editor: AUGenericView builds one from the parameter list.
	// LoudMax and similar minimal effects often ship no custom view at all.
	return [[AUGenericView alloc] initWithAudioUnit:unit];
}

static void open_editor(struct qci_au_filter *f)
{
	// Main thread only, checked rather than dispatched to. The sole caller is an obs_properties
	// button callback, which the frontend always fires on the UI thread, so this is a guard and
	// not a limitation. Hopping threads here would be actively harmful: holding engine_mutex
	// across a dispatch_sync to main deadlocks against an update() that is already blocked on
	// that mutex, and a dispatch_async would leave a block holding `f` scheduled after the
	// operator may have removed the filter.
	if (![NSThread isMainThread]) {
		do_log(LOG_WARNING, "editor requested off the main thread; ignoring");
		return;
	}

	// Held for the whole presentation, not just the pointer load: the view is built against a
	// live AudioUnit, and update() disposes units under this same mutex. Deadlock-free because
	// update() closes the editor BEFORE acquiring the mutex — see the comment there — so no
	// thread ever waits on main while main waits on this lock.
	pthread_mutex_lock(&f->engine_mutex);

	struct au_engine *e = atomic_load(&f->engine);
	if (!e) {
		pthread_mutex_unlock(&f->engine_mutex);
		return;
	}

	// Already open — including the case where the operator clicked the window's close button,
	// which only orders it out (releasedWhenClosed is NO), so the same window and the same
	// plug-in view come back rather than a second one being built.
	if (f->editor) {
		[f->editor makeKeyAndOrderFront:nil];
		pthread_mutex_unlock(&f->engine_mutex);
		return;
	}

	NSView *view = copy_au_view(e->unit);
	NSRect frame = view.frame;

	// A unit whose view reports no size yet, and AUGenericView before layout, both land here.
	if (NSIsEmptyRect(frame))
		frame = NSMakeRect(0, 0, 480, 320);

	NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
						      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
								NSWindowStyleMaskMiniaturizable
							backing:NSBackingStoreBuffered
							  defer:NO];

	window.title = [NSString stringWithUTF8String:obs_source_get_name(f->context) ?: "AudioUnit"];
	// The filter owns this window's lifetime, not AppKit: close_editor() has to be able to tear
	// the view down before the AudioUnit behind it is disposed, which it cannot do if closing the
	// window already released it.
	window.releasedWhenClosed = NO;
	window.contentView = view;
	[window center];
	[window makeKeyAndOrderFront:nil];

	f->editor = window;

	pthread_mutex_unlock(&f->engine_mutex);
}

static bool open_ui_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	struct qci_au_filter *f = data;
	if (f)
		open_editor(f);

	return false;
}

static void close_editor(struct qci_au_filter *f)
{
	NSWindow *window = f->editor;
	if (!window)
		return;

	f->editor = nil;

	// The view belongs to the plug-in bundle and must be torn down on the main thread, before
	// the AudioUnit it is bound to is disposed.
	void (^teardown)(void) = ^{
		window.contentView = nil;
		[window close];
	};

	// NOT an unconditional dispatch_sync: a filter is usually destroyed from the UI thread
	// (the operator removes it, or closes the source's filter dialog), and dispatch_sync onto
	// the queue you are already running on deadlocks that thread outright. The teardown has to
	// be synchronous when it is not already on main, because the AudioUnit backing this view
	// is disposed immediately after this returns.
	if ([NSThread isMainThread])
		teardown();
	else
		dispatch_sync(dispatch_get_main_queue(), teardown);
}

// ── obs_source_info ────────────────────────────────────────────────────────────────────────

static const char *qci_au_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioUnit.Filter");
}

static void *qci_au_create(obs_data_t *settings, obs_source_t *filter)
{
	struct qci_au_filter *f = bzalloc(sizeof(*f));
	f->context = filter;
	atomic_init(&f->engine, NULL);
	atomic_init(&f->in_render, false);
	pthread_mutex_init(&f->engine_mutex, NULL);

	qci_au_update(f, settings);
	return f;
}

static void qci_au_destroy(void *data)
{
	struct qci_au_filter *f = data;

	// Order matters: the window draws a view owned by the plug-in and bound to the AudioUnit, so
	// it has to be gone before the unit is disposed. Outside the mutex, because close_editor may
	// block on the main thread.
	close_editor(f);

	pthread_mutex_lock(&f->engine_mutex);
	engine_swap(f, NULL);
	pthread_mutex_unlock(&f->engine_mutex);

	pthread_mutex_destroy(&f->engine_mutex);
	bfree(f);
}

// LATENCY, SURFACED. This rig is optimised for live streaming and has no buffer left to hide
// delay in, so the millisecond cost of a plug-in belongs on screen next to the plug-in, not only
// in the log. The value is whatever the unit reports through kAudioUnitProperty_Latency.
//
// What the number means, exactly: filter_audio back-dates the outgoing timestamp by it, which
// keeps this source ALIGNED with video — it does NOT remove the delay. The operator still hears
// it, and so does anyone monitoring. That is why it is shown rather than quietly compensated.
//
// Above 5 ms it is drawn as a warning. The threshold is the operator's own use case: they play
// instruments live while monitoring themselves, and round-trip monitoring stops feeling
// immediate somewhere around 10 ms total, so a single plug-in eating half that budget is worth
// flagging. Below it, the label is informational.
static void add_latency_property(obs_properties_t *props, struct qci_au_filter *f)
{
	char text[192];
	bool loaded = false;
	double ms = 0.0;

	if (f) {
		pthread_mutex_lock(&f->engine_mutex);
		struct au_engine *e = atomic_load(&f->engine);
		if (e) {
			loaded = true;
			ms = (double)e->latency_ns / 1.0e6;
		}
		pthread_mutex_unlock(&f->engine_mutex);
	}

	// Assembled from a plain label rather than passing a locale string to snprintf as a format:
	// a translator who drops or reorders the conversion specifier would turn a UI string into
	// undefined behaviour.
	if (loaded)
		snprintf(text, sizeof(text), "%s %.2f ms", obs_module_text("AudioUnit.LatencyPrefix"), ms);
	else
		snprintf(text, sizeof(text), "%s %s", obs_module_text("AudioUnit.LatencyPrefix"),
			 obs_module_text("AudioUnit.LatencyUnknown"));

	obs_property_t *info = obs_properties_add_text(props, "latency_info", text, OBS_TEXT_INFO);

	if (loaded && ms > 5.0)
		obs_property_text_set_info_type(info, OBS_TEXT_INFO_WARNING);
}

static obs_properties_t *qci_au_properties(void *data)
{
	struct qci_au_filter *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_COMPONENT, obs_module_text("AudioUnit.Effect"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_component_list(list);

	obs_properties_add_button2(props, S_OPEN_UI, obs_module_text("AudioUnit.OpenInterface"), open_ui_clicked,
				   data);

	add_latency_property(props, f);

	return props;
}

static struct obs_source_info qci_au_filter_info = {
	.id = "qci_audio_unit",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = qci_au_name,
	.create = qci_au_create,
	.destroy = qci_au_destroy,
	.update = qci_au_update,
	.get_properties = qci_au_properties,
	.filter_audio = qci_au_filter_audio,
	.filter_add = qci_au_filter_add,
	.save = qci_au_save,
};

bool obs_module_load(void)
{
	obs_register_source(&qci_au_filter_info);
	blog(LOG_INFO, "[qci-audio-unit] loaded (AudioUnit v2 effect host)");
	return true;
}
