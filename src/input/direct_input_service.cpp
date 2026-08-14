#ifndef _WIN32
#error "The DirectInput input backend is only available on Windows."
#endif

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "flight_axis/axis_mapping.hpp"
#include "flight_axis/direct_input_service.hpp"

#include <obs-module.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace flight_axis::input {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPollInterval = std::chrono::milliseconds(8);
constexpr auto kRefreshInterval = std::chrono::seconds(2);

[[nodiscard]] std::string utf8_from_wide(const wchar_t *value)
{
	if (value == nullptr || *value == L'\0')
		return {};

	const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0,
					       nullptr, nullptr);
	if (length <= 1)
		return {};

	std::string converted(static_cast<std::size_t>(length), '\0');
	const int converted_length = WideCharToMultiByte(
		CP_UTF8, 0, value, -1, converted.data(), length, nullptr, nullptr);
	if (converted_length <= 1)
		return {};
	converted.resize(static_cast<std::size_t>(converted_length - 1));
	return converted;
}

[[nodiscard]] std::string guid_to_string(const GUID &guid)
{
	wchar_t text[40]{};
	if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) <= 0)
		return {};
	return utf8_from_wide(text);
}

[[nodiscard]] std::string offset_to_axis_id(DWORD offset)
{
	return "offset:" + std::to_string(offset);
}

[[nodiscard]] std::string object_to_axis_id(DWORD object_type, DWORD offset)
{
	return "object:" + std::to_string(object_type) + ":" + std::to_string(offset);
}

[[nodiscard]] std::optional<DWORD>
standard_joystick_state_offset(DWORD object_type) noexcept
{
	// EnumObjects reports offsets from the device's current/default data
	// format. After SetDataFormat(c_dfDIJoystick2), those offsets are not
	// necessarily valid. Read every standard absolute axis at the location
	// defined by DIJOYSTATE2 instead.
	switch (DIDFT_GETINSTANCE(object_type)) {
	case 0:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lX));
	case 1:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lY));
	case 2:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lZ));
	case 3:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lRx));
	case 4:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lRy));
	case 5:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, lRz));
	case 6:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, rglSlider[0]));
	case 7:
		return static_cast<DWORD>(offsetof(DIJOYSTATE2, rglSlider[1]));
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::uint64_t now_milliseconds()
{
	const auto duration = Clock::now().time_since_epoch();
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

[[nodiscard]] HWND default_cooperative_window()
{
	if (HWND foreground = GetForegroundWindow(); foreground != nullptr)
		return foreground;
	return GetDesktopWindow();
}

class ComPtr {
public:
	ComPtr() = default;
	explicit ComPtr(IDirectInputDevice8W *value) : value_(value) {}
	~ComPtr() { reset(); }

	ComPtr(const ComPtr &) = delete;
	ComPtr &operator=(const ComPtr &) = delete;
	ComPtr(ComPtr &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
	ComPtr &operator=(ComPtr &&other) noexcept
	{
		if (this != &other)
			reset(std::exchange(other.value_, nullptr));
		return *this;
	}

	[[nodiscard]] IDirectInputDevice8W *get() const noexcept { return value_; }
	[[nodiscard]] IDirectInputDevice8W **put() noexcept
	{
		reset();
		return &value_;
	}
	void reset(IDirectInputDevice8W *value = nullptr) noexcept
	{
		if (value_ != nullptr)
			value_->Release();
		value_ = value;
	}
	[[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
	IDirectInputDevice8W *value_ = nullptr;
};

struct DeviceRuntime {
	DeviceDescriptor descriptor;
	GUID instance_guid{};
	ComPtr device;
	DIJOYSTATE2 state{};
	bool acquired = false;
	DeviceSnapshot snapshot{};
};

struct AxisEnumerationContext {
	std::vector<AxisDescriptor> axes;
};

BOOL CALLBACK enumerate_axis_callback(const DIDEVICEOBJECTINSTANCEW *instance,
				      VOID *context)
{
	auto *result = static_cast<AxisEnumerationContext *>(context);
	if (instance == nullptr || result == nullptr)
		return DIENUM_CONTINUE;

	if ((DIDFT_GETTYPE(instance->dwType) & DIDFT_ABSAXIS) == 0)
		return DIENUM_CONTINUE;

	const DWORD native_offset = instance->dwOfs;
	const DWORD object_type = instance->dwType;

	const auto duplicate = std::find_if(
		result->axes.begin(), result->axes.end(),
		[native_offset, object_type](const AxisDescriptor &axis) {
			return axis.native_offset == native_offset &&
			       axis.object_type == object_type;
		});
	if (duplicate != result->axes.end())
		return DIENUM_CONTINUE;

	AxisDescriptor axis;
	axis.legacy_id = offset_to_axis_id(native_offset);
	axis.id = object_to_axis_id(object_type, native_offset);
	axis.display_name = utf8_from_wide(instance->tszName);
	if (axis.display_name.empty())
		axis.display_name = "Axis " + std::to_string(result->axes.size() + 1);
	axis.native_offset = native_offset;
	const auto state_offset = standard_joystick_state_offset(object_type);
	if (!state_offset)
		return DIENUM_CONTINUE;
	axis.offset = *state_offset;
	axis.object_type = object_type;
	result->axes.emplace_back(std::move(axis));
	return DIENUM_CONTINUE;
}

class DeviceEnumerator {
public:
	DeviceEnumerator(IDirectInput8W *direct_input, HWND cooperative_window,
			 std::vector<DeviceRuntime> *devices, std::string *last_error)
		: direct_input_(direct_input), cooperative_window_(cooperative_window),
		  devices_(devices), last_error_(last_error)
	{
	}

	static BOOL CALLBACK callback(const DIDEVICEINSTANCEW *instance, VOID *context)
	{
		if (instance == nullptr || context == nullptr)
			return DIENUM_CONTINUE;
		return static_cast<DeviceEnumerator *>(context)->add_device(*instance);
	}

	BOOL add_device(const DIDEVICEINSTANCEW &instance)
	{
		ComPtr device;
		if (FAILED(direct_input_->CreateDevice(instance.guidInstance, device.put(),
						       nullptr)))
			return DIENUM_CONTINUE;

		const HWND window = cooperative_window_ != nullptr ? cooperative_window_
								  : default_cooperative_window();
		// Non-exclusive background access is intentionally used so the plugin
		// remains a passive visualizer and never captures the flight controls.
		if (FAILED(device.get()->SetCooperativeLevel(
			    window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)))
			return DIENUM_CONTINUE;

		AxisEnumerationContext axes;
		if (FAILED(device.get()->EnumObjects(enumerate_axis_callback, &axes,
						     DIDFT_AXIS)))
		    return DIENUM_CONTINUE;

		// Linear overlays also support single-axis controls such as handwheels.
		if (axes.axes.empty())
			return DIENUM_CONTINUE;

		// V1 consumes the standard DirectInput joystick state layout. Objects
		// outside DIJOYSTATE2 cannot be read safely from GetDeviceState and are
		// intentionally omitted instead of guessing at a vendor HID report.
		axes.axes.erase(
			std::remove_if(axes.axes.begin(), axes.axes.end(),
				       [](const AxisDescriptor &axis) {
					       return axis.offset + sizeof(LONG) >
						      sizeof(DIJOYSTATE2);
				       }),
			axes.axes.end());
		if (axes.axes.empty())
			return DIENUM_CONTINUE;

		std::sort(axes.axes.begin(), axes.axes.end(), [](const AxisDescriptor &left,
								 const AxisDescriptor &right) {
			if (left.offset != right.offset)
				return left.offset < right.offset;
			return left.object_type < right.object_type;
		});

		DeviceRuntime runtime;
		if (FAILED(device.get()->SetDataFormat(&c_dfDIJoystick2)))
			return DIENUM_CONTINUE;

		// Standardize the raw state values so source instances see a single
		// device-independent normalized domain. Some vendor drivers reject
		// DIPROP_RANGE; retain their native range as a fallback.
		for (auto &axis : axes.axes) {
			DIPROPRANGE range{};
			range.diph.dwSize = sizeof(range);
			range.diph.dwHeaderSize = sizeof(range.diph);
			range.diph.dwHow = DIPH_BYID;
			range.diph.dwObj = axis.object_type;
			if (SUCCEEDED(device.get()->GetProperty(DIPROP_RANGE,
							       &range.diph)) &&
			    range.lMin < range.lMax) {
				axis.raw_minimum = range.lMin;
				axis.raw_maximum = range.lMax;
			}

			DIPROPRANGE standard_range = range;
			standard_range.lMin = -1000;
			standard_range.lMax = 1000;
			if (SUCCEEDED(device.get()->SetProperty(DIPROP_RANGE,
							       &standard_range.diph))) {
				axis.raw_minimum = -1000;
				axis.raw_maximum = 1000;
			}
		}

		DIPROPGUIDANDPATH path{};
		path.diph.dwSize = sizeof(path);
		path.diph.dwHeaderSize = sizeof(path.diph);
		path.diph.dwHow = DIPH_DEVICE;
		const HRESULT path_result = device.get()->GetProperty(
			DIPROP_GUIDANDPATH, &path.diph);

		DeviceDescriptor descriptor;
		descriptor.display_name = utf8_from_wide(instance.tszInstanceName);
		if (descriptor.display_name.empty())
			descriptor.display_name = utf8_from_wide(instance.tszProductName);
		if (descriptor.display_name.empty())
			descriptor.display_name = "DirectInput controller";
		descriptor.instance_guid = guid_to_string(instance.guidInstance);
		descriptor.device_path =
			SUCCEEDED(path_result) ? utf8_from_wide(path.wszPath) : std::string{};
		descriptor.id = descriptor.device_path.empty()
					? "guid:" + descriptor.instance_guid
					: "path:" + descriptor.device_path;
		descriptor.axes = std::move(axes.axes);

		runtime.descriptor = std::move(descriptor);
		runtime.instance_guid = instance.guidInstance;
		runtime.device = std::move(device);
		runtime.snapshot.connected = true;
		devices_->emplace_back(std::move(runtime));
		return DIENUM_CONTINUE;
	}

private:
	IDirectInput8W *direct_input_;
	HWND cooperative_window_;
	std::vector<DeviceRuntime> *devices_;
	std::string *last_error_;
};

} // namespace

std::optional<float> DeviceSnapshot::axis_value(std::string_view axis_id) const
{
	const auto found = axes.find(std::string(axis_id));
	if (found == axes.end())
		return std::nullopt;
	return found->second;
}

struct DirectInputService::Impl {
	std::atomic_bool stop_requested{false};
	std::atomic_bool refresh_requested{true};
	std::atomic_bool running{false};
	std::atomic<HWND> cooperative_window{nullptr};
	std::thread worker;

	mutable std::mutex mutex;
	std::condition_variable wakeup;
	std::vector<DeviceDescriptor> devices;
	std::unordered_map<std::string, DeviceSnapshot> snapshots;
	std::string last_error;

	void set_error(std::string value)
	{
		std::lock_guard lock(mutex);
		last_error = std::move(value);
	}

	void publish(const std::vector<DeviceRuntime> &runtime_devices)
	{
		std::vector<DeviceDescriptor> descriptors;
		std::unordered_map<std::string, DeviceSnapshot> updated_snapshots;
		descriptors.reserve(runtime_devices.size());
		for (const auto &device : runtime_devices) {
			descriptors.emplace_back(device.descriptor);
			updated_snapshots.emplace(device.descriptor.id, device.snapshot);
		}

		std::lock_guard lock(mutex);
		devices = std::move(descriptors);
		for (auto &[device_id, snapshot] : updated_snapshots)
			snapshots[device_id] = std::move(snapshot);
	}

	void publish_all_offline()
	{
		std::lock_guard lock(mutex);
		devices.clear();
		for (auto &[device_id, snapshot] : snapshots) {
			(void)device_id;
			snapshot.connected = false;
			snapshot.axes.clear();
		}
	}

	void poll_device(DeviceRuntime &runtime)
	{
		if (!runtime.device)
			return;

		if (!runtime.acquired) {
			const HRESULT acquired = runtime.device.get()->Acquire();
			if (FAILED(acquired)) {
				runtime.snapshot.connected = false;
				return;
			}
			runtime.acquired = true;
		}

		// DirectInput game controllers are polled devices.  Calling Poll before
		// GetDeviceState is required by a number of joystick drivers and keeps
		// throttle/pedal values advancing instead of returning a stale snapshot.
		HRESULT poll_result = runtime.device.get()->Poll();
		if (FAILED(poll_result)) {
			runtime.acquired = false;
			if (poll_result == DIERR_INPUTLOST || poll_result == DIERR_NOTACQUIRED) {
				const HRESULT reacquired = runtime.device.get()->Acquire();
				if (SUCCEEDED(reacquired)) {
					runtime.acquired = true;
					poll_result = runtime.device.get()->Poll();
				}
			}
		}
		if (FAILED(poll_result)) {
			runtime.snapshot.connected = false;
			if (poll_result == DIERR_UNPLUGGED || poll_result == DIERR_DEVICENOTREG)
				refresh_requested.store(true);
			return;
		}

		const HRESULT state_result = runtime.device.get()->GetDeviceState(
			sizeof(runtime.state), &runtime.state);
		if (FAILED(state_result)) {
			runtime.snapshot.connected = false;
			runtime.acquired = false;
			runtime.device.get()->Unacquire();
			// A lost device is removed on the next loop, then picked up by the
			// regular enumeration pass when Windows exposes it again.
			if (state_result == DIERR_UNPLUGGED ||
			    state_result == DIERR_DEVICENOTREG)
				refresh_requested.store(true);
			return;
		}

		runtime.snapshot.connected = true;
		runtime.snapshot.updated_at_ms = now_milliseconds();
		++runtime.snapshot.sequence;
		runtime.snapshot.axes.clear();
		const auto *bytes = reinterpret_cast<const std::byte *>(&runtime.state);
		for (const auto &axis : runtime.descriptor.axes) {
			LONG raw_value = 0;
			std::memcpy(&raw_value, bytes + axis.offset, sizeof(raw_value));
			const float value = normalize_direct_input_value(
				raw_value, axis.raw_minimum, axis.raw_maximum);
			runtime.snapshot.axes[axis.id] = value;
			if (!axis.legacy_id.empty())
				runtime.snapshot.axes[axis.legacy_id] = value;
		}
	}

	void enumerate(IDirectInput8W *direct_input)
	{
		std::unordered_map<std::string, DeviceSnapshot> previous_snapshots;
		{
			std::lock_guard lock(mutex);
			previous_snapshots = snapshots;
		}

		std::vector<DeviceRuntime> devices_runtime;
		DeviceEnumerator enumerator(direct_input, cooperative_window.load(),
					   &devices_runtime, &last_error);
		const HRESULT enumeration_result = direct_input->EnumDevices(
			DI8DEVCLASS_GAMECTRL, DeviceEnumerator::callback, &enumerator,
			DIEDFL_ATTACHEDONLY);
		if (FAILED(enumeration_result)) {
			set_error("DirectInput device enumeration failed.");
			return;
		}

		// Keep a known disconnected snapshot for a previously selected device
		// while it is absent. OBS sources can then render their offline state
		// instead of treating a re-enumeration as an unset setting.
		std::unordered_map<std::string, DeviceSnapshot> published_snapshots =
			std::move(previous_snapshots);
		for (auto &[device_id, snapshot] : published_snapshots)
			snapshot.connected = false;
		for (const auto &runtime : devices_runtime)
			published_snapshots[runtime.descriptor.id] = runtime.snapshot;

		{
			std::lock_guard lock(mutex);
			devices.clear();
			devices.reserve(devices_runtime.size());
			for (const auto &runtime : devices_runtime)
				devices.emplace_back(runtime.descriptor);
			snapshots = std::move(published_snapshots);
		}
		active_devices = std::move(devices_runtime);
	}

	void worker_main()
	{
		HINSTANCE instance = GetModuleHandleW(nullptr);
		IDirectInput8W *direct_input = nullptr;
		const HRESULT create_result = DirectInput8Create(
			instance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
			reinterpret_cast<void **>(&direct_input), nullptr);
		if (FAILED(create_result)) {
			set_error("Unable to initialize DirectInput 8.");
			publish_all_offline();
			running.store(false);
			return;
		}

		auto next_refresh = Clock::now();
		while (!stop_requested.load()) {
			const auto now = Clock::now();
			if (refresh_requested.exchange(false) || now >= next_refresh) {
				enumerate(direct_input);
				next_refresh = now + kRefreshInterval;
			}

			for (auto &device : active_devices)
				poll_device(device);
			publish(active_devices);

			std::unique_lock lock(mutex);
			wakeup.wait_for(lock, kPollInterval, [this] {
				return stop_requested.load() || refresh_requested.load();
			});
		}

		for (auto &device : active_devices) {
			if (device.device && device.acquired)
				device.device.get()->Unacquire();
		}
		active_devices.clear();
		publish_all_offline();
		direct_input->Release();
		running.store(false);
	}

	std::vector<DeviceRuntime> active_devices;
};

DirectInputService::DirectInputService() : impl_(std::make_unique<Impl>()) {}

DirectInputService::~DirectInputService()
{
	stop();
}

bool DirectInputService::start()
{
	if (impl_->running.load())
		return true;

	if (impl_->worker.joinable())
		impl_->worker.join();

	impl_->stop_requested.store(false);
	impl_->refresh_requested.store(true);
	impl_->running.store(true);
	try {
		impl_->worker = std::thread([impl = impl_.get()] { impl->worker_main(); });
	} catch (...) {
		impl_->running.store(false);
		impl_->set_error("Unable to start the DirectInput polling thread.");
		return false;
	}
	return true;
}

void DirectInputService::stop()
{
	if (!impl_)
		return;

	impl_->stop_requested.store(true);
	impl_->wakeup.notify_all();
	if (impl_->worker.joinable())
		impl_->worker.join();
	impl_->running.store(false);
}

bool DirectInputService::running() const noexcept
{
	return impl_ && impl_->running.load();
}

std::vector<DeviceDescriptor> DirectInputService::devices() const
{
	std::lock_guard lock(impl_->mutex);
	return impl_->devices;
}

std::optional<DeviceSnapshot>
DirectInputService::snapshot_for(std::string_view device_id) const
{
	std::lock_guard lock(impl_->mutex);
	const auto found = impl_->snapshots.find(std::string(device_id));
	if (found == impl_->snapshots.end())
		return std::nullopt;
	return found->second;
}

std::optional<float>
DirectInputService::axis_value(std::string_view device_id,
			       std::string_view axis_id) const
{
	const auto snapshot = snapshot_for(device_id);
	return snapshot.has_value() ? snapshot->axis_value(axis_id) : std::nullopt;
}

std::string DirectInputService::last_error() const
{
	std::lock_guard lock(impl_->mutex);
	return impl_->last_error;
}

void DirectInputService::log_device_diagnostics(std::string_view device_id) const
{
	const auto descriptors = devices();
	const auto snapshots = snapshot_for(device_id);
	for (const auto &descriptor : descriptors) {
		if (!device_id.empty() && descriptor.id != device_id)
			continue;
		blog(LOG_INFO, "[obs-flight-axis-overlay] device '%s' id='%s' axes=%zu",
		     descriptor.display_name.c_str(), descriptor.id.c_str(),
		     descriptor.axes.size());
		for (const auto &axis : descriptor.axes) {
			const auto value = snapshots ? snapshots->axis_value(axis.id) : std::nullopt;
			blog(LOG_INFO,
			     "[obs-flight-axis-overlay]   axis name='%s' id='%s' legacy='%s' type=0x%08lX enum_offset=%lu state_offset=%lu range=[%ld,%ld] value=%s",
			     axis.display_name.c_str(), axis.id.c_str(),
			     axis.legacy_id.c_str(), static_cast<unsigned long>(axis.object_type),
			     static_cast<unsigned long>(axis.native_offset),
			     static_cast<unsigned long>(axis.offset),
			     static_cast<long>(axis.raw_minimum),
			     static_cast<long>(axis.raw_maximum),
			     value ? std::to_string(*value).c_str() : "<missing>");
		}
	}
	if (descriptors.empty())
		blog(LOG_INFO, "[obs-flight-axis-overlay] no DirectInput devices with at least one absolute axis");
}

void DirectInputService::request_refresh() noexcept
{
	impl_->refresh_requested.store(true);
	impl_->wakeup.notify_all();
}

void DirectInputService::set_cooperative_window(void *native_window) noexcept
{
	impl_->cooperative_window.store(static_cast<HWND>(native_window));
	request_refresh();
}

DirectInputService &shared_direct_input_service()
{
	static DirectInputService service;
	return service;
}

} // namespace flight_axis::input
