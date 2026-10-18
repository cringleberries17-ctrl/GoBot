#pragma once

#include <cstdint>

namespace gobot {

/// The "no real input" switch: once it is on, nothing in this process acts on
/// the real desktop or on another program.
///
/// For gobot_test, which turns it on before its first test and never off. On
/// 2026-10-15 a test whose flow had no pretend hands drove a real login
/// crossing on the player's PC for three minutes, after every test had been
/// checked by hand and none found that could. A check by hand misses things;
/// this does not. With the switch on, each of these is refused:
///
///   - a key, a click or a move of the pointer;
///   - bringing a window to the front, or restoring or minimising one;
///   - a message posted to another program's window;
///   - starting a program, or ending one;
///   - reading a launcher's path off a running program, to adopt it.
///
/// Refused means: the error log names what was refused, and the call returns
/// the failure it returns anyway when Windows says no -- so a test that
/// reaches for the desktop by mistake fails loudly instead of acting.
/// Reading the screen and listing windows and processes go on as before: they
/// change nothing, and tests need them.
///
/// Off unless turned on, so gobot_gui, gobot_cli and gobot_sniff act exactly
/// as they always have. gobot_sniff_files, which only reads recordings, turns
/// it on first thing, as gobot_test does.
///
/// A new way of reaching the desktop or another program is guarded the same
/// way, by `if (refuse_real_input("what it does")) return <its failure>;` just
/// before its call to Windows.
void forbid_real_input();

/// Whether the switch is on.
bool real_input_forbidden();

/// The guard. With the switch on it logs an error naming `action` -- a few
/// words, "press a key" -- counts it and returns true, and the caller returns
/// its failure without calling Windows. With the switch off it returns false
/// and says nothing.
bool refuse_real_input(const char* action);

/// How many times the guard has refused in this process: for a test to check
/// that a call it made was refused, and was refused once.
std::uint64_t real_input_refusals();

} // namespace gobot
