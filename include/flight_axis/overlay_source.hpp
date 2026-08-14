#pragma once

namespace flight_axis::overlay {

// Display modes share one native source type so existing scene collections
// keep loading while the source can render joystick, throttle, rudder, and
// wheel layouts.
enum class DisplayMode : int {
	Joystick = 0,
	Throttle = 1,
	Rudder = 2,
	Wheel = 3,
};

// Registers the native OBS source type. Call once from obs_module_load().
void register_source();

} // namespace flight_axis::overlay
