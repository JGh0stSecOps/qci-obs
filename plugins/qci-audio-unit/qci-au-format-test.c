// Standalone unit test for the pure half of the AudioUnit host filter.
//
// Runs with no OBS instance, no audio device, and no third-party AudioUnit installed — which
// matters, because none of the four effects this feature was requested for (TDR Nova, TDR VOS
// SlickEQ, TDR Kotelnikov, LoudMax) are installed on this machine. A test that needed one of
// them present would be a test that never ran.
//
// Structure follows plugins/obs-x264/obs-x264-test.c: a CHECK macro, exit(1) on the first
// failure, no external test framework. The tree's cmocka harness (test/cmocka/) is not usable
// here — it is never added by the root CMakeLists, and CMocka is not installed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qci-au-format.h"

static int checks_run = 0;

// A FourCharCode spelled from its bytes rather than as a multi-character character constant.
//
// 'aufx' is the natural spelling and is what CoreAudio's own headers use, but it warns under this
// tree's build settings: -Wfour-char-constants is in _obs_clang_common_options
// (cmake/common/compiler_common.cmake) and applies to every target, while it is in neither -Wall
// nor -Wextra — so this file built clean standalone and then produced warnings the moment it was
// compiled by the real project.
//
// Whether that warning is fatal depends on which preset is building, and this was checked rather
// than assumed. The tree hard-codes only -Werror=return-type and
// -Werror=block-capture-autoreleasing, and the one blanket add_compile_options(-Werror) lives in
// cmake/linux/compilerconfig.cmake, which never runs for a MACOS-only plugin. But the `macos-ci`
// preset sets CMAKE_COMPILE_WARNING_AS_ERROR (CMakePresets.json), so under CI every warning is an
// error — a local `macos` build would merely warn, and CI would then fail on it. The macro is not
// optional cosmetics.
//
// The plugin's other two sources were never affected: they use the kAudioUnitType_* symbolic
// constants and mention 'aufx' only in comments.
//
// The byte order here is the one clang gives a multi-char constant: leftmost character in the
// most significant byte. That is not taken on faith — the very first assertion below compares
// the formatted key against the literal hex string "61756678:…", which is 'a','u','f','x' in
// that order, so a wrong shift would fail the test rather than pass a changed value.
#define FOURCC(a, b, c, d)                                                                           \
	((uint32_t)(((uint32_t)(unsigned char)(a) << 24) | ((uint32_t)(unsigned char)(b) << 16) |     \
		    ((uint32_t)(unsigned char)(c) << 8) | (uint32_t)(unsigned char)(d)))

#define CHECK(condition)                                                                             \
	do {                                                                                         \
		++checks_run;                                                                        \
		if (!(condition)) {                                                                  \
			fprintf(stderr, "%s:%d: error: check failed: %s\n", __FILE__, __LINE__,       \
				#condition);                                                         \
			exit(1);                                                                     \
		}                                                                                    \
	} while (0)

// ── component identity round trip ──────────────────────────────────────────────────────────

static void test_key_round_trip(void)
{
	char key[QCI_AU_KEY_SIZE];
	uint32_t t = 0, s = 0, m = 0;

	// 'aufx' / 'nova' / 'Tdrl' — the shape of a real third-party effect's identity.
	qci_au_key_format(key, FOURCC('a', 'u', 'f', 'x'), FOURCC('n', 'o', 'v', 'a'),
			  FOURCC('T', 'd', 'r', 'l'));
	CHECK(strcmp(key, "61756678:6e6f7661:5464726c") == 0);

	CHECK(qci_au_key_parse(key, &t, &s, &m));
	CHECK(t == FOURCC('a', 'u', 'f', 'x'));
	CHECK(s == FOURCC('n', 'o', 'v', 'a'));
	CHECK(m == FOURCC('T', 'd', 'r', 'l'));

	// Every byte value must survive, including non-printable ones — the reason this is hex.
	char key2[QCI_AU_KEY_SIZE];
	qci_au_key_format(key2, 0x00010203u, 0xfffefdfcu, 0x80000001u);
	CHECK(qci_au_key_parse(key2, &t, &s, &m));
	CHECK(t == 0x00010203u);
	CHECK(s == 0xfffefdfcu);
	CHECK(m == 0x80000001u);

	// Apple's own units use a lowercase manufacturer 'appl'.
	qci_au_key_format(key, FOURCC('a', 'u', 'f', 'x'), FOURCC('n', 'b', 'e', 'q'),
			  FOURCC('a', 'p', 'p', 'l'));
	CHECK(qci_au_key_parse(key, &t, &s, &m));
	CHECK(t == FOURCC('a', 'u', 'f', 'x') && s == FOURCC('n', 'b', 'e', 'q') &&
	      m == FOURCC('a', 'p', 'p', 'l'));
}

static void test_key_parse_rejects_garbage(void)
{
	uint32_t t = 0xdeadbeefu, s = 0xdeadbeefu, m = 0xdeadbeefu;

	CHECK(!qci_au_key_parse(NULL, &t, &s, &m));
	CHECK(!qci_au_key_parse("", &t, &s, &m));
	CHECK(!qci_au_key_parse("not a key", &t, &s, &m));
	// too few fields
	CHECK(!qci_au_key_parse("61756678:6e6f7661", &t, &s, &m));
	// wrong separator
	CHECK(!qci_au_key_parse("61756678-6e6f7661-5464726c", &t, &s, &m));
	// short field
	CHECK(!qci_au_key_parse("6175667:6e6f7661:5464726c", &t, &s, &m));
	// non-hex digit
	CHECK(!qci_au_key_parse("6175667g:6e6f7661:5464726c", &t, &s, &m));
	// trailing junk after a valid key must not be accepted silently
	CHECK(!qci_au_key_parse("61756678:6e6f7661:5464726c:extra", &t, &s, &m));

	// A rejected parse must not have written anything.
	CHECK(t == 0xdeadbeefu && s == 0xdeadbeefu && m == 0xdeadbeefu);
}

// ── stream format ──────────────────────────────────────────────────────────────────────────

static void test_asbd_is_non_interleaved_float(void)
{
	AudioStreamBasicDescription a;
	memset(&a, 0xff, sizeof(a));

	qci_au_make_asbd(&a, 48000.0, 2);

	CHECK(a.mSampleRate == 48000.0);
	CHECK(a.mFormatID == kAudioFormatLinearPCM);
	CHECK((a.mFormatFlags & kAudioFormatFlagIsFloat) != 0);
	CHECK((a.mFormatFlags & kAudioFormatFlagIsPacked) != 0);
	CHECK((a.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0);
	// OBS hands us native-endian float; the flag must NOT be set on little-endian arm64.
	CHECK((a.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0);
	CHECK(a.mBitsPerChannel == 32);
	CHECK(a.mChannelsPerFrame == 2);
	CHECK(a.mFramesPerPacket == 1);

	// The whole point: per-channel sizes, NOT channels * 4.
	CHECK(a.mBytesPerFrame == 4);
	CHECK(a.mBytesPerPacket == 4);
	CHECK(a.mReserved == 0);
}

static void test_asbd_mono_and_rates(void)
{
	AudioStreamBasicDescription a;

	qci_au_make_asbd(&a, 48000.0, 1);
	CHECK(a.mChannelsPerFrame == 1);
	CHECK(a.mBytesPerFrame == 4);   // unchanged by channel count
	CHECK(a.mBytesPerPacket == 4);

	// Not every operator runs at 48k; the rate comes from audio_output_get_sample_rate().
	qci_au_make_asbd(&a, 44100.0, 6);
	CHECK(a.mSampleRate == 44100.0);
	CHECK(a.mChannelsPerFrame == 6);
	CHECK(a.mBytesPerFrame == 4);
}

// ── channel negotiation ────────────────────────────────────────────────────────────────────

static void test_supports_no_property(void)
{
	// A unit that does not implement kAudioUnitProperty_SupportedNumChannels handles any
	// matched configuration.
	CHECK(qci_au_supports_channels(NULL, 0, 1, 1));
	CHECK(qci_au_supports_channels(NULL, 0, 2, 2));
	CHECK(qci_au_supports_channels(NULL, 0, 8, 8));
	CHECK(!qci_au_supports_channels(NULL, 0, 1, 2));
	CHECK(!qci_au_supports_channels(NULL, 0, 2, 1));
}

static void test_supports_wildcards(void)
{
	// {-1,-1}: any count, but input and output must match.
	const AUChannelInfo matched[] = {{-1, -1}};
	CHECK(qci_au_supports_channels(matched, 1, 1, 1));
	CHECK(qci_au_supports_channels(matched, 1, 2, 2));
	CHECK(!qci_au_supports_channels(matched, 1, 1, 2));

	// {-1,-2}: any count on input, any on output, independently.
	const AUChannelInfo independent[] = {{-1, -2}};
	CHECK(qci_au_supports_channels(independent, 1, 1, 2));
	CHECK(qci_au_supports_channels(independent, 1, 2, 2));
	CHECK(qci_au_supports_channels(independent, 1, 4, 1));

	// mirrored form
	const AUChannelInfo mirrored[] = {{-2, -1}};
	CHECK(qci_au_supports_channels(mirrored, 1, 1, 2));
	CHECK(qci_au_supports_channels(mirrored, 1, 2, 2));

	// A lone -1 on one side means "any" for that side only.
	const AUChannelInfo any_in_stereo_out[] = {{-1, 2}};
	CHECK(qci_au_supports_channels(any_in_stereo_out, 1, 1, 2));
	CHECK(qci_au_supports_channels(any_in_stereo_out, 1, 2, 2));
	CHECK(!qci_au_supports_channels(any_in_stereo_out, 1, 2, 1));
}

static void test_supports_explicit(void)
{
	// The overwhelmingly common third-party effect: mono and stereo, matched.
	const AUChannelInfo mono_stereo[] = {{1, 1}, {2, 2}};
	CHECK(qci_au_supports_channels(mono_stereo, 2, 1, 1));
	CHECK(qci_au_supports_channels(mono_stereo, 2, 2, 2));
	CHECK(!qci_au_supports_channels(mono_stereo, 2, 1, 2));
	CHECK(!qci_au_supports_channels(mono_stereo, 2, 6, 6));

	// Stereo-only.
	const AUChannelInfo stereo_only[] = {{2, 2}};
	CHECK(qci_au_supports_channels(stereo_only, 1, 2, 2));
	CHECK(!qci_au_supports_channels(stereo_only, 1, 1, 1));
}

static void test_plan_direct(void)
{
	const AUChannelInfo stereo_only[] = {{2, 2}};
	struct qci_au_channel_plan p = qci_au_plan_channels(stereo_only, 1, 2, false);
	CHECK(p.mode == QCI_AU_CHAN_DIRECT);
	CHECK(p.au_channels == 2);
	CHECK(p.obs_planes == 2);

	// No property at all, stereo mix.
	p = qci_au_plan_channels(NULL, 0, 2, false);
	CHECK(p.mode == QCI_AU_CHAN_DIRECT);
	CHECK(p.au_channels == 2);
}

static void test_plan_mono_source(void)
{
	// Mono mic into a stereo mix, unit does mono: run it once over plane 0.
	const AUChannelInfo mono_stereo[] = {{1, 1}, {2, 2}};
	struct qci_au_channel_plan p = qci_au_plan_channels(mono_stereo, 2, 2, true);
	CHECK(p.mode == QCI_AU_CHAN_MONO);
	CHECK(p.au_channels == 1);
	CHECK(p.obs_planes == 2);

	// Mono mic, but the unit is stereo-only: fall back to running all planes.
	const AUChannelInfo stereo_only[] = {{2, 2}};
	p = qci_au_plan_channels(stereo_only, 1, 2, true);
	CHECK(p.mode == QCI_AU_CHAN_DIRECT);
	CHECK(p.au_channels == 2);

	// A genuinely mono OBS mix with a mono source is just DIRECT — there is nothing to mirror.
	p = qci_au_plan_channels(mono_stereo, 2, 1, true);
	CHECK(p.mode == QCI_AU_CHAN_DIRECT);
	CHECK(p.au_channels == 1);
}

static void test_plan_unsupported(void)
{
	// Unit is mono-only, mix is stereo, source is not mono. An OBS filter may not change the
	// channel count, so there is no legal configuration: the filter must pass audio through.
	const AUChannelInfo mono_only[] = {{1, 1}};
	struct qci_au_channel_plan p = qci_au_plan_channels(mono_only, 1, 2, false);
	CHECK(p.mode == QCI_AU_CHAN_UNSUPPORTED);

	// Zero planes is nonsense input and must not be reported as workable.
	p = qci_au_plan_channels(NULL, 0, 0, false);
	CHECK(p.mode == QCI_AU_CHAN_UNSUPPORTED);
}

// ── engine validity ────────────────────────────────────────────────────────────────────────

static void test_engine_still_valid(void)
{
	const char *nova = "61756678:6e6f7661:5464726c";
	const char *slick = "61756678:736c6371:5464726c";

	const struct qci_au_mix stereo_48k = {2, 48000, false};

	// The case this predicate exists for: OBS writes a setting, update() runs, nothing about the
	// mix or the chosen effect has changed. Rebuilding here would discard the operator's tuning.
	CHECK(qci_au_engine_still_valid(nova, stereo_48k, nova, stereo_48k));

	// A different effect is obviously a rebuild.
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, slick, stereo_48k));

	// Channel count and sample rate are baked in at AudioUnitInitialize.
	const struct qci_au_mix mono_48k = {1, 48000, false};
	const struct qci_au_mix stereo_44k = {2, 44100, false};
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, nova, mono_48k));
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, nova, stereo_44k));

	// THE REGRESSION THIS GUARDS. A filter has no parent at create() time, so source_is_mono is
	// false then and only becomes true once filter_add binds it to a mono mic. If that
	// transition is not treated as a change, the mono plan can never be selected and a mono mic
	// is processed as if it were stereo for the lifetime of the filter.
	const struct qci_au_mix stereo_48k_mono_src = {2, 48000, true};
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, nova, stereo_48k_mono_src));
	// ...and back again, when the operator swaps to a stereo device.
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k_mono_src, nova, stereo_48k));

	// "No effect chosen" is never a live engine, whichever side it appears on.
	CHECK(!qci_au_engine_still_valid("", stereo_48k, nova, stereo_48k));
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, "", stereo_48k));
	CHECK(!qci_au_engine_still_valid(NULL, stereo_48k, nova, stereo_48k));
	CHECK(!qci_au_engine_still_valid(nova, stereo_48k, NULL, stereo_48k));
	CHECK(!qci_au_engine_still_valid("", stereo_48k, "", stereo_48k));
}

// ── latency ────────────────────────────────────────────────────────────────────────────────

static void test_latency(void)
{
	CHECK(qci_au_latency_ns(0.0) == 0);
	CHECK(qci_au_latency_ns(0.005) == 5000000ULL);
	CHECK(qci_au_latency_ns(0.001) == 1000000ULL);

	// A negative report is meaningless; treat as none rather than shifting the wrong way.
	CHECK(qci_au_latency_ns(-0.01) == 0);

	// Absurd values clamp instead of throwing this source out of sync with video.
	CHECK(qci_au_latency_ns(100.0) == QCI_AU_MAX_LATENCY_NS);
	CHECK(qci_au_latency_ns(1.0) == QCI_AU_MAX_LATENCY_NS);

	// NaN and infinity must not propagate into timestamp arithmetic.
	CHECK(qci_au_latency_ns(0.0 / 0.0) == 0);
	CHECK(qci_au_latency_ns(1.0 / 0.0) == QCI_AU_MAX_LATENCY_NS);
}

// ── ClassInfo persistence ──────────────────────────────────────────────────────────────────

static CFDictionaryRef make_fake_classinfo(void)
{
	// Shaped like a real ClassInfo dictionary: the documented preset keys plus an opaque
	// CFData blob, which is where third-party units keep their actual state.
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							    &kCFTypeDictionaryValueCallBacks);
	// int, not uint32_t: kCFNumberIntType below describes the storage this pointer points at.
	int type = (int)FOURCC('a', 'u', 'f', 'x');
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &type);
	CFDictionarySetValue(d, CFSTR(kAUPresetTypeKey), n);
	CFRelease(n);

	CFDictionarySetValue(d, CFSTR(kAUPresetNameKey), CFSTR("QCi mic chain"));

	const uint8_t blob[] = {0x00, 0x01, 0xfe, 0xff, 0x7f, 0x80};
	CFDataRef data = CFDataCreate(NULL, blob, sizeof(blob));
	CFDictionarySetValue(d, CFSTR(kAUPresetDataKey), data);
	CFRelease(data);

	return d;
}

static void test_classinfo_round_trip(void)
{
	CFDictionaryRef original = make_fake_classinfo();

	char *str = qci_au_classinfo_to_string(original);
	CHECK(str != NULL);
	CHECK(strlen(str) > 0);
	// XML plist, so it is plain UTF-8 text that survives OBS's JSON scene file unescaped.
	CHECK(strstr(str, "<?xml") == str);
	CHECK(strstr(str, "plist") != NULL);
	// No embedded NUL: strlen must account for the whole document.
	CHECK(strstr(str, "</plist>") != NULL);

	CFPropertyListRef parsed = qci_au_classinfo_from_string(str);
	CHECK(parsed != NULL);
	CHECK(CFGetTypeID(parsed) == CFDictionaryGetTypeID());
	// The binary blob must come back byte-identical, not merely present.
	CHECK(CFEqual(parsed, original));

	CFRelease(parsed);
	free(str);
	CFRelease(original);
}

static void test_classinfo_rejects_bad_input(void)
{
	CHECK(qci_au_classinfo_to_string(NULL) == NULL);

	CHECK(qci_au_classinfo_from_string(NULL) == NULL);
	CHECK(qci_au_classinfo_from_string("") == NULL);
	CHECK(qci_au_classinfo_from_string("this is not a property list") == NULL);
	// Well-formed XML that is not a plist.
	CHECK(qci_au_classinfo_from_string("<html><body>hi</body></html>") == NULL);

	// A VALID plist whose root is an array, not a dictionary. AudioUnitSetProperty for
	// ClassInfo expects a dictionary; handing a third-party unit an array is undefined
	// behaviour rather than a clean failure, so it must be rejected here.
	CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(arr, CFSTR("nope"));
	char *arr_str = qci_au_classinfo_to_string(arr);
	// to_string itself only serialises; it is from_string that enforces the dictionary rule.
	CHECK(arr_str != NULL);
	CHECK(qci_au_classinfo_from_string(arr_str) == NULL);
	free(arr_str);
	CFRelease(arr);
}

int main(void)
{
	test_key_round_trip();
	test_key_parse_rejects_garbage();
	test_asbd_is_non_interleaved_float();
	test_asbd_mono_and_rates();
	test_supports_no_property();
	test_supports_wildcards();
	test_supports_explicit();
	test_plan_direct();
	test_plan_mono_source();
	test_plan_unsupported();
	test_engine_still_valid();
	test_latency();
	test_classinfo_round_trip();
	test_classinfo_rejects_bad_input();

	printf("qci-au-format: all %d checks passed\n", checks_run);
	return 0;
}
