#include "gobot/routine.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <set>

namespace gobot {
namespace {

constexpr const char* kPrefix = "Routine.";

std::string key_for(int step, const char* field) {
    return std::to_string(step) + "." + field;
}

} // namespace

std::vector<Routine> Routine::load(const std::string& path) {
    std::vector<Routine> out;

    IniFile ini;
    if (!ini.load(path)) return out;

    for (const std::string& section : ini.sections()) {
        if (section.rfind(kPrefix, 0) != 0) continue;

        Routine routine;
        routine.name = section.substr(std::char_traits<char>::length(kPrefix));
        if (routine.name.empty()) continue;

        // Numbered from one, stopping at the first gap. A step commented out
        // truncates the routine instead of quietly renumbering the rest, which
        // would turn "I disabled step two" into "steps three and four now run
        // in the wrong place".
        for (int n = 1; n <= 64; ++n) {
            const std::string find = ini.get(section, key_for(n, "find"), {});
            if (find.empty()) break;

            DialogDriver::Step step;
            step.find       = find;
            step.confirm    = ini.get(section, key_for(n, "confirm"), {});
            step.gone       = ini.get_bool(section, key_for(n, "gone"), false);
            step.optional   = ini.get_bool(section, key_for(n, "optional"), false);
            step.click_dx   = ini.get_int(section, key_for(n, "click_dx"), -1);
            step.click_dy   = ini.get_int(section, key_for(n, "click_dy"), -1);
            step.score      = ini.get_float(section, key_for(n, "score"), 0.80f);
            step.wait_ms    = ini.get_int(section, key_for(n, "wait_ms"), 6000);
            step.confirm_ms = ini.get_int(section, key_for(n, "confirm_ms"), 4000);
            step.attempts   = ini.get_int(section, key_for(n, "attempts"), 3);
            routine.steps.push_back(step);
        }

        if (routine.valid()) out.push_back(std::move(routine));
    }

    GB_INFO("Routines: %zu from %s", out.size(), path.c_str());
    return out;
}

bool Routine::save(const std::vector<Routine>& routines, const std::string& path,
                   const std::string& header) {
    IniFile ini;
    for (const Routine& routine : routines) {
        const std::string section = kPrefix + routine.name;
        for (std::size_t i = 0; i < routine.steps.size(); ++i) {
            const DialogDriver::Step& step = routine.steps[i];
            const int n = static_cast<int>(i) + 1;
            ini.set(section, key_for(n, "find"), step.find);
            if (!step.confirm.empty()) ini.set(section, key_for(n, "confirm"), step.confirm);
            if (step.gone)     ini.set_bool(section, key_for(n, "gone"), true);
            if (step.optional) ini.set_bool(section, key_for(n, "optional"), true);
            if (step.click_dx >= 0) ini.set_int(section, key_for(n, "click_dx"), step.click_dx);
            if (step.click_dy >= 0) ini.set_int(section, key_for(n, "click_dy"), step.click_dy);
            ini.set_float(section, key_for(n, "score"), step.score);
            ini.set_int(section, key_for(n, "wait_ms"), step.wait_ms);
            ini.set_int(section, key_for(n, "confirm_ms"), step.confirm_ms);
            ini.set_int(section, key_for(n, "attempts"), step.attempts);
        }
    }
    return ini.save(path, header);
}

const Routine* Routine::find(const std::vector<Routine>& routines,
                             const std::string& name) {
    for (const Routine& routine : routines)
        if (routine.name == name) return &routine;
    return nullptr;
}

std::vector<std::string> Routine::pictures_used(const std::vector<Routine>& routines) {
    std::set<std::string> seen;
    for (const Routine& routine : routines)
        for (const DialogDriver::Step& step : routine.steps) {
            if (!step.find.empty())    seen.insert(step.find);
            if (!step.confirm.empty()) seen.insert(step.confirm);
        }
    return { seen.begin(), seen.end() };
}

std::vector<std::string> Routine::missing_pictures(const std::vector<Routine>& routines,
                                                   const TemplateStore& store) {
    std::vector<std::string> missing;
    for (const std::string& name : pictures_used(routines))
        if (!store.has(name)) missing.push_back(name);
    return missing;
}

} // namespace gobot
