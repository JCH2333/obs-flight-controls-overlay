#include <obs-module.h>
#include <plugin-support.h>

#include "flight_axis/direct_input_service.hpp"
#include "flight_axis/overlay_source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("OBS Flight Controls Overlay Contributors")

bool obs_module_load(void)
{
	if (!flight_axis::input::shared_direct_input_service().start()) {
		blog(LOG_WARNING, "DirectInput service could not be started: %s",
			flight_axis::input::shared_direct_input_service().last_error().c_str());
	}
	flight_axis::overlay::register_source();
	blog(LOG_INFO, "obs-flight-axis-overlay loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	flight_axis::input::shared_direct_input_service().stop();
	blog(LOG_INFO, "obs-flight-axis-overlay unloaded");
}
