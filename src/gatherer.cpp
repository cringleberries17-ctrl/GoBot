#include "gobot/gatherer.hpp"
#include "gobot/logger.hpp"
#include "gobot/template_matcher.hpp"

#include <algorithm>

namespace gobot {

const char* Gatherer::action_name(Action a) {
    switch (a) {
        case Action::Nothing: return "wait";
        case Action::Work:    return "work";
        case Action::Replace: return "replace the tool";
        case Action::HandIn:  return "hand in";
        case Action::Stop:    return "stop";
    }
    return "?";
}

// ------------------------------------------------------------------ settings

void Gatherer::Settings::load(const IniFile& ini, const std::string& section) {
    name             = ini.get(section, "name", section);
    tool_picture     = ini.get(section, "tool_picture", tool_picture);
    broken_picture   = ini.get(section, "broken_picture", broken_picture);
    buy_routine      = ini.get(section, "buy_routine", buy_routine);
    hand_in_routine  = ini.get(section, "hand_in_routine", hand_in_routine);
    work_key         = static_cast<std::uint32_t>(
                           ini.get_int(section, "work_key", static_cast<int>(work_key)));
    work_interval_ms = ini.get_int(section, "work_interval_ms", work_interval_ms);
    hand_in_every    = ini.get_int(section, "hand_in_every", hand_in_every);
    stop_after       = ini.get_int(section, "stop_after", stop_after);
    pause_below_hp_pct = ini.get_int(section, "pause_below_hp_pct", pause_below_hp_pct);
    pause_below_mp_pct = ini.get_int(section, "pause_below_mp_pct", pause_below_mp_pct);
    give_up_after    = ini.get_int(section, "give_up_after", give_up_after);

    work_interval_ms = std::clamp(work_interval_ms, 200, 120000);
    give_up_after    = std::clamp(give_up_after, 1, 20);
    pause_below_hp_pct = std::clamp(pause_below_hp_pct, 0, 99);
    pause_below_mp_pct = std::clamp(pause_below_mp_pct, 0, 99);
}

void Gatherer::Settings::save(IniFile& ini, const std::string& section) const {
    ini.set(section, "name", name);
    ini.set(section, "tool_picture", tool_picture);
    ini.set(section, "broken_picture", broken_picture);
    ini.set(section, "buy_routine", buy_routine);
    ini.set(section, "hand_in_routine", hand_in_routine);
    ini.set_int(section, "work_key", static_cast<int>(work_key));
    ini.set_int(section, "work_interval_ms", work_interval_ms);
    ini.set_int(section, "hand_in_every", hand_in_every);
    ini.set_int(section, "stop_after", stop_after);
    ini.set_int(section, "pause_below_hp_pct", pause_below_hp_pct);
    ini.set_int(section, "pause_below_mp_pct", pause_below_mp_pct);
    ini.set_int(section, "give_up_after", give_up_after);
}

std::string Gatherer::Settings::trouble() const {
    if (work_key == 0)
        return "no key is set for the thing it is supposed to press";
    if (tool_picture.empty())
        return "no picture is set for the tool, so there is no way to notice it "
               "running out";
    if (buy_routine.empty())
        return "no routine is set for replacing the tool";
    if (hand_in_every > 0 && hand_in_routine.empty())
        return "it is set to hand in every " + std::to_string(hand_in_every) +
               " cycles, but no routine is named to do it with";
    return {};
}

// ------------------------------------------------------------------- looking

Gatherer::Look Gatherer::see(const Frame& frame, const TemplateStore& store) const {
    Settings s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = settings_;
    }

    Look look;
    if (!frame.valid()) return look;
    const ScreenRect whole{ 0, 0, frame.width, frame.height };

    if (!s.tool_picture.empty()) {
        if (const Frame* picture = store.get(s.tool_picture))
            look.tool_present =
                TemplateMatcher::find(frame, whole, *picture, 0.85f).found;
    }
    if (!s.broken_picture.empty()) {
        if (const Frame* picture = store.get(s.broken_picture))
            look.tool_broken =
                TemplateMatcher::find(frame, whole, *picture, 0.85f).found;
    }
    return look;
}

// ------------------------------------------------------------------ deciding

Gatherer::Action Gatherer::decide(const Look& look) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (const std::string wrong = settings_.trouble(); !wrong.empty()) {
        trouble_ = wrong;
        return Action::Stop;
    }

    if (stats_.failed_replacements >= settings_.give_up_after) {
        trouble_ = "could not replace the " + settings_.name + " tool after " +
                   std::to_string(stats_.failed_replacements) + " tries";
        return Action::Stop;
    }

    if (settings_.stop_after > 0 && stats_.cycles >= settings_.stop_after) {
        trouble_ = "finished the " + std::to_string(settings_.stop_after) +
                   " cycles it was asked for";
        return Action::Stop;
    }

    // The tool, before anything else. Working without one is pressing a key at
    // nothing, which looks exactly like working.
    if (look.tool_broken || !look.tool_present) return Action::Replace;

    // Hand in before starting another cycle, not after -- a bag that fills
    // mid-cycle loses whatever came out of it.
    if (settings_.hand_in_every > 0 &&
        stats_.since_hand_in >= settings_.hand_in_every)
        return Action::HandIn;

    // Low on health or mana: hold rather than start another go. The healing
    // feature does the drinking; this only declines to make things worse in the
    // meantime. A reading of -1 means nothing is being read, and an absent
    // reading must not hold the loop forever.
    if (settings_.pause_below_hp_pct > 0 && look.hp_pct >= 0.0f &&
        look.hp_pct < static_cast<float>(settings_.pause_below_hp_pct))
        return Action::Nothing;
    if (settings_.pause_below_mp_pct > 0 && look.mp_pct >= 0.0f &&
        look.mp_pct < static_cast<float>(settings_.pause_below_mp_pct))
        return Action::Nothing;

    return Action::Work;
}

// ------------------------------------------------------------------ bookkeeping

void Gatherer::set_settings(const Settings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = s;
}

Gatherer::Settings Gatherer::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void Gatherer::worked() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.cycles;
    ++stats_.since_hand_in;
}

void Gatherer::handed_in() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.hand_ins;
    stats_.since_hand_in = 0;
}

void Gatherer::replaced(bool ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        ++stats_.replacements;
        // A success clears the run of failures. Three failures spread over an
        // hour of working is not the same thing as three in a row, and only
        // the second means something is actually wrong.
        stats_.failed_replacements = 0;
    } else {
        ++stats_.failed_replacements;
    }
}

void Gatherer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
    trouble_.clear();
}

Gatherer::Stats Gatherer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string Gatherer::trouble() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trouble_;
}

} // namespace gobot
