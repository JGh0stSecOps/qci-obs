#include "qci-vision-frame.h"

OSType qci_vision_fourcc(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		// BGRX's unused byte is read as alpha and then ignored: VTPixelTransferSession
		// writes into an opaque BGRA buffer and the mosaic never samples it.
		return kCVPixelFormatType_32BGRA;
	case VIDEO_FORMAT_RGBA:
		return kCVPixelFormatType_32RGBA;
	case VIDEO_FORMAT_UYVY:
		return kCVPixelFormatType_422YpCbCr8;
	case VIDEO_FORMAT_YUY2:
		return kCVPixelFormatType_422YpCbCr8_yuvs;
	case VIDEO_FORMAT_NV12:
		return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	case VIDEO_FORMAT_P010:
		// THE ONE THAT WAS MISSING. mac-avcapture maps BOTH 10-bit biplanar subtypes —
		// full-range and video-range — onto VIDEO_FORMAT_P010, so the obs format alone
		// cannot say which came in. Video-range is the one AVFoundation hands over for
		// camera capture, and the range is a colorimetry choice that only affects the
		// mosaic's shade: getting it wrong tints the censored blocks, where getting the
		// LAYOUT wrong (or refusing the frame, which is what shipped) shows the face.
		return kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
	default:
		// Not "unknown, try anyway". Every remaining format either has no CoreVideo
		// equivalent this filter has verified, or has a plane layout the wrap below does
		// not build — and a CVPixelBuffer with the wrong stride is a garbage picture or a
		// crash, not a mask. The caller fails closed on the 0.
		return 0;
	}
}

bool qci_vision_is_planar(enum video_format format)
{
	return format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_P010;
}

int qci_vision_plane_rows(enum video_format format, uint32_t height, uint32_t rows[QCI_VISION_MAX_PLANES])
{
	// (height + 1) / 2, exactly as libobs does it. Rounding DOWN on an odd height leaves the
	// final chroma row of the frame carrying the original picture.
	const uint32_t half = (height + 1u) / 2u;

	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I010:
		rows[0] = height;
		rows[1] = half;
		rows[2] = half;
		return 3;

	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
		rows[0] = height;
		rows[1] = half;
		return 2;

	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
		rows[0] = height;
		rows[1] = height;
		rows[2] = height;
		return 3;

	case VIDEO_FORMAT_I40A:
		rows[0] = height;
		rows[1] = half;
		rows[2] = half;
		rows[3] = height;
		return 4;

	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_YA2L:
		rows[0] = height;
		rows[1] = height;
		rows[2] = height;
		rows[3] = height;
		return 4;

	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		// libobs itself marks these "Unimplemented" in copy_frame_data, which means it has
		// never stated their geometry and this file must not invent one. 0 planes tells the
		// caller to DROP the frame instead of writing into memory whose extent nobody in
		// this process knows.
		return 0;

	case VIDEO_FORMAT_NONE:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_V210:
	case VIDEO_FORMAT_R10L:
		rows[0] = height;
		return 1;
	}

	// No default label above, deliberately: -Wswitch then makes a format added to libobs a
	// COMPILE error here rather than a frame this file quietly treats as single-plane. This
	// line is only reached by a value that is not in the enum at all.
	rows[0] = height;
	return 1;
}
