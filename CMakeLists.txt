cmake_minimum_required(VERSION 3.24)

# GTG_VERSION lives there; see its header for why it is not written here.
include("${CMAKE_CURRENT_LIST_DIR}/version.cmake")
project(GpuThermalGuard VERSION ${GTG_VERSION} LANGUAGES CXX)

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
option(GTG_BUILD_INTERACTIVE_SMOKE
    "Build local HWND smoke tests (requires an interactive desktop)"
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
    src/logging/deferred_queue.hpp
    src/timing/protection_thread_policy.hpp
    src/timing/protection_timing.hpp
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
    src/telemetry/telemetry_freshness.hpp
    src/sysmem/host_memory.hpp
    src/sysmem/reclaim.hpp
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
    src/logging/logger.cpp
)
target_link_libraries(GpuThermalGuardProbe PRIVATE gtg_nvml gtg_build_options)

# The version is declared once, in version.cmake, and written out from there
# into everything that needs it. Nothing below is hand-edited.
set(GTG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/resources/gtg_version.h.in"
               "${GTG_GENERATED_DIR}/gtg_version.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/resources/GpuThermalGuard.manifest.in"
               "${GTG_GENERATED_DIR}/GpuThermalGuard.manifest" @ONLY)

add_executable(GpuThermalGuard WIN32
    src/app_main.cpp
    # iphlpapi is resolved at run time, so nothing is added to the link line.
    src/net/action.hpp
    src/net/estats_win32.cpp
    src/net/estats_win32.hpp
    src/net/history.hpp
    src/net/throughput.hpp
    src/net/verdict.hpp
    src/net/throughput_win32.cpp
    src/net/throughput_win32.hpp
    src/supervision/supervisor.cpp
    src/service/service_main.cpp
    src/service/service_main.hpp
    src/settings/settings.cpp
    src/settings/settings.hpp
    src/ipc/client.cpp
    src/ipc/client.hpp
    src/ipc/protocol.hpp
    src/logging/logger.cpp
    src/logging/logger.hpp
    src/protection/local_protection_worker.cpp
    src/protection/local_protection_worker.hpp
    src/tray/history_chart.cpp
    src/tray/history_chart.hpp
    src/tray/main_dialog.cpp
    src/tray/main_dialog.hpp
    src/tray/ui_theme.cpp
    src/tray/ui_theme.hpp
    src/tray/osd_overlay.cpp
    src/tray/osd_overlay.hpp
    src/fps/fps_rate.hpp
    src/fps/dxgi_event.hpp
    src/fps/dxgi_observer.cpp
    src/fps/dxgi_observer.hpp
    src/sysmem/host_memory.hpp
    src/sysmem/reclaim.hpp
    src/sysmem/reclaim_win32.hpp
    src/sysmem/reclaim_nt.hpp
    src/resources/GpuThermalGuard.rc
    "${GTG_GENERATED_DIR}/GpuThermalGuard.manifest"
)
target_include_directories(GpuThermalGuard PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # so rc.exe can find the generated gtg_version.h
    "${GTG_GENERATED_DIR}")
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
    wtsapi32
    dwmapi
    advapi32
    tdh
)
if(GTG_OSD_CLICK_THROUGH)
    target_compile_definitions(GpuThermalGuard PRIVATE GTG_OSD_CLICK_THROUGH=1)
else()
    target_compile_definitions(GpuThermalGuard PRIVATE GTG_OSD_CLICK_THROUGH=0)
endif()
target_link_options(GpuThermalGuard PRIVATE /MANIFESTUAC:NO)

include(CTest)
# On by default (AF-20261006-overlay-default-on): the in-game overlay is part
# of GTG. gtg_overlay.dll is built beside the EXE; the EXE shows its overlay
# controls only when that DLL is present, so one EXE serves both downloads.
# imgui comes from vcpkg (vcpkg.json, x64-windows-static). OFF builds need
# neither vcpkg nor splice.
option(GTG_FEATURE_OVERLAY "Build the in-game overlay (gtg_overlay.dll and its host in GpuThermalGuard.exe)" ON)
if(GTG_FEATURE_OVERLAY)
    add_subdirectory(overlay/integration)
endif()
if(BUILD_TESTING)
    add_executable(gtg_tests
        tests/test_main.cpp
        src/supervision/supervisor.cpp
        tests/wtl_compile_probe.cpp
        src/logging/logger.cpp
    )
    target_link_libraries(gtg_tests PRIVATE gtg_core gtg_telemetry gtg_snapshot
        gtg_build_options nvml_headers wtl user32 gdi32 advapi32)
    add_test(NAME gtg_tests COMMAND gtg_tests)
    set_tests_properties(gtg_tests PROPERTIES TIMEOUT 45)

    if(GTG_BUILD_INTERACTIVE_SMOKE)
        add_executable(gtg_osd_fps_smoke
            tests/osd_overlay_smoke.cpp
            src/logging/logger.cpp
            # The overlay's network action reaches the real probe; the smoke
            # build includes the overlay whole, so it needs the same source.
            src/net/estats_win32.cpp
            src/fps/dxgi_observer.cpp
        )
        target_include_directories(gtg_osd_fps_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(gtg_osd_fps_smoke PRIVATE
            gtg_telemetry gtg_build_options wtl gdiplus wtsapi32 dwmapi advapi32
            user32 gdi32 shell32 tdh)
        add_test(NAME gtg_osd_fps_smoke COMMAND gtg_osd_fps_smoke)
        set_tests_properties(gtg_osd_fps_smoke PROPERTIES TIMEOUT 30)
        add_executable(gtg_fps_history_chart_smoke
            tests/fps_history_chart_smoke.cpp
            src/tray/history_chart.cpp
        )
        target_include_directories(gtg_fps_history_chart_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(gtg_fps_history_chart_smoke PRIVATE
            gtg_telemetry gtg_build_options wtl gdiplus user32 gdi32)
        add_test(NAME gtg_fps_history_chart_smoke
            COMMAND gtg_fps_history_chart_smoke)
        set_tests_properties(gtg_fps_history_chart_smoke PROPERTIES TIMEOUT 15)
        add_executable(gtg_dpi_legibility_bench
            tests/dpi_legibility_probe.cpp
            src/tray/history_chart.cpp
        )
        target_include_directories(gtg_dpi_legibility_bench PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(gtg_dpi_legibility_bench PRIVATE
            gtg_telemetry gtg_build_options wtl gdiplus user32 gdi32)
        add_test(NAME gtg_dpi_legibility_bench
            COMMAND gtg_dpi_legibility_bench
                    "${CMAKE_CURRENT_BINARY_DIR}/dpi-legibility" 96)
        set_tests_properties(gtg_dpi_legibility_bench PROPERTIES TIMEOUT 30)
        add_executable(gtg_fps_observer_smoke
            tests/fps_observer_smoke.cpp
            src/fps/dxgi_observer.cpp
        )
        target_include_directories(gtg_fps_observer_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(gtg_fps_observer_smoke PRIVATE
            GTG_FPS_ISOLATED_SMOKE=1)
        target_link_libraries(gtg_fps_observer_smoke PRIVATE
            gtg_build_options advapi32 tdh user32)
    endif()
endif()

if(MSVC)
    target_link_options(GpuThermalGuardProbe PRIVATE
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
    )
    target_link_options(GpuThermalGuard PRIVATE
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>
    )
endif()

# ─── darkmodelib: the main window's dark theme ─────────────────────────────
# Static, /MT, into GpuThermalGuard.exe (AF-20261004-main-ui-redesign). MPL-2.0;
# the sources are used as published. Only src/tray/ui_theme.cpp includes it.
set(GTG_DMLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/darkmodelib")
if(NOT EXISTS "${GTG_DMLIB_DIR}/src/Darkmodelib.cpp")
    message(FATAL_ERROR
        "third_party/darkmodelib is empty. Run: git submodule update --init third_party/darkmodelib")
endif()
add_library(gtg_darkmodelib STATIC
    ${GTG_DMLIB_DIR}/src/Darkmodelib.cpp
    ${GTG_DMLIB_DIR}/src/DmlibColor.cpp
    ${GTG_DMLIB_DIR}/src/DmlibDpi.cpp
    ${GTG_DMLIB_DIR}/src/DmlibHook.cpp
    ${GTG_DMLIB_DIR}/src/DmlibIni.cpp
    ${GTG_DMLIB_DIR}/src/DmlibPaintHelper.cpp
    ${GTG_DMLIB_DIR}/src/DmlibSubclass.cpp
    ${GTG_DMLIB_DIR}/src/DmlibSubclassControl.cpp
    ${GTG_DMLIB_DIR}/src/DmlibSubclassWindow.cpp
    ${GTG_DMLIB_DIR}/src/DmlibWinApi.cpp
)
target_include_directories(gtg_darkmodelib PUBLIC ${GTG_DMLIB_DIR}/include)
# The library's own build settings (vs.proj/darkmodelib.Cpp.props).
target_compile_definitions(gtg_darkmodelib PRIVATE
    _WINDOWS STRICT_TYPED_ITEMIDS NOMINMAX WIN32_LEAN_AND_MEAN VC_EXTRALEAN
    UNICODE _UNICODE _DARKMODELIB_NO_INI_CONFIG _DARKMODELIB_CUSTOM_MEM=0x002)
target_compile_options(gtg_darkmodelib PRIVATE /W3 /utf-8 /permissive-)
target_link_libraries(gtg_darkmodelib PUBLIC comctl32 uxtheme dwmapi gdi32 user32 shlwapi)
target_link_libraries(GpuThermalGuard PRIVATE gtg_darkmodelib)

# ─── Research: the UI theme lab ────────────────────────────────────────────
# Off by default. The lab shows the production IDD_MAIN dialog and
# HistoryChart with fabricated readings, themed by darkmodelib; it never
# touches NVML, the service or settings. Kept as the tool for tuning colours.
# See docs/ui-modernization-research-20261003.md.
option(GTG_BUILD_UI_THEME_LAB "Build the darkmodelib UI theme lab (research)" OFF)
if(GTG_BUILD_UI_THEME_LAB)
    add_executable(gtg_ui_theme_lab WIN32
        tests/ui_theme_lab.cpp
        src/tray/history_chart.cpp
        src/resources/GpuThermalGuard.rc
        tests/ui_theme_lab.manifest
    )
    target_include_directories(gtg_ui_theme_lab PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GTG_GENERATED_DIR}")
    target_link_libraries(gtg_ui_theme_lab PRIVATE
        gtg_darkmodelib gtg_telemetry gtg_build_options wtl gdiplus comctl32 uxtheme)
endif()
