#include "flight_axis/overlay_source.hpp"

#include "flight_axis/axis_mapping.hpp"
#include "flight_axis/direct_input_service.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Gdiplus;

namespace flight_axis::overlay {
namespace {

constexpr uint32_t kCanvasWidth = 360;
constexpr uint32_t kCanvasHeight = 220;
constexpr uint32_t kRenderScale = 2;
constexpr uint32_t kRenderWidth = kCanvasWidth * kRenderScale;
constexpr uint32_t kRenderHeight = kCanvasHeight * kRenderScale;

constexpr char kDevice[] = "device_id";
constexpr char kMode[] = "display_mode";
constexpr char kXAxis[] = "x_axis";
constexpr char kYAxis[] = "y_axis";
constexpr char kAxis1[] = "axis_1";
constexpr char kAxis2[] = "axis_2";
constexpr char kAxis3[] = "axis_3";
constexpr char kAxis4[] = "axis_4";
constexpr char kLabel[] = "label";
constexpr char kInvertX[] = "invert_x";
constexpr char kInvertY[] = "invert_y";
constexpr char kInvertAxis1[] = "invert_axis_1";
constexpr char kInvertAxis2[] = "invert_axis_2";
constexpr char kInvertAxis3[] = "invert_axis_3";
constexpr char kInvertAxis4[] = "invert_axis_4";
constexpr char kXMin[] = "x_min";
constexpr char kXCenter[] = "x_center";
constexpr char kXMax[] = "x_max";
constexpr char kYMin[] = "y_min";
constexpr char kYCenter[] = "y_center";
constexpr char kYMax[] = "y_max";
constexpr char kDeadzone[] = "deadzone";
constexpr char kSmoothing[] = "smoothing_ms";
constexpr char kColor[] = "accent_color";
constexpr char kOpacity[] = "opacity";
constexpr char kDotSize[] = "dot_size";
constexpr char kMargin[] = "safe_margin";
constexpr char kAutoHide[] = "auto_hide";
constexpr char kStartHidden[] = "start_hidden";
constexpr char kHideDelay[] = "hide_delay_seconds";
constexpr char kDeviceGroup[] = "device_group";
constexpr char kCalibrationGroup[] = "calibration_group";
constexpr char kDisplayGroup[] = "display_group";
constexpr char kStatus[] = "device_status";
constexpr char kDetectionStatus[] = "axis_detection_status";
constexpr char kModeGroup[] = "mode_group";
constexpr char kLinearGroup[] = "linear_group";
constexpr char kDetectXAxis[] = "detect_x_axis";
constexpr char kDetectYAxis[] = "detect_y_axis";
constexpr float kActivityThreshold = 0.01F;
constexpr std::array<const char *, 4> kLinearAxes = {
	kAxis1,
	kAxis2,
	kAxis3,
	kAxis4,
};
constexpr std::array<const char *, 4> kLinearInverts = {
	kInvertAxis1,
	kInvertAxis2,
	kInvertAxis3,
	kInvertAxis4,
};
constexpr std::array<const char *, 4> kLinearLabels = {
	"LinearAxis1",
	"LinearAxis2",
	"LinearAxis3",
	"LinearAxis4",
};
constexpr std::array<const char *, 4> kDetectLinearAxes = {
	"detect_axis_1",
	"detect_axis_2",
	"detect_axis_3",
	"detect_axis_4",
};

// Keep this separator in UTF-8 without making the source file depend on a
// compiler-specific execution character set.
constexpr char kOfflineSeparator[] = " \xC2\xB7 ";

[[nodiscard]] const char *default_label() noexcept
{
	return obs_module_text("DefaultLabel");
}

std::once_flag g_gdiplus_once;
ULONG_PTR g_gdiplus_token = 0;

void ensure_gdiplus()
{
	std::call_once(g_gdiplus_once, [] {
		GdiplusStartupInput input;
		if (GdiplusStartup(&g_gdiplus_token, &input, nullptr) != Ok)
			g_gdiplus_token = 0;
	});
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view text)
{
	if (text.empty())
		return {};
	const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
							      static_cast<int>(text.size()), nullptr, 0);
	if (needed <= 0)
		return std::wstring(text.begin(), text.end());
	std::wstring result(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
					    static_cast<int>(text.size()), result.data(), needed);
	return result;
}

[[nodiscard]] Color color_from_obs(uint32_t rgba, BYTE alpha_override = 255)
{
	const BYTE r = static_cast<BYTE>((rgba >> 24) & 0xffU);
	const BYTE g = static_cast<BYTE>((rgba >> 16) & 0xffU);
	const BYTE b = static_cast<BYTE>((rgba >> 8) & 0xffU);
	const BYTE a = static_cast<BYTE>(rgba & 0xffU);
	return Color(static_cast<BYTE>(std::min<int>(a, alpha_override)), r, g, b);
}

void apply_overall_opacity(std::vector<uint8_t> &pixels, float opacity)
{
	const float factor = std::isfinite(opacity) ? std::clamp(opacity, 0.0F, 1.0F) : 1.0F;
	if (factor >= 0.9999F)
		return;
	if (factor <= 0.0001F) {
		std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(0));
		return;
	}

	// OBS renders this texture using premultiplied-alpha blending. Scaling
	// every BGRA component preserves anti-aliased edges while fading all UI
	// elements together, including text, frames, and offline indicators.
	for (uint8_t &component : pixels) {
		component = static_cast<uint8_t>(std::clamp(
			std::lround(static_cast<float>(component) * factor), 0L, 255L));
	}
}

[[nodiscard]] AxisCalibration read_calibration(obs_data_t *settings, const char *minimum,
								const char *center, const char *maximum)
{
	AxisCalibration calibration;
	calibration.minimum = static_cast<float>(obs_data_get_double(settings, minimum));
	calibration.center = static_cast<float>(obs_data_get_double(settings, center));
	calibration.maximum = static_cast<float>(obs_data_get_double(settings, maximum));
	if (!is_valid_calibration(calibration))
		calibration = {};
	return calibration;
}

[[nodiscard]] float finite_clamp(float value, float minimum, float maximum, float fallback) noexcept
{
	if (!std::isfinite(value))
		return fallback;
	return std::clamp(value, minimum, maximum);
}

[[nodiscard]] DisplayMode read_display_mode(obs_data_t *settings) noexcept
{
	const auto raw = settings ? obs_data_get_int(settings, kMode) : 0;
	return static_cast<DisplayMode>(std::clamp<long long>(
		raw, static_cast<long long>(DisplayMode::Joystick),
		static_cast<long long>(DisplayMode::Wheel)));
}

[[nodiscard]] bool is_joystick_mode(DisplayMode mode) noexcept
{
	return mode == DisplayMode::Joystick;
}

[[nodiscard]] bool is_throttle_mode(DisplayMode mode) noexcept
{
	return mode == DisplayMode::Throttle;
}

[[nodiscard]] bool is_rudder_mode(DisplayMode mode) noexcept
{
	return mode == DisplayMode::Rudder;
}

[[nodiscard]] bool is_wheel_mode(DisplayMode mode) noexcept
{
	return mode == DisplayMode::Wheel;
}

[[nodiscard]] float unipolar_progress(float value) noexcept
{
	return (clamp_unit(value) + 1.0F) * 0.5F;
}

void draw_track(Graphics &graphics, const RectF &track, const Color &accent)
{
	SolidBrush background(Color(26, 4, 10, 12));
	graphics.FillRectangle(&background, track);
	Pen border(Color(96, accent.GetRed(), accent.GetGreen(), accent.GetBlue()), 1.0F);
	graphics.DrawRectangle(&border, track);
}

void draw_centered_horizontal_axis(Graphics &graphics, const RectF &track,
					   float value, const Color &accent)
{
	draw_track(graphics, track, accent);
	const REAL center = track.X + track.Width * 0.5F;
	const REAL position = track.X + unipolar_progress(value) * track.Width;
	SolidBrush fill(accent);
	const REAL left = std::min(center, position);
	const REAL width = std::max<REAL>(1.0F, std::abs(position - center));
	graphics.FillRectangle(&fill, left, track.Y + 1.0F, width,
			       std::max<REAL>(1.0F, track.Height - 2.0F));
	Pen zero_line(Color(122, 255, 255, 255), 1.0F);
	graphics.DrawLine(&zero_line, center, track.Y - 3.0F, center,
			  track.Y + track.Height + 3.0F);
}

void draw_unipolar_vertical_axis(Graphics &graphics, const RectF &track,
				 float value, const Color &accent)
{
	draw_track(graphics, track, accent);
	const REAL filled = unipolar_progress(value) * track.Height;
	SolidBrush fill(accent);
	graphics.FillRectangle(&fill, track.X + 1.0F,
			       track.Y + track.Height - filled,
			       std::max<REAL>(1.0F, track.Width - 2.0F),
			       std::max<REAL>(1.0F, filled));
}

void draw_joystick_frame(Graphics &graphics, const RectF &field,
			 bool connected, const Color &accent)
{
	const Color base = connected
		? Color(122, accent.GetRed(), accent.GetGreen(), accent.GetBlue())
		: Color(150, 176, 107, 99);
	const Color corner = connected
		? Color(210, accent.GetRed(), accent.GetGreen(), accent.GetBlue())
		: Color(192, 196, 116, 106);
	Pen frame(base, 1.0F);
	Pen emphasis(corner, 1.25F);
	graphics.DrawRectangle(&frame, field);

	const REAL corner_length = std::clamp(field.Width * 0.09F, 10.0F, 16.0F);
	const REAL left = field.X;
	const REAL top = field.Y;
	const REAL right = field.GetRight();
	const REAL bottom = field.GetBottom();
	graphics.DrawLine(&emphasis, left, top, left + corner_length, top);
	graphics.DrawLine(&emphasis, left, top, left, top + corner_length);
	graphics.DrawLine(&emphasis, right - corner_length, top, right, top);
	graphics.DrawLine(&emphasis, right, top, right, top + corner_length);
	graphics.DrawLine(&emphasis, left, bottom - corner_length, left, bottom);
	graphics.DrawLine(&emphasis, left, bottom, left + corner_length, bottom);
	graphics.DrawLine(&emphasis, right, bottom - corner_length, right, bottom);
	graphics.DrawLine(&emphasis, right - corner_length, bottom, right, bottom);

	const REAL cx = field.X + field.Width * 0.5F;
	const REAL cy = field.Y + field.Height * 0.5F;
	const REAL mark = std::clamp(field.Width * 0.065F, 8.0F, 12.0F);
	Pen center_line(Color(78, 236, 245, 244), 1.0F);
	graphics.DrawLine(&center_line, cx - mark, cy, cx + mark, cy);
	graphics.DrawLine(&center_line, cx, cy - mark, cx, cy + mark);

	Pen tick(Color(98, accent.GetRed(), accent.GetGreen(), accent.GetBlue()), 1.0F);
	const REAL edge_tick = std::clamp(field.Width * 0.035F, 4.0F, 7.0F);
	graphics.DrawLine(&tick, cx, top, cx, top + edge_tick);
	graphics.DrawLine(&tick, cx, bottom - edge_tick, cx, bottom);
	graphics.DrawLine(&tick, left, cy, left + edge_tick, cy);
	graphics.DrawLine(&tick, right - edge_tick, cy, right, cy);
}

struct OverlaySource {
	struct SelectionSnapshot {
		std::string device_id;
		DisplayMode mode = DisplayMode::Joystick;
		std::string x_axis;
		std::string y_axis;
		std::array<std::string, 4> linear_axes{};
		bool axis_detection_active = false;
		std::string axis_detection_message;
		bool auto_hide = false;
	};

	mutable std::mutex mutex;
	obs_weak_source_t *source = nullptr;
	std::string device_id;
	DisplayMode mode = DisplayMode::Joystick;
	std::string x_axis;
	std::string y_axis;
	std::array<std::string, 4> linear_axes{};
	std::string label;
	AxisTransform x_transform{};
	AxisTransform y_transform{};
	float opacity = 1.0F;
	uint32_t accent_color = 0x54D6C7FFU;
	int dot_size = 14;
	int safe_margin = 16;
	bool auto_hide = false;
	bool start_hidden = false;
	float hide_delay_seconds = 15.0F;
	float idle_elapsed = 0.0F;
	bool hidden = false;
	bool activity_initialized = false;
	std::unordered_map<std::string, float> activity_values;
	AxisSmoother x_smoother;
	AxisSmoother y_smoother;
	std::array<AxisSmoother, 4> linear_smoothers;
	std::array<bool, 4> linear_inverted{};
	std::array<float, 4> linear_values{};
	float x = 0.0F;
	float y = 0.0F;
	bool connected = false;
	float capture_remaining = 0.0F;
	AxisAutoRangeCapture x_capture;
	AxisAutoRangeCapture y_capture;
	gs_texture_t *texture = nullptr;
	std::vector<uint8_t> pixels;
	bool texture_dirty = true;
	uint64_t last_sequence = 0;
	float elapsed = 0.0F;
	bool axis_detection_active = false;
	std::string axis_detection_target;
	float axis_detection_remaining = 0.0F;
	std::unordered_map<std::string, float> axis_detection_minimums;
	std::unordered_map<std::string, float> axis_detection_maximums;
	std::string axis_detection_message;

	struct DetectedAxisTask {
		obs_weak_source_t *source = nullptr;
		std::string setting;
		std::string axis_id;
	};

	struct CalibrationTask {
		obs_weak_source_t *source = nullptr;
		AxisCalibration x_calibration;
		AxisCalibration y_calibration;
	};

	static void refresh_properties_task(void *param)
	{
		std::unique_ptr<obs_weak_source_t, void (*)(obs_weak_source_t *)> source(
			static_cast<obs_weak_source_t *>(param),
			[](obs_weak_source_t *value) {
				if (value)
					obs_weak_source_release(value);
			});
		if (!source)
			return;
		obs_source_t *target_source = obs_weak_source_get_source(source.get());
		if (!target_source)
			return;
		obs_source_update_properties(target_source);
		obs_source_release(target_source);
	}

	static void apply_detected_axis_task(void *param)
	{
		std::unique_ptr<DetectedAxisTask> task(static_cast<DetectedAxisTask *>(param));
		if (!task || !task->source)
			return;

		obs_source_t *target_source = obs_weak_source_get_source(task->source);
		obs_weak_source_release(task->source);
		task->source = nullptr;
		if (!target_source)
			return;

		obs_data_t *settings = obs_source_get_settings(target_source);
		if (settings) {
			obs_data_set_string(settings, task->setting.c_str(), task->axis_id.c_str());
			obs_source_update(target_source, settings);
			obs_data_release(settings);
			obs_source_update_properties(target_source);
		}
		obs_source_release(target_source);
	}

	static void apply_calibration_task(void *param)
	{
		std::unique_ptr<CalibrationTask> task(static_cast<CalibrationTask *>(param));
		if (!task || !task->source)
			return;

		obs_source_t *target_source = obs_weak_source_get_source(task->source);
		obs_weak_source_release(task->source);
		task->source = nullptr;
		if (!target_source)
			return;

		obs_data_t *settings = obs_source_get_settings(target_source);
		if (settings) {
			obs_data_set_double(settings, kXMin, task->x_calibration.minimum);
			obs_data_set_double(settings, kXCenter, task->x_calibration.center);
			obs_data_set_double(settings, kXMax, task->x_calibration.maximum);
			obs_data_set_double(settings, kYMin, task->y_calibration.minimum);
			obs_data_set_double(settings, kYCenter, task->y_calibration.center);
			obs_data_set_double(settings, kYMax, task->y_calibration.maximum);
			obs_source_update(target_source, settings);
			obs_data_release(settings);
		}
		obs_source_release(target_source);
	}

	[[nodiscard]] SelectionSnapshot selection_snapshot() const
	{
		std::lock_guard lock(mutex);
		return SelectionSnapshot{device_id, mode, x_axis, y_axis, linear_axes,
					 axis_detection_active, axis_detection_message,
					 auto_hide};
	}

	OverlaySource(obs_data_t *settings, obs_source_t *obs_source)
		: source(obs_source ? obs_source_get_weak_source(obs_source) : nullptr)
	{
		ensure_gdiplus();
		pixels.resize(static_cast<size_t>(kRenderWidth) * kRenderHeight * 4U);
		update(settings);
	}

	~OverlaySource()
	{
		gs_texture_t *texture_to_destroy = nullptr;
		obs_weak_source_t *source_to_release = nullptr;
		{
			std::lock_guard lock(mutex);
			texture_to_destroy = std::exchange(texture, nullptr);
			source_to_release = std::exchange(source, nullptr);
		}
		if (texture_to_destroy) {
			obs_enter_graphics();
			gs_texture_destroy(texture_to_destroy);
			obs_leave_graphics();
		}
		if (source_to_release)
			obs_weak_source_release(source_to_release);
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex);
		const char *new_device = obs_data_get_string(settings, kDevice);
		const char *new_x_axis = obs_data_get_string(settings, kXAxis);
		const char *new_y_axis = obs_data_get_string(settings, kYAxis);
		const std::string next_device = new_device ? new_device : "";
		const DisplayMode next_mode = read_display_mode(settings);
		const std::string next_x_axis = new_x_axis ? new_x_axis : "";
		const std::string next_y_axis = new_y_axis ? new_y_axis : "";
		std::array<std::string, 4> next_linear_axes;
		for (size_t index = 0; index < kLinearAxes.size(); ++index) {
			const char *axis = obs_data_get_string(settings, kLinearAxes[index]);
			next_linear_axes[index] = axis ? axis : "";
		}
		const bool binding_changed = next_device != device_id || next_mode != mode ||
					     next_x_axis != x_axis || next_y_axis != y_axis ||
					     next_linear_axes != linear_axes;
		const bool previous_auto_hide = auto_hide;
		const bool previous_start_hidden = start_hidden;
		device_id = next_device;
		mode = next_mode;
		x_axis = next_x_axis;
		y_axis = next_y_axis;
		linear_axes = std::move(next_linear_axes);
		if (binding_changed) {
			// Calibration samples belong to one concrete device/axis selection.
			// Never let a properties update blend samples from an earlier binding.
			capture_remaining = 0.0F;
			x_capture.reset();
			y_capture.reset();
			axis_detection_active = false;
			axis_detection_remaining = 0.0F;
			axis_detection_minimums.clear();
			axis_detection_maximums.clear();
		}
		const char *new_label = obs_data_get_string(settings, kLabel);
		label = (new_label && *new_label) ? new_label : default_label();

		x_transform.calibration = read_calibration(settings, kXMin, kXCenter, kXMax);
		y_transform.calibration = read_calibration(settings, kYMin, kYCenter, kYMax);
		x_transform.inverted = obs_data_get_bool(settings, kInvertX);
		y_transform.inverted = obs_data_get_bool(settings, kInvertY);
		for (size_t index = 0; index < kLinearInverts.size(); ++index)
			linear_inverted[index] = obs_data_get_bool(settings, kLinearInverts[index]);
		x_transform.deadzone = finite_clamp(static_cast<float>(obs_data_get_double(settings, kDeadzone)), 0.0F,
									    0.95F, 0.03F);
		y_transform.deadzone = x_transform.deadzone;
		x_transform.smoothing_ms = finite_clamp(static_cast<float>(obs_data_get_double(settings, kSmoothing)), 0.0F,
										1000.0F, 0.0F);
		y_transform.smoothing_ms = x_transform.smoothing_ms;
		accent_color = static_cast<uint32_t>(obs_data_get_int(settings, kColor));
		opacity = finite_clamp(static_cast<float>(obs_data_get_double(settings, kOpacity)), 0.0F, 1.0F, 1.0F);
		dot_size = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, kDotSize), 6, 40));
		safe_margin = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, kMargin), 6, 48));
		auto_hide = obs_data_get_bool(settings, kAutoHide);
		start_hidden = obs_data_get_bool(settings, kStartHidden);
		hide_delay_seconds = finite_clamp(
			static_cast<float>(obs_data_get_double(settings, kHideDelay)),
			1.0F, 300.0F, 15.0F);
		if (previous_auto_hide != auto_hide || previous_start_hidden != start_hidden) {
			hidden = auto_hide && start_hidden;
			idle_elapsed = 0.0F;
			activity_values.clear();
			activity_initialized = false;
		} else if (!auto_hide) {
			hidden = false;
			idle_elapsed = 0.0F;
		}
		x_smoother.reset();
		y_smoother.reset();
		for (auto &smoother : linear_smoothers)
			smoother.reset();
		linear_values.fill(0.0F);
		connected = false;
		x = 0.0F;
		y = 0.0F;
		last_sequence = 0;
		texture_dirty = true;
	}

	void begin_axis_detection(const char *button_name)
	{
		if (!button_name)
			return;

		std::string target_setting;
		if (std::strcmp(button_name, kDetectXAxis) == 0)
			target_setting = kXAxis;
		else if (std::strcmp(button_name, kDetectYAxis) == 0)
			target_setting = kYAxis;
		else {
			for (size_t index = 0; index < kDetectLinearAxes.size(); ++index) {
				if (std::strcmp(button_name, kDetectLinearAxes[index]) == 0) {
					target_setting = kLinearAxes[index];
					break;
				}
			}
		}
		if (target_setting.empty())
			return;

		std::string device_id_copy;
		{
			std::lock_guard lock(mutex);
			device_id_copy = device_id;
			axis_detection_active = true;
			axis_detection_target = std::move(target_setting);
			axis_detection_remaining = 3.0F;
			axis_detection_minimums.clear();
			axis_detection_maximums.clear();
			axis_detection_message = obs_module_text("MoveAxisHint");
		}

		const auto snapshot =
			input::shared_direct_input_service().snapshot_for(device_id_copy);
		if (snapshot && snapshot->connected) {
			std::lock_guard lock(mutex);
			for (const auto &[axis_id, value] : snapshot->axes) {
				if (axis_id.rfind("offset:", 0) == 0)
					continue;
				axis_detection_minimums[axis_id] = value;
				axis_detection_maximums[axis_id] = value;
			}
		}
	}

	void save_detected_axis(const std::string &setting, const std::string &axis_id)
	{
		auto *task = new DetectedAxisTask;
		{
			std::lock_guard lock(mutex);
			task->source = source;
			if (task->source)
				obs_weak_source_addref(task->source);
		}
		if (!task->source) {
			delete task;
			return;
		}
		task->setting = setting;
		task->axis_id = axis_id;
		obs_queue_task(OBS_TASK_UI, apply_detected_axis_task, task, false);
	}

	void request_properties_refresh()
	{
		obs_weak_source_t *source_copy = nullptr;
		{
			std::lock_guard lock(mutex);
			source_copy = source;
			if (source_copy)
				obs_weak_source_addref(source_copy);
		}
		if (source_copy)
			obs_queue_task(OBS_TASK_UI, refresh_properties_task, source_copy, false);
	}

	void save_calibration(const AxisCalibration &x_cal, const AxisCalibration &y_cal)
	{
		auto *task = new CalibrationTask;
		{
			std::lock_guard lock(mutex);
			x_transform.calibration = x_cal;
			y_transform.calibration = y_cal;
			task->source = source;
			if (task->source)
				obs_weak_source_addref(task->source);
			texture_dirty = true;
		}
		if (!task->source) {
			delete task;
			return;
		}
		task->x_calibration = x_cal;
		task->y_calibration = y_cal;
		obs_queue_task(OBS_TASK_UI, apply_calibration_task, task, false);
	}

	void set_current_center()
	{
		std::string device_id_copy;
		std::string x_axis_copy;
		std::string y_axis_copy;
		AxisCalibration x_calibration;
		AxisCalibration y_calibration;
		{
			std::lock_guard lock(mutex);
			capture_remaining = 0.0F;
			x_capture.reset();
			y_capture.reset();
			device_id_copy = device_id;
			x_axis_copy = x_axis;
			y_axis_copy = y_axis;
			x_calibration = x_transform.calibration;
			y_calibration = y_transform.calibration;
		}

		const auto &service = input::shared_direct_input_service();
		const auto x_value = service.axis_value(device_id_copy, x_axis_copy);
		const auto y_value = service.axis_value(device_id_copy, y_axis_copy);
		if (!x_value || !y_value)
			return;
		save_calibration(with_current_center(x_calibration, *x_value),
					 with_current_center(y_calibration, *y_value));
	}

	void begin_auto_range()
	{
		std::lock_guard lock(mutex);
		x_capture.reset();
		y_capture.reset();
		capture_remaining = 4.0F;
	}

	void reset_calibration()
	{
		{
			std::lock_guard lock(mutex);
			capture_remaining = 0.0F;
			x_capture.reset();
			y_capture.reset();
		}
		save_calibration(AxisCalibration{}, AxisCalibration{});
	}

	void tick(float seconds)
	{
		const float delta = std::clamp(seconds, 0.0F, 0.25F);
		const auto &service = input::shared_direct_input_service();
		std::optional<std::pair<AxisCalibration, AxisCalibration>> completed_calibration;
		std::optional<std::pair<std::string, std::string>> completed_detection;
		bool detection_finished = false;
		bool needs_redraw = false;
		bool input_activity = false;
		{
			std::lock_guard lock(mutex);
			elapsed = delta;
			const auto snapshot = service.snapshot_for(device_id);
			const bool was_connected = connected;
			const bool was_hidden = hidden;
			const float previous_x = x;
			const float previous_y = y;
			const auto previous_linear_values = linear_values;
			connected = snapshot.has_value() && snapshot->connected;
			std::unordered_map<std::string, float> observed_activity_values;
			auto observe_axis = [&](const std::string &axis_id) {
				if (axis_id.empty())
					return;
				if (snapshot) {
					const auto value = snapshot->axis_value(axis_id);
					if (value)
						observed_activity_values[axis_id] = *value;
				}
			};
			if (snapshot && snapshot->connected) {
				if (is_joystick_mode(mode)) {
					observe_axis(x_axis);
					observe_axis(y_axis);
				} else {
					const size_t required_axes =
						is_rudder_mode(mode) ? 3U : (is_wheel_mode(mode) ? 1U : 0U);
					for (size_t index = 0; index < kLinearAxes.size(); ++index) {
						if (linear_axes[index].empty())
							continue;
						if (index >= required_axes && !is_throttle_mode(mode))
							continue;
						observe_axis(linear_axes[index]);
					}
				}
			}
			const bool complete_activity_sample =
				snapshot && snapshot->connected &&
				!observed_activity_values.empty();
			if (complete_activity_sample) {
				const bool same_axes =
					activity_initialized &&
					activity_values.size() == observed_activity_values.size();
				if (same_axes) {
					for (const auto &[axis_id, value] : observed_activity_values) {
						const auto previous = activity_values.find(axis_id);
						if (previous != activity_values.end() &&
						    std::abs(value - previous->second) >= kActivityThreshold) {
							input_activity = true;
							break;
						}
					}
				}
				if (!same_axes || input_activity) {
					activity_values = observed_activity_values;
					activity_initialized = true;
				}
			} else {
				activity_values.clear();
				activity_initialized = false;
			}
			if (axis_detection_active) {
				if (snapshot && snapshot->connected) {
					for (const auto &[axis_id, value] : snapshot->axes) {
						if (axis_id.rfind("offset:", 0) == 0)
							continue;
						auto minimum = axis_detection_minimums.find(axis_id);
						auto maximum = axis_detection_maximums.find(axis_id);
						if (minimum == axis_detection_minimums.end()) {
							axis_detection_minimums.emplace(axis_id, value);
							axis_detection_maximums.emplace(axis_id, value);
						} else {
							minimum->second = std::min(minimum->second, value);
							maximum->second = std::max(maximum->second, value);
						}
					}
				}
				axis_detection_remaining =
					std::max(0.0F, axis_detection_remaining - delta);
				if (axis_detection_remaining <= 0.0F) {
					detection_finished = true;
					std::string best_axis;
					float best_span = 0.0F;
					for (const auto &[axis_id, minimum] : axis_detection_minimums) {
						const auto maximum = axis_detection_maximums.find(axis_id);
						if (maximum == axis_detection_maximums.end())
							continue;
						const float span = maximum->second - minimum;
						if (span > best_span) {
							best_span = span;
							best_axis = axis_id;
						}
					}
					const std::string target = axis_detection_target;
					axis_detection_active = false;
					axis_detection_target.clear();
					axis_detection_minimums.clear();
					axis_detection_maximums.clear();
					if (!best_axis.empty() && best_span >= 0.08F) {
						axis_detection_message = obs_module_text("AxisDetected");
						completed_detection =
							std::make_pair(target, std::move(best_axis));
					} else {
						axis_detection_message =
							obs_module_text("AxisDetectionFailed");
					}
				}
			}
			if (connected) {
				if (is_joystick_mode(mode)) {
					const auto x_value = snapshot->axis_value(x_axis);
					const auto y_value = snapshot->axis_value(y_axis);
					if (x_value && y_value) {
						const float px = transform_axis(*x_value, x_transform);
						const float py = transform_axis(*y_value, y_transform);
						if (capture_remaining > 0.0F) {
							x_capture.add_sample(*x_value);
							y_capture.add_sample(*y_value);
						}
						x = x_smoother.update(px, x_transform.smoothing_ms, delta);
						y = y_smoother.update(py, y_transform.smoothing_ms, delta);
						last_sequence = snapshot->sequence;
					} else {
						connected = false;
					}
				} else {
					const size_t required_axes =
						is_rudder_mode(mode) ? 3U : (is_wheel_mode(mode) ? 1U : 0U);
					bool has_required_axes = true;
					bool has_any_axis = false;
					for (size_t index = 0; index < kLinearAxes.size(); ++index) {
						if (linear_axes[index].empty()) {
							if (index < required_axes)
								has_required_axes = false;
							continue;
						}
						if (index >= required_axes && !is_throttle_mode(mode))
							continue;
						has_any_axis = true;
						const auto axis_value = snapshot->axis_value(linear_axes[index]);
						if (!axis_value) {
							has_required_axes = false;
							continue;
						}
						float value = apply_deadzone(*axis_value, x_transform.deadzone);
						if (linear_inverted[index])
							value = -value;
						linear_values[index] = linear_smoothers[index].update(
							value, x_transform.smoothing_ms, delta);
					}
					connected = has_required_axes && has_any_axis;
					if (connected)
						last_sequence = snapshot->sequence;
				}
			}
			if (!auto_hide) {
				hidden = false;
				idle_elapsed = 0.0F;
			} else if (!connected) {
				// Preserve the configured/current visibility while the input
				// service is starting or the controller is disconnected.
				idle_elapsed = 0.0F;
			} else if (input_activity) {
				hidden = false;
				idle_elapsed = 0.0F;
			} else {
				idle_elapsed = std::min(hide_delay_seconds, idle_elapsed + delta);
				if (!hidden && idle_elapsed >= hide_delay_seconds)
					hidden = true;
			}
			needs_redraw = texture_dirty || was_connected != connected ||
				       was_hidden != hidden ||
				       std::abs(previous_x - x) > 0.0001F ||
				       std::abs(previous_y - y) > 0.0001F ||
				       previous_linear_values != linear_values;
			if (capture_remaining > 0.0F) {
				capture_remaining = std::max(0.0F, capture_remaining - delta);
				if (capture_remaining == 0.0F) {
					const auto x_cal = x_capture.make_calibration(0.0F);
					const auto y_cal = y_capture.make_calibration(0.0F);
					if (x_cal && y_cal) {
						x_transform.calibration = *x_cal;
						y_transform.calibration = *y_cal;
						completed_calibration = std::make_pair(*x_cal, *y_cal);
					}
				}
			}
		}
		if (completed_calibration)
			save_calibration(completed_calibration->first, completed_calibration->second);
		if (completed_detection)
			save_detected_axis(completed_detection->first,
					   completed_detection->second);
		else if (detection_finished)
			request_properties_refresh();
		if (needs_redraw || completed_calibration)
			render_cpu();
	}

	void render_cpu()
	{
		std::lock_guard lock(mutex);
		if (pixels.size() != static_cast<size_t>(kRenderWidth) * kRenderHeight * 4U)
			pixels.resize(static_cast<size_t>(kRenderWidth) * kRenderHeight * 4U);
		if (hidden) {
			std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(0));
			texture_dirty = true;
			return;
		}
		if (!g_gdiplus_token) {
			std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(0));
			texture_dirty = true;
			return;
		}

		Bitmap bitmap(kRenderWidth, kRenderHeight, PixelFormat32bppARGB);
		Graphics graphics(&bitmap);
		graphics.SetSmoothingMode(SmoothingModeAntiAlias);
		graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
		graphics.ScaleTransform(static_cast<REAL>(kRenderScale),
					static_cast<REAL>(kRenderScale));
		graphics.Clear(Color(0, 0, 0, 0));

		const int inset = std::clamp(safe_margin / 2, 8, 24);
		const int label_height = 24;
		const Color accent = color_from_obs(accent_color);
		std::string text = label;
		if (!connected)
			text += kOfflineSeparator + std::string(obs_module_text("Offline"));
		const std::wstring wide_text = utf8_to_wide(text);
		FontFamily family(L"Microsoft YaHei UI");
		Font font(&family, 13.0F, FontStyleRegular, UnitPixel);
		SolidBrush label_brush(connected ? Color(232, 244, 245, 242) : Color(210, 136, 121, 225));
		const REAL status_x = static_cast<REAL>(inset + 4);
		const REAL status_y = static_cast<REAL>(inset + 12);
		SolidBrush status_brush(connected
			? Color(220, accent.GetRed(), accent.GetGreen(), accent.GetBlue())
			: Color(206, 196, 110, 101));
		graphics.FillEllipse(&status_brush, status_x - 2.5F, status_y - 2.5F, 5.0F, 5.0F);
		RectF label_rect(static_cast<REAL>(inset + 14), static_cast<REAL>(inset + 2),
					 static_cast<REAL>(kCanvasWidth - inset * 2 - 14),
					 static_cast<REAL>(label_height));
		StringFormat format;
		format.SetAlignment(StringAlignmentNear);
		format.SetLineAlignment(StringAlignmentNear);
		graphics.DrawString(wide_text.c_str(), static_cast<INT>(wide_text.size()), &font, label_rect, &format,
						&label_brush);

		const REAL left = static_cast<REAL>(inset);
		const REAL right = static_cast<REAL>(kCanvasWidth - inset);
		const REAL top = static_cast<REAL>(inset + label_height + 6);
		const REAL bottom = static_cast<REAL>(kCanvasHeight - inset);
		if (is_joystick_mode(mode)) {
			const REAL field_side = std::min(right - left, bottom - top);
			const RectF field(left + (right - left - field_side) * 0.5F,
					  top + (bottom - top - field_side) * 0.5F,
					  field_side, field_side);
			const REAL radius = static_cast<REAL>(std::clamp(dot_size, 6, 40)) * 0.5F;
			// Keep the dot and its subtle shadow inside the safe activity area.
			const REAL edge_padding = radius + 2.0F;
			const REAL activity_left = std::min(field.X + edge_padding,
							    field.GetRight() - edge_padding);
			const REAL activity_right = std::max(field.GetRight() - edge_padding,
							     field.X + edge_padding);
			const REAL activity_top = std::min(field.Y + edge_padding,
							   field.GetBottom() - edge_padding);
			const REAL activity_bottom = std::max(field.GetBottom() - edge_padding,
							      field.Y + edge_padding);
			const REAL px = activity_left + (clamp_unit(x) + 1.0F) * 0.5F *
							(activity_right - activity_left);
			// Positive physical Y points forward/up, so invert screen space here.
			const REAL py = activity_top + (1.0F - (clamp_unit(y) + 1.0F) * 0.5F) *
							(activity_bottom - activity_top);

			draw_joystick_frame(graphics, field, connected, accent);
			if (connected) {
				SolidBrush shadow(Color(126, 0, 0, 0));
				graphics.FillEllipse(&shadow, px - radius - 2.0F, py - radius - 2.0F,
						    (radius + 2.0F) * 2.0F, (radius + 2.0F) * 2.0F);
				SolidBrush dot_brush(accent);
				graphics.FillEllipse(&dot_brush, px - radius, py - radius,
						    radius * 2.0F, radius * 2.0F);
				Pen inner_pen(Color(228, 255, 255, 255), 1.0F);
				graphics.DrawEllipse(&inner_pen, px - radius + 1.0F, py - radius + 1.0F,
						    (radius - 1.0F) * 2.0F, (radius - 1.0F) * 2.0F);
			}
		} else if (is_throttle_mode(mode)) {
			const REAL gap = 10.0F;
			std::array<size_t, 4> bound_indices{};
			size_t bound_count = 0;
			for (size_t index = 0; index < kLinearAxes.size(); ++index) {
				if (!linear_axes[index].empty())
					bound_indices[bound_count++] = index;
			}
			const size_t track_count = std::max<size_t>(bound_count, 1U);
			const REAL width =
				(right - left - gap * static_cast<REAL>(track_count - 1)) /
				static_cast<REAL>(track_count);
			const REAL total_width =
				width * static_cast<REAL>(track_count) +
				gap * static_cast<REAL>(track_count - 1);
			const REAL start = left + (right - left - total_width) * 0.5F;
			for (size_t slot = 0; slot < track_count; ++slot) {
				const bool has_binding = bound_count != 0;
				const size_t index = has_binding ? bound_indices[slot] : 0;
				const RectF track(start + static_cast<REAL>(slot) * (width + gap),
						  top + 9.0F, width, bottom - top - 18.0F);
				if (has_binding && connected)
					draw_unipolar_vertical_axis(graphics, track, linear_values[index], accent);
				else
					draw_track(graphics, track, accent);
			}
		} else if (is_rudder_mode(mode)) {
			const REAL brake_width = 42.0F;
			const REAL brake_top = top + 8.0F;
			const REAL brake_bottom = top + (bottom - top) * 0.55F;
			const RectF left_brake(left + 30.0F, brake_top, brake_width, brake_bottom - brake_top);
			const RectF right_brake(right - 30.0F - brake_width, brake_top, brake_width,
					       brake_bottom - brake_top);
			if (connected) {
				draw_unipolar_vertical_axis(graphics, left_brake, linear_values[1], accent);
				draw_unipolar_vertical_axis(graphics, right_brake, linear_values[2], accent);
			} else {
				draw_track(graphics, left_brake, accent);
				draw_track(graphics, right_brake, accent);
			}
			const RectF rudder(left + 20.0F, bottom - 32.0F, right - left - 40.0F, 18.0F);
			if (connected)
				draw_centered_horizontal_axis(graphics, rudder, linear_values[0], accent);
			else
				draw_track(graphics, rudder, accent);
		} else {
			const RectF wheel(left + 20.0F, top + (bottom - top) * 0.42F,
					  right - left - 40.0F, 22.0F);
			if (connected)
				draw_centered_horizontal_axis(graphics, wheel, linear_values[0], accent);
			else
				draw_track(graphics, wheel, accent);
		}

		BitmapData bitmap_data{};
		const Rect lock_rect(0, 0, static_cast<INT>(kRenderWidth),
				    static_cast<INT>(kRenderHeight));
		if (bitmap.LockBits(&lock_rect, ImageLockModeRead, PixelFormat32bppARGB, &bitmap_data) == Ok) {
			const auto *src = static_cast<const uint8_t *>(bitmap_data.Scan0);
			const size_t row_bytes = static_cast<size_t>(kRenderWidth) * 4U;
			for (uint32_t row = 0; row < kRenderHeight; ++row) {
				const auto *row_src = src + static_cast<ptrdiff_t>(row) * bitmap_data.Stride;
				std::memcpy(pixels.data() + static_cast<size_t>(row) * row_bytes, row_src, row_bytes);
			}
			bitmap.UnlockBits(&bitmap_data);
			apply_overall_opacity(pixels, opacity);
		}
		texture_dirty = true;
	}

	void render(gs_effect_t *effect)
	{
		std::lock_guard lock(mutex);
		if (!effect)
			return;
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(gs_get_linear_srgb());
		if (!texture)
			texture = gs_texture_create(kRenderWidth, kRenderHeight, GS_BGRA, 1,
						    nullptr, GS_DYNAMIC);
		if (!texture) {
			gs_enable_framebuffer_srgb(previous_srgb);
			return;
		}
		if (texture_dirty && !pixels.empty()) {
			// GDI+ stores PixelFormat32bppARGB in memory as BGRA on Windows,
			// matching the OBS GS_BGRA texture format.
			gs_texture_set_image(texture, pixels.data(), kRenderWidth * 4U, false);
			texture_dirty = false;
		}
		gs_blend_state_push();
		// The GDI+ bitmap is premultiplied-alpha friendly on the OBS path;
		// match OBS's native text/image sources so translucent edges remain
		// smooth over arbitrary scene content.
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		if (gs_get_linear_srgb())
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_draw_sprite(texture, 0, kCanvasWidth, kCanvasHeight);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous_srgb);
	}
};

[[nodiscard]] const char *source_name(void *)
{
	return obs_module_text("AxisOverlay");
}

std::string populate_axis_list(obs_properties_t *props, obs_data_t *settings,
			       const char *property_name,
			       const std::string &device_id,
			       const std::string &selected,
			       const std::string &excluded,
			       bool write_selection);

void set_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kDevice, "");
	obs_data_set_default_int(settings, kMode,
				 static_cast<long long>(DisplayMode::Joystick));
	obs_data_set_default_string(settings, kXAxis, "offset:0");
	obs_data_set_default_string(settings, kYAxis, "offset:4");
	for (const char *axis_name : kLinearAxes)
		obs_data_set_default_string(settings, axis_name, "");
	for (const char *invert_name : kLinearInverts)
		obs_data_set_default_bool(settings, invert_name, false);
	obs_data_set_default_string(settings, kLabel, default_label());
	obs_data_set_default_bool(settings, kInvertX, false);
	obs_data_set_default_bool(settings, kInvertY, false);
	obs_data_set_default_double(settings, kXMin, -1.0);
	obs_data_set_default_double(settings, kXCenter, 0.0);
	obs_data_set_default_double(settings, kXMax, 1.0);
	obs_data_set_default_double(settings, kYMin, -1.0);
	obs_data_set_default_double(settings, kYCenter, 0.0);
	obs_data_set_default_double(settings, kYMax, 1.0);
	obs_data_set_default_double(settings, kDeadzone, 0.03);
	obs_data_set_default_double(settings, kSmoothing, 0.0);
	obs_data_set_default_int(settings, kColor, static_cast<long long>(0x54D6C7FFU));
	obs_data_set_default_double(settings, kOpacity, 1.0);
	obs_data_set_default_int(settings, kDotSize, 14);
	obs_data_set_default_int(settings, kMargin, 16);
	obs_data_set_default_bool(settings, kAutoHide, false);
	obs_data_set_default_bool(settings, kStartHidden, false);
	obs_data_set_default_double(settings, kHideDelay, 15.0);
}

void populate_linear_axis_lists(obs_properties_t *props, obs_data_t *settings,
				const std::string &device_id,
				const std::array<std::string, 4> &selected,
				DisplayMode mode, bool write_selection)
{
	const size_t auto_select_count =
		is_rudder_mode(mode) ? 3U : 1U;
	for (size_t index = 0; index < kLinearAxes.size(); ++index) {
		(void)populate_axis_list(props, settings, kLinearAxes[index],
					 device_id, selected[index], {},
					 write_selection && index < auto_select_count);
	}
}

std::string populate_axis_list(obs_properties_t *props, obs_data_t *settings,
										 const char *property_name, const std::string &device_id,
										 const std::string &selected, const std::string &excluded = {},
										 bool write_selection = true)
{
	obs_property_t *property = obs_properties_get(props, property_name);
	if (!property)
			return {};
	obs_property_list_clear(property);
	obs_property_list_add_string(property, obs_module_text("UnboundAxis"), "");
	const auto devices = input::shared_direct_input_service().devices();
	bool selected_valid = false;
	const char *first_axis = nullptr;
	const char *first_alternate = nullptr;
	std::string selected_resolved;
	for (const auto &device : devices) {
		if (device.id != device_id)
			continue;
		for (const auto &axis : device.axes) {
			if (!first_axis)
				first_axis = axis.id.c_str();
			const bool excluded_axis = axis.id == excluded ||
				(!axis.legacy_id.empty() && axis.legacy_id == excluded);
			if (!excluded_axis && !first_alternate)
				first_alternate = axis.id.c_str();
			obs_property_list_add_string(property, axis.display_name.c_str(), axis.id.c_str());
			if (!excluded_axis &&
			    (axis.id == selected || axis.legacy_id == selected)) {
				selected_valid = true;
				selected_resolved = axis.id;
			}
		}
	}
	std::string resolved;
	if (selected_valid)
		resolved = selected_resolved;
	else if (first_alternate)
		resolved = first_alternate;
	else if (first_axis)
		resolved = first_axis;
	if (write_selection && settings && !resolved.empty())
		obs_data_set_string(settings, property_name, resolved.c_str());
	return resolved;
}

[[nodiscard]] const char *device_status_description(const std::string &device_id)
{
	if (device_id.empty())
		return obs_module_text("SelectDevice");

	const auto snapshot = input::shared_direct_input_service().snapshot_for(device_id);
	return snapshot.has_value() && snapshot->connected ? obs_module_text("Connected")
										 : obs_module_text("Offline");
}

void set_advanced_visibility(obs_properties_t *props, bool has_device)
{
	// Device selection and display styling remain available without a bound
	// controller. Calibration controls only become useful after a device is
	// selected, which keeps the initial properties panel compact.
	const char *calibration[] = {kXAxis, kYAxis, kCalibrationGroup, kInvertX,
							 kInvertY, kDeadzone, kSmoothing, "set_center", "auto_range",
							 "reset_calibration", "calibration_hint"};
	for (const char *name : calibration) {
		if (obs_property_t *property = obs_properties_get(props, name))
			obs_property_set_visible(property, has_device);
	}

	for (const char *name : {kDetectXAxis, kDetectYAxis}) {
		if (obs_property_t *property = obs_properties_get(props, name))
			obs_property_set_visible(property, has_device);
	}

	if (obs_property_t *display_group = obs_properties_get(props, kDisplayGroup))
		obs_property_set_visible(display_group, true);
}

void set_mode_visibility(obs_properties_t *props, DisplayMode mode,
			 bool has_device)
{
	const bool joystick = has_device && is_joystick_mode(mode);
	const bool linear = has_device && !joystick;
	const bool throttle = linear && is_throttle_mode(mode);
	const bool rudder = linear && is_rudder_mode(mode);
	const bool wheel = linear && is_wheel_mode(mode);

	for (const char *property_name : {kXAxis, kYAxis, kDetectXAxis, kDetectYAxis,
					 kInvertX, kInvertY,
					 "set_center", "auto_range",
					 "reset_calibration", "calibration_hint"}) {
		if (obs_property_t *property = obs_properties_get(props, property_name))
			obs_property_set_visible(property, joystick);
	}
	if (obs_property_t *property = obs_properties_get(props, kCalibrationGroup))
		obs_property_set_visible(property, joystick);

	const bool linear_axes_visible = throttle || rudder || wheel;
	for (size_t index = 0; index < kLinearAxes.size(); ++index) {
		const bool visible = linear_axes_visible &&
			(throttle || (rudder && index < 3U) || (wheel && index == 0U));
		if (obs_property_t *property = obs_properties_get(props, kLinearAxes[index]))
			obs_property_set_visible(property, visible);
		if (obs_property_t *property = obs_properties_get(props, kLinearInverts[index]))
			obs_property_set_visible(property, visible);
		if (obs_property_t *property = obs_properties_get(props, kDetectLinearAxes[index]))
			obs_property_set_visible(property, visible);
	}
	if (obs_property_t *property = obs_properties_get(props, kLinearGroup))
		obs_property_set_visible(property, linear);
}

void update_detection_status(obs_properties_t *props,
				     const OverlaySource::SelectionSnapshot &selection)
{
	if (obs_property_t *status = obs_properties_get(props, kDetectionStatus)) {
		const char *text = selection.axis_detection_message.empty()
					   ? obs_module_text("DetectionIdle")
					   : selection.axis_detection_message.c_str();
		obs_property_set_description(status, text);
	}
}

bool device_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const char *device = obs_data_get_string(settings, kDevice);
	const std::string id = device ? device : "";
	const DisplayMode mode = read_display_mode(settings);
	const char *selected_x_value = obs_data_get_string(settings, kXAxis);
	const char *selected_y_value = obs_data_get_string(settings, kYAxis);
	const std::string selected_x = selected_x_value ? selected_x_value : "";
	const std::string selected_y = selected_y_value ? selected_y_value : "";
	const std::string resolved_x = populate_axis_list(props, settings, kXAxis, id, selected_x);
	(void)populate_axis_list(props, settings, kYAxis, id, selected_y, resolved_x);
	std::array<std::string, 4> selected_linear_axes;
	for (size_t index = 0; index < kLinearAxes.size(); ++index) {
		const char *axis = obs_data_get_string(settings, kLinearAxes[index]);
		selected_linear_axes[index] = axis ? axis : "";
	}
	populate_linear_axis_lists(props, settings, id, selected_linear_axes, mode, true);
	if (obs_property_t *status = obs_properties_get(props, kStatus))
		obs_property_set_description(status, device_status_description(id));
	if (obs_property_t *status = obs_properties_get(props, kDetectionStatus))
		obs_property_set_description(status, obs_module_text("DetectionIdle"));
	set_advanced_visibility(props, !id.empty());
	set_mode_visibility(props, mode, !id.empty());
	return true;
}

bool mode_modified(obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings)
{
	const char *device = obs_data_get_string(settings, kDevice);
	const std::string id = device ? device : "";
	const DisplayMode mode = read_display_mode(settings);
	std::array<std::string, 4> selected_linear_axes;
	for (size_t index = 0; index < kLinearAxes.size(); ++index) {
		const char *axis = obs_data_get_string(settings, kLinearAxes[index]);
		selected_linear_axes[index] = axis ? axis : "";
	}
	populate_linear_axis_lists(props, settings, id, selected_linear_axes, mode, true);
	if (obs_property_t *status = obs_properties_get(props, kDetectionStatus))
		obs_property_set_description(status, obs_module_text("DetectionIdle"));
	set_advanced_visibility(props, !id.empty());
	set_mode_visibility(props, mode, !id.empty());
	return true;
}

bool auto_hide_modified(obs_properties_t *props, obs_property_t *,
			obs_data_t *settings)
{
	const bool enabled = settings && obs_data_get_bool(settings, kAutoHide);
	if (obs_property_t *property = obs_properties_get(props, kStartHidden))
		obs_property_set_enabled(property, enabled);
	if (obs_property_t *property = obs_properties_get(props, kHideDelay))
		obs_property_set_enabled(property, enabled);
	return true;
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
	input::shared_direct_input_service().request_refresh();
	// The service refresh is asynchronous. Returning true asks OBS to rebuild
	// the properties view after the worker has had a chance to publish a new
	// device snapshot.
	return true;
}

bool refresh_devices_modified(void *, obs_properties_t *props, obs_property_t *,
			      obs_data_t *settings)
{
	const auto devices = input::shared_direct_input_service().devices();
	if (obs_property_t *device = obs_properties_get(props, kDevice)) {
		obs_property_list_clear(device);
		obs_property_list_add_string(device, obs_module_text("SelectDevice"), "");
		const char *selected = settings ? obs_data_get_string(settings, kDevice) : "";
		bool selected_present = false;
		for (const auto &descriptor : devices) {
			if (!descriptor.axes.empty())
				obs_property_list_add_string(device, descriptor.display_name.c_str(), descriptor.id.c_str());
			selected_present = selected_present || descriptor.id == (selected ? selected : "");
		}
		if (selected && *selected && !selected_present)
			obs_property_list_add_string(device, obs_module_text("BoundOffline"), selected);
	}
	return true;
}

bool detect_axis_clicked(obs_properties_t *, obs_property_t *property, void *data)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->begin_axis_detection(obs_property_name(property));
	// Starting detection only changes runtime state. Rebuilding the OBS
	// properties tree from inside this button callback can re-enter Qt's
	// property editor and crash OBS, so leave the existing controls intact.
	return false;
}

bool diagnostics_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (auto *overlay = static_cast<OverlaySource *>(data)) {
		const auto selection = overlay->selection_snapshot();
		input::shared_direct_input_service().log_device_diagnostics(selection.device_id);
	}
	return false;
}

bool center_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->set_current_center();
	return true;
}

bool autorange_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->begin_auto_range();
	return true;
}

bool reset_calibration_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->reset_calibration();
	return true;
}

obs_properties_t *source_properties(void *data)
{
	input::shared_direct_input_service().start();
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *device_group = obs_properties_create();
	obs_properties_t *mode_group = obs_properties_create();
	obs_properties_t *linear_group = obs_properties_create();
	const auto selection = data ? static_cast<OverlaySource *>(data)->selection_snapshot()
						   : OverlaySource::SelectionSnapshot{};
	const std::string selected_device = selection.device_id;
	obs_property_t *device = obs_properties_add_list(device_group, kDevice, obs_module_text("Device"),
										 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_t *status = obs_properties_add_text(device_group, kStatus,
									 device_status_description({}), OBS_TEXT_INFO);
	obs_properties_add_text(device_group, kDetectionStatus,
					selection.axis_detection_message.empty()
						? obs_module_text("DetectionIdle")
						: selection.axis_detection_message.c_str(),
					OBS_TEXT_INFO);
	const auto devices = input::shared_direct_input_service().devices();
	obs_property_list_add_string(device, obs_module_text("SelectDevice"), "");
	bool selected_device_present = false;
	for (const auto &descriptor : devices) {
		if (!descriptor.axes.empty()) {
			obs_property_list_add_string(device, descriptor.display_name.c_str(), descriptor.id.c_str());
			selected_device_present = selected_device_present || descriptor.id == selected_device;
		}
	}
	if (!selected_device.empty() && !selected_device_present) {
		// Keep a persisted binding visible while DirectInput is temporarily
		// disconnected; choosing another device replaces the stored ID.
		obs_property_list_add_string(device, obs_module_text("BoundOffline"), selected_device.c_str());
	}
	obs_property_set_modified_callback(device, device_modified);
	obs_property_t *refresh = obs_properties_add_button2(device_group, "refresh_devices", obs_module_text("Refresh"), refresh_clicked, nullptr);
	obs_property_set_modified_callback2(refresh, refresh_devices_modified, nullptr);
	obs_properties_add_button2(device_group, "diagnostics", obs_module_text("Diagnostics"),
				   diagnostics_clicked, data);

	obs_property_t *mode = obs_properties_add_list(
		mode_group, kMode, obs_module_text("DisplayMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("ModeJoystick"),
				  static_cast<long long>(DisplayMode::Joystick));
	obs_property_list_add_int(mode, obs_module_text("ModeThrottle"),
				  static_cast<long long>(DisplayMode::Throttle));
	obs_property_list_add_int(mode, obs_module_text("ModeRudder"),
				  static_cast<long long>(DisplayMode::Rudder));
	obs_property_list_add_int(mode, obs_module_text("ModeWheel"),
				  static_cast<long long>(DisplayMode::Wheel));
	obs_property_set_modified_callback(mode, mode_modified);

	obs_properties_add_list(device_group, kXAxis, obs_module_text("XAxis"), OBS_COMBO_TYPE_LIST,
								 OBS_COMBO_FORMAT_STRING);
	obs_properties_add_list(device_group, kYAxis, obs_module_text("YAxis"), OBS_COMBO_TYPE_LIST,
								 OBS_COMBO_FORMAT_STRING);
	obs_properties_add_button2(device_group, kDetectXAxis,
				   obs_module_text("DetectAxis"), detect_axis_clicked, data);
	obs_properties_add_button2(device_group, kDetectYAxis,
				   obs_module_text("DetectAxis"), detect_axis_clicked, data);
	const std::string selected_x = selection.x_axis;
	const std::string selected_y = selection.y_axis;
	const std::string resolved_x =
		populate_axis_list(device_group, nullptr, kXAxis, selected_device, selected_x, {}, false);
	(void)populate_axis_list(device_group, nullptr, kYAxis, selected_device, selected_y, resolved_x, false);
	for (size_t index = 0; index < kLinearAxes.size(); ++index) {
		obs_properties_add_list(linear_group, kLinearAxes[index],
					obs_module_text(kLinearLabels[index]), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
		obs_properties_add_button2(linear_group, kDetectLinearAxes[index],
					   obs_module_text("DetectAxis"),
					   detect_axis_clicked, data);
		obs_properties_add_bool(linear_group, kLinearInverts[index],
					obs_module_text("InvertLinearAxis"));
	}
	populate_linear_axis_lists(linear_group, nullptr, selected_device,
				   selection.linear_axes, selection.mode, false);

	obs_properties_t *calibration_group = obs_properties_create();
	obs_properties_add_bool(calibration_group, kInvertX, obs_module_text("InvertX"));
	obs_properties_add_bool(calibration_group, kInvertY, obs_module_text("InvertY"));
	obs_properties_add_float_slider(calibration_group, kDeadzone, obs_module_text("Deadzone"), 0.0, 0.5, 0.01);
	obs_properties_add_float_slider(calibration_group, kSmoothing, obs_module_text("Smoothing"), 0.0, 1000.0, 10.0);
	obs_properties_add_button2(calibration_group, "set_center", obs_module_text("SetCenter"), center_clicked, data);
	obs_properties_add_button2(calibration_group, "auto_range", obs_module_text("AutoRange"), autorange_clicked, data);
	obs_properties_add_button2(calibration_group, "reset_calibration", obs_module_text("ResetCalibration"),
							 reset_calibration_clicked, data);
	obs_properties_add_text(calibration_group, "calibration_hint", obs_module_text("CalibrationHint"), OBS_TEXT_INFO);

	obs_properties_t *display_group = obs_properties_create();
	obs_properties_add_text(display_group, kLabel, obs_module_text("Label"), OBS_TEXT_DEFAULT);
	obs_properties_add_color_alpha(display_group, kColor, obs_module_text("AccentColor"));
	obs_properties_add_float_slider(display_group, kOpacity, obs_module_text("OverallOpacity"), 0.0, 1.0, 0.05);
	obs_properties_add_int_slider(display_group, kDotSize, obs_module_text("DotSize"), 6, 40, 1);
	obs_properties_add_int_slider(display_group, kMargin, obs_module_text("SafeMargin"), 6, 48, 1);
	obs_properties_add_bool(display_group, kAutoHide, obs_module_text("AutoHide"));
	obs_properties_add_bool(display_group, kStartHidden, obs_module_text("StartHidden"));
	obs_properties_add_float_slider(display_group, kHideDelay,
					obs_module_text("HideDelay"), 1.0, 300.0, 1.0);
	if (obs_property_t *auto_hide = obs_properties_get(display_group, kAutoHide))
		obs_property_set_modified_callback(auto_hide, auto_hide_modified);
	if (obs_property_t *start_hidden = obs_properties_get(display_group, kStartHidden))
		obs_property_set_enabled(start_hidden, selection.auto_hide);
	if (obs_property_t *hide_delay = obs_properties_get(display_group, kHideDelay))
		obs_property_set_enabled(hide_delay, selection.auto_hide);

	obs_properties_add_group(props, kDeviceGroup, obs_module_text("DeviceGroup"), OBS_GROUP_NORMAL, device_group);
	obs_properties_add_group(props, kModeGroup, obs_module_text("ModeGroup"), OBS_GROUP_NORMAL,
				 mode_group);
	obs_properties_add_group(props, kLinearGroup, obs_module_text("LinearGroup"), OBS_GROUP_NORMAL,
				 linear_group);
	obs_properties_add_group(props, kCalibrationGroup, obs_module_text("CalibrationGroup"), OBS_GROUP_NORMAL,
						 calibration_group);
	obs_properties_add_group(props, kDisplayGroup, obs_module_text("DisplayGroup"), OBS_GROUP_NORMAL, display_group);

	obs_property_set_description(status, device_status_description(selected_device));
	update_detection_status(props, selection);
	set_advanced_visibility(props, !selected_device.empty());
	set_mode_visibility(props, selection.mode, !selected_device.empty());
	return props;
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	input::shared_direct_input_service().start();
	return new OverlaySource(settings, source);
}

void source_destroy(void *data)
{
	delete static_cast<OverlaySource *>(data);
}

void source_update(void *data, obs_data_t *settings)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->update(settings);
}

uint32_t source_width(void *)
{
	return kCanvasWidth;
}

uint32_t source_height(void *)
{
	return kCanvasHeight;
}

void source_tick(void *data, float seconds)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->tick(seconds);
}

void source_render(void *data, gs_effect_t *effect)
{
	if (auto *overlay = static_cast<OverlaySource *>(data))
		overlay->render(effect);
}

struct obs_source_info source_info = {
	.id = "flight_axis_overlay",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = source_name,
	.create = source_create,
	.destroy = source_destroy,
	.get_width = source_width,
	.get_height = source_height,
	.get_defaults = set_defaults,
	.get_properties = source_properties,
	.update = source_update,
	.video_tick = source_tick,
	.video_render = source_render,
};

} // namespace

void register_source()
{
	obs_register_source(&source_info);
}

} // namespace flight_axis::overlay
