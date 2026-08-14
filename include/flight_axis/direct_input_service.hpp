#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace flight_axis::input {

struct AxisDescriptor {
	// Persist this opaque string in OBS source settings. It identifies the
	// DirectInput object type/instance instead of relying on a fixed state
	// buffer offset, which varies across vendor drivers.
	std::string id;
	// Old offset-based IDs are retained during migration so existing scene
	// collections keep working after the custom data format is introduced.
	std::string legacy_id;
	std::string display_name;
	// Offset reported by the driver's default data format, retained only for
	// diagnostics and compatibility with old settings.
	std::uint32_t native_offset = 0;
	// Offset assigned in this plugin's per-device DirectInput data format.
	std::uint32_t offset = 0;
	std::uint32_t object_type = 0;
	std::int32_t raw_minimum = -1000;
	std::int32_t raw_maximum = 1000;
};

struct DeviceDescriptor {
	// This is normally derived from the HID device path and only falls back to
	// the DirectInput instance GUID when the path is unavailable.
	std::string id;
	std::string display_name;
	std::string instance_guid;
	std::string device_path;
	std::vector<AxisDescriptor> axes;
};

struct DeviceSnapshot {
	bool connected = false;
	std::uint64_t sequence = 0;
	std::uint64_t updated_at_ms = 0;
	std::unordered_map<std::string, float> axes;

	[[nodiscard]] std::optional<float> axis_value(std::string_view axis_id) const;
};

// A process-wide DirectInput 8 poller. All DirectInput operations happen on
// its worker thread; callers can safely read copies of descriptors/snapshots
// from OBS UI and render threads.
class DirectInputService final {
public:
	DirectInputService();
	~DirectInputService();

	DirectInputService(const DirectInputService &) = delete;
	DirectInputService &operator=(const DirectInputService &) = delete;

	bool start();
	void stop();

	[[nodiscard]] bool running() const noexcept;
	[[nodiscard]] std::vector<DeviceDescriptor> devices() const;
	[[nodiscard]] std::optional<DeviceSnapshot>
	snapshot_for(std::string_view device_id) const;
	[[nodiscard]] std::optional<float>
	axis_value(std::string_view device_id, std::string_view axis_id) const;
	[[nodiscard]] std::string last_error() const;

	// Emits the device's DirectInput object IDs, ranges, and latest normalized
	// values to the OBS log. This is intended for diagnosing vendor mappings.
	void log_device_diagnostics(std::string_view device_id) const;

	// Requests an asynchronous re-enumeration. It is safe to call from an OBS
	// properties callback and never blocks on a hardware operation.
	void request_refresh() noexcept;

	// OBS may provide its main native window to improve DirectInput's
	// cooperative-level setup. Passing nullptr restores automatic discovery.
	void set_cooperative_window(void *native_window) noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

[[nodiscard]] DirectInputService &shared_direct_input_service();

} // namespace flight_axis::input
