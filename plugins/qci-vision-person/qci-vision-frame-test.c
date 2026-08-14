// Standalone unit test for the pure half of the person-mask filter.
//
// Runs with no OBS instance, no camera and no Vision framework. That is the point: the defect
// this file was written against is a camera format THIS MAC CANNOT BE MADE TO PRODUCE ON DEMAND
// (P010, 10-bit/HDR), so a test that needed the hardware to emit it would be a test that never
// ran — and the bug it guards puts an unmasked face on a live stream.
//
// Structure follows plugins/qci-audio-unit/qci-au-format-test.c: a CHECK macro, exit(1) on the
// first failure, no external test framework.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "qci-vision-frame.h"

static int checks_run = 0;

#define CHECK(condition)                                                                             \
	do {                                                                                         \
		++checks_run;                                                                        \
		if (!(condition)) {                                                                  \
			fprintf(stderr, "FAILED at %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
			exit(1);                                                                     \
		}                                                                                    \
	} while (0)

// Every value of enum video_format, in declaration order. Spelled out rather than iterated as
// an integer range so that a format added to libobs has to be acknowledged HERE instead of
// quietly extending the sweep with a value this file has never considered.
//
// The static_assert below is what makes that sentence true, and it is anchored to libobs rather
// than to a literal — `CHECK(ALL_N == 26)`, which is what used to stand at the sweep below,
// compared this array against itself and folded to `26 == 26`. VIDEO_FORMAT_R10L is the last
// enumerator in libobs/media-io/video-io.h and that enum assigns no explicit values, so
// `VIDEO_FORMAT_R10L + 1` IS the format count as libobs declares it today. A format inserted
// anywhere at or before R10L shifts that value and fails the BUILD here.
//
// WHAT IT CANNOT CATCH, said plainly because the comment it replaces claimed a guarantee the code
// did not provide: a format APPENDED after VIDEO_FORMAT_R10L leaves R10L at 25 and this assertion
// still holds. libobs's enum has no COUNT/LAST sentinel, and C has no constant expression that
// yields an enum's cardinality, so nothing inside this file can see that case. The guard that
// does see it is the deliberately default-less switch in qci-vision-frame.c (see its comment at
// the end of qci_vision_plane_rows): -Wswitch names the newcomer there.
//
// That switch is a HARD ERROR in every build of this tree, local ones included — not a warning
// that scrolls past, and not something only CI catches. The chain, verified rather than assumed:
// cmake/common/compiler_common.cmake:85-86 sets CMAKE_COMPILE_WARNING_AS_ERROR ON whenever it is
// not already defined (so a plain `cmake build_macos` gets it, no preset required), and
// cmake/macos/xcode.cmake:155-157 turns that into GCC_TREAT_WARNINGS_AS_ERRORS=YES. Alongside it,
// xcode.cmake sets GCC_WARN_CHECK_SWITCH_STATEMENTS=YES, which is -Wswitch. Both land in the
// generated project — confirmed by reading GCC_TREAT_WARNINGS_AS_ERRORS and
// GCC_WARN_CHECK_SWITCH_STATEMENTS back out of build_macos/obs-studio.xcodeproj/project.pbxproj,
// and by compiling a two-case switch over a three-value enum, which clang rejects with
// "error: enumeration value 'C' not handled in switch [-Werror,-Wswitch]".
//
// Worth stating because the opposite is easy to conclude and wrong: cmake/macos/compilerconfig.cmake
// includes compiler_common but never passes _obs_clang_common_options to add_compile_options, so
// the -W flags in THAT list really are inert on macOS. The warnings that matter here come from
// Xcode's own settings instead, and they are errors. So: this assertion is the second gate, the
// switch is the first, and both fail the build rather than deferring to CI.
static const enum video_format ALL[] = {
	VIDEO_FORMAT_NONE,  VIDEO_FORMAT_I420, VIDEO_FORMAT_NV12, VIDEO_FORMAT_YVYU, VIDEO_FORMAT_YUY2,
	VIDEO_FORMAT_UYVY,  VIDEO_FORMAT_RGBA, VIDEO_FORMAT_BGRA, VIDEO_FORMAT_BGRX, VIDEO_FORMAT_Y800,
	VIDEO_FORMAT_I444,  VIDEO_FORMAT_BGR3, VIDEO_FORMAT_I422, VIDEO_FORMAT_I40A, VIDEO_FORMAT_I42A,
	VIDEO_FORMAT_YUVA,  VIDEO_FORMAT_AYUV, VIDEO_FORMAT_I010, VIDEO_FORMAT_P010, VIDEO_FORMAT_I210,
	VIDEO_FORMAT_I412,  VIDEO_FORMAT_YA2L, VIDEO_FORMAT_P216, VIDEO_FORMAT_P416, VIDEO_FORMAT_V210,
	VIDEO_FORMAT_R10L,
};
#define ALL_N ((int)(sizeof(ALL) / sizeof(ALL[0])))

static_assert(ALL_N == VIDEO_FORMAT_R10L + 1,
	      "enum video_format changed size in libobs: add the new format(s) to ALL[] above and to "
	      "BOTH switches in qci-vision-frame.c before touching this line");

int main(void)
{
	// ── 1 · EVERY FORMAT THE MAC CAMERA STACK CAN HAND US IS ONE VISION CAN READ ────────
	//
	// plugins/mac-avcapture/OBSAVCapture.m formatFromSubtype: is the whole list of what a
	// macOS capture device turns into: UYVY, YUY2, BGRA, NV12, P010. Anything on that list
	// that this filter cannot wrap is a format on which the mask passes raw video through.
	// P010 is the one that was missing, and it is the one a 10-bit/HDR camera negotiates.
	const enum video_format from_avcapture[] = {VIDEO_FORMAT_UYVY, VIDEO_FORMAT_YUY2, VIDEO_FORMAT_BGRA,
						    VIDEO_FORMAT_NV12, VIDEO_FORMAT_P010};
	for (int i = 0; i < (int)(sizeof(from_avcapture) / sizeof(from_avcapture[0])); i++)
		CHECK(qci_vision_fourcc(from_avcapture[i]) != 0);

	// The mappings themselves, so a typo in a FourCC is a failure and not a wrong picture.
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_BGRA) == kCVPixelFormatType_32BGRA);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_BGRX) == kCVPixelFormatType_32BGRA);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_RGBA) == kCVPixelFormatType_32RGBA);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_UYVY) == kCVPixelFormatType_422YpCbCr8);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_YUY2) == kCVPixelFormatType_422YpCbCr8_yuvs);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_NV12) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_P010) == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

	// …and NOT everything: a format with no answer must say so rather than guess one. A wrong
	// guess here is a CVPixelBuffer describing memory with the wrong stride, which is a crash
	// or a garbage picture, not a mask.
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_NONE) == 0);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_I420) == 0);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_BGR3) == 0);
	CHECK(qci_vision_fourcc(VIDEO_FORMAT_V210) == 0);

	// ── 2 · THE TWO-PLANE FORMATS ARE THE ONLY PLANAR ONES WE ACCEPT ────────────────────
	// CVPixelBufferCreateWithBytes and …WithPlanarBytes are different calls; handing a
	// two-plane frame to the packed one reads chroma as if it were more luma.
	CHECK(qci_vision_is_planar(VIDEO_FORMAT_NV12));
	CHECK(qci_vision_is_planar(VIDEO_FORMAT_P010));
	CHECK(!qci_vision_is_planar(VIDEO_FORMAT_BGRA));
	CHECK(!qci_vision_is_planar(VIDEO_FORMAT_UYVY));
	// A format we cannot wrap at all is not "planar"; the caller must never reach the question.
	CHECK(!qci_vision_is_planar(VIDEO_FORMAT_I420));
	// Every format is answered by exactly ONE of the two creation calls, and WHICH one is not a
	// free choice. This loop used to read `CHECK(is_planar(ALL[i]) || !is_planar(ALL[i]))`, which
	// is true of every bool: it asserted nothing while adding one to the pass count per format,
	// and BGRX, RGBA and YUY2 ended up with no planarity coverage anywhere in the tree.
	//
	// The expected answer is pinned against the two things the CALLER does with it, so merely
	// agreeing with a wrong qci_vision_is_planar() is not enough to pass:
	//
	//   · the FourCC — the biplanar CoreVideo types are exactly the ones …WithPlanarBytes exists
	//     for, so a format added to qci_vision_fourcc() as biplanar and forgotten in
	//     qci_vision_is_planar() would reach CVPixelBufferCreateWithBytes with only data[0],
	//     leaving the chroma plane carrying the original picture;
	//   · the geometry — vision-person-mask.mm's planar branch refuses anything that is not
	//     exactly 2 planes, so "planar" and "plane_rows() == 2" must be the same set or the
	//     filter drops frames it can read, or wraps frames it cannot.
	//
	// Both hold for every value of the enum, not just the wrappable ones: a format this filter
	// cannot see is not planar, it is nothing.
	uint32_t rows[QCI_VISION_MAX_PLANES]; // scratch, shared with the geometry sections below
	for (int i = 0; i < ALL_N; i++) {
		const OSType cv = qci_vision_fourcc(ALL[i]);
		const bool biplanar = cv == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
				      cv == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
		CHECK(qci_vision_is_planar(ALL[i]) == biplanar);
		CHECK(qci_vision_is_planar(ALL[i]) == (qci_vision_plane_rows(ALL[i], 480, rows) == 2));
	}

	// ── 3 · PLANE GEOMETRY, MIRRORED FROM libobs ────────────────────────────────────────
	// These numbers are libobs/obs-source.c copy_frame_data()'s, and they exist here so the
	// filter can DESTROY a frame it cannot read. Writing one row too many is a heap overflow
	// in the middle of a live stream; writing one too few leaves a strip of the face visible.
	// (`rows` is declared up in section 2, which asks the same question of every format.)

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_BGRA, 720, rows) == 1);
	CHECK(rows[0] == 720);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_NV12, 720, rows) == 2);
	CHECK(rows[0] == 720 && rows[1] == 360);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_P010, 720, rows) == 2);
	CHECK(rows[0] == 720 && rows[1] == 360);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_I420, 720, rows) == 3);
	CHECK(rows[0] == 720 && rows[1] == 360 && rows[2] == 360);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_I010, 720, rows) == 3);
	CHECK(rows[0] == 720 && rows[1] == 360 && rows[2] == 360);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_I444, 720, rows) == 3);
	CHECK(rows[0] == 720 && rows[1] == 720 && rows[2] == 720);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_I40A, 720, rows) == 4);
	CHECK(rows[0] == 720 && rows[1] == 360 && rows[2] == 360 && rows[3] == 720);

	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_YUVA, 720, rows) == 4);
	CHECK(rows[0] == 720 && rows[1] == 720 && rows[2] == 720 && rows[3] == 720);

	// AN ODD HEIGHT ROUNDS UP, exactly as libobs does with (height + 1) / 2. Rounding DOWN
	// leaves the last chroma row of the frame carrying the original picture.
	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_NV12, 721, rows) == 2);
	CHECK(rows[0] == 721 && rows[1] == 361);
	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_I420, 1, rows) == 3);
	CHECK(rows[0] == 1 && rows[1] == 1 && rows[2] == 1);

	// A zero-height frame asks for no rows anywhere, and must not underflow into a huge count.
	CHECK(qci_vision_plane_rows(VIDEO_FORMAT_NV12, 0, rows) == 2);
	CHECK(rows[0] == 0 && rows[1] == 0);

	// ── 4 · THE SWEEP: NO FORMAT IS SILENTLY SINGLE-PLANE ───────────────────────────────
	// The failure this catches is a format added to libobs and not to this file: it would
	// default to one plane, the blank would leave the chroma planes carrying the picture, and
	// the only symptom is a recognisable face in the wrong colours on a live stream.
	//
	// ALL[] must BE libobs's enum, not merely be the same LENGTH as it. The length is pinned at
	// build time by the static_assert above (see the comment on ALL[] for exactly what that does
	// and does not catch); this pins the contents, so a duplicated or reordered entry — which
	// keeps the count right while dropping some format out of every sweep in this file — fails
	// here. enum video_format assigns no explicit values, so the i'th enumerator is i.
	for (int i = 0; i < ALL_N; i++)
		CHECK(ALL[i] == (enum video_format)i);

	for (int i = 0; i < ALL_N; i++) {
		int n = qci_vision_plane_rows(ALL[i], 480, rows);
		// P216/P416 are the two libobs itself marks "Unimplemented" in copy_frame_data, so
		// there is no geometry to state and the caller is told to drop rather than blank.
		if (ALL[i] == VIDEO_FORMAT_P216 || ALL[i] == VIDEO_FORMAT_P416) {
			CHECK(n == 0);
			continue;
		}
		CHECK(n >= 1 && n <= QCI_VISION_MAX_PLANES);
		CHECK(rows[0] == 480);                       // plane 0 is always the full height
		for (int p = 1; p < n; p++)
			CHECK(rows[p] == 480 || rows[p] == 240);
	}

	// …and a frame this filter CAN read is always a frame it can also blank, which is what
	// makes "fail closed" reachable from every path in vision_filter_video.
	for (int i = 0; i < ALL_N; i++)
		if (qci_vision_fourcc(ALL[i]) != 0)
			CHECK(qci_vision_plane_rows(ALL[i], 480, rows) >= 1);

	printf("qci-vision-frame: all %d checks passed\n", checks_run);
	return 0;
}
