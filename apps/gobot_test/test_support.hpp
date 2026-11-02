// gobot_test -- what every suite shares beyond check() and section(): a part
// left out, counted, and the frames saved from the live client, read only as
// they were when the checks were written. All three are defined in main.cpp.
#pragma once

#include "gobot/screen_capture.hpp"

#include <string>

/// A part of the tests left out here -- what it needs is not on this machine --
/// said as "  [skip] <why>" and counted: the run ends "P passed, F failed, S
/// skipped", and package.ps1 warns, naming each, when S is not 0. `format` is
/// printf's; a newline at its end is optional. S counts parts, not checks.
void skip(const char* format, ...);

/// Whether `path` holds, byte for byte, one of the frames saved from the live
/// client that the checks were written against (FNV-1a over the file). frame.png
/// and playerlist.png are rewritten by gobot_cli --read and --playerlist, and
/// GoBot writes and prunes trash_debug and quest_debug as it plays: a frame
/// that is not one of these is no failure of the code, and its checks skip.
bool known_live_frame(const std::string& path);

/// load_png for such a frame: false, as for a missing file, unless
/// known_live_frame knows it.
bool load_live_frame(const std::string& path, gobot::Frame& out);
