#include <util/text-lookup.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <obs-module.h>
#include <file-updater/file-updater.h>

#include "rtmp-format-ver.h"

/* QCi: service-specific/ is deleted. Twitch / Amazon IVS / Dacast / Nimo TV / SHOWROOM
 * ingest resolution is gone along with the services themselves; data/services.json now
 * carries only Restream.io, whose ingest servers are plain static URLs in that file. */

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rtmp-services", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS core RTMP services";
}

#if defined(ENABLE_SERVICE_UPDATES)
static const char *RTMP_SERVICES_LOG_STR = "[rtmp-services plugin] ";
static const char *RTMP_SERVICES_URL = (const char *)SERVICES_URL;
#endif

extern struct obs_service_info rtmp_common_service;
extern struct obs_service_info rtmp_custom_service;

static update_info_t *update_info = NULL;
static struct dstr module_name = {0};

/* QCi: get_module_name() removed -- its only caller was service-specific/dacast.c. */

#if defined(ENABLE_SERVICE_UPDATES)
static bool confirm_service_file(void *param, struct file_download_data *file)
{
	if (astrcmpi(file->name, "services.json") == 0) {
		obs_data_t *data;
		int format_version;

		data = obs_data_create_from_json((char *)file->buffer.array);
		if (!data)
			return false;

		format_version = (int)obs_data_get_int(data, "format_version");
		obs_data_release(data);

		if (format_version != RTMP_SERVICES_FORMAT_VERSION)
			return false;
	}

	UNUSED_PARAMETER(param);
	return true;
}
#endif

bool obs_module_load(void)
{
	dstr_copy(&module_name, "rtmp-services plugin (libobs ");
	dstr_cat(&module_name, obs_get_version_string());
	dstr_cat(&module_name, ")");

	/* QCi: the "twitch_ingests_refresh" / "amazon_ivs_ingests_refresh" procs are no longer
	 * registered. The autoconfig wizard still calls them by name; proc_handler_call()
	 * returns false for an unknown proc, so those calls are simply no-ops now. */

#if defined(ENABLE_SERVICE_UPDATES)
	/* QCi: OFF by default -- see the comment on ENABLE_SERVICE_UPDATES in CMakeLists.txt.
	 * This block is the only thing in this plugin that reaches the network, and leaving it
	 * enabled would re-download the full obsproject.com services list over the
	 * Restream-only data/services.json. */
	char *local_dir = obs_module_file("");
	char *cache_dir = obs_module_config_path("");
	char update_url[128];
	snprintf(update_url, sizeof(update_url), "%s/v%d", RTMP_SERVICES_URL, RTMP_SERVICES_FORMAT_VERSION);

	if (cache_dir) {
		update_info = update_info_create(RTMP_SERVICES_LOG_STR, module_name.array, update_url, local_dir,
						 cache_dir, confirm_service_file, NULL);
	}

	bfree(local_dir);
	bfree(cache_dir);
#endif

	obs_register_service(&rtmp_common_service);
	obs_register_service(&rtmp_custom_service);
	return true;
}

void obs_module_unload(void)
{
	update_info_destroy(update_info);
	dstr_free(&module_name);
}
