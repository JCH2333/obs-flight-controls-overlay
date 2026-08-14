#include "flight_axis/axis_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flight_axis {
namespace {

constexpr float kCalibrationEpsilon = 0.0001F;
constexpr float kMinimumCaptureSpan = 0.10F;

[[nodiscard]] float finite_or_zero(float value) noexcept
{
	return std::isfinite(value) ? value : 0.0F;
}

} // namespace

bool is_valid_calibration(const AxisCalibration &calibration) noexcept
{
	return std::isfinite(calibration.minimum) && std::isfinite(calibration.center) &&
	       std::isfinite(calibration.maximum) &&
	       calibration.minimum <= calibration.center &&
	       calibration.center <= calibration.maximum &&
	       calibration.maximum - calibration.minimum >= kCalibrationEpsilon;
}

float clamp_unit(float value) noexcept
{
	return std::clamp(finite_or_zero(value), -1.0F, 1.0F);
}

float normalize_direct_input_value(long value) noexcept
{
	return clamp_unit(static_cast<float>(value) / 1000.0F);
}

float normalize_direct_input_value(long value, long minimum, long maximum) noexcept
{
	if (minimum >= maximum)
		return normalize_direct_input_value(value);

	const double span = static_cast<double>(maximum) -
			    static_cast<double>(minimum);
	const double normalized =
		((static_cast<double>(value) - static_cast<double>(minimum)) / span) *
			2.0 -
		1.0;
	return clamp_unit(static_cast<float>(normalized));
}

float apply_calibration(float value, const AxisCalibration &calibration) noexcept
{
	value = clamp_unit(value);
	if (!is_valid_calibration(calibration))
		return value;

	if (value >= calibration.center) {
		const float span = calibration.maximum - calibration.center;
		// center == maximum is the negative/left one-sided case. The only
		// reachable value on this branch is the endpoint itself.
		if (span <= kCalibrationEpsilon)
			return 0.0F;
		return clamp_unit((value - calibration.center) / span);
	}

	const float span = calibration.center - calibration.minimum;
	// center == minimum is the positive/right one-sided case. Values below
	// minimum are unreachable after input clamping, but remain well-defined.
	if (span <= kCalibrationEpsilon)
		return 0.0F;
	return clamp_unit((value - calibration.center) / span);
}

float apply_deadzone(float value, float deadzone) noexcept
{
	value = clamp_unit(value);
	if (!std::isfinite(deadzone))
		deadzone = 0.0F;

	deadzone = std::clamp(deadzone, 0.0F, 0.999F);
	const float magnitude = std::abs(value);
	if (magnitude <= deadzone)
		return 0.0F;

	// Rescale the remaining travel so both physical endpoints stay at +/- 1.
	const float adjusted = (magnitude - deadzone) / (1.0F - deadzone);
	return std::copysign(clamp_unit(adjusted), value);
}

float transform_axis(float value, const AxisTransform &transform) noexcept
{
	float result = apply_calibration(value, transform.calibration);
	if (transform.inverted)
		result = -result;
	return apply_deadzone(result, transform.deadzone);
}

AxisCalibration with_current_center(AxisCalibration calibration,
				    float current_value) noexcept
{
	if (!is_valid_calibration(calibration))
		return calibration;

	current_value = clamp_unit(current_value);
	if (current_value < calibration.minimum - kCalibrationEpsilon ||
	    current_value > calibration.maximum + kCalibrationEpsilon)
		return calibration;

	calibration.center = std::clamp(current_value, calibration.minimum,
					calibration.maximum);
	return calibration;
}

void AxisSmoother::reset(std::optional<float> value) noexcept
{
	value_ = value.has_value() ? std::optional<float>(clamp_unit(*value))
				   : std::nullopt;
}

float AxisSmoother::update(float target, float smoothing_ms,
			   float elapsed_seconds) noexcept
{
	target = clamp_unit(target);
	if (!value_.has_value()) {
		value_ = target;
		return target;
	}

	if (!std::isfinite(smoothing_ms) || smoothing_ms <= 0.0F) {
		value_ = target;
		return target;
	}

	if (!std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0F)
		return *value_;

	const float time_constant = smoothing_ms / 1000.0F;
	const float alpha = 1.0F - std::exp(-elapsed_seconds / time_constant);
	*value_ += (target - *value_) * std::clamp(alpha, 0.0F, 1.0F);
	*value_ = clamp_unit(*value_);
	return *value_;
}

std::optional<float> AxisSmoother::value() const noexcept
{
	return value_;
}

void AxisAutoRangeCapture::reset() noexcept
{
	sample_count_ = 0;
	minimum_ = 1.0F;
	maximum_ = -1.0F;
}

void AxisAutoRangeCapture::add_sample(float normalized_value) noexcept
{
	const float value = clamp_unit(normalized_value);
	minimum_ = std::min(minimum_, value);
	maximum_ = std::max(maximum_, value);
	++sample_count_;
}

bool AxisAutoRangeCapture::has_samples() const noexcept
{
	return sample_count_ != 0;
}

std::size_t AxisAutoRangeCapture::sample_count() const noexcept
{
	return sample_count_;
}

float AxisAutoRangeCapture::minimum() const noexcept
{
	return has_samples() ? minimum_ : 0.0F;
}

float AxisAutoRangeCapture::maximum() const noexcept
{
	return has_samples() ? maximum_ : 0.0F;
}

std::optional<AxisCalibration>
AxisAutoRangeCapture::make_calibration(float center_value) const noexcept
{
	if (!has_samples() || maximum_ - minimum_ < kMinimumCaptureSpan)
		return std::nullopt;

	// A one-sided control often has its logical center outside the observed
	// capture interval (for example, a throttle resting at its lower stop).
	// Snap that requested center to the nearest captured endpoint.
	float center = clamp_unit(center_value);
	center = std::clamp(center, minimum_, maximum_);
	const AxisCalibration calibration{
		.minimum = minimum_,
		.center = center,
		.maximum = maximum_,
	};

	if (!is_valid_calibration(calibration))
		return std::nullopt;

	return calibration;
}

} // namespace flight_axis
