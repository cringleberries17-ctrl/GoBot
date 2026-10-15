cmake_minimum_required(VERSION 3.21)
project(GoBot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "GoBot targets Windows only (SendInput + Npcap).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
    # Static CRT keeps the output a single self-contained exe.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Npcap REQUIRED)

# ------------------------------------------------------------------- OpenCV
# Template matching, which is how every dialog in this game is recognised.
#
# Both clients this project replaces use OpenCV: the commercial one ships
# opencv_world4.dll at 18 MB, version 4.8.0, and the other helper ships
# OpenCvSharpExtern.dll at 59 MB. This project hand-rolled a correlation
# matcher and an image pyramid instead, and that was a mistake twice over --
# once for the afternoon lost to a pyramid bug matchTemplate cannot have, and
# once because the features still to be built match dozens of pictures per
# frame, where a hand-rolled matcher stops being fast enough to use.
#
# Built from source rather than taken prebuilt, for two reasons. The official
# Windows build is a 7-zip self-extractor and there is no 7-Zip on a clean
# machine to unpack it. And the prebuilt links the dynamic CRT, which would
# mean shipping DLLs beside the executable and giving up the single
# self-contained exe; built here it is static and nothing changes.
#
# Only core and imgproc. Everything else -- video, codecs, ML, the Python and
# Java bindings -- is build time spent on things this project never calls.
option(GOBOT_USE_OPENCV "Match templates with OpenCV" ON)

set(GOBOT_OPENCV_SHA256 "9dc6a9a95edc133e165e9f6db9412dd899e28d4e5e4979f17cb5966f4b7f3fb1")

if(GOBOT_USE_OPENCV)
    include(FetchContent)

    # What OpenCV's own CMake reads. Set before it is pulled in, because these
    # are cache variables it consults at configure time.
    set(BUILD_LIST "core,imgproc" CACHE STRING "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
    set(OPENCV_FORCE_3RDPARTY_BUILD OFF CACHE BOOL "" FORCE)

    foreach(_off
            BUILD_TESTS BUILD_PERF_TESTS BUILD_EXAMPLES BUILD_DOCS
            BUILD_opencv_apps BUILD_opencv_python2 BUILD_opencv_python3
            BUILD_JAVA BUILD_PACKAGE BUILD_ITT
            # IPP is the one that matters: left on, OpenCV's configure step
            # downloads a thirty megabyte binary blob before it will build.
            WITH_IPP WITH_ITT WITH_EIGEN WITH_OPENCL WITH_OPENCLAMDFFT
            WITH_OPENCLAMDBLAS WITH_PROTOBUF WITH_ADE WITH_QUIRC
            WITH_FFMPEG WITH_MSMF WITH_DSHOW WITH_VFW WITH_WIN32UI
            WITH_JPEG WITH_PNG WITH_TIFF WITH_WEBP WITH_OPENJPEG
            WITH_JASPER WITH_OPENEXR WITH_IMGCODEC_HDR WITH_IMGCODEC_SUNRASTER
            WITH_IMGCODEC_PXM WITH_IMGCODEC_PFM WITH_LAPACK
            OPENCV_GENERATE_SETUPVARS OPENCV_GENERATE_PKGCONFIG)
        set(${_off} OFF CACHE BOOL "" FORCE)
    endforeach()

    # OpenCV 4.8 asks for CMake 3.5.1, and CMake 4 refuses anything that old
    # unless told what to assume instead.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "" FORCE)

    FetchContent_Declare(opencv
        URL          https://github.com/opencv/opencv/archive/refs/tags/4.8.0.zip
        URL_HASH     SHA256=${GOBOT_OPENCV_SHA256}
        DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/opencv-download"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(opencv)

    # OpenCV's headers live in several places in a source build; the modules it
    # built know where their own are.
    set(GOBOT_OPENCV_INCLUDES
        "${OPENCV_CONFIG_FILE_INCLUDE_DIR}"
        "${opencv_SOURCE_DIR}/include"
        "${opencv_SOURCE_DIR}/modules/core/include"
        "${opencv_SOURCE_DIR}/modules/imgproc/include")
endif()

# Ask for elevation at launch, the way the original client does.
#
# Windows refuses input injected by a process running at a lower integrity
# level than the window that currently has the foreground. The game is normally
# started from an elevated shell, so a bot started normally sends keystrokes
# that go nowhere: SendInput reports success, the macro runs, and the game
# receives nothing. It looks exactly like an anticheat and is not one.
#
# The commercial client solves this by requiring administrator in its manifest
# and prompting on every launch, which is why it never has the problem. Npcap
# wants the same privilege anyway.
#
# Not applied to the test runner: it has no business asking for a prompt, and
# the packaging script runs it unelevated as a release gate.
function(gobot_require_admin target)
    if(MSVC)
        target_link_options(${target} PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endif()
endfunction()

# Resolve wpcap.dll only when a capture function is actually called.
#
# A static import means Windows refuses to start the process at all when the
# DLL is absent. This was applied to the GUI alone, and the consequence was
# only found by dumping the import table of a package meant for somebody else:
# gobot_cli.exe would not launch on a machine without Npcap -- so neither would
# `--doctor`, which is the first thing a new user is asked to run when
# something does not work, and which has nothing to do with capturing packets.
#
# Every one of these reads the screen for a living. Npcap is for the protocol
# tools and nothing else, and the dependency belongs to them alone.
function(gobot_delay_npcap target)
    if(MSVC)
        target_link_libraries(${target} PRIVATE delayimp)
        target_link_options(${target} PRIVATE "/DELAYLOAD:wpcap.dll")
    endif()
endfunction()

# ---------------------------------------------------------------- core library
add_library(gobot_core STATIC
    src/util.cpp
    src/logger.cpp
    src/config.cpp
    src/framing.cpp
    src/capture_file.cpp
    src/tcp_reassembler.cpp
    src/packet_capture.cpp
    src/net_discovery.cpp
    src/protocol_analyzer.cpp
    src/macro.cpp
    src/screen_capture.cpp
    src/vitals_reader.cpp
    src/mob_detector.cpp
    src/glyph_font.cpp
    src/camera_tracker.cpp
    src/world_anchor.cpp
    src/field_finder.cpp
    src/navigator.cpp
    src/minimap_tracker.cpp
    src/hunter.cpp
    src/map_mapper.cpp
    src/map_file.cpp
    src/map_table.cpp
    src/map_locator.cpp
    src/template_matcher.cpp
    src/template_matcher_cv.cpp
    src/session_manager.cpp
    src/crash_watch.cpp
    src/party_invite.cpp
    src/player_list.cpp
    src/auto_login.cpp
    src/bag.cpp
    src/auto_chat.cpp
    src/alerts.cpp
    src/quick_quest.cpp
    src/dialog_driver.cpp
    src/routine.cpp
    src/gatherer.cpp
    src/clr_bridge.cpp
    src/party.cpp
    src/party_chat.cpp
    src/calibrator.cpp
    src/opcode_registry.cpp
    src/game_state.cpp
    src/protocol_handler.cpp
    src/window_focus.cpp
    src/input_manager.cpp
    src/pathfinder.cpp
    src/execution_coordinator.cpp
    src/command_executor.cpp
    src/bot_brain.cpp
)

target_include_directories(gobot_core
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(gobot_core
    PUBLIC  Npcap::Npcap
    # normaliz: NormalizeString, which takes the marks off a chat line.
    # winhttp and crypt32: the Discord alerts' webhook, posted over HTTPS and
    # kept encrypted with DPAPI -- the two GoC itself uses for the same jobs.
    PRIVATE ws2_32 iphlpapi user32 gdiplus normaliz winhttp crypt32
)

if(GOBOT_USE_OPENCV)
    target_compile_definitions(gobot_core PRIVATE GOBOT_HAVE_OPENCV)
    target_include_directories(gobot_core PRIVATE ${GOBOT_OPENCV_INCLUDES})
    target_link_libraries(gobot_core PRIVATE opencv_core opencv_imgproc)

    # OpenCV's own headers are not warning-clean at /W4 and are not ours to fix.
    set_source_files_properties(src/template_matcher_cv.cpp
                                PROPERTIES COMPILE_FLAGS "/W2")
endif()

# ------------------------------------------------------------------- bot runner
add_executable(gobot_cli apps/gobot_cli/main.cpp)
target_link_libraries(gobot_cli PRIVATE gobot_core)
gobot_require_admin(gobot_cli)
gobot_delay_npcap(gobot_cli)

# --------------------------------------------------- protocol discovery toolkit
add_executable(gobot_sniff apps/gobot_sniff/main.cpp)
target_link_libraries(gobot_sniff PRIVATE gobot_core)
gobot_require_admin(gobot_sniff)
gobot_delay_npcap(gobot_sniff)

# ----------------------------------------------------------------------- GUI
# Dear ImGui with the Win32 + Direct3D 11 backend — the same combination the
# original client used, and the one with no dependencies beyond the Windows SDK.
option(GOBOT_BUILD_GUI "Build the ImGui front-end" ON)

# Pinned so a compromised or re-tagged archive cannot silently change what
# gets compiled into the binary.
set(GOBOT_IMGUI_SHA256 "e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8")

if(GOBOT_BUILD_GUI)
    include(FetchContent)
    # Source archive rather than a git clone: this keeps git off the list of
    # build prerequisites, and the toolchain installed by bootstrap.ps1 does
    # not include it.
    FetchContent_Declare(imgui
        URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
        URL_HASH SHA256=${GOBOT_IMGUI_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(imgui)

    # ImGui ships sources, not a CMake target; compile the core plus the two
    # backends we need directly into the executable.
    add_executable(gobot_gui WIN32
        apps/gobot_gui/main.cpp
        apps/gobot_gui/gui_app.cpp
        apps/gobot_gui/simple_view.cpp
        apps/gobot_gui/simple_alerts.cpp
        apps/gobot_gui/simple_quest.cpp
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
    )

    target_include_directories(gobot_gui PRIVATE
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/gobot_gui"
    )

    target_link_libraries(gobot_gui PRIVATE
        gobot_core
        d3d11 dxgi d3dcompiler dwmapi
        delayimp
    )
    gobot_require_admin(gobot_gui)

    # Resolve wpcap.dll only if a capture function is actually called.
    #
    # A static import means Windows refuses to start the process at all when
    # the DLL is absent, so a machine without Npcap could not run the keyboard
    # macro — which needs no capture whatsoever. Delay-loading keeps the
    # front-end usable and confines the dependency to the feature that has it.
    if(MSVC)
        target_link_options(gobot_gui PRIVATE "/DELAYLOAD:wpcap.dll")
    endif()

    # ImGui's own sources are not warning-clean at /W4 and are not ours to fix.
    if(MSVC)
        set_source_files_properties(
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
            "${imgui_SOURCE_DIR}/imgui_demo.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
            PROPERTIES COMPILE_FLAGS "/W0"
        )
    endif()

    add_custom_command(TARGET gobot_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/configs
                -DDST=$<TARGET_FILE_DIR:gobot_gui>/configs
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_configs.cmake
        COMMENT "Staging any missing configs next to gobot_gui"
    )
endif()

# ---------------------------------------------------------------- self-tests
# Covers the layers that can be checked without the game running: framing,
# reassembly, packet decoding, world model, pathfinding.
add_executable(gobot_test apps/gobot_test/main.cpp apps/gobot_test/alerts_test.cpp
                          apps/gobot_test/quest_test.cpp)
target_link_libraries(gobot_test PRIVATE gobot_core)
gobot_delay_npcap(gobot_test)
# Where the configs that ship are, so a test checks those against the code --
# not the copies beside the exe, which are a player's to change.
target_compile_definitions(gobot_test PRIVATE
    GOBOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME selftest COMMAND gobot_test)

# ------------------------------------------------------------- runtime payload
# Config files live next to the executables so the tools run from the build dir.
#
# Only the ones that are not there yet. `copy_directory` overwrites, which meant
# every build threw away whatever had been configured: a channel chosen in the
# GUI went back to the default the next time anything was compiled, and the bot
# then logged into channel 1 while the person who set it watched it do so.
add_custom_command(TARGET gobot_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/configs
            -DDST=$<TARGET_FILE_DIR:gobot_cli>/configs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_configs.cmake
    COMMENT "Staging any missing configs next to the executables"
)

# ------------------------------------------------------- the game's UI pictures
# Staged into the build so a built bot carries what it needs and starts without
# hunting for somebody else's install.
#
# Staged, not committed. These are the game's own artwork -- its dialogs,
# buttons and item icons -- and a machine that has the game has them already.
# Copying them into the build output on the machine that built it is one thing;
# checking them into this repository would be publishing somebody's art, which
# is quite another, so build/ keeps them and git never sees them.
#
# The preferred source is the commercial client's extracted set, for two
# reasons: every picture is named after what it is, and it carries BOTH of the
# game's interface skins. A set taken from the wrong skin matches nothing at
# all, and matches nothing *silently*, which is how an afternoon disappears.
set(GOBOT_UI_IMAGES "C:/Program Files/ZTeam/GoC/data/images"
    CACHE PATH "Extracted game UI pictures to stage into the build")

if(EXISTS "${GOBOT_UI_IMAGES}")
    add_custom_command(TARGET gobot_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GOBOT_UI_IMAGES}" "$<TARGET_FILE_DIR:gobot_cli>/templates"
        COMMENT "Staging the game's UI pictures into the build"
    )
    message(STATUS "UI pictures will be staged from ${GOBOT_UI_IMAGES}")
else()
    message(STATUS "No UI pictures at ${GOBOT_UI_IMAGES}. The bot will look for "
                   "them at run time; without them the dialog features stay off.")
endif()

install(TARGETS gobot_cli gobot_sniff gobot_test RUNTIME DESTINATION .)
install(DIRECTORY configs/ DESTINATION configs)

