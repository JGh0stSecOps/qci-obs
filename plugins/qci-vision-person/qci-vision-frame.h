// The pure half of the person-mask filter: what a frame's PIXELS look like, with no libobs
// instance, no camera and no Vision request in sight.
//
// WHY THIS FILE EXISTS. vision-person-mask.mm had two switches over `enum video_format` and both
// were wrong in the same direction — the direction that passes an unmasked face to the stream:
//
//   1. wrap_frame() knew BGRA/BGRX/RGBA/UYVY/YUY2/NV12 and `return NULL`ed on everything else,
//      and its caller answered a NULL by returning the ORIGINAL FRAME UNTOUCHED. But
//      plugins/mac-avcapture/OBSAVCapture.m formatFromSubtype: also emits VIDEO_FORMAT_P010 —
//      the 10-bit/HDR camera format — which was not in the list. Switch a camera to a 10-bit
//      format and all four `vision_person_mask` instances in the operator's collection pass the
//      raw face through, announced by one LOG_WARNING in a file nobody is watching mid-stream.
//
//   2. There was no second switch at all, because there was nothing to fail closed WITH. A
//      filter that cannot read a frame still has to destroy it; destroying it means knowing how
//      many planes the frame owns and how many rows are in each, and that geometry is stated
//      once in libobs (obs-source.c copy_frame_data) and nowhere a plugin can reach.
//
// Both are decisions about a format id and an integer, so both are testable without a machine
// that can produce the format — which matters, because this Mac's cameras negotiate NV12 and
// P010 cannot be reproduced here on demand. Same split, and the same reason, as
// plugins/qci-audio-unit/qci-au-format.h.
//
// The geometry below MIRRORS libobs/obs-source.c copy_frame_data(). If a new format is added to
// libobs, qci-vision-frame-test.c fails on the exhaustive sweep rather than this file silently
// treating the newcomer as single-plane.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <CoreVideo/CVPixelBuffer.h>
#include <media-io/video-io.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCI_VISION_MAX_PLANES 4

/** The CoreVideo pixel format Vision can be handed for this obs format, or 0 when there is none.
 *  0 is not "black", it is "this filter cannot see this frame" — the caller must fail closed. */
OSType qci_vision_fourcc(enum video_format format);

/** True when qci_vision_fourcc()'s answer must be built with CVPixelBufferCreateWithPlanarBytes
 *  rather than CVPixelBufferCreateWithBytes. */
bool qci_vision_is_planar(enum video_format format);

/** Rows owned by each plane of a frame of this format and height, written into `rows`.
 *  Returns the plane count, which is ZERO ONLY for the two formats libobs itself declares
 *  unimplemented (P216/P416) — a caller that gets 0 must drop the frame, not blank it, because
 *  it has been told nothing it can safely write to. */
int qci_vision_plane_rows(enum video_format format, uint32_t height, uint32_t rows[QCI_VISION_MAX_PLANES]);

#ifdef __cplusplus
}
#endif
