#pragma once

// Windows' own folder, and where a program that runs as administrator takes
// its DLLs from. The rest of util.cpp -- paths, files, the version -- is
// declared in gobot/common.hpp.

#include <string>

namespace gobot {

/// Windows' own folder of programs and DLLs -- C:\Windows\System32 on almost
/// every machine, but asked of Windows (GetSystemDirectoryW), never assumed --
/// in UTF-8, with no slash at the end. With `name`, that name inside it:
/// system32_path("shutdown.exe"), system32_path("Npcap"). Empty when Windows
/// will not say; a caller then starts and loads nothing.
std::string system32_path(const std::string& name = {});

/// From here on, a DLL named without its path is looked for in System32 and
/// in Npcap's own folder (System32\Npcap, when Npcap is installed), and
/// nowhere else: not the program's folder, not the working folder, not PATH.
///
/// The first thing gobot_gui, gobot_cli and gobot_sniff do. They run as
/// administrator from a folder a player made, and Windows looked in a
/// program's own folder first, so an xinput1_4.dll (Dear ImGui asks for it by
/// name) or a wpcap.dll left there ran as administrator at the next start.
/// The DLLs a program imports are loaded before this can run;
/// /DEPENDENTLOADFLAG in CMakeLists.txt holds those to System32.
///
/// Runs once per process; calling it again changes nothing. False when
/// Windows refused, or is too old to be asked (Windows 7 without KB2533623):
/// the process then searches as it always did, but for the working folder.
bool harden_dll_search();

} // namespace gobot
