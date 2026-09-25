cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZcrVersion)

project(zcr VERSION ${ZCR_VERSION} LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ZCR targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ZCR is x64 only. nvEncodeAPI64.dll is the only encoder it loads.")
endif()

option(ZCR_WERROR    "Treat warnings as errors" OFF)
option(ZCR_HARDENING "Apply exploit-mitigation compiler and linker flags" ON)

include(ZcrWarnings)
include(ZcrHardening)
include(ZcrManifest)

# ---------------------------------------------------------------------------
# Shaders
#
# Compiled at build time into headers holding the bytecode, so the exe carries
# them and nothing is compiled at runtime (no d3dcompiler_47.dll dependency).
# fxc ships with every Windows SDK; take the one matching the SDK CMake chose.
# ---------------------------------------------------------------------------
set(_zcr_sdk_bin "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
find_program(ZCR_FXC fxc
    HINTS
        "${_zcr_sdk_bin}"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    REQUIRED)

set(ZCR_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert.hlsl")
set(ZCR_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${ZCR_SHADER_OUT}")

# name | entry point | profile. Each becomes shaders/<name>.h holding g_<entry>.
set(ZCR_SHADERS
    "convert_vs|VSMain|vs_5_0"
    "convert_ps_luma|PSLuma|ps_5_0"
    "convert_ps_chroma|PSChroma|ps_5_0"
)
set(ZCR_SHADER_HEADERS "")
foreach(_spec IN LISTS ZCR_SHADERS)
    string(REPLACE "|" ";" _parts "${_spec}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _entry)
    list(GET _parts 2 _profile)
    set(_out "${ZCR_SHADER_OUT}/${_name}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${ZCR_FXC}" /nologo /O3 /WX /T ${_profile} /E ${_entry}
                /Vn g_${_entry} /Fh "${_out}" "${ZCR_SHADER_SRC}"
        DEPENDS "${ZCR_SHADER_SRC}"
        COMMENT "fxc ${_entry} (${_profile})"
        VERBATIM)
    list(APPEND ZCR_SHADER_HEADERS "${_out}")
endforeach()
add_custom_target(zcr_shaders DEPENDS ${ZCR_SHADER_HEADERS})

# ---------------------------------------------------------------------------
# Resources
# ---------------------------------------------------------------------------
set(ZCR_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/zcr.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zcr.rc"
    @ONLY
)

# ---------------------------------------------------------------------------
# zcr.exe
#
# WIN32 is the subsystem: a GUI binary, so launching the tray never flashes a
# console. Command-line control borrows the caller's console or its redirected
# handles; see parent_console.
# ---------------------------------------------------------------------------
add_executable(zcr WIN32
    src/main.cpp
    src/win32.cpp
    src/json.cpp
    src/diag_log.cpp
    src/parent_console.cpp
    src/autostart.cpp
    src/settings.cpp
    src/tray.cpp
    src/ipc.cpp
    src/monitors.cpp
    src/capture.cpp
    src/convert.cpp
    src/nvenc.cpp
    src/mp4mux.cpp
    src/recorder.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/zcr.rc"
)
add_dependencies(zcr zcr_shaders)

target_include_directories(zcr PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"          # shaders/*.h
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc"
)

# rc.exe needs src/ for resource.h.
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/zcr.rc" PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(zcr PRIVATE
    ZCR_VERSION_STRING="${ZCR_VERSION}"
)

# All in-box. nvEncodeAPI64.dll is not linked: it ships with the NVIDIA driver
# and is loaded at runtime from System32.
target_link_libraries(zcr PRIVATE
    user32
    shell32     # Shell_NotifyIconW, SHGetKnownFolderPath, ShellExecuteW
    advapi32    # the HKCU Run key
    gdi32
    ole32
    shlwapi
    pathcch
    d3d11
    dxgi
    avrt        # AvSetMmThreadCharacteristicsW, MMCSS "Capture"
)

zcr_apply_warnings(zcr)
zcr_apply_hardening(zcr)
zcr_embed_manifest(zcr "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")

install(TARGETS zcr RUNTIME DESTINATION .)

# ---------------------------------------------------------------------------
# mux_test: muxes a raw HEVC elementary stream into fragmented MP4 so the
# container can be checked with ffprobe without a GPU capture in the loop.
# ---------------------------------------------------------------------------
add_executable(mux_test
    tests/mux_test.cpp
    src/mp4mux.cpp
    src/win32.cpp
)
target_include_directories(mux_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mux_test PRIVATE user32 shell32 ole32 shlwapi pathcch)
zcr_apply_warnings(mux_test)
set_target_properties(mux_test PROPERTIES EXCLUDE_FROM_ALL ON)

message(STATUS "ZCR ${ZCR_VERSION}  hardening=${ZCR_HARDENING} werror=${ZCR_WERROR}")
