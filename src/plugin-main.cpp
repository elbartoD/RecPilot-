#include "camera-tally-filter.h"
#include "rec-trigger-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

void create_dock_after_frontend_load(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		create_rec_trigger_dock();
}

} // namespace

bool obs_module_load(void)
{
	register_camera_tally_filter();
	camera_tally_register_frontend_events();
	obs_frontend_add_event_callback(create_dock_after_frontend_load, nullptr);
	obs_log(LOG_INFO, "RecPilot loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(create_dock_after_frontend_load, nullptr);
	destroy_rec_trigger_dock();
	camera_tally_unregister_frontend_events();
	obs_log(LOG_INFO, "RecPilot unloaded");
}

const char *obs_module_description(void)
{
	return "RecPilot starts and stops OBS recording from a camera tally, then names recordings from OCR clip names.";
}
