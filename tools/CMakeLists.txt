cmake_minimum_required(VERSION 3.20)
project(OpenSteamToolTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

add_executable(ipc_codegen
    ipc_codegen/ipc_codegen.cpp
)
target_compile_features(ipc_codegen PRIVATE cxx_std_20)


add_executable(extract_tickets
    extract_tickets/extract_tickets.cpp
)
target_compile_features(extract_tickets PRIVATE cxx_std_20)

# ---------------------------------------------------------------------------
# ost-loader / ost-manager
#
#   ost-loader.exe   windowless IFEO helper Windows starts instead of steam.exe
#   ost-manager.exe  CLI (ost-manager.exe install dwmapi ...), console menu and
#                    the windowed manager
#
# Both link the same helpers from tools/common. The manager stages the payload,
# the three proxies and the loader next to itself so a bundle is always
# consistent. Built on demand:
#   cmake --build build --config Release --target ost-manager ost-loader
# ---------------------------------------------------------------------------
set(OST_VERSION_STRING "${OPENSTEAMTOOL_VERSION}")
if(OST_VERSION_STRING STREQUAL "" OR OST_VERSION_STRING STREQUAL "${PROJECT_VERSION}")
    set(OST_VERSION_STRING "dev")
endif()
string(REGEX REPLACE "^[vV]" "" OST_VERSION_STRING "${OST_VERSION_STRING}")
if(NOT OST_VERSION_STRING MATCHES "^[0-9]")
    set(OST_VERSION_STRING "dev")
endif()
set(OST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/ost-manager/ost.manifest")
# Forward slashes: the resource compiler accepts them and the path needs no escaping.
set(OST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/ost-manager/ost.ico")
set(OST_RC_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/ost-manager/ost.rc.in")
set(OST_STAGE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/ost-manager/stage_file.cmake")
set(OST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${OST_GENERATED_DIR}")

# configure_ost_rc(<output> <internal name> <description>)
function(configure_ost_rc output internal_name description)
    set(OST_RC_DESCRIPTION "${description}")
    set(OST_RC_INTERNAL_NAME "${internal_name}")
    configure_file("${OST_RC_TEMPLATE}" "${output}" @ONLY)
endfunction()

set(OST_TOOL_SOURCES
    common/Common.cpp
    common/Log.cpp
)

add_executable(ost-loader EXCLUDE_FROM_ALL
    ost-loader/Loader.cpp
    ost-loader/main.cpp
    ${OST_TOOL_SOURCES}
)
configure_ost_rc("${OST_GENERATED_DIR}/ost_loader_version.rc" "ost-loader.exe"
                 "OpenSteamTool IFEO loader")
target_sources(ost-loader PRIVATE "${OST_GENERATED_DIR}/ost_loader_version.rc")

add_executable(ost-manager EXCLUDE_FROM_ALL
    ost-manager/Commands.cpp
    ost-manager/Gui.cpp
    ost-manager/Ifeo.cpp
    ost-manager/Inject.cpp
    ost-manager/Proxies.cpp
    ost-manager/Steam.cpp
    ost-manager/Ui.cpp
    ost-manager/main.cpp
    ${OST_TOOL_SOURCES}
)
configure_ost_rc("${OST_GENERATED_DIR}/ost_manager_version.rc" "ost-manager.exe"
                 "OpenSteamTool manager and IFEO installer")
target_sources(ost-manager PRIVATE "${OST_GENERATED_DIR}/ost_manager_version.rc")

foreach(tool ost-loader ost-manager)
    target_compile_features(${tool} PRIVATE cxx_std_20)
    target_compile_definitions(${tool} PRIVATE
        UNICODE
        _UNICODE
        OST_TOOL_VERSION="${OPENSTEAMTOOL_VERSION}"
    )
    target_include_directories(${tool} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/${tool}"
    )
    target_link_libraries(${tool} PRIVATE
        advapi32
        comctl32
        gdi32
        shell32
        user32
        uxtheme
    )
    if(MSVC)
        # /utf-8 keeps the wide literals intact; both tools are warning-clean at /W4.
        target_compile_options(${tool} PRIVATE /utf-8 /W4 /permissive-)
        # The manifest is embedded as a resource (RT_MANIFEST in ost.rc.in); stop
        # the linker from generating a second one.
        target_link_options(${tool} PRIVATE "/MANIFEST:NO")
    endif()
    set_target_properties(${tool} PROPERTIES
        WIN32_EXECUTABLE TRUE
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tools/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/tools/Debug"
    )
endforeach()

# The manager is the entry point of the bundle, and it needs the loader next to
# it for IFEO installs, so building it builds everything.
add_dependencies(ost-manager ost-loader OpenSteamTool dwmapi xinput1_4 version)

# Ship a complete, consistent bundle: ost-manager.exe always finds the payload,
# the proxies and the loader it was built with next to itself. Without this a
# stale payload copy elsewhere could be injected while a proxy loads the fresh
# one, which is exactly the two-payload state that crashes Steam.
add_custom_command(TARGET ost-manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:OpenSteamTool>" -DDST="$<TARGET_FILE_DIR:ost-manager>/OpenSteamTool.dll" -P "${OST_STAGE_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:dwmapi>" -DDST="$<TARGET_FILE_DIR:ost-manager>/dwmapi.dll" -P "${OST_STAGE_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:xinput1_4>" -DDST="$<TARGET_FILE_DIR:ost-manager>/xinput1_4.dll" -P "${OST_STAGE_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:version>" -DDST="$<TARGET_FILE_DIR:ost-manager>/version.dll.disabled" -P "${OST_STAGE_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:ost-loader>" -DDST="$<TARGET_FILE_DIR:ost-manager>/ost-loader.exe" -P "${OST_STAGE_SCRIPT}"
    COMMENT "Staging OpenSteamTool.dll, the proxies and ost-loader.exe next to ost-manager.exe"
)

