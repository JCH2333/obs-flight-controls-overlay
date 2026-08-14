#pragma once

#include <cstddef>
#include <optional>

namespace flight_axis {

// Calibration values use the normalized DirectInput domain: [-1.0, 1.0].
// The center may equal minimum or maximum for one-sided controls such as
// throttles, sliders, and pedals.
struct AxisCalibration {
	float minimum = -1.0F;
	float center = 0.0F;
	float maximum = 1.0F;
};

struct AxisTransform {
	AxisCalibration calibration{};
	bool inverted = false;
	float deadzone = 0.03F;
	float smoothing_ms = 0.0F;
};

[[nodiscard]] bool is_valid_calibration(const AxisCalibration &calibration) noexcept;
[[nodiscard]] float clamp_unit(float value) noexcept;
[[nodiscard]] float normalize_direct_input_value(long value) noexcept;
[[nodiscard]] float normalize_direct_input_value(long value, long minimum,
						  long maximum) noexcept;
[[nodiscard]] float apply_calibration(float value, const AxisCalibration &calibration) noexcept;
[[nodiscard]] float apply_deadzone(float value, float deadzone) noexcept;
[[nodiscard]] float transform_axis(float value, const AxisTransform &transform) noexcept;
[[nodiscard]] AxisCalibration with_current_center(AxisCalibration calibration,
						   float current_value) noexcept;

class AxisSmoother {
public:
	void reset(std::optional<float> value = std::nullopt) noexcept;
	[[nodiscard]] float update(float target, float smoothing_ms,
				   float elapsed_seconds) noexcept;
	[[nodiscard]] std::optional<float> value() const noexcept;

private:
	std::optional<float> value_;
};

class AxisAutoRangeCapture {
public:
	void reset() noexcept;
	void add_sample(float normalized_value) noexcept;

	[[nodiscard]] bool has_samples() const noexcept;
	[[nodiscard]] std::size_t sample_count() const noexcept;
	[[nodiscard]] float minimum() const noexcept;
	[[nodiscard]] float maximum() const noexcept;
	[[nodiscard]] std::optional<AxisCalibration>
	make_calibration(float center_value) const noexcept;

private:
	std::size_t sample_count_ = 0;
	float minimum_ = 1.0F;
	float maximum_ = -1.0F;
};

} // namespace flight_axis
