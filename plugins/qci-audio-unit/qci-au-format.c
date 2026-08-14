// qci-au-format — pure implementation. See qci-au-format.h for why this is separate.
//
// No libobs, no Objective-C, no live AudioUnit: everything here is exercised by
// qci-au-format-test.c on a machine with no audio device.

#include "qci-au-format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── component identity ─────────────────────────────────────────────────────────────────────

void qci_au_key_format(char out[QCI_AU_KEY_SIZE], uint32_t type, uint32_t subtype, uint32_t manufacturer)
{
	snprintf(out, QCI_AU_KEY_SIZE, "%08x:%08x:%08x", type, subtype, manufacturer);
}

// Exactly eight hex digits, no sign, no prefix, no whitespace. strtoul is not used on purpose:
// it accepts leading spaces, a "+"/"-" sign and short fields, any of which would let a mangled
// key resolve to a plausible-looking component that is not the one the operator chose.
static bool parse_hex8(const char *p, uint32_t *out)
{
	uint32_t v = 0;

	for (int i = 0; i < 8; i++) {
		char c = p[i];
		uint32_t digit;

		if (c >= '0' && c <= '9')
			digit = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			digit = (uint32_t)(c - 'a') + 10u;
		else if (c >= 'A' && c <= 'F')
			digit = (uint32_t)(c - 'A') + 10u;
		else
			return false;

		v = (v << 4) | digit;
	}

	*out = v;
	return true;
}

bool qci_au_key_parse(const char *key, uint32_t *type, uint32_t *subtype, uint32_t *manufacturer)
{
	// 8 + ':' + 8 + ':' + 8. Length is checked before indexing, and checked EXACTLY so that
	// trailing junk is a rejection rather than something we quietly ignore.
	static const size_t KEY_LEN = 26;

	if (!key || !type || !subtype || !manufacturer)
		return false;
	if (strlen(key) != KEY_LEN)
		return false;
	if (key[8] != ':' || key[17] != ':')
		return false;

	uint32_t t, s, m;
	if (!parse_hex8(key + 0, &t) || !parse_hex8(key + 9, &s) || !parse_hex8(key + 18, &m))
		return false;

	// Written only once every field has parsed: a half-applied identity is worse than none.
	*type = t;
	*subtype = s;
	*manufacturer = m;
	return true;
}

// ── stream format ──────────────────────────────────────────────────────────────────────────

void qci_au_make_asbd(AudioStreamBasicDescription *asbd, double sample_rate, uint32_t channels)
{
	if (!asbd)
		return;

	memset(asbd, 0, sizeof(*asbd));

	asbd->mSampleRate = sample_rate;
	asbd->mFormatID = kAudioFormatLinearPCM;
	// kAudioFormatFlagsNativeEndian is 0 on little-endian, so the big-endian flag stays clear
	// on arm64. Spelled out rather than using kAudioFormatFlagsNativeFloatPacked so the
	// non-interleaved bit is visible at the point it matters.
	asbd->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
			     kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
	asbd->mBitsPerChannel = 32;
	asbd->mChannelsPerFrame = channels;
	asbd->mFramesPerPacket = 1;
	// Per-channel, NOT channels * 4 — a non-interleaved stream is N separate mono buffers, so
	// the frame being described belongs to one of them. See the header comment.
	asbd->mBytesPerFrame = sizeof(float);
	asbd->mBytesPerPacket = sizeof(float);
}

// ── channel negotiation ────────────────────────────────────────────────────────────────────

bool qci_au_supports_channels(const AUChannelInfo *infos, size_t count, uint32_t in, uint32_t out)
{
	if (in == 0 || out == 0)
		return false;

	// No kAudioUnitProperty_SupportedNumChannels: by convention the unit handles any
	// configuration where input and output match.
	if (!infos || count == 0)
		return in == out;

	for (size_t i = 0; i < count; i++) {
		const SInt16 ci = infos[i].inChannels;
		const SInt16 co = infos[i].outChannels;

		// {-1,-1}: any count, but the two sides must agree. Keep scanning on a mismatch —
		// a later entry may still permit this pair.
		if (ci == -1 && co == -1) {
			if (in == out)
				return true;
			continue;
		}

		// {-1,-2} / {-2,-1}: any count on either side, independently.
		if ((ci == -1 && co == -2) || (ci == -2 && co == -1))
			return true;

		// A lone -1 is "any" for that side only. Values below -2 encode a total across
		// every bus on that scope; this host only ever configures bus 0, so rather than
		// guess at a multi-bus topology we decline. Declining costs a bypass, guessing
		// wrong costs mangled audio on the operator's live mic.
		const bool in_ok = (ci == -1) || (ci >= 0 && (uint32_t)ci == in);
		const bool out_ok = (co == -1) || (co >= 0 && (uint32_t)co == out);

		if (in_ok && out_ok)
			return true;
	}

	return false;
}

struct qci_au_channel_plan qci_au_plan_channels(const AUChannelInfo *infos, size_t count, uint32_t obs_planes,
					        bool source_is_mono)
{
	struct qci_au_channel_plan plan = {QCI_AU_CHAN_UNSUPPORTED, 0, obs_planes};

	if (obs_planes == 0)
		return plan;

	// A mono parent whose signal libobs has already duplicated across the mix's planes. If
	// the unit does 1-in/1-out we run it once and mirror, which is both cheaper and what a
	// mono chain actually means for anything with stereo linking or a stereo detector.
	if (source_is_mono && obs_planes > 1 && qci_au_supports_channels(infos, count, 1, 1)) {
		plan.mode = QCI_AU_CHAN_MONO;
		plan.au_channels = 1;
		return plan;
	}

	if (qci_au_supports_channels(infos, count, obs_planes, obs_planes)) {
		plan.mode = QCI_AU_CHAN_DIRECT;
		plan.au_channels = obs_planes;
		return plan;
	}

	// Nothing legal. An OBS audio filter cannot change the channel count, so the caller must
	// pass the buffer through untouched rather than render something of the wrong shape.
	return plan;
}

// ── engine validity ────────────────────────────────────────────────────────────────────────

bool qci_au_engine_still_valid(const char *built_key, struct qci_au_mix built, const char *wanted_key,
			       struct qci_au_mix now)
{
	if (!built_key || !wanted_key || !*built_key || !*wanted_key)
		return false;

	if (strcmp(built_key, wanted_key) != 0)
		return false;

	// Channel count and sample rate are baked into the unit at AudioUnitInitialize. source_is_mono
	// is not, but it selects the channel PLAN, so a change there is also a rebuild.
	return built.planes == now.planes && built.sample_rate == now.sample_rate &&
	       built.source_is_mono == now.source_is_mono;
}

// ── latency ────────────────────────────────────────────────────────────────────────────────

uint64_t qci_au_latency_ns(double latency_seconds)
{
	// NaN fails every comparison, so test it first rather than relying on the <= below.
	if (isnan(latency_seconds) || latency_seconds <= 0.0)
		return 0;

	if (isinf(latency_seconds))
		return QCI_AU_MAX_LATENCY_NS;

	const double ns = latency_seconds * 1e9;

	if (ns >= (double)QCI_AU_MAX_LATENCY_NS)
		return QCI_AU_MAX_LATENCY_NS;

	// Round rather than truncate: 0.005 is not exactly representable, and truncating leaves
	// the reported latency one nanosecond short of the value the unit actually claims.
	return (uint64_t)(ns + 0.5);
}

// ── ClassInfo persistence ──────────────────────────────────────────────────────────────────

char *qci_au_classinfo_to_string(CFPropertyListRef plist)
{
	if (!plist)
		return NULL;

	CFErrorRef error = NULL;
	CFDataRef data = CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListXMLFormat_v1_0, 0,
						  &error);
	if (!data) {
		if (error)
			CFRelease(error);
		return NULL;
	}

	const CFIndex length = CFDataGetLength(data);
	char *out = malloc((size_t)length + 1);

	if (out) {
		memcpy(out, CFDataGetBytePtr(data), (size_t)length);
		out[length] = '\0';
	}

	CFRelease(data);
	return out;
}

CFPropertyListRef qci_au_classinfo_from_string(const char *str)
{
	if (!str || !*str)
		return NULL;

	CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)str, (CFIndex)strlen(str));
	if (!data)
		return NULL;

	CFErrorRef error = NULL;
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable,
							       NULL, &error);
	CFRelease(data);

	if (error)
		CFRelease(error);
	if (!plist)
		return NULL;

	// kAudioUnitProperty_ClassInfo is documented as CFDictionaryRef. CFPropertyList will
	// happily hand back a string or an array for input that merely parses; passing one of
	// those to AudioUnitSetProperty is undefined behaviour inside a third-party unit rather
	// than a clean error, so anything that is not a dictionary is rejected here.
	if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		CFRelease(plist);
		return NULL;
	}

	return plist;
}
