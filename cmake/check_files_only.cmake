# Run after gobot_sniff_files links (CMakeLists.txt). It exists to be run, on
# recordings alone, by agents that can never answer a UAC prompt, so two things
# must never be in it:
#
#   - "requireAdministrator", which only a manifest asking for elevation
#     carries: Windows would put up the prompt before it even starts;
#   - "wpcap.dll", which is there only when something in it imports from
#     Npcap -- statically, so that it would not start without Npcap, or
#     delay-loaded, so that it could load it.
#
# Either fails the build, and the executable is removed: a copy that would ask
# for elevation or load Npcap is not left beside the others to be run.
#
# Usage: cmake -DEXE=<path to gobot_sniff_files.exe> -P check_files_only.cmake

if(NOT EXE OR NOT EXISTS "${EXE}")
    message(FATAL_ERROR "check_files_only: no executable at '${EXE}'")
endif()

file(STRINGS "${EXE}" admin REGEX "requireAdministrator")
file(STRINGS "${EXE}" npcap REGEX "[Ww][Pp][Cc][Aa][Pp]\\.[Dd][Ll][Ll]")

if(admin OR npcap)
    file(REMOVE "${EXE}")
    set(why "")
    if(admin)
        string(APPEND why " It asks for administrator: requireAdministrator is in it.")
    endif()
    if(npcap)
        string(APPEND why " It imports from wpcap.dll.")
    endif()
    message(FATAL_ERROR "gobot_sniff_files must run without elevation and never load Npcap."
                        "${why} Removed ${EXE}.")
endif()
