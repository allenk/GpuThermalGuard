# The updater: GTG updates itself from GTG's public releases on GitHub, or from
# a UNC folder this machine's administrator configures. On by default; off
# builds GTG without it. bcrypt, wintrust and winhttp are resolved at run time,
# so nothing is added to the link line.
option(GTG_FEATURE_UPDATE "Build the updater (GitHub releases or a configured UNC source)" ON)
if(NOT GTG_FEATURE_UPDATE)
    return()
endif()

set(GTG_UPDATE_SOURCES
    src/update/decision.hpp
    src/update/digest.cpp
    src/update/digest.hpp
    src/update/files.hpp
    src/update/handoff.cpp
    src/update/handoff.hpp
    src/update/hosts_trigger.hpp
    src/update/hosts_updates.cpp
    src/update/hosts_updates.hpp
    src/update/hosts_wire.hpp
    src/update/http.cpp
    src/update/http.hpp
    src/update/json.cpp
    src/update/json.hpp
    src/update/manifest.cpp
    src/update/manifest.hpp
    src/update/plan.cpp
    src/update/plan.hpp
    src/update/preconditions.cpp
    src/update/preconditions.hpp
    src/update/preference.cpp
    src/update/preference.hpp
    src/update/safe_moment.hpp
    src/update/source.cpp
    src/update/source.hpp
    src/update/tray_badge.cpp
    src/update/tray_badge.hpp
    src/update/trust.cpp
    src/update/trust.hpp
    src/update/work_folder.cpp
    src/update/work_folder.hpp
    src/update/update_resource.h
    # The script and the settings dialog; rc resolves the script beside it.
    src/update/update.rc
)
target_sources(GpuThermalGuard PRIVATE ${GTG_UPDATE_SOURCES}
    # The Tray's side: menu, consent, settings dialog (WTL), the Hosts
    # dialog's words.
    src/update/hosts_text.cpp
    src/update/hosts_text.hpp
    src/update/settings_dialog.cpp
    src/update/settings_dialog.hpp
    src/update/tray_updates.cpp
    src/update/tray_updates.hpp)
target_compile_definitions(GpuThermalGuard PRIVATE GTG_FEATURE_UPDATE)

if(NOT BUILD_TESTING)
    return()
endif()

# With the peer link built as the updater build has it, for the two update
# datagram types.
add_executable(gtg_update_tests tests/update_tests.cpp ${GTG_UPDATE_SOURCES}
    src/logging/logger.cpp
    src/peer/crypto.cpp
    src/peer/link.cpp
    src/peer/pairing.cpp)
target_include_directories(gtg_update_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(gtg_update_tests PRIVATE
    GTG_FEATURE_UPDATE GTG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# ws2_32 for the loopback test's own socket (the link resolves Winsock at run
# time) and gdi32 for reading the badged tray icons back.
target_link_libraries(gtg_update_tests PRIVATE gtg_build_options advapi32 ws2_32 user32 gdi32)
add_test(NAME gtg_update_tests COMMAND gtg_update_tests)
set_tests_properties(gtg_update_tests PROPERTIES TIMEOUT 60)

# The fake hosts answer update requests too, so the Hosts dialog's updates can
# be tried without other machines.
if(TARGET gtg_fake_hosts)
    target_compile_definitions(gtg_fake_hosts PRIVATE GTG_FEATURE_UPDATE)
endif()

# The updater starts the system's Windows PowerShell 5.1; its tests use it too,
# without the module path a PowerShell 7 shell leaves behind, as the updater
# starts it.
set(GTG_WINDOWS_POWERSHELL "${CMAKE_COMMAND}" -E env --unset=PSModulePath
    "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
add_test(NAME gtg_update_trust
    COMMAND ${GTG_WINDOWS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/update/trust_test.ps1"
            -Tests $<TARGET_FILE:gtg_update_tests>)
set_tests_properties(gtg_update_trust PROPERTIES TIMEOUT 120)

# The script end to end against stand-ins that run the real supervisor and
# hand-off code with the script compiled in. Test tools: never part of a
# release.
add_executable(gtg_update_standin WIN32
    tests/update/standin.cpp
    src/supervision/supervisor.cpp
    src/logging/logger.cpp
    ${GTG_UPDATE_SOURCES})
target_include_directories(gtg_update_standin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(gtg_update_standin PRIVATE GTG_FEATURE_UPDATE)
target_link_libraries(gtg_update_standin PRIVATE gtg_build_options advapi32 shell32 user32)
add_library(gtg_update_standin_dll SHARED tests/update/standin_dll.cpp)
target_link_libraries(gtg_update_standin_dll PRIVATE gtg_build_options)
add_test(NAME gtg_update_apply
    COMMAND ${GTG_WINDOWS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/update/apply_test.ps1"
            -Standin $<TARGET_FILE:gtg_update_standin>
            -Overlay $<TARGET_FILE:gtg_update_standin_dll>)
# The stand-ins share a supervisor singleton and an update mutex: one run at a
# time.
set_tests_properties(gtg_update_apply PROPERTIES TIMEOUT 300 RESOURCE_LOCK gtg_update_standin)
# The publish tool's output read by the updater's own code.
add_test(NAME gtg_update_publish
    COMMAND ${GTG_WINDOWS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/update/publish_test.ps1"
            -Tests $<TARGET_FILE:gtg_update_tests>
            -Exe $<TARGET_FILE:gtg_update_standin>
            -Dll $<TARGET_FILE:gtg_update_standin_dll>)
set_tests_properties(gtg_update_publish PROPERTIES TIMEOUT 180)
# Injected defects in the script, each of which must turn a scenario red.
add_test(NAME gtg_update_apply_mutants
    COMMAND ${GTG_WINDOWS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/update/mutate_apply.ps1"
            -Standin $<TARGET_FILE:gtg_update_standin>
            -Overlay $<TARGET_FILE:gtg_update_standin_dll>)
set_tests_properties(gtg_update_apply_mutants PROPERTIES TIMEOUT 600 RESOURCE_LOCK gtg_update_standin)
