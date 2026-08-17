# Build and local-deployment helpers for obs-flight-axis-overlay.
#
# The main CMakeLists.txt owns dependency acquisition.  Call
# obs_flight_axis_overlay_prepare_obs_packages() before find_package(libobs)
# to make a locally built OBS SDK take precedence over the default .deps path.

include_guard(GLOBAL)

set(
  OBS_SDK_PREFIX
  ""
  CACHE PATH
  "Installed OBS SDK prefix containing the libobs CMake package (optional)"
)
set(
  OBS_ROOT
  ""
  CACHE PATH
  "OBS source, build, or install root used to locate a locally built SDK (optional)"
)
set(
  OBS_SOURCE_ROOT
  ""
  CACHE PATH
  "OBS 32.2.1 source root used by the runtime import-library fallback (optional)"
)
set(
  OBS_RUNTIME_ROOT
  ""
  CACHE PATH
  "OBS Studio runtime installation root used by the import-library fallback (optional)"
)
set(
  OBS_RUNTIME_DLL
  ""
  CACHE FILEPATH
  "Path to the OBS runtime obs.dll used by the import-library fallback (optional)"
)
set(
  OBS_RUNTIME_IMPORT_LIB
  ""
  CACHE FILEPATH
  "Existing x64 OBS import library to use instead of generating one (optional)"
)
option(
  OBS_USE_RUNTIME_IMPORT_LIB
  "Use the local OBS runtime import-library fallback when no OBS SDK package is available"
  OFF
)
set(
  OBS_LOCAL_INSTALL_ROOT
  "C:/Program Files/obs-studio"
  CACHE PATH
  "Local OBS Studio runtime installation used by the deploy-local target"
)
option(
  OBS_FLIGHT_AXIS_OVERLAY_WARNINGS_AS_ERRORS
  "Treat compiler warnings from this plugin as errors"
  OFF
)

function(_obs_flight_axis_overlay_prepend_prefix prefix prefixes_variable)
  if(NOT prefix OR NOT IS_DIRECTORY "${prefix}")
    return()
  endif()

  set(_prefixes "${${prefixes_variable}}")
  get_filename_component(_prefix "${prefix}" ABSOLUTE)
  list(FIND _prefixes "${_prefix}" _prefix_index)
  if(_prefix_index EQUAL -1)
    list(APPEND _prefixes "${_prefix}")
  endif()
  set(${prefixes_variable} "${_prefixes}" PARENT_SCOPE)
endfunction()

function(obs_flight_axis_overlay_prepare_obs_packages)
  # OBS_SDK_PREFIX is the explicit, highest-priority override.  OBS_ROOT is
  # intentionally permissive so a source checkout, build folder, or installed
  # prefix can be supplied without changing the project files.
  set(_obs_flight_axis_overlay_prefixes "")
  _obs_flight_axis_overlay_prepend_prefix(
    "${OBS_SDK_PREFIX}"
    _obs_flight_axis_overlay_prefixes
  )

  if(OBS_ROOT)
    _obs_flight_axis_overlay_prepend_prefix("${OBS_ROOT}" _obs_flight_axis_overlay_prefixes)
    _obs_flight_axis_overlay_prepend_prefix("${OBS_ROOT}/build" _obs_flight_axis_overlay_prefixes)
    _obs_flight_axis_overlay_prepend_prefix(
      "${OBS_ROOT}/build/install"
      _obs_flight_axis_overlay_prefixes
    )
    _obs_flight_axis_overlay_prepend_prefix("${OBS_ROOT}/install" _obs_flight_axis_overlay_prefixes)
    _obs_flight_axis_overlay_prepend_prefix(
      "${OBS_ROOT}/cmake-prefix"
      _obs_flight_axis_overlay_prefixes
    )
  endif()

  if(_obs_flight_axis_overlay_prefixes)
    list(PREPEND CMAKE_PREFIX_PATH ${_obs_flight_axis_overlay_prefixes})
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    message(
      STATUS
      "obs-flight-axis-overlay: prioritizing local OBS package prefixes: "
      "${_obs_flight_axis_overlay_prefixes}"
    )
  endif()
endfunction()

function(_obs_flight_axis_overlay_find_source_root result_variable)
  set(_candidates "")
  if(OBS_SOURCE_ROOT)
    list(APPEND _candidates "${OBS_SOURCE_ROOT}")
  endif()
  if(OBS_ROOT)
    list(APPEND _candidates "${OBS_ROOT}")
  endif()
  # Keep the local checkout convenient without making it part of the normal
  # SDK search path.  The directory is ignored by the repository.
  list(APPEND _candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/.tooling/obs-studio-32.2.1-src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio-32.2.1-src"
  )

  foreach(_candidate IN LISTS _candidates)
    if(EXISTS "${_candidate}/libobs/obs.h")
      get_filename_component(_absolute_candidate "${_candidate}" ABSOLUTE)
      set(${result_variable} "${_absolute_candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(${result_variable} "" PARENT_SCOPE)
endfunction()

function(_obs_flight_axis_overlay_find_runtime_dll result_variable)
  set(_runtime_dll "${OBS_RUNTIME_DLL}")
  if(NOT _runtime_dll AND OBS_RUNTIME_ROOT)
    set(_runtime_dll "${OBS_RUNTIME_ROOT}/bin/64bit/obs.dll")
  endif()
  if(NOT _runtime_dll AND OBS_LOCAL_INSTALL_ROOT)
    set(_runtime_dll "${OBS_LOCAL_INSTALL_ROOT}/bin/64bit/obs.dll")
  endif()

  if(_runtime_dll AND EXISTS "${_runtime_dll}")
    get_filename_component(_absolute_runtime_dll "${_runtime_dll}" ABSOLUTE)
    set(${result_variable} "${_absolute_runtime_dll}" PARENT_SCOPE)
  else()
    set(${result_variable} "" PARENT_SCOPE)
  endif()
endfunction()

function(obs_flight_axis_overlay_setup_runtime_import)
  if(TARGET OBS::libobs)
    return()
  endif()
  if(NOT OBS_USE_RUNTIME_IMPORT_LIB)
    return()
  endif()

  _obs_flight_axis_overlay_find_source_root(_obs_source_root)
  if(NOT _obs_source_root)
    message(
      FATAL_ERROR
      "OBS runtime fallback needs OBS_SOURCE_ROOT (or OBS_ROOT) containing libobs/obs.h."
    )
  endif()

  set(_obs_headers "${_obs_source_root}/libobs")
  if(NOT EXISTS "${_obs_headers}/obs.h")
    message(FATAL_ERROR "OBS headers were not found below: ${_obs_headers}")
  endif()

  set(_import_root "${CMAKE_BINARY_DIR}/obs-runtime-import")
  set(_generated_include "${_import_root}/include")
  set(_generated_config "${_generated_include}/obsconfig.h")
  file(MAKE_DIRECTORY "${_generated_include}")

  # Prefer the matching generated OBS config from the source checkout.  A
  # source archive without one still gets a minimal config that matches the
  # installed OBS layout used by this plugin.
  if(EXISTS "${_obs_headers}/obsconfig.h")
    configure_file("${_obs_headers}/obsconfig.h" "${_generated_config}" COPYONLY)
  elseif(NOT EXISTS "${_generated_config}")
    file(WRITE "${_generated_config}"
      "#pragma once\n"
      "#define OBS_DATA_PATH \"data\"\n"
      "#define OBS_PLUGIN_PATH \"obs-plugins/64bit\"\n"
      "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n"
      "#define OBS_RELEASE_CANDIDATE 0\n"
      "#define OBS_BETA 0\n"
    )
  endif()

  if(OBS_RUNTIME_IMPORT_LIB)
    if(NOT EXISTS "${OBS_RUNTIME_IMPORT_LIB}")
      message(FATAL_ERROR "OBS_RUNTIME_IMPORT_LIB does not exist: ${OBS_RUNTIME_IMPORT_LIB}")
    endif()
    get_filename_component(_import_lib "${OBS_RUNTIME_IMPORT_LIB}" ABSOLUTE)
    set(_import_target "")
  else()
    _obs_flight_axis_overlay_find_runtime_dll(_obs_runtime_dll)
    if(NOT _obs_runtime_dll)
      message(
        FATAL_ERROR
        "Generating the OBS import library needs OBS_RUNTIME_DLL or "
        "OBS_RUNTIME_ROOT/bin/64bit/obs.dll. Alternatively set OBS_RUNTIME_IMPORT_LIB "
        "to an existing x64 import library."
      )
    endif()
    find_program(_obs_gendef NAMES gendef gendef.exe)
    get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(
      _obs_lib_tool
      NAMES lib.exe llvm-lib.exe
      HINTS "${_compiler_bin}"
    )
    if(NOT _obs_gendef OR NOT _obs_lib_tool)
      message(
        FATAL_ERROR
        "Generating the OBS import library requires gendef and MSVC lib.exe. "
        "Set OBS_RUNTIME_IMPORT_LIB to an existing x64 import library instead."
      )
    endif()

    set(_def_file "${_import_root}/obs.def")
    set(_import_lib "${_import_root}/obs.lib")
    add_custom_command(
      OUTPUT "${_import_lib}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_import_root}"
      COMMAND "${_obs_gendef}" "${_obs_runtime_dll}"
      COMMAND "${_obs_lib_tool}" /nologo "/def:${_def_file}" /machine:x64 "/out:${_import_lib}"
      WORKING_DIRECTORY "${_import_root}"
      DEPENDS "${_obs_runtime_dll}"
      VERBATIM
      COMMENT "Generating the x64 OBS runtime import library"
    )
    set(_import_target "obs-flight-axis-overlay-obs-runtime-import")
    add_custom_target("${_import_target}" DEPENDS "${_import_lib}")
  endif()

  if(NOT TARGET OBS::libobs)
    # The generated .lib is an import library, but treating it as a static
    # imported target keeps CMake from adding the runtime DLL itself to the
    # linker command (which MSVC rejects as LNK1107).
    add_library(OBS::libobs STATIC IMPORTED GLOBAL)
    set_target_properties(
      OBS::libobs
      PROPERTIES
        IMPORTED_LOCATION "${_import_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_generated_include};${_obs_headers}"
        INTERFACE_COMPILE_DEFINITIONS "HAVE_OBSCONFIG_H"
    )
  endif()

  set(OBS_FLIGHT_AXIS_OVERLAY_RUNTIME_IMPORT_TARGET "${_import_target}" PARENT_SCOPE)
  set(OBS_FLIGHT_AXIS_OVERLAY_RUNTIME_IMPORT_LIB "${_import_lib}" PARENT_SCOPE)
  message(STATUS "obs-flight-axis-overlay: using OBS runtime import fallback")
  message(STATUS "  OBS headers: ${_obs_headers}")
  if(_obs_runtime_dll)
    message(STATUS "  OBS runtime: ${_obs_runtime_dll}")
  else()
    message(STATUS "  OBS runtime: not required (prebuilt import library supplied)")
  endif()
  message(STATUS "  OBS import:  ${_import_lib}")
endfunction()

function(obs_flight_axis_overlay_configure_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "obs_flight_axis_overlay_configure_target: unknown target '${target}'")
  endif()

  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "obs-flight-axis-overlay supports only 64-bit Windows builds")
  endif()

  target_compile_features("${target}" PRIVATE cxx_std_20)
  target_compile_definitions(
    "${target}"
    PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  )
  target_link_libraries("${target}" PRIVATE dinput8 dxguid gdiplus)

  if(MSVC)
    # A release ZIP must work on a clean Windows installation. Statically
    # linking the MSVC runtime removes the VCRUNTIME/MSVCP DLL dependency;
    # obs.dll remains intentionally supplied by the user's OBS installation.
    set_property(
      TARGET "${target}"
      PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options("${target}" PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(OBS_FLIGHT_AXIS_OVERLAY_WARNINGS_AS_ERRORS)
      target_compile_options("${target}" PRIVATE /WX)
    endif()
  endif()

  # OBS discovers native modules by DLL name.  Explicitly clear the normal
  # CMake module prefix so output is obs-flight-axis-overlay.dll, not lib*.dll.
  set_target_properties(
    "${target}"
    PROPERTIES
      PREFIX ""
      OUTPUT_NAME "obs-flight-axis-overlay"
  )
endfunction()

function(obs_flight_axis_overlay_add_deploy_target target)
  set(options)
  set(one_value_args DATA_DIRECTORY)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

  if(NOT TARGET "${target}")
    message(FATAL_ERROR "obs_flight_axis_overlay_add_deploy_target: unknown target '${target}'")
  endif()

  if(TARGET deploy-local)
    message(FATAL_ERROR "Only one deploy-local target may be added to this project")
  endif()

  if(NOT ARG_DATA_DIRECTORY)
    set(ARG_DATA_DIRECTORY "${CMAKE_SOURCE_DIR}/data/obs-plugins/obs-flight-axis-overlay")
  endif()
  get_filename_component(ARG_DATA_DIRECTORY "${ARG_DATA_DIRECTORY}" ABSOLUTE)

  set(_deploy_script "${CMAKE_SOURCE_DIR}/scripts/deploy-local.ps1")
  if(NOT EXISTS "${_deploy_script}")
    message(FATAL_ERROR "Local deployment script was not found: ${_deploy_script}")
  endif()

  find_program(
    OBS_FLIGHT_AXIS_OVERLAY_POWERSHELL
    NAMES pwsh powershell
    REQUIRED
  )

  add_custom_target(
    deploy-local
    COMMAND
      "${OBS_FLIGHT_AXIS_OVERLAY_POWERSHELL}"
      -NoLogo
      -NoProfile
      -ExecutionPolicy
      Bypass
      -File
      "${_deploy_script}"
      -PluginDll
      "$<TARGET_FILE:${target}>"
      -PluginData
      "${ARG_DATA_DIRECTORY}"
      -ObsRoot
      "${OBS_LOCAL_INSTALL_ROOT}"
      -Configuration
      "$<CONFIG>"
    DEPENDS "${target}"
    USES_TERMINAL
    VERBATIM
    COMMENT "Deploying obs-flight-axis-overlay to the local OBS installation"
  )
endfunction()
