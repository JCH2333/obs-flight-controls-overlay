cmake_minimum_required(VERSION 3.28)

project(obs-flight-axis-overlay VERSION 1.0.1 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "obs-flight-axis-overlay supports only 64-bit Windows builds.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ObsFlightAxisOverlay.cmake)
obs_flight_axis_overlay_prepare_obs_packages()

# A locally staged OBS development tree can be supplied explicitly with
# -DOBS_SDK_PREFIX or -DOBS_ROOT. The runtime OBS install is not an SDK. For
# this machine-only build, -DOBS_USE_RUNTIME_IMPORT_LIB=ON enables a fallback
# that links against the installed OBS 32.2.1 obs.dll using matching headers.
# Skip the package probe in fallback mode: a failed libobs package may leave a
# NOTFOUND imported target behind, which would shadow the generated target.
if(OBS_USE_RUNTIME_IMPORT_LIB)
  obs_flight_axis_overlay_setup_runtime_import()
else()
  find_package(libobs QUIET)
endif()
if(NOT TARGET OBS::libobs)
  message(FATAL_ERROR "OBS::libobs was not found. Provide OBS_SDK_PREFIX/OBS_ROOT or enable OBS_USE_RUNTIME_IMPORT_LIB.")
endif()

add_library(obs-flight-axis-overlay MODULE
  src/plugin-main.cpp
  src/input/axis_mapping.cpp
  src/input/direct_input_service.cpp
  src/overlay/overlay_source.cpp
)

target_include_directories(obs-flight-axis-overlay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(obs-flight-axis-overlay PRIVATE OBS::libobs)
if(DEFINED OBS_FLIGHT_AXIS_OVERLAY_RUNTIME_IMPORT_TARGET
   AND TARGET "${OBS_FLIGHT_AXIS_OVERLAY_RUNTIME_IMPORT_TARGET}")
  add_dependencies(
    obs-flight-axis-overlay
    "${OBS_FLIGHT_AXIS_OVERLAY_RUNTIME_IMPORT_TARGET}"
  )
endif()
obs_flight_axis_overlay_configure_target(obs-flight-axis-overlay)

# OBS module data is installed below data/obs-plugins/<module-id>.
obs_flight_axis_overlay_add_deploy_target(
  obs-flight-axis-overlay
  DATA_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-plugins/obs-flight-axis-overlay"
)

install(
  TARGETS obs-flight-axis-overlay
  RUNTIME DESTINATION "obs-plugins/64bit"
  LIBRARY DESTINATION "obs-plugins/64bit"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-plugins/obs-flight-axis-overlay"
  DESTINATION "data/obs-plugins"
)
install(
  PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Install-OBSFlightControlsOverlay.ps1"
  DESTINATION "."
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Install-OBSFlightControlsOverlay.cmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  DESTINATION "."
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME "obs-flight-controls-overlay-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)

add_custom_target(
  package-release
  COMMAND
    "${CMAKE_CPACK_COMMAND}"
    --config
    "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
    -G
    ZIP
    -C
    "$<CONFIG>"
  DEPENDS obs-flight-axis-overlay
  USES_TERMINAL
  VERBATIM
  COMMENT "Packaging the Windows x64 OBS plugin release archive"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(axis_mapping_tests tests/axis_mapping_tests.cpp src/input/axis_mapping.cpp)
  target_include_directories(axis_mapping_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(axis_mapping_tests PRIVATE cxx_std_20)
  add_test(NAME axis_mapping_tests COMMAND axis_mapping_tests)
endif()
