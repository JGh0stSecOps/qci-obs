#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mac-capture", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "macOS audio input/output and window/display capture";
}

extern struct obs_source_info coreaudio_input_capture_info;
extern struct obs_source_info coreaudio_output_capture_info;
extern struct obs_source_info display_capture_info;
extern struct obs_source_info window_capture_info;

extern bool is_screen_capture_available() WEAK_IMPORT_ATTRIBUTE;

bool obs_module_load(void)
{
	/* Core Audio process taps (AudioHardwareCreateProcessTap) are macOS 14.2+. The source is
	 * simply absent below that rather than registering and failing at create time. */
	if (__builtin_available(macOS 14.2, *)) {
		extern struct obs_source_info coreaudio_app_audio_capture_info;
		extern void app_tap_sweep_orphans(void);
		/* A private aggregate device dies with us, but a TAP outlives its creator, so a
		 * SIGKILL or a crash leaves one behind. Sweep ours -- and only ours, identified by
		 * the creator pid stamped into the tap's name -- exactly once, here at module load.
		 * Deliberately NOT reachable from source destroy: bolting a global cleanup onto a
		 * targeted teardown is what turned the external tool's `stop` into a command that
		 * silenced a live rig. */
		app_tap_sweep_orphans();
		obs_register_source(&coreaudio_app_audio_capture_info);
	}

	if (is_screen_capture_available()) {
		extern struct obs_source_info sck_video_capture_info;
		obs_register_source(&sck_video_capture_info);
		if (__builtin_available(macOS 13.0, *)) {
			display_capture_info.output_flags |= OBS_SOURCE_DEPRECATED;
			window_capture_info.output_flags |= OBS_SOURCE_DEPRECATED;
			coreaudio_output_capture_info.output_flags |= OBS_SOURCE_DEPRECATED;
			extern struct obs_source_info sck_audio_capture_info;
			obs_register_source(&sck_audio_capture_info);
		}
	}
	obs_register_source(&display_capture_info);
	obs_register_source(&window_capture_info);
	obs_register_source(&coreaudio_input_capture_info);
	obs_register_source(&coreaudio_output_capture_info);
	return true;
}
