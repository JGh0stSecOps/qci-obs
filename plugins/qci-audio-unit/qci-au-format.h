// qci-au-format — the PURE half of the AudioUnit host filter.
//
// Nothing in this header touches libobs, Objective-C, or a live AudioUnit. That is deliberate:
// it lets qci-au-format-test.c compile and run the tricky parts on a machine with no OBS
// instance, no audio device, and no third-party AU installed. The parts worth testing are
// exactly the parts that are easy to get subtly wrong and impossible to notice at runtime:
//
//   * the non-interleaved ASBD, where mBytesPerFrame means something different than it does
//     for the interleaved case almost every example on the internet shows;
//   * AUChannelInfo negotiation, whose wildcard encoding is genuinely obscure;
//   * the component identity we persist into the scene JSON, which must survive a round trip
//     or the operator's mic chain silently comes back empty after a restart.
//
// The impure half (instantiation, render, Cocoa view) lives in qci-audio-unit.m.
//
// SCOPE: AudioUnit v2 only (kAudioUnitType_Effect 'aufx' and kAudioUnitType_MusicEffect 'aumf').
// AUv3 / out-of-process hosting via AUAudioUnit + audioComponentRegistrations is OUT OF SCOPE —
// it needs an XPC-backed instantiation path (AUAudioUnit instantiateWithComponentDescription:),
// a different render block signature, and app-group entitlement plumbing for the extension.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Component identity ─────────────────────────────────────────────────────────────────────
//
// What gets stored in obs_data so a chosen effect survives a restart. It is NOT the display
// name: names are localised, change between plugin versions, and collide (two vendors ship an
// "EQ"). The triple (type, subtype, manufacturer) is what AudioComponentFindNext matches on.
//
// Encoded as three fixed-width hex fields rather than the printable FourCC the codes usually
// spell. FourCC bytes are not required to be printable, and a non-printable byte landing in a
// scene-collection JSON string is a corruption we would only discover when the operator's
// filter came back empty. Hex is total. The friendly name is carried separately, for display.
#define QCI_AU_KEY_SIZE 32

// Writes "%08x:%08x:%08x" into `out`. Always NUL-terminates.
void qci_au_key_format(char out[QCI_AU_KEY_SIZE], uint32_t type, uint32_t subtype, uint32_t manufacturer);

// Strict inverse of qci_au_key_format. Returns false and leaves the outputs untouched on any
// malformed input — a partially-parsed key would instantiate the WRONG effect, which is worse
// than instantiating none.
bool qci_au_key_parse(const char *key, uint32_t *type, uint32_t *subtype, uint32_t *manufacturer);

// ── Stream format ──────────────────────────────────────────────────────────────────────────

// Builds the non-interleaved float32 ASBD to set on kAudioUnitProperty_StreamFormat for both
// the input and output scope of bus 0.
//
// THE TRAP: for a non-interleaved (planar) format, mBytesPerFrame and mBytesPerPacket describe
// ONE channel — 4 bytes — not channels * 4. CoreAudio treats a non-interleaved stream as N
// mono buffers, so the "frame" being sized is a frame of a single buffer. Setting these to
// channels * 4 is the classic mistake; it is accepted by some AUs and produces garbage in
// others, which is precisely the kind of bug that survives a casual listen.
void qci_au_make_asbd(AudioStreamBasicDescription *asbd, double sample_rate, uint32_t channels);

// ── Channel negotiation ────────────────────────────────────────────────────────────────────
//
// AUChannelInfo wildcard encoding, per AudioUnitProperties.h (the kAudioUnitProperty_
// SupportedNumChannels documentation block):
//
//   {-1, -1}  any channel count, provided input and output MATCH each other
//   {-1, -2}  any count on input and any count on output, independently
//   {-2, -1}  the same, mirrored
//   {n,  m}   exactly n in, m out
//   a lone -1 on one side means "any" for that side
//   0 on a side means the unit has no elements on that scope
//
// An audio unit that does not implement the property at all (count == 0) is by convention able
// to handle any matched configuration.

// True when the unit can be configured with `in` channels on its input bus and `out` on its
// output bus.
bool qci_au_supports_channels(const AUChannelInfo *infos, size_t count, uint32_t in, uint32_t out);

enum qci_au_channel_mode {
	// The unit cannot be run without changing the channel count, which an OBS audio filter
	// is not permitted to do. The filter must pass audio through untouched.
	QCI_AU_CHAN_UNSUPPORTED = 0,
	// Run the unit at the full OBS plane count; every plane goes through.
	QCI_AU_CHAN_DIRECT,
	// The parent source is mono. Run the unit 1-in/1-out over plane 0 and mirror the result
	// to the remaining planes.
	QCI_AU_CHAN_MONO,
};

struct qci_au_channel_plan {
	enum qci_au_channel_mode mode;
	uint32_t au_channels;  // what to configure on both AU buses
	uint32_t obs_planes;   // planes present in the obs_audio_data buffer
};

// Chooses how to drive the unit.
//
// `obs_planes` is audio_output_get_channels(obs_get_audio()) — by the time filter_audio runs,
// libobs has ALREADY resampled the source into the global mix format (obs-source.c
// copy_audio_data uses audio_output_get_planes), so a 44.1k mono USB mic arrives here as
// obs_planes identical planes at the mix rate.
//
// `source_is_mono` is obs_source_get_speaker_layout(parent) == SPEAKERS_MONO. Those planes are
// duplicates of one signal, so running a stereo-configured effect over them does redundant work
// and, for anything with stereo linking or a stereo-dependent detector, is not what the
// operator means by a mono mic chain. When the unit can do 1-in/1-out we take that path.
//
// An OBS audio filter CANNOT change the channel count — obs_source_output_audio has already
// fixed the layout — so only in == out == (au_channels) configurations are ever selected.
struct qci_au_channel_plan qci_au_plan_channels(const AUChannelInfo *infos, size_t count, uint32_t obs_planes,
					        bool source_is_mono);

// ── Engine validity ────────────────────────────────────────────────────────────────────────
//
// An AudioUnit is configured for a channel count and a sample rate before it is initialised, so
// a change to either means tearing it down and building a new one. Everything else — reopening
// the properties pane, OBS writing an unrelated setting — must NOT rebuild, because rebuilding
// discards every parameter the operator has adjusted in the plug-in's own window since the last
// scene save. Getting that predicate wrong is silent in both directions: too eager and the
// operator's tuning evaporates, too lazy and a stale unit renders at the wrong rate.

struct qci_au_mix {
	uint32_t planes;      // audio_output_get_channels()
	uint32_t sample_rate; // audio_output_get_sample_rate()
	// obs_source_get_speaker_layout(parent) == SPEAKERS_MONO. False both when the parent is
	// genuinely multi-channel AND when it is not yet known (a filter has no parent at create()
	// time, and a parent reports SPEAKERS_UNKNOWN until its first buffer) — which is why a
	// transition from false to true has to be treated as a real change.
	bool source_is_mono;
};

// True when an engine built for `built_key` under `built` can keep running as-is under `now`.
// Empty or NULL keys are never valid: "no effect chosen" is not a running engine.
bool qci_au_engine_still_valid(const char *built_key, struct qci_au_mix built, const char *wanted_key,
			       struct qci_au_mix now);

// ── Latency ────────────────────────────────────────────────────────────────────────────────

// Ceiling applied to whatever kAudioUnitProperty_Latency reports, in nanoseconds.
//
// A misbehaving unit reporting a nonsense latency would otherwise be handed straight to the
// timestamp arithmetic in filter_audio and shift this source a long way out of sync with video.
// One second is far beyond any real effect's lookahead and still finite.
#define QCI_AU_MAX_LATENCY_NS 1000000000ULL

// Converts kAudioUnitProperty_Latency (Float64 seconds, global scope, read-only) into the
// nanoseconds subtracted from the outgoing timestamp. Negative, NaN and infinite inputs all
// yield 0; anything above the ceiling clamps to it.
uint64_t qci_au_latency_ns(double latency_seconds);

// ── Preset state ───────────────────────────────────────────────────────────────────────────
//
// kAudioUnitProperty_ClassInfo is a CFDictionaryRef holding the unit's complete state. We
// serialise it as an XML property list rather than a binary one plus base64: XML plists are
// UTF-8 text with no embedded NULs, so they drop straight into an obs_data string and through
// OBS's JSON scene file without a second encoding layer. CFData values inside the dictionary
// (which is where a unit's opaque blob usually lives) are base64'd by the plist writer itself.

// Serialises to a NUL-terminated UTF-8 XML plist. Caller frees with free(). NULL on failure or
// if `plist` is NULL.
char *qci_au_classinfo_to_string(CFPropertyListRef plist);

// Parses a string produced by qci_au_classinfo_to_string. Returns a +1 reference the caller
// releases with CFRelease, or NULL.
//
// Rejects anything that is not a CFDictionary at the top level: AudioUnitSetProperty for
// ClassInfo expects a dictionary, and handing it an array or a bare string is undefined
// behaviour in third-party units rather than a clean error.
CFPropertyListRef qci_au_classinfo_from_string(const char *str);

#ifdef __cplusplus
}
#endif
