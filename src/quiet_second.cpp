#include "gobot/quiet_second.hpp"
#include "gobot/dpi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <windows.h>

namespace gobot {

std::uint64_t QuietBackOff::peek_ms(std::uint64_t cap_ms) const {
    const std::uint64_t step = kStepsMs[std::clamp(level_, 0, kSteps - 1)];
    return std::max(kStepsMs[0], std::min(step, cap_ms));
}

std::uint64_t QuietBackOff::next_ms(std::uint64_t cap_ms) {
    const std::uint64_t ms = peek_ms(cap_ms);
    if (level_ < kSteps - 1) ++level_;
    return ms;
}

void QuietBackOff::set_level(int level) { level_ = std::clamp(level, 0, kSteps - 1); }

InputGlance InputGlancer::glance() const {
    InputGlance g;
    if (cursor) g.cursor = cursor(g.x, g.y);
    if (button_down) {
        const struct { int vk; unsigned bit; } kButtons[] = {
            { VK_LBUTTON, InputGlance::kLeft },  { VK_RBUTTON, InputGlance::kRight },
            { VK_MBUTTON, InputGlance::kMiddle }, { VK_XBUTTON1, InputGlance::kX1 },
            { VK_XBUTTON2, InputGlance::kX2 },
        };
        for (const auto& b : kButtons)
            if (button_down(b.vk)) g.buttons |= b.bit;
    }
    if (front_program) g.front = front_program();
    return g;
}

InputGlancer InputGlancer::real() {
    InputGlancer g;
    g.cursor = [](long& x, long& y) {
        POINT p{};
        if (!GetCursorPos(&p)) return false;
        x = p.x;
        y = p.y;
        return true;
    };
    g.button_down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    g.front_program = [] {
        const HWND front = GetForegroundWindow();
        if (front == nullptr) return std::string();
        const std::string path = window_program(front);
        const std::size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    return g;
}

std::string quiet_spoiled_text(const InputGlance& before, const InputGlance& after) {
    char buf[160];
    std::string out;
    const unsigned held = before.buttons | after.buttons;
    if (!before.cursor || !after.cursor) {
        out = "the pointer not read";
    } else if (before.x == after.x && before.y == after.y) {
        std::snprintf(buf, sizeof buf, "the pointer still at %ld,%ld", after.x, after.y);
        out = buf;
        if (held == 0) out += " (a key, the wheel, or a move there and back)";
    } else {
        const double dx = static_cast<double>(after.x - before.x);
        const double dy = static_cast<double>(after.y - before.y);
        std::snprintf(buf, sizeof buf, "the pointer moved %ld px (%ld,%ld -> %ld,%ld)",
                      static_cast<long>(std::lround(std::sqrt(dx * dx + dy * dy))), before.x, before.y, after.x,
                      after.y);
        out = buf;
    }
    if (held == 0) {
        out += "; no mouse button held";
    } else {
        std::string names;
        const struct { unsigned bit; const char* name; } kNames[] = {
            { InputGlance::kLeft, "left" }, { InputGlance::kRight, "right" }, { InputGlance::kMiddle, "middle" },
            { InputGlance::kX1, "X1" },     { InputGlance::kX2, "X2" },
        };
        for (const auto& n : kNames)
            if (held & n.bit) names += (names.empty() ? "" : ", ") + std::string(n.name);
        out += "; buttons held: " + names;
    }
    const std::string was = before.front.empty() ? "?" : before.front;
    const std::string now = after.front.empty() ? "?" : after.front;
    out += "; in front: " + (was == now ? now : was + ", then " + now);
    return out;
}

std::uint64_t resumed_clock(std::uint64_t kept, std::uint64_t now, std::uint64_t grace_ms) {
    return std::max(kept, now + grace_ms);
}

} // namespace gobot
