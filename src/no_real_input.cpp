#include "gobot/no_real_input.hpp"
#include "gobot/logger.hpp"

#include <atomic>

namespace gobot {
namespace {

// Atomic, because the paths it guards run on every thread GoBot has: the
// macro's timers, the keeper, the crossing, the GUI.
std::atomic<bool>          g_forbidden{ false };
std::atomic<std::uint64_t> g_refusals{ 0 };

} // namespace

void forbid_real_input() { g_forbidden.store(true); }

bool real_input_forbidden() { return g_forbidden.load(); }

bool refuse_real_input(const char* action) {
    if (!g_forbidden.load()) return false;
    g_refusals.fetch_add(1);
    GB_ERROR("No real input in this process: refused to %s", action != nullptr ? action : "?");
    return true;
}

std::uint64_t real_input_refusals() { return g_refusals.load(); }

} // namespace gobot
