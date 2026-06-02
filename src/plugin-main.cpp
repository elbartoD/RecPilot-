#include "camera-tally-filter.h"
#include "rec-trigger-dock.h"

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	register_camera_tally_filter();
	camera_tally_register_frontend_events();
	create_rec_trigger_dock();
	obs_log(LOG_INFO, "RecPilot loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	destroy_rec_trigger_dock();
	camera_tally_unregister_frontend_events();
	obs_log(LOG_INFO, "RecPilot unloaded");
}

const char *obs_module_description(void)
{
	return "RecPilot starts and stops OBS recording from a camera tally, then names recordings from OCR clip names.";
}
