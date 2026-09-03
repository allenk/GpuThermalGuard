cmake_minimum_required(VERSION 3.24)

project(GpuThermalGuard VERSION 0.8.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "GpuThermalGuard currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(GTG_OSD_CLICK_THROUGH
    "Allow pointer input to pass through the OSD body (the top drag strip remains interactive)"
    OFF
)

add_library(gtg_build_options INTERFACE)
target_compile_definitions(gtg_build_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)
target_compile_options(gtg_build_options INTERFACE
    /W4
    /permissive-
    /Zc:__cplusplus
    /utf-8
    /EHsc
)

add_library(wtl INTERFACE)
target_include_directories(wtl SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wtl/Include"
)

add_library(nvml_headers INTERFACE)
target_include_directories(nvml_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvml/include"
)

add_library(gtg_core STATIC
    src/core/protection.cpp
    src/core/protection.hpp
    src/core/restore_policy.hpp
)
target_include_directories(gtg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gtg_core PUBLIC gtg_build_options)

add_library(gtg_nvml STATIC
    src/nvml/nvml_loader.cpp
    src/nvml/nvml_loader.hpp
)
target_include_directories(gtg_nvml PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gtg_nvml PUBLIC gtg_build_options nvml_headers)

add_library(gtg_telemetry STATIC
    src/telemetry/telemetry_history.cpp
    src/telemetry/telemetry_history.hpp
)
target_include_directories(gtg_telemetry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gtg_telemetry PUBLIC gtg_build_options)

add_library(gtg_snapshot STATIC
    src/snapshot/ui_snapshot.cpp
    src/snapshot/ui_snapshot.hpp
)
target_include_directories(gtg_snapshot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gtg_snapshot PUBLIC gtg_build_options gdiplus shell32)

add_executable(GpuThermalGuardProbe
    src/probe_main.cpp
)
target_link_libraries(GpuThermalGuardProbe PRIVATE gtg_nvml gtg_build_options)

add_executable(GpuThermalGuard WIN32
    src/app_main.cpp
    src/service/service_main.cpp
    src/service/service_main.hpp
    src/settings/settings.cpp
    src/settings/settings.hpp
    src/ipc/client.cpp
    src/ipc/client.hpp
    src/ipc/protocol.hpp
    src/logging/logger.cpp
    src/logging/logger.hpp
    src/tray/history_chart.cpp
    src/tray/history_chart.hpp
    src/tray/main_dialog.cpp
    src/tray/main_dialog.hpp
    src/tray/osd_overlay.cpp
    src/tray/osd_overlay.hpp
    src/resources/resource.h
    src/resources/GpuThermalGuard.rc
    src/resources/GpuThermalGuard.manifest
)
target_include_directories(GpuThermalGuard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(GpuThermalGuard PRIVATE
    gtg_core
    gtg_nvml
    gtg_telemetry
    gtg_snapshot
    gtg_build_options
    wtl
    shell32
    comctl32
    uxtheme
    gdiplus
)
if(GTG_OSD_CLICK_THROUGH)
    target_compile_definitions(GpuThermalGuard PRIVATE GTG_OSD_CLICK_THROUGH=1)
else()
    target_compile_definitions(GpuThermalGuard PRIVATE GTG_OSD_CLICK_THROUGH=0)
endif()
target_link_options(GpuThermalGuard PRIVATE /MANIFESTUAC:NO)

include(CTest)
if(BUILD_TESTING)
    add_executable(gtg_tests
        tests/test_main.cpp
        tests/wtl_compile_probe.cpp
    )
    target_link_libraries(gtg_tests PRIVATE gtg_core gtg_telemetry gtg_snapshot
        gtg_build_options wtl user32 gdi32)
    add_test(NAME gtg_tests COMMAND gtg_tests)
endif()

if(MSVC)
    target_link_options(GpuThermalGuardProbe PRIVATE
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
    )
    target_link_options(GpuThermalGuard PRIVATE
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
    )
endif()
