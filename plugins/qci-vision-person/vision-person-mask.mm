// qci-vision-person — person segmentation via Apple's Vision framework.
//
// FIRST-PARTY since the fold-in of 2026-08-13. This file used to live in
// ~/obs-vision-plugin, where it linked libobs out of /Applications/OBS.app
// against a VENDORED copy of the obs-studio 32.2.1 headers (obs-src/, simde-src/,
// ~83 MB of pinned upstream tree). That arrangement had two failure modes this
// move removes outright:
//
//   * the binary was bound to whatever libobs the STOCK app shipped, so an OBS
//     update silently changed the ABI underneath a load-bearing privacy filter;
//   * the fork cannot see it at all. QCi Studio searches only
//     ~/Library/Application Support/qci-studio/plugins/, never stock's directory —
//     deliberately, see the comment at frontend/widgets/QCiBasic.cpp:145-155.
//
// It now compiles against the fork's own in-tree headers and links the fork's
// libobs target, and is embedded in the app bundle at Contents/PlugIns/, which
// libobs adds as a module path in obs-cocoa.m:add_default_module_paths().
//
// ⚠️ CONTRACT — DO NOT RENAME EITHER OF THESE:
//     filter id    "vision_person_mask"     (4 live instances in the operator's collection)
//     display name "Person Mask (Vision)"   (the name the rig's ensureFilter() looks up)
// The rig (qci-rig/lib/provision.mjs) creates and finds this filter by that exact
// pair, and the privacy watchdog treats it as a live dependency. Changing either
// orphans the four filters and silently opens the privacy chain. That is also why
// the strings below stay LITERALS rather than obs_module_text() lookups: a
// translated display name would change what the OBS UI pre-fills when the operator
// adds the filter by hand, desyncing it from what the rig expects to find.
//
// Drop-in replacement for obs-backgroundremoval's mask stage. It writes the
// person mask into the frame's ALPHA channel, so the existing
// privacy-single-pass.shader consumes it unchanged.
//
// Why this exists: obs-backgroundremoval stages every frame off the GPU into
// CPU memory for ONNX Runtime and uploads the mask back. That synchronous round
// trip cost ~18ms/frame on an M1 — the model itself is not the expense.
// This filter is ASYNC, so it receives the CPU frame macos-avcapture already
// produced, wraps it in a CVPixelBuffer with no copy, and hands it to Vision,
// which runs on the ANE/GPU. No readback, no upload.
//
// VNGeneratePersonSegmentationRequest segments EVERY person in frame, not just
// the nearest subject.

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>

// The pure, testable half: which pixel formats Vision can be handed, and how many rows of each
// plane a frame of a given format owns. Split out so the P010 gap and the fail-closed blank can
// be executed by qci-vision-frame-test on a machine whose cameras only ever negotiate NV12.
#include "qci-vision-frame.h"

// ⚠️ THROTTLE THE LOG, NEVER THE BLANKING.
//
// Every fail-closed path below runs PER FRAME, so a persistent fault — an unsupported format, a
// pixel transfer session that will not allocate, a camera renegotiating mid-stream — is not one
// event, it is 60 a second, forever. Two of those paths log twice (their own reason line plus
// blank_frame's), which is ~120 lines/sec written from the CAPTURE THREAD while libobs holds
// source->filter_mutex in filter_async_video. The blocking file write is what makes this more than
// noise: it is backpressure on the thread delivering camera frames, so the log about dropped
// privacy would itself start costing frames.
//
// The blanking is NOT throttled and must never be — every frame is still erased. Only the
// reporting is rate-limited, and the suppressed count is printed so a throttled fault cannot read
// as an intermittent one. Statics are per-call-site by construction (each expansion declares its
// own), so one chatty path cannot starve another's first report.
#define VP_LOG_MIN_INTERVAL_NS 1000000000ULL // one line per fault site per second

#define VP_LOG_THROTTLED(level, ...)                                                                  \
	do {                                                                                          \
		static std::atomic<uint64_t> _vp_next{0};                                             \
		static std::atomic<uint64_t> _vp_skipped{0};                                          \
		const uint64_t _vp_now = os_gettime_ns();                                             \
		if (_vp_now < _vp_next.load(std::memory_order_relaxed)) {                             \
			_vp_skipped.fetch_add(1, std::memory_order_relaxed);                          \
			break;                                                                        \
		}                                                                                     \
		_vp_next.store(_vp_now + VP_LOG_MIN_INTERVAL_NS, std::memory_order_relaxed);          \
		const uint64_t _vp_n = _vp_skipped.exchange(0, std::memory_order_relaxed);            \
		blog(level, __VA_ARGS__);                                                             \
		if (_vp_n)                                                                            \
			blog(level, "[vision-person] (%llu more identical to the line above were "     \
				    "suppressed in the last second)",                                 \
			     (unsigned long long)_vp_n);                                              \
	} while (0)

#import <Vision/Vision.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Accelerate/Accelerate.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("qci-vision-person", "en-US")

#define S_QUALITY "quality"
#define S_THRESHOLD "threshold"
#define S_DESPECKLE "despeckle"
#define S_FG_BLOCK "fg_block"
#define S_BG_BLOCK "bg_block"
#define S_LEVELS "levels"
#define S_NOISE "noise"
#define S_LOWLIGHT "lowlight"
#define S_BLACKPOINT "blackpoint"

struct vision_filter {
	obs_source_t *context;

	API_AVAILABLE(macos(12.0))
	VNGeneratePersonSegmentationRequest *request;
	VNSequenceRequestHandler *handler;

	VTPixelTransferSessionRef transfer;
	CVPixelBufferRef bgra;   // reused; per-frame CVPixelBufferCreate leaked hard
	CVPixelBufferRef mos;    // reused mosaic output

	// BGRA scratch we own and hand back to OBS
	struct obs_source_frame out;
	uint8_t *out_data;
	uint32_t out_w, out_h;

	// mask resampled to frame size
	uint8_t *mask_scaled;
	uint8_t *mask_prev;
	uint8_t *mask_tmp;
	uint8_t *m_small, *m_stmp, *m_sprev;
	uint8_t *grid; uint32_t grid_cap;
	uint32_t mw, mh;
	uint32_t mask_cap;

	bool logged;
	int despeckle;
	int fg_block;
	int bg_block;
	int levels;
	float noise;
	float lowlight;
	float blackpoint;
	uint32_t tick;
	int quality;
	float threshold;
};

static const char *vision_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Person Mask (Vision)";
}

static void free_buffers(struct vision_filter *f)
{
	bfree(f->out_data);
	f->out_data = NULL;
	bfree(f->mask_scaled);
	f->mask_scaled = NULL;
	bfree(f->mask_prev);
	f->mask_prev = NULL;
	bfree(f->mask_tmp);
	f->mask_tmp = NULL;
	bfree(f->m_small); f->m_small = NULL;
	bfree(f->m_stmp); f->m_stmp = NULL;
	bfree(f->m_sprev); f->m_sprev = NULL;
	bfree(f->grid); f->grid = NULL; f->grid_cap = 0;
	f->mw = f->mh = 0;
	f->out_w = f->out_h = 0;
	f->mask_cap = 0;
}

static void vision_update(void *data, obs_data_t *settings)
{
	struct vision_filter *f = (struct vision_filter *)data;
	f->quality = (int)obs_data_get_int(settings, S_QUALITY);
	f->threshold = (float)obs_data_get_double(settings, S_THRESHOLD);
	f->despeckle = (int)obs_data_get_int(settings, S_DESPECKLE);
	f->fg_block = (int)obs_data_get_int(settings, S_FG_BLOCK);
	f->bg_block = (int)obs_data_get_int(settings, S_BG_BLOCK);
	f->levels = (int)obs_data_get_int(settings, S_LEVELS);
	f->noise = (float)obs_data_get_double(settings, S_NOISE);
	f->lowlight = (float)obs_data_get_double(settings, S_LOWLIGHT);
	f->blackpoint = (float)obs_data_get_double(settings, S_BLACKPOINT);

	if (@available(macOS 12.0, *)) {
		if (f->request) {
			switch (f->quality) {
			case 0:
				f->request.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelFast;
				break;
			case 1:
				f->request.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
				break;
			default:
				f->request.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelAccurate;
				break;
			}
		}
	}
}

static void *vision_create(obs_data_t *settings, obs_source_t *context)
{
	struct vision_filter *f = (struct vision_filter *)bzalloc(sizeof(*f));
	f->context = context;

	if (@available(macOS 12.0, *)) {
		f->request = [[VNGeneratePersonSegmentationRequest alloc] init];
		f->request.outputPixelFormat = kCVPixelFormatType_OneComponent8;
		f->handler = [[VNSequenceRequestHandler alloc] init];
	} else {
		blog(LOG_ERROR, "[vision-person] requires macOS 12.0 or later");
		bfree(f);
		return NULL;
	}

	// The transfer session is the ONLY thing that ever writes masked pixels back into a frame
	// (see the write-back at the bottom of vision_filter_video). Its status used to be discarded
	// here, so a session that failed to create still logged "filter created" and handed back a
	// filter that could not obscure a single pixel.
	//
	// It is checked now — but the answer to a failure is NOT `return NULL`. libobs keeps the
	// source either way ("allow the source to be created even if creation fails so that the
	// user's data doesn't become lost", libobs/obs-source.c:489-494) and then SKIPS the filter in
	// the chain, because obs-source.c:3343 only calls filter_video `if (filter->context.data &&
	// filter->info.filter_video)`. The filter would still be listed on the source, still look
	// enabled in the UI, and still satisfy the rig's ensureFilter() lookup and the privacy
	// watchdog — while every frame walked past it RAW. That is precisely the fail-open the rest
	// of this file is written against. So the filter stays ALIVE with a NULL session and
	// vision_filter_video blanks every frame instead: black picture, loud log, chain intact.
	const OSStatus vt_status = VTPixelTransferSessionCreate(kCFAllocatorDefault, &f->transfer);
	if (vt_status != noErr) {
		// Do not trust the out-param on a failed create. f was bzalloc'd so it started NULL;
		// put it back that way, releasing anything the call did leave behind.
		if (f->transfer) {
			CFRelease(f->transfer);
			f->transfer = NULL;
		}
		blog(LOG_ERROR,
		     "[vision-person] VTPixelTransferSessionCreate failed (%d) — this filter cannot mask "
		     "anything; every frame will be BLANKED, not passed through",
		     (int)vt_status);
	}
	vision_update(f, settings);
	blog(LOG_INFO, "[vision-person] filter created");
	return f;
}

static void vision_destroy(void *data)
{
	struct vision_filter *f = (struct vision_filter *)data;
	if (@available(macOS 12.0, *)) {
		f->request = nil;
		f->handler = nil;
	}
	if (f->transfer)
		VTPixelTransferSessionInvalidate(f->transfer);
	if (f->transfer)
		CFRelease(f->transfer);
	if (f->bgra)
		CVPixelBufferRelease(f->bgra);
	if (f->mos)
		CVPixelBufferRelease(f->mos);
	free_buffers(f);
	bfree(f);
}

static obs_properties_t *vision_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *q = obs_properties_add_list(props, S_QUALITY, "Quality", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(q, "Fast (live video)", 0);
	obs_property_list_add_int(q, "Balanced", 1);
	obs_property_list_add_int(q, "Accurate", 2);
	obs_properties_add_float_slider(props, S_THRESHOLD, "Mask threshold", 0.0, 1.0, 0.01);
	obs_properties_add_int_slider(props, S_DESPECKLE, "Despeckle (kill false positives)", 0, 24, 1);
	obs_properties_add_int_slider(props, S_FG_BLOCK, "Person block size (px)", 2, 96, 1);
	obs_properties_add_int_slider(props, S_BG_BLOCK, "Background block size (px)", 2, 96, 1);
	obs_properties_add_int_slider(props, S_LEVELS, "Color levels", 2, 64, 1);
	obs_properties_add_float_slider(props, S_NOISE, "Temporal noise", 0.0, 0.3, 0.01);
	obs_properties_add_float_slider(props, S_LOWLIGHT, "Low-light lift (night)", 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(props, S_BLACKPOINT, "Black point (kills grey haze)", 0.0, 0.35, 0.01);
	return props;
}

static void vision_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_QUALITY, 0);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.35);
	obs_data_set_default_int(settings, S_DESPECKLE, 10);
	obs_data_set_default_int(settings, S_FG_BLOCK, 18);
	obs_data_set_default_int(settings, S_BG_BLOCK, 18);
	obs_data_set_default_int(settings, S_LEVELS, 32);
	obs_data_set_default_double(settings, S_NOISE, 0.02);
	obs_data_set_default_double(settings, S_LOWLIGHT, 0.0);
	obs_data_set_default_double(settings, S_BLACKPOINT, 0.06);
}

// Map an obs frame onto a CVPixelBuffer without copying where the layout allows.
//
// The format decision is qci-vision-frame.c's, not a switch here: it is testable without a
// camera, and the camera format that used to be missing from it (P010) is one this Mac cannot be
// made to produce on demand. See that file's header for what the omission cost.
static CVPixelBufferRef wrap_frame(struct obs_source_frame *frame)
{
	const OSType fourcc = qci_vision_fourcc(frame->format);
	if (fourcc == 0)
		return NULL;

	CVPixelBufferRef pb = NULL;
	if (qci_vision_is_planar(frame->format)) {
		uint32_t rows[QCI_VISION_MAX_PLANES] = {0};
		const int planes_n = qci_vision_plane_rows(frame->format, frame->height, rows);
		if (planes_n != 2)
			return NULL;
		void *planes[2] = {frame->data[0], frame->data[1]};
		size_t widths[2] = {frame->width, (frame->width + 1) / 2};
		size_t heights[2] = {rows[0], rows[1]};
		size_t strides[2] = {frame->linesize[0], frame->linesize[1]};
		CVPixelBufferCreateWithPlanarBytes(kCFAllocatorDefault, frame->width, frame->height, fourcc, NULL, 0, 2,
						   planes, widths, heights, strides, NULL, NULL, NULL, &pb);
	} else {
		CVPixelBufferCreateWithBytes(kCFAllocatorDefault, frame->width, frame->height, fourcc, frame->data[0],
					     frame->linesize[0], NULL, NULL, NULL, &pb);
	}
	return pb;
}

// DESTROY a frame this filter could not mask, in the frame's own memory, and return it.
//
// ⚠️ THE FAILURE MODE THIS EXISTS FOR. Below the mask stage the filter already fails closed —
// "no mask means no person anywhere, which still mosaics the entire frame". Above it, four paths
// did the opposite: an unsupported pixel format, either buffer allocation failing, and macOS
// older than 12 all did `return frame`, which is the ORIGINAL PICTURE, UNTOUCHED. On a filter
// literally named for privacy, the outcome of "I cannot process this" was a raw face on the
// stream with one LOG_WARNING behind it.
//
// A FIFTH path was missed by that sweep and is guarded now: the mosaic write-back at the end of
// vision_filter_video is the only code that puts masked pixels into the frame at all, and its
// OSStatus was discarded, so a failed transfer also meant `return frame` — untouched. The rule is
// the whole function's, not just its entry checks: no path may return a frame it did not obscure.
//
// Returning NULL is not the answer either: libobs's filter_frame() increments the frame's
// refcount and only decrements it when the filter hands the frame BACK, so a NULL return leaks
// the frame out of the async cache — the same ownership rule the header above already learned
// the hard way. So the frame is kept and its pixels are erased.
//
// A false-open here is a face; a wrong plane count here is a heap overflow on a live stream.
// The geometry therefore comes from qci-vision-frame.c, which mirrors libobs's own
// copy_frame_data() and is swept exhaustively by qci-vision-frame-test.
static struct obs_source_frame *blank_frame(struct obs_source_frame *frame, const char *why)
{
	uint32_t rows[QCI_VISION_MAX_PLANES] = {0};
	int planes_n = qci_vision_plane_rows(frame->format, frame->height, rows);

	// 0 planes means libobs has never stated this format's layout (P216/P416, which it marks
	// Unimplemented and which no macOS capture path produces). Plane 0 is the luma or the
	// packed image in EVERY format libobs defines, and it always owns `height` rows — so that
	// much is erased even here. For a planar YUV layout that alone is a black picture: luma 0
	// is at or below the black point and clamps there whatever the chroma planes still hold.
	if (planes_n == 0) {
		rows[0] = frame->height;
		planes_n = 1;
	}

	for (int p = 0; p < planes_n; p++) {
		if (!frame->data[p] || !frame->linesize[p])
			continue;
		memset(frame->data[p], 0, (size_t)frame->linesize[p] * rows[p]);
	}

	VP_LOG_THROTTLED(LOG_WARNING, "[vision-person] %s (format %d) — frame BLANKED, not passed through", why,
			 (int)frame->format);
	return frame;
}

static bool ensure_buffers(struct vision_filter *f, uint32_t w, uint32_t h)
{
	if (f->out_w == w && f->out_h == h && f->out_data)
		return true;
	free_buffers(f);
	f->out_data = (uint8_t *)bmalloc((size_t)w * h * 4);
	f->mask_scaled = (uint8_t *)bmalloc((size_t)w * h);
	f->mask_prev = (uint8_t *)bzalloc((size_t)w * h);
	f->mask_tmp = (uint8_t *)bzalloc((size_t)w * h);
	if (!f->out_data || !f->mask_scaled || !f->mask_prev || !f->mask_tmp)
		return false;
	f->out_w = w;
	f->out_h = h;
	f->mask_cap = w * h;
	return true;
}


static inline uint32_t hash_u32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}

// Mosaic one class of blocks (person or background) in place.
// Colour for a block comes ONLY from that block's mean, then quantization and a
// per-block per-tick noise nudge, so block_px^2 pixels collapse to one noisy
// value. Two passes let person and background carry different strengths while
// BOTH stay obscured -- a mask error changes strength, never exposure.
//
// `src`/`sstride` USED to be parameters here and are gone on purpose. They went dead the
// moment the block mean moved to vImageScale's box downscale (the `grid` argument): nothing
// in this function reads the full-resolution frame any more. They survived in the standalone
// repo only because its CMakeLists built with a bare `-Wall` and no -Werror; the fork sets
// GCC_WARN_UNUSED_PARAMETER=YES and GCC_TREAT_WARNINGS_AS_ERRORS=YES (cmake/macos/xcode.cmake
// :148,156), under which a straight copy of this file does not compile. Measured, verbatim:
//     vision-person-mask.mm:273:40: error: unused parameter 'src' [-Werror,-Wunused-parameter]
//     vision-person-mask.mm:273:52: error: unused parameter 'sstride' [-Werror,-Wunused-parameter]
// Removing them is the only change to this function's behaviour: none.
static void mosaic_pass(uint8_t *dst, uint32_t dstride,
			const uint8_t *mask, uint32_t w, uint32_t h, uint32_t bs, bool want_person,
			uint8_t cut, int levels, float noise, uint32_t tick, float lift, float black,
			uint8_t *grid)
{
	if (bs < 1) bs = 1;
	const float lv = (float)(levels < 2 ? 2 : levels);
	for (uint32_t by = 0; by < h; by += bs) {
		uint32_t y1 = by + bs > h ? h : by + bs;
		for (uint32_t bx = 0; bx < w; bx += bs) {
			uint32_t x1 = bx + bs > w ? w : bx + bs;

			uint32_t cy = (by + y1) >> 1, cx = (bx + x1) >> 1;
			bool person = mask[(size_t)cy * w + cx] >= cut;
			if (person != want_person)
				continue;

			// Block mean already computed by vImageScale's box downscale.
			const uint32_t gw = (w + bs - 1) / bs;
			const uint8_t *gp = grid + ((size_t)(by / bs) * gw + (bx / bs)) * 4;
			float fb = gp[0], fg = gp[1], fr = gp[2];

			// Night curve. Subtract the sensor's noise floor FIRST, then lift.
			// A bare gamma amplifies that floor into grey haze — which is why
			// "brighter" kept meaning "washed-out blacks". Rescaling after the
			// subtraction keeps true black at 0 while opening up the shadows.
			if (black > 0.001f || lift > 0.001f) {
				float bp = black * 255.0f;
				float sc = 255.0f / fmaxf(255.0f - bp, 1.0f);
				fb = (fb - bp) * sc; if (fb < 0.0f) fb = 0.0f;
				fg = (fg - bp) * sc; if (fg < 0.0f) fg = 0.0f;
				fr = (fr - bp) * sc; if (fr < 0.0f) fr = 0.0f;
				if (lift > 0.001f) {
					float g = 1.0f / (1.0f + lift * 1.6f);
					fb = 255.0f * powf(fb / 255.0f, g);
					fg = 255.0f * powf(fg / 255.0f, g);
					fr = 255.0f * powf(fr / 255.0f, g);
				}
			}

			float q = 255.0f / (lv - 1.0f);
			fb = q * floorf(fb / q + 0.5f);
			fg = q * floorf(fg / q + 0.5f);
			fr = q * floorf(fr / q + 0.5f);

			if (noise > 0.0001f) {
				uint32_t hsh = hash_u32((bx / bs) * 73856093u ^ (by / bs) * 19349663u ^ tick * 83492791u);
				float nz = (((float)(hsh & 0xffff) / 65535.0f) - 0.5f) * noise * 255.0f;
				fb += nz; fg += nz; fr += nz;
			}

			uint8_t ob = (uint8_t)(fb < 0 ? 0 : fb > 255 ? 255 : fb);
			uint8_t og = (uint8_t)(fg < 0 ? 0 : fg > 255 ? 255 : fg);
			uint8_t orr = (uint8_t)(fr < 0 ? 0 : fr > 255 ? 255 : fr);

			for (uint32_t y = by; y < y1; y++) {
				uint8_t *dp = dst + (size_t)y * dstride + (size_t)bx * 4;
				for (uint32_t x = bx; x < x1; x++, dp += 4) {
					dp[0] = ob; dp[1] = og; dp[2] = orr; dp[3] = 255;
				}
			}
		}
	}
}
// Both scratch pixel buffers are allocated ONCE per resolution and reused.
static bool ensure_pixbufs(struct vision_filter *f, uint32_t w, uint32_t h)
{
	if (f->bgra && (CVPixelBufferGetWidth(f->bgra) != w || CVPixelBufferGetHeight(f->bgra) != h)) {
		CVPixelBufferRelease(f->bgra); f->bgra = NULL;
		if (f->mos) { CVPixelBufferRelease(f->mos); f->mos = NULL; }
	}
	if (!f->bgra || !f->mos) {
		NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
		if (!f->bgra)
			CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
					    (__bridge CFDictionaryRef)attrs, &f->bgra);
		if (!f->mos)
			CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
					    (__bridge CFDictionaryRef)attrs, &f->mos);
	}
	return f->bgra && f->mos;
}

// Process a frame IN PLACE and return the SAME pointer.
//
// ⚠️ This is the whole reason the plugin leaked ~110 MB/s: libobs hands an async
// filter a frame from the source's async cache and releases whatever pointer the
// filter RETURNS. Returning our own buffer meant the cached input frame was never
// returned to the free list, so the cache grew without bound (1280x720 UYVY x
// 60fps). Never return a foreign frame from filter_video — convert the result
// back into the frame's own memory and return `frame`.
static struct obs_source_frame *vision_filter_video(void *data, struct obs_source_frame *frame)
{
	struct vision_filter *f = (struct vision_filter *)data;
	if (!frame)
		return frame;

	// No transfer session means the write-back at the bottom — the only code that ever puts
	// masked pixels into `frame` — cannot run, so nothing below could obscure anything. Fail
	// closed up front rather than spending a whole frame's work to arrive at the same place.
	// Reachable: VTPixelTransferSessionCreate can fail in vision_create, which deliberately
	// keeps the filter alive rather than letting libobs skip it; see the comment there.
	if (!f->transfer)
		return blank_frame(frame, "no pixel transfer session");

	if (@available(macOS 12.0, *)) {
	  @autoreleasepool {
		CVPixelBufferRef src = wrap_frame(frame);
		if (!src)
			return blank_frame(frame, "unsupported frame format");
		if (!ensure_buffers(f, frame->width, frame->height)) {
			CFRelease(src);
			return blank_frame(frame, "scratch buffers unavailable");
		}
		if (!ensure_pixbufs(f, frame->width, frame->height)) {
			CFRelease(src);
			return blank_frame(frame, "pixel buffers unavailable");
		}

		// input -> BGRA (Vision returns garbage from a raw UYVY buffer)
		//
		// f->bgra is allocated once per resolution and REUSED, so a failure here does not
		// leave it empty — it leaves the PREVIOUS frame's picture in it. Carrying on would
		// segment the mask from the wrong image and then stamp a mosaic of stale pixels
		// over this one: obscured, but a lie about what the camera is seeing right now, and
		// on a frozen mask. Blank instead — this filter's contract is that "I cannot process
		// this" comes out black.
		const OSStatus to_bgra = VTPixelTransferSessionTransferImage(f->transfer, src, f->bgra);
		if (to_bgra != noErr) {
			VP_LOG_THROTTLED(LOG_ERROR, "[vision-person] pixel transfer (frame -> BGRA) failed (%d)",
			     (int)to_bgra);
			CFRelease(src);
			return blank_frame(frame, "input conversion to BGRA failed");
		}

		// --- segment every person in frame ---
		const size_t npx = (size_t)frame->width * frame->height;
		bool have_mask = false;
		NSError *err = nil;
		if ([f->handler performRequests:@[f->request] onCVPixelBuffer:f->bgra error:&err] &&
		    f->request.results.count > 0) {
			VNPixelBufferObservation *res = (VNPixelBufferObservation *)f->request.results.firstObject;
			CVPixelBufferRef mask = res.pixelBuffer;
			// Clean the mask at ITS OWN resolution (256x192), not the full frame.
			// A 21x21 morphological pass over 1280x720 is ~19x more work for an
			// identical result — that was the 32ms render / dropped frames.
			CVPixelBufferLockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
			uint32_t mw = (uint32_t)CVPixelBufferGetWidth(mask);
			uint32_t mh = (uint32_t)CVPixelBufferGetHeight(mask);
			size_t mstr = CVPixelBufferGetBytesPerRow(mask);
			if (f->mw != mw || f->mh != mh) {
				bfree(f->m_small); bfree(f->m_stmp); bfree(f->m_sprev);
				f->m_small = (uint8_t *)bmalloc((size_t)mw * mh);
				f->m_stmp  = (uint8_t *)bmalloc((size_t)mw * mh);
				f->m_sprev = (uint8_t *)bzalloc((size_t)mw * mh);
				f->mw = mw; f->mh = mh;
			}
			const uint8_t *mb = (const uint8_t *)CVPixelBufferGetBaseAddress(mask);
			for (uint32_t y = 0; y < mh; y++)
				memcpy(f->m_small + (size_t)y * mw, mb + (size_t)y * mstr, mw);
			CVPixelBufferUnlockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);

			if (f->despeckle > 0) {
				uint32_t k = (uint32_t)(f->despeckle | 1);
				vImage_Buffer a = {f->m_small, mh, mw, mw};
				vImage_Buffer t = {f->m_stmp, mh, mw, mw};
				vImageMin_Planar8(&a, &t, NULL, 0, 0, k, k, kvImageEdgeExtend);
				vImageMax_Planar8(&t, &a, NULL, 0, 0, k, k, kvImageEdgeExtend);
			}
			const size_t mn = (size_t)mw * mh;
			for (size_t i = 0; i < mn; i++) {
				int cur = f->m_small[i], prv = f->m_sprev[i];
				f->m_small[i] = (uint8_t)(cur > prv ? prv + ((cur - prv) * 3) / 4
								   : prv + ((cur - prv) * 1) / 4);
			}
			memcpy(f->m_sprev, f->m_small, mn);

			vImage_Buffer vsrc = {f->m_small, mh, mw, mw};
			vImage_Buffer vdst = {f->mask_scaled, frame->height, frame->width, frame->width};
			vImageScale_Planar8(&vsrc, &vdst, NULL, kvImageNoFlags);
			have_mask = true;
		} else if (err) {
			blog(LOG_WARNING, "[vision-person] request failed: %s", err.localizedDescription.UTF8String);
		}

		// FAIL CLOSED: no mask means "no person anywhere", which still mosaics
		// the entire frame at the background strength. Never pass raw video.
		if (!have_mask)
			memset(f->mask_scaled, 0, npx);


		// --- mosaic BGRA -> mosaic buffer ---
		CVPixelBufferLockBaseAddress(f->bgra, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferLockBaseAddress(f->mos, 0);
		const uint8_t *bsrc = (const uint8_t *)CVPixelBufferGetBaseAddress(f->bgra);
		size_t bstride = CVPixelBufferGetBytesPerRow(f->bgra);
		uint8_t *mdst = (uint8_t *)CVPixelBufferGetBaseAddress(f->mos);
		size_t mstride = CVPixelBufferGetBytesPerRow(f->mos);

		const uint8_t cut = (uint8_t)(f->threshold * 255.0f);
		f->tick++;
		uint32_t tk = f->tick / 10;

		// One hardware box-downscale per block size replaces the scalar
		// per-pixel accumulate loop: vImageScale IS the block mean.
		const uint32_t need = ((frame->width / 2 + 1) * (frame->height / 2 + 1)) * 4;
		if (f->grid_cap < need) {
			bfree(f->grid);
			f->grid = (uint8_t *)bmalloc(need);
			f->grid_cap = need;
		}
		const int blocks[2] = {f->fg_block, f->bg_block};
		for (int pass = 0; pass < 2; pass++) {
			uint32_t bs = (uint32_t)(blocks[pass] < 1 ? 1 : blocks[pass]);
			if (pass == 1 && blocks[1] == blocks[0]) {
				// same size: grid already built, just run the other class
			} else {
				uint32_t gw = (frame->width + bs - 1) / bs;
				uint32_t gh = (frame->height + bs - 1) / bs;
				vImage_Buffer gsrc = {(void *)bsrc, frame->height, frame->width, bstride};
				vImage_Buffer gdst = {f->grid, gh, gw, (size_t)gw * 4};
				vImageScale_ARGB8888(&gsrc, &gdst, NULL, kvImageNoFlags);
			}
			mosaic_pass(mdst, (uint32_t)mstride, f->mask_scaled, frame->width,
				    frame->height, bs, pass == 0, cut, f->levels, f->noise, tk,
				    f->lowlight, f->blackpoint, f->grid);
		}

		CVPixelBufferUnlockBaseAddress(f->mos, 0);
		CVPixelBufferUnlockBaseAddress(f->bgra, kCVPixelBufferLock_ReadOnly);

		// mosaic -> back into the ORIGINAL frame's own memory, then return `frame`
		//
		// ⚠️ THE FIFTH FAIL-OPEN — the one blank_frame's header missed. This is the ONLY
		// write of masked pixels into the outgoing frame: `src` merely ALIASES the frame's
		// own planes (wrap_frame, no copy), the mosaic was composed into f->mos, and nothing
		// else in this function touches frame->data. So when this transfer fails, `frame` is
		// still bit-for-bit the camera's input, and a bare `return frame` puts a RAW FACE on
		// the stream while the UI and the rig both show the filter as active — silently,
		// since the status was discarded. Every sibling failure above blanks; this one alone
		// passed a face through.
		//
		// Blank rather than drop: libobs's filter chain releases whatever this returns, so
		// returning NULL leaks the frame out of the source's async cache (the ownership rule
		// in blank_frame's header and in this function's own header). Keeping the frame and
		// erasing its pixels is both fail-closed and leak-free — and a black picture is a
		// signal the operator cannot miss mid-stream, which a dropped frame is not.
		const OSStatus writeback = VTPixelTransferSessionTransferImage(f->transfer, f->mos, src);
		CFRelease(src);
		if (writeback != noErr) {
			VP_LOG_THROTTLED(LOG_ERROR, "[vision-person] pixel transfer (mosaic -> frame) failed (%d)",
			     (int)writeback);
			return blank_frame(frame, "mosaic write-back to frame failed");
		}
		return frame;
	  }
	}
	// macOS < 12: VNGeneratePersonSegmentationRequest does not exist, so there is no mask and
	// never will be on this host. This used to `return frame` — the raw picture — which made
	// the entire filter a no-op on an older OS while still sitting in the chain looking
	// enabled. Fail closed like every other path: the operator sees black and reads the log.
	return blank_frame(frame, "macOS 12+ required for person segmentation");
}

static struct obs_source_info vision_filter_info = {
	.id = "vision_person_mask",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = vision_name,
	.create = vision_create,
	.destroy = vision_destroy,
	.get_defaults = vision_defaults,
	.get_properties = vision_properties,
	.update = vision_update,
	.filter_video = vision_filter_video,
};

bool obs_module_load(void)
{
	obs_register_source(&vision_filter_info);
	blog(LOG_INFO, "[vision-person] loaded (Apple Vision person segmentation)");
	return true;
}
