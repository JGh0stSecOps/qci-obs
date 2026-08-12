#pragma once

#include <CoreAudio/CoreAudio.h>
#include <media-io/audio-io.h>

/* Shared CoreAudio -> libobs format translation.
 *
 * Copied VERBATIM from mac-audio.c so the process-tap source (mac-process-tap-audio.m) uses the
 * exact same translation: a tap's kAudioTapPropertyFormat is an ordinary
 * AudioStreamBasicDescription, and a second, subtly different copy of this logic is how two
 * sources end up disagreeing about what "float32 stereo" means.
 *
 * NOT YET DELETED FROM mac-audio.c. The intended end state is that mac-audio.c includes this
 * header and drops its own two copies -- that is the upstreamable shape, and it is a pure move
 * with no behaviour change. It was left undone here only because mac-audio.c was being edited
 * concurrently by other work in this same tree, and silently losing that edit (or colliding with
 * it into a static-inline redefinition) is a worse outcome than a duplicated 45 lines. Both
 * copies are `static inline` in separate translation units, so the duplication is legal and
 * inert, but it is duplication and it should be collapsed in the same change that lands this.
 *
 * If you are the one collapsing it: delete mac-audio.c:164-205 (the two functions) and add
 * `#include "mac-audio-format.h"` next to its `#include "audio-device-enum.h"`. Nothing else. */

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
