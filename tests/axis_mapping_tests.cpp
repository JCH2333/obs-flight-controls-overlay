#include "flight_axis/axis_mapping.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void require(bool condition, std::string_view message)
{
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		std::exit(EXIT_FAILURE);
	}
}

bool nearly_equal(float left, float right, float tolerance = 0.0001F)
{
	return std::abs(left - right) <= tolerance;
}

void test_direct_input_normalization()
{
	require(nearly_equal(flight_axis::normalize_direct_input_value(-1000), -1.0F),
		"DirectInput negative endpoint");
	require(nearly_equal(flight_axis::normalize_direct_input_value(0), 0.0F),
		"DirectInput center");
	require(nearly_equal(flight_axis::normalize_direct_input_value(1000), 1.0F),
		"DirectInput positive endpoint");
	require(nearly_equal(flight_axis::normalize_direct_input_value(4000), 1.0F),
		"DirectInput values clamp high");
	require(nearly_equal(
			flight_axis::normalize_direct_input_value(32767, 0, 65535),
			0.0F, 0.0001F),
		"native DirectInput range normalization");
}

void test_calibration_and_inversion()
{
	const flight_axis::AxisCalibration calibration{-0.5F, 0.25F, 0.75F};
	require(flight_axis::is_valid_calibration(calibration),
		"asymmetric calibration is valid");
	require(nearly_equal(flight_axis::apply_calibration(-0.5F, calibration), -1.0F),
		"calibrated negative endpoint");
	require(nearly_equal(flight_axis::apply_calibration(0.25F, calibration), 0.0F),
		"calibrated center");
	require(nearly_equal(flight_axis::apply_calibration(0.75F, calibration), 1.0F),
		"calibrated positive endpoint");

	flight_axis::AxisTransform transform;
	transform.inverted = true;
	transform.deadzone = 0.0F;
	require(nearly_equal(flight_axis::transform_axis(0.6F, transform), -0.6F),
		"axis inversion");

	const flight_axis::AxisCalibration positive_one_sided{0.0F, 0.0F, 1.0F};
	require(flight_axis::is_valid_calibration(positive_one_sided),
		"positive one-sided calibration is valid");
	require(nearly_equal(
			flight_axis::apply_calibration(0.0F, positive_one_sided), 0.0F),
		"positive one-sided center");
	require(nearly_equal(
			flight_axis::apply_calibration(0.5F, positive_one_sided), 0.5F),
		"positive one-sided midpoint");
	require(nearly_equal(
			flight_axis::apply_calibration(1.0F, positive_one_sided), 1.0F),
		"positive one-sided endpoint");

	const flight_axis::AxisCalibration negative_one_sided{-1.0F, 0.0F, 0.0F};
	require(flight_axis::is_valid_calibration(negative_one_sided),
		"negative one-sided calibration is valid");
	require(nearly_equal(
			flight_axis::apply_calibration(-1.0F, negative_one_sided), -1.0F),
		"negative one-sided endpoint");
	require(nearly_equal(
			flight_axis::apply_calibration(-0.5F, negative_one_sided), -0.5F),
		"negative one-sided midpoint");
	require(nearly_equal(
			flight_axis::apply_calibration(0.0F, negative_one_sided), 0.0F),
		"negative one-sided center");
}

void test_deadzone_preserves_endpoints()
{
	require(nearly_equal(flight_axis::apply_deadzone(0.03F, 0.03F), 0.0F),
		"deadzone includes its boundary");
	require(nearly_equal(flight_axis::apply_deadzone(0.515F, 0.03F), 0.5F),
		"deadzone rescales remaining positive travel");
	require(nearly_equal(flight_axis::apply_deadzone(-0.515F, 0.03F), -0.5F),
		"deadzone rescales remaining negative travel");
	require(nearly_equal(flight_axis::apply_deadzone(1.0F, 0.03F), 1.0F),
		"deadzone keeps positive endpoint");
	require(nearly_equal(flight_axis::apply_deadzone(-1.0F, 0.03F), -1.0F),
		"deadzone keeps negative endpoint");
}

void test_smoothing()
{
	flight_axis::AxisSmoother smoother;
	require(nearly_equal(smoother.update(1.0F, 0.0F, 0.008F), 1.0F),
		"zero smoothing updates immediately");

	smoother.reset(0.0F);
	const float halfway = smoother.update(1.0F, 100.0F, 0.069314718F);
	require(nearly_equal(halfway, 0.5F, 0.001F),
		"exponential smoothing reaches half after one time constant ln(2)");
	require(nearly_equal(smoother.update(1.0F, 100.0F, 1.0F), 1.0F, 0.0001F),
		"smoothing converges on target");
}

void test_center_and_auto_range()
{
	const flight_axis::AxisCalibration defaults{};
	const auto valid_center =
		flight_axis::with_current_center(defaults, 0.35F);
	require(nearly_equal(valid_center.center, 0.35F),
		"set current center changes an interior center");
	const auto invalid_center =
		flight_axis::with_current_center(defaults, 1.0F);
	require(nearly_equal(invalid_center.center, 1.0F),
		"set current center accepts maximum endpoint");
	const auto minimum_center =
		flight_axis::with_current_center(defaults, -1.0F);
	require(nearly_equal(minimum_center.center, -1.0F),
		"set current center accepts minimum endpoint");

	flight_axis::AxisAutoRangeCapture capture;
	capture.add_sample(-0.80F);
	capture.add_sample(0.70F);
	const auto calibration = capture.make_calibration(0.05F);
	require(calibration.has_value(), "valid auto range creates calibration");
	require(nearly_equal(calibration->minimum, -0.80F),
		"auto range minimum");
	require(nearly_equal(calibration->maximum, 0.70F),
		"auto range maximum");

	capture.reset();
	capture.add_sample(0.20F);
	capture.add_sample(0.90F);
	const auto throttle_calibration = capture.make_calibration(0.0F);
	require(throttle_calibration.has_value(),
		"one-sided auto range creates calibration");
	require(nearly_equal(throttle_calibration->center, 0.20F),
		"one-sided auto range snaps center to minimum");
	require(nearly_equal(
			flight_axis::apply_calibration(0.20F, *throttle_calibration), 0.0F),
		"one-sided auto range minimum maps to center");
	require(nearly_equal(
			flight_axis::apply_calibration(0.90F, *throttle_calibration), 1.0F),
		"one-sided auto range maximum maps to endpoint");

	capture.reset();
	capture.add_sample(-0.90F);
	capture.add_sample(-0.20F);
	const auto reverse_one_sided = capture.make_calibration(0.0F);
	require(reverse_one_sided.has_value(),
		"reverse one-sided auto range creates calibration");
	require(nearly_equal(reverse_one_sided->center, -0.20F),
		"reverse one-sided auto range snaps center to maximum");
	require(nearly_equal(
			flight_axis::apply_calibration(-0.90F, *reverse_one_sided), -1.0F),
		"reverse one-sided auto range minimum maps to endpoint");
	require(nearly_equal(
			flight_axis::apply_calibration(-0.20F, *reverse_one_sided), 0.0F),
		"reverse one-sided auto range maximum maps to center");

	capture.reset();
	capture.add_sample(0.01F);
	capture.add_sample(0.02F);
	require(!capture.make_calibration(0.015F).has_value(),
		"narrow auto range is rejected");
}

void test_invalid_values_and_limits()
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	require(nearly_equal(flight_axis::clamp_unit(nan), 0.0F),
		"NaN clamps to neutral");
	require(nearly_equal(flight_axis::clamp_unit(infinity), 0.0F),
		"infinity clamps to neutral");

	const flight_axis::AxisCalibration invalid{0.5F, 0.0F, 1.0F};
	require(!flight_axis::is_valid_calibration(invalid),
		"inverted calibration range is invalid");
	require(nearly_equal(flight_axis::apply_calibration(0.8F, invalid), 0.8F),
		"invalid calibration falls back to standardized input");

	flight_axis::AxisTransform transform;
	transform.deadzone = 5.0F;
	require(nearly_equal(flight_axis::transform_axis(1.0F, transform), 1.0F),
		"oversized deadzone retains endpoint after limiting");
	require(nearly_equal(flight_axis::transform_axis(0.5F, transform), 0.0F),
		"oversized deadzone suppresses interior values");

	flight_axis::AxisSmoother smoother;
	smoother.reset(0.25F);
	require(nearly_equal(smoother.update(1.0F, 100.0F, 0.0F), 0.25F),
		"zero elapsed smoothing preserves previous position");
	require(nearly_equal(smoother.update(1.0F, nan, 0.5F), 1.0F),
		"non-finite smoothing updates immediately");
}

} // namespace

int main()
{
	test_direct_input_normalization();
	test_calibration_and_inversion();
	test_deadzone_preserves_endpoints();
	test_smoothing();
	test_center_and_auto_range();
	test_invalid_values_and_limits();
	std::cout << "axis_mapping_tests passed\n";
	return EXIT_SUCCESS;
}
