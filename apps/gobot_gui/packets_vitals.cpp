// Your HP and MP from the packets (the packets plan, item D): the gate against
// the bars, the resting reader, the map-load hold.
//
// The packets' HP and MP (wrapped 0x0051 and 0x0050, PacketSelf) are not
// proven on today's server, so they are checked first, read by read, against
// the bars off the screen (BarCheck, gobot/vitals_trust.hpp). Only a bar whose
// check has passed, whose value came in the packets' current epoch, on the
// capture running now -- the one started for the positions, on the game
// window's own process -- with the server heard within 5 s and a maximum known,
// is gone by -- usable -- and that is asked afresh every time, never kept:
// by the potions (gate_vitals), by every job's HP guard (hp_percent_now), by
// the reader's resting pace (vitals_pace) and nowhere else. The moment a bar
// is not usable the screen decides, exactly as before; the moment its check
// disagrees, the read it disagreed with goes to the macro at once (HandOver).
//
// What stays on the screen whatever the packets say: the death panel and the
// revive, HP coming back from 0, the pet's HP, and whether the character is in
// the world at all -- the packets may only add a hold to that, while a map
// loads (packet_map_loading).
//
// The checks are read from any thread under VitalsPart::mutex, and whose
// capture it is as an atomic; everything else of VitalsPart is GoBot's
// thread's alone.

#include "gui_app.hpp"
#include "gobot/logger.hpp"
#include "gobot/vitals_trust.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gobot {
namespace {

/// The time of day `at_ms` (now_ms()'s clock) was, as the log writes it:
/// HH:MM:SS, or HH:MM:SS.mmm with `millis`.
std::string clock_at(std::uint64_t at_ms, bool millis) {
    const std::uint64_t now = now_ms();
    const std::chrono::system_clock::time_point wall =
        std::chrono::system_clock::now() - std::chrono::milliseconds(now > at_ms ? now - at_ms : 0);
    const std::time_t t = std::chrono::system_clock::to_time_t(wall);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000;
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return "?";
    char buf[24];
    if (millis)
        std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03lld", local.tm_hour, local.tm_min, local.tm_sec, ms);
    else
        std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

/// How long before `now` `at` was, in seconds with one decimal.
std::string seconds_before(std::uint64_t now, std::uint64_t at) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(now > at ? now - at : 0) / 1000.0);
    return buf;
}

/// How far `at` was from the read at `read_at`: "1.2 s before the read", or
/// "0.6 s after the read".
std::string around_read(std::uint64_t at, std::uint64_t read_at) {
    char buf[48];
    const bool after = at > read_at;
    std::snprintf(buf, sizeof buf, "%.1f s %s the read",
                  static_cast<double>(after ? at - read_at : read_at - at) / 1000.0, after ? "after" : "before");
    return buf;
}

/// A bar's disagreements written to the log in one minute, at most.
constexpr int kDisagreementsSaid = 5;

std::string percent_text(float pct) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(pct));
    return buf;
}

/// A read of the bar, for the log: "6100/9232 (66%)", or "66% (no figures)".
std::string read_text(const BarCheck::Read& r) {
    if (r.figure >= 0 && r.figure_max > 0)
        return std::to_string(r.figure) + "/" + std::to_string(r.figure_max) + " (" + percent_text(r.pct) + ")";
    return percent_text(r.pct) + " (no figures)";
}

/// A packet value, for the log: "7344 (79%)", or "7344" with no maximum.
std::string packet_text(const BarCheck::Point& p, float pct) {
    return std::to_string(p.value) + (pct >= 0.0f ? " (" + percent_text(pct) + ")" : std::string());
}

const char* max_from_text(PacketSelf::MaxFrom from) {
    switch (from) {
        case PacketSelf::MaxFrom::StatSheet: return "0x0050";
        case PacketSelf::MaxFrom::Screen:    return "the bars' figures";
        case PacketSelf::MaxFrom::Seen:      return "the highest seen (not gone by)";
        case PacketSelf::MaxFrom::None:      return "nowhere yet";
    }
    return "?";
}

/// The percentage the reader worked out of the figures it printed, as it
/// works it out (VitalsReader's loop): equal to the bar's reading only when
/// this very read made the figures out. -1 for none.
float figure_percent(int value, int max) {
    if (value < 0 || max <= 0) return -1.0f;
    return std::clamp(static_cast<float>(value) * 100.0f / static_cast<float>(max), 0.0f, 100.0f);
}

/// A read of one bar for its check: the figures with it only when they are
/// this read's own (`now`, VitalsReader::Vitals), not ones kept from a read up
/// to two seconds before -- which a read in the caption's shadow goes by
/// (VitalsReader::shadowed_percent), its percentage then theirs exactly.
BarCheck::Read bar_read(std::uint64_t read_at, float pct, int value, int max, bool now) {
    BarCheck::Read r;
    r.read_at_ms = read_at;
    r.pct = pct;
    if (now && pct >= 0.0f && figure_percent(value, max) == pct) {
        r.figure = value;
        r.figure_max = max;
    }
    return r;
}

/// Why the reader keeps the run's pace, for the log; "" when it may rest.
const char* rest_why_not(const VitalsRest& in, std::uint64_t now) {
    if (!in.hp_usable || !in.mp_usable) return "HP and MP are not both from the packets";
    if (in.usable_since_ms == 0 || now < in.usable_since_ms + kRestAfterUsableMs)
        return "HP and MP from the packets for under 10 s";
    if (in.pet_key) return "a pet potion key is in use, and the pet's HP is the screen's";
    if (in.critical) return "a critical hold";
    if (in.finding_bars) return "the bars are being looked for";
    if (in.hp_zero_since_ms != 0) return "HP reads 0 on the screen";
    if (in.death_seen_ms != 0) return "a death panel under a minute ago";
    return "";
}

} // namespace

struct GuiApp::VitalsPart {
    /// What PacketSelf says now, taken in an order that errs towards not
    /// usable: the start count first, then the values, then the epoch.
    struct SelfNow {
        std::uint64_t      starts = 0;
        PacketSelf::Vitals v;
        std::uint32_t      epoch = 0;
        std::uint64_t      last_server_ms = 0;
    };
    static SelfNow self_now(const GuiApp& app, std::uint64_t now) {
        SelfNow s;
        s.starts = app.packet_self_.start_count();
        s.v = app.packet_self_.vitals();
        s.epoch = app.packet_self_.epoch();
        s.last_server_ms = app.packet_self_.link(now).last_server_ms;
        return s;
    }
    static bool max_known(const PacketSelf::Vitals& v) {
        return v.max_from == PacketSelf::MaxFrom::StatSheet || v.max_from == PacketSelf::MaxFrom::Screen;
    }

    /// The part, made at the first ask -- vitals_pace(), on the line of tick()
    /// that comes before anything of the tick can start a job's thread, and
    /// before any other thread asks. Only ever from GoBot's thread; another
    /// thread reads vitals_part_ as it is, and takes none for the screen.
    static VitalsPart& of(const GuiApp& app) {
        if (!app.vitals_part_) const_cast<GuiApp&>(app).vitals_part_.reset(new VitalsPart);
        return *app.vitals_part_;
    }

    /// The two checks, read from any thread: under `mutex`.
    mutable std::mutex mutex;
    BarCheck           hp{ true };
    BarCheck           mp{ false };
    /// Whether the capture running is the one started for the positions, on
    /// the game window's own process, as vitals_check_tick last found on
    /// GoBot's thread -- whose position_capture_ and position_link_ are --
    /// for any thread to read.
    std::atomic<bool>  on_window{ false };

    // ---- GoBot's thread only, from here ----
    HandOver           hp_hand, mp_hand;
    /// The newest packet value given to the checks, by its VitalsPoint::seq,
    /// not its time -- two frames of one segment, or of one read of the
    /// capture, share a millisecond -- and the maximum the last ones went with.
    std::uint64_t      fed_seq = 0;
    std::uint32_t      fed_max_hp = 0, fed_max_mp = 0;
    bool               fed_known = false;
    std::uint64_t      read_seen_ms = 0;          ///< the last read of the bars given to the checks
    std::uint64_t      over_max_seen = 0;
    /// The figures' maxima, and in how many reads in a row both were the same:
    /// three, with no stat sheet, make the packets' maxima.
    int                run_hp_max = 0, run_mp_max = 0, run_n = 0;
    int                screen_max_hp = 0, screen_max_mp = 0;   ///< as last handed to PacketSelf
    /// Usable as the last vitals_check_tick found, and both since when.
    bool               hp_usable = false, mp_usable = false;
    std::uint64_t      usable_since_ms = 0;
    std::uint64_t      tick_ms = 0;
    bool               zero_said = false;
    // The minute, for note_vitals_minute.
    std::uint64_t      minute_from_ms = 0, hp_usable_ms = 0, mp_usable_ms = 0, frames_seen = 0;
    /// Of the points fed, those in the same millisecond as the point fed before
    /// them, and of those the ones with another MP; and the last point fed.
    std::uint64_t      same_ms = 0, same_ms_mp = 0;
    std::uint64_t      fed_at_ms = 0;
    std::uint32_t      fed_mp = 0;
    int                hp_dis_said = 0, mp_dis_said = 0;   ///< disagreements written out, at most kDisagreementsSaid
    // The pace (vitals_pace): idle, the run's, or resting.
    enum class Pace : std::uint8_t { Idle, Run, Rest };
    PeriodWindow       periods;
    Pace               pace = Pace::Idle;
    std::uint64_t      pace_ms = 0, rest_ms = 0;
    int                flips = 0;
    // Map loads by the packets (packet_map_loading).
    std::uint64_t      load_entry_ms = 0, load_at_ms = 0, held_ms = 0;
    int                loads = 0, hud_first = 0;
    /// The MP caption read short of its leading 1, kept once a session
    /// (keep_mp_caption), and its writing, off this thread.
    bool               caption_kept = false;
    std::future<void>  caption_keeping;

    /// The MP bar and its caption, cut from the reader's newest frame and
    /// written to vitals_debug once a session: the figure `figure` of the read
    /// at `read_at` was the packets' `packet` short of its leading 1 (2026.11.08,
    /// 4798 for 14798 all run). The misread is steady, so the newest frame
    /// shows what that read saw. Named apart from vitals_debug's own frames,
    /// whose rotation never takes it; cut and written off GoBot's thread.
    void keep_mp_caption(const VitalsReader& reader, int figure, std::uint32_t packet, std::uint64_t read_at) {
        const std::uint64_t frame_at = reader.last_read_ms();
        const BarSpec bar = reader.mp_bar();
        Frame shot;
        if (!bar.valid() || !reader.latest_frame(shot)) return;     // tried again at the next
        caption_kept = true;
        const std::string read_text = clock_at(read_at, true);
        const std::string frame_text = clock_at(frame_at, true);
        caption_keeping = std::async(std::launch::async, [shot = std::move(shot), bar, figure, packet, read_text,
                                                          frame_text] {
            // The bar and the box the reader reads its caption in, a few
            // pixels round them.
            const ScreenRect box = VitalsReader::caption_box(bar);
            const int x0 = std::max(0, std::min(bar.x, box.x) - 4);
            const int y0 = std::max(0, std::min(bar.y, box.y) - 4);
            const int x1 = std::min(shot.width, std::max(bar.right(), box.right()) + 4);
            const int y1 = std::min(shot.height, std::max(bar.bottom(), box.bottom()) + 4);
            Frame cut;
            if (x1 > x0 && y1 > y0) {
                cut.width = x1 - x0;
                cut.height = y1 - y0;
                const std::size_t row = static_cast<std::size_t>(cut.width) * 4;
                cut.pixels.resize(row * static_cast<std::size_t>(cut.height));
                for (int y = 0; y < cut.height; ++y) {
                    const std::size_t from = (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(shot.width) +
                                              static_cast<std::size_t>(x0)) * 4;
                    std::copy_n(shot.pixels.begin() + static_cast<std::ptrdiff_t>(from), row,
                                cut.pixels.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)));
                }
            }
            const std::string dir = exe_relative("vitals_debug");
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char name[48];
            std::strftime(name, sizeof name, "mp_caption_%Y%m%d_%H%M%S.png", &tm);
            if (!cut.valid() || !make_folder(dir) || !save_png(cut, dir + "\\" + name)) {
                GB_INFO("Vitals: MP read %d where the packets say %u -- its leading 1 lost; the caption could not "
                        "be kept",
                        figure, static_cast<unsigned>(packet));
                return;
            }
            GB_INFO("Vitals: MP read %d where the packets say %u -- its leading 1 lost (the read at %s); kept the MP "
                    "bar and its caption, of the frame read at %s, as vitals_debug\\%s",
                    figure, static_cast<unsigned>(packet), read_text.c_str(), frame_text.c_str(), name);
        });
    }

    /// One bar's usable() inputs, now.
    VitalsUsable usable_in(const GuiApp& app, bool hp_bar, const SelfNow& s) const {
        VitalsUsable in;
        in.use_on = app.packets_settings_.vitals != PacketUse::Off;
        {
            std::lock_guard<std::mutex> lock(mutex);
            in.trusted = (hp_bar ? hp : mp).trusted();
        }
        in.reading = app.vitals_.running();
        // packet_positions=off: nothing from the packets, whoever captures.
        in.capturing = app.capture_.running() && !app.packets_off_.load();
        in.on_window = on_window.load();
        in.self_starts = s.starts;
        in.capture_starts = app.capture_.start_count();
        in.value_epoch = hp_bar ? s.v.hp_epoch : s.v.mp_epoch;
        in.epoch = s.epoch;
        in.value_ms = hp_bar ? s.v.hp_ms : s.v.mp_ms;
        in.last_server_ms = s.last_server_ms;
        in.max_known = max_known(s.v) && (hp_bar ? s.v.max_hp : s.v.max_mp) > 0;
        return in;
    }

    /// The resting pace's inputs, now: usable asked afresh.
    VitalsRest rest_in(const GuiApp& app, std::uint64_t now) const {
        const SelfNow s = self_now(app, now);
        VitalsRest in;
        in.hp_usable = vitals_usable(usable_in(app, true, s), now);
        in.mp_usable = vitals_usable(usable_in(app, false, s), now);
        in.usable_since_ms = usable_since_ms;
        if (app.macro_ && app.macro_->running())
            for (const MacroKey& k : app.macro_config_.keys)
                if (k.enabled && k.keycode != 0 && macro_key_kind(k) == MacroKeyKind::PetPotion) in.pet_key = true;
        in.critical = app.coord_.critical_active();
        in.finding_bars = app.simple_find_bars_ || app.full_find_bars_;
        in.hp_zero_since_ms = app.hp_zero_since_ms_;
        in.death_seen_ms = app.death_seen_ms_.load();
        return in;
    }

    /// The pace asked for at `now`, and what it was asked as; `actual_ms` the
    /// reader's period as it stood.
    void note_pace(std::uint64_t now, int period_ms, int actual_ms, Pace asked, const char* why) {
        periods.note(now, period_ms);
        if (pace == Pace::Rest && pace_ms != 0 && now > pace_ms) rest_ms += now - pace_ms;
        // A job's start cut a resting pace to the run's for a read
        // (vitals_for_job): counted, not said.
        if (pace == Pace::Rest && asked == Pace::Rest && actual_ms != period_ms) ++flips;
        // Between resting and the run's pace, logged and counted: idle is no
        // part of it.
        if ((pace == Pace::Rest && asked == Pace::Run) || (pace == Pace::Run && asked == Pace::Rest)) {
            ++flips;
            if (asked == Pace::Rest)
                GB_INFO("Vitals: the bars every %d ms now -- HP and MP from the packets for 10 s and more", period_ms);
            else
                GB_INFO("Vitals: the bars at the run's pace again, every %d ms -- %s", period_ms, why);
        }
        pace = asked;
        pace_ms = now;
    }

    /// One call of packet_map_loading: counted, and its start said.
    void note_load(const GuiApp& app, std::uint64_t now, bool loading, std::uint64_t entry_ms) {
        if (!loading) {
            load_at_ms = 0;
            return;
        }
        if (load_at_ms != 0 && now > load_at_ms) held_ms += now - load_at_ms;
        load_at_ms = now;
        if (entry_ms == load_entry_ms) return;
        load_entry_ms = entry_ms;
        ++loads;
        // Whether the HUD's own rule had the character out already: asked as
        // tick() asks it, the reader's last read before its sight of HP.
        const std::uint64_t frame_at = app.vitals_.last_read_ms();
        const bool hud_out = !VitalsReader::in_world(app.vitals_.hp_seen_ms(), frame_at, app.in_world_window_ms());
        if (hud_out) ++hud_first;
        GB_INFO("World: a map is loading by the packets -- its entry %s ago, no report of yours since; the macro held "
                "until one comes or 3 s%s",
                seconds_before(now, entry_ms).c_str(), hud_out ? " (the HUD was out already)" : "");
    }
};

void GuiApp::VitalsPartFree::operator()(VitalsPart* p) const { delete p; }

float GuiApp::hp_percent_now() const {
    // The packets' HP when it is usable this moment, read live under
    // PacketSelf's lock; the bars' otherwise, as before. Any thread: no part
    // yet is the bars'.
    //
    // Only while the bars' own last read saw the bar: HP -1 is how the quest,
    // the charms and the shop tell the character is not in the world -- a
    // map loading, the death panel, the character screen -- and the packets
    // never say it is (they only ever add a hold to that).
    const float screen = vitals_.hp_percent();
    if (screen < 0.0f) return screen;
    if (const VitalsPart* part = vitals_part_.get()) {
        const std::uint64_t now = now_ms();
        const VitalsPart::SelfNow s = VitalsPart::self_now(*this, now);
        if (vitals_usable(part->usable_in(*this, true, s), now)) return BarCheck::percent(s.v.hp, s.v.max_hp);
    }
    return screen;
}

float GuiApp::mp_percent_now() const {
    const float screen = vitals_.mp_percent();
    if (screen < 0.0f) return screen;
    if (const VitalsPart* part = vitals_part_.get()) {
        const std::uint64_t now = now_ms();
        const VitalsPart::SelfNow s = VitalsPart::self_now(*this, now);
        if (vitals_usable(part->usable_in(*this, false, s), now)) return BarCheck::percent(s.v.mp, s.v.max_mp);
    }
    return screen;
}

bool GuiApp::capture_on_window() const {
    const VitalsPart* part = vitals_part_.get();
    return part != nullptr && part->on_window.load();
}

void GuiApp::vitals_check_tick(std::uint64_t now) {
    VitalsPart& part = VitalsPart::of(*this);
    // Whose capture it is, for usable() on any thread: the one started for the
    // positions, on the process that owns the game's window now -- as the map
    // load's hold asks. A capture of the Protocol tab's or Autopilot's takes
    // every client's traffic to the server; and with the window switched to
    // another client, the old capture is the old one's until it is restarted.
    // Only a window handle's owner asked here: nothing that waits.
    part.on_window.store(capture_.running() && position_capture_ && position_link_.pid != 0 &&
                         position_link_.pid == pid_for_window(input_.target_window()));
    const VitalsPart::SelfNow s = VitalsPart::self_now(*this, now);
    const PacketSelf::Vitals& v = s.v;

    // ---- the packets' side: every value that came since the last tick ----
    //
    // Told apart by their seq, not their time: those of one millisecond --
    // one segment, one read of the capture -- all go in, in the order they
    // came, so the last of them is the check's as it is the potions'.
    //
    // With the maximum and the epoch of now, which they came under unless the
    // maximum changed meanwhile: then they go with none, and are judged by
    // the figures' value alone, never against a maximum they did not have.
    std::vector<BarCheck::Point> hp_points, mp_points;
    const bool known = VitalsPart::max_known(v);
    const bool same_max = v.max_hp == part.fed_max_hp && v.max_mp == part.fed_max_mp && known == part.fed_known;
    if (v.points > part.fed_seq) {
        for (const PacketSelf::VitalsPoint& p : packet_self_.history()) {
            if (p.seq <= part.fed_seq) continue;
            hp_points.push_back(BarCheck::Point{ p.at_ms, p.hp, v.max_hp, known && same_max, v.hp_epoch });
            mp_points.push_back(BarCheck::Point{ p.at_ms, p.mp, v.max_mp, known && same_max, v.mp_epoch });
            // How often a point shares its millisecond with the one before,
            // for the minute's line.
            if (p.at_ms == part.fed_at_ms) {
                ++part.same_ms;
                if (p.mp != part.fed_mp) ++part.same_ms_mp;
            }
            part.fed_at_ms = p.at_ms;
            part.fed_mp = p.mp;
            part.fed_seq = p.seq;
            ++part.frames_seen;
        }
    }
    part.fed_max_hp = v.max_hp;
    part.fed_max_mp = v.max_mp;
    part.fed_known = known;
    // HP above a maximum the stat sheet gave: a disagreement, once a tick.
    if (v.over_max < part.over_max_seen) part.over_max_seen = v.over_max;    // a new game: counted afresh
    const bool over = v.over_max > part.over_max_seen;
    part.over_max_seen = v.over_max;

    // ---- the screen's side: the reader's newest read, if it is new ----
    //
    // Its time asked before and after its values: a read that landed in
    // between is dropped, never paired with the other read's time (S20).
    bool have_read = false;
    BarCheck::Read hp_read, mp_read;
    bool figures_only = false;
    if (vitals_.running()) {
        const std::uint64_t before = vitals_.last_read_ms();
        if (before != 0 && before != part.read_seen_ms) {
            const float hp_pct = vitals_.hp_percent();
            const float mp_pct = vitals_.mp_percent();
            const VitalsReader::Vitals figures = vitals_.values();
            const std::uint64_t after = vitals_.last_read_ms();
            if (before == after) {
                part.read_seen_ms = before;
                have_read = true;
                hp_read = bar_read(before, hp_pct, figures.hp, figures.hp_max, figures.hp_now);
                mp_read = bar_read(before, mp_pct, figures.mp, figures.mp_max, figures.mp_now);
                figures_only = vitals_.has_font();
                // The maxima from the figures, while no stat sheet has given
                // them: read the same three times in a row. Given again after
                // a new game forgot them, said only when they are new.
                if (hp_read.figure >= 0 && mp_read.figure >= 0) {
                    if (hp_read.figure_max == part.run_hp_max && mp_read.figure_max == part.run_mp_max) {
                        ++part.run_n;
                    } else {
                        part.run_hp_max = hp_read.figure_max;
                        part.run_mp_max = mp_read.figure_max;
                        part.run_n = 1;
                    }
                    const bool differ = part.run_hp_max != part.screen_max_hp || part.run_mp_max != part.screen_max_mp;
                    if (part.run_n >= 3 && v.max_from != PacketSelf::MaxFrom::StatSheet &&
                        (v.max_from != PacketSelf::MaxFrom::Screen || differ)) {
                        packet_self_.set_max_from_screen(static_cast<std::uint32_t>(part.run_hp_max),
                                                         static_cast<std::uint32_t>(part.run_mp_max), now);
                        if (differ)
                            GB_INFO("Vitals: the packets' maxima from the bars' figures, HP %d and MP %d -- no stat "
                                    "sheet (0x0050) yet",
                                    part.run_hp_max, part.run_mp_max);
                        part.screen_max_hp = part.run_hp_max;
                        part.screen_max_mp = part.run_mp_max;
                    }
                }
            }
        }
    }

    // ---- the checks ----
    std::vector<BarCheck::Change> hp_changes, mp_changes;
    std::vector<BarCheck::Disagreement> hp_dis, mp_dis;
    {
        std::lock_guard<std::mutex> lock(part.mutex);
        if (!same_max) {
            part.hp.max_changed();
            part.mp.max_changed();
        }
        for (const BarCheck::Point& p : hp_points) part.hp.packet(p);
        for (const BarCheck::Point& p : mp_points) part.mp.packet(p);
        if (over) part.hp.over_max(now);
        if (have_read) {
            part.hp.set_figures_only(figures_only);
            part.mp.set_figures_only(figures_only);
            part.hp.screen(hp_read);
            part.mp.screen(mp_read);
        }
        part.hp.settle(now, s.epoch);
        part.mp.settle(now, s.epoch);
        hp_changes = part.hp.take_changes();
        mp_changes = part.mp.take_changes();
        hp_dis = part.hp.take_disagreements();
        mp_dis = part.mp.take_disagreements();
    }

    // Each change said. A bar that stops being trusted hands the read it
    // disagreed with to the macro (gate_vitals), until the screen has a newer
    // read of its own to give, or that one is stale.
    const auto said = [&](const char* bar, const BarCheck::Change& c, HandOver& hand, bool was_usable) {
        using State = BarCheck::State;
        const std::string pk = c.over_max ? std::string("HP ") + std::to_string(v.hp) + " above the stat sheet's maximum"
                                          : packet_text(c.packet, c.packet_pct);
        const std::string bars =
            c.read.pct >= 0.0f ? read_text(c.read) + " at " + clock_at(c.read.read_at_ms, true) : std::string("-");
        // Only to a macro running now: one started after the moment would
        // take a read from before its start, at the idle pace. Nor a read
        // from before the pace was last cut: a BẮT ĐẦU's own read is coming.
        if (c.from == State::Trusted && c.read.pct >= 0.0f && macro_ && macro_->running() &&
            c.read.read_at_ms >= vitals_.paced_from_ms())
            hand.start(c.read.pct, c.read.read_at_ms);
        const bool handed = hand.live() && was_usable;
        if (c.to == State::Trusted && c.from == State::Learning) {
            GB_INFO("Vitals: %s from the packets now -- %d agreements over %d values (exact %d)%s", bar, c.agreements,
                    c.values, c.exact,
                    packets_settings_.vitals == PacketUse::Off ? "; [Packets] vitals=off keeps the screen deciding"
                                                               : "");
        } else if (c.to == State::Trusted) {
            GB_INFO("Vitals: %s from the packets again -- %d agreements in a row since the doubt", bar,
                    BarCheck::kDoubtClears);
        } else if (c.to == State::Doubt) {
            GB_WARN("Vitals: %s from the screen again -- packets %s, the bars %s; %s", bar, pk.c_str(), bars.c_str(),
                    handed ? "the bars' read handed to the macro" : "in doubt until 5 agreements in a row");
        } else if (c.to == State::Learning) {
            GB_WARN("Vitals: %s distrusted (%s) -- packets %s, the bars %s; from the screen, learning again: %d "
                    "agreements needed (distrust %d of %d)%s",
                    bar, c.in_a_row ? "2 disagreements in a row" : "3 disagreements among the last 20", pk.c_str(),
                    bars.c_str(), BarCheck::kRelearnAgreements, c.distrusts, BarCheck::kDistrustsOff,
                    handed ? "; the bars' read handed to the macro" : "");
        } else {
            GB_WARN("Vitals: %s from the screen for the rest of this session -- distrusted %d times; packets %s, the "
                    "bars %s%s",
                    bar, c.distrusts, pk.c_str(), bars.c_str(), handed ? "; the bars' read handed to the macro" : "");
        }
    };
    for (const BarCheck::Change& c : hp_changes) said("HP", c, part.hp_hand, part.hp_usable);
    for (const BarCheck::Change& c : mp_changes) said("MP", c, part.mp_hand, part.mp_usable);

    // Each disagreement, at most kDisagreementsSaid a bar a minute, beside what
    // else said that bar's value around the read: the newest packets, 0x02F0
    // for MP, and the bar's potion key. For the log alone.
    const auto last_press = [&](MacroKeyKind kind, std::uint64_t read_at) {
        std::vector<std::uint32_t> keys;
        for (const MacroKey& k : macro_config_.keys)
            if (k.enabled && k.keycode != 0 && macro_key_kind(k) == kind) keys.push_back(k.keycode);
        std::uint64_t at = 0;
        for (const PacketSelf::Press& p : packet_self_.presses_since(read_at > 10'000 ? read_at - 10'000 : 0))
            if (p.at_ms <= read_at && std::find(keys.begin(), keys.end(), p.vk) != keys.end()) at = p.at_ms;
        return at != 0 ? "last pressed " + seconds_before(read_at, at) + " before the read"
                       : std::string("none pressed in the 10 s before");
    };
    const auto said_disagreement = [&](const char* bar, const BarCheck::Disagreement& d, bool hp_bar) {
        const std::uint64_t read_at = d.read.read_at_ms;
        const std::uint32_t newest = hp_bar ? v.hp : v.mp;
        const std::uint64_t newest_ms = hp_bar ? v.hp_ms : v.mp_ms;
        std::ostringstream line;
        line << "Vitals: " << bar << " disagreement (" << BarCheck::state_name(d.state) << ") -- the bars "
             << read_text(d.read) << " at " << clock_at(read_at, true) << "; the check's packets "
             << packet_text(d.packet, d.packet.max_known ? BarCheck::percent(d.packet.value, d.packet.max) : -1.0f)
             << ", " << around_read(d.packet.at_ms, read_at) << "; the newest packets " << bar << " "
             << (newest_ms != 0 ? std::to_string(newest) + ", " + around_read(newest_ms, read_at)
                                : std::string("none"));
        if (!hp_bar)
            line << "; 0x02F0 MP "
                 << (v.udp_mp_ms != 0 ? std::to_string(v.udp_mp) + ", " + around_read(v.udp_mp_ms, read_at)
                                      : std::string("none"));
        line << "; " << bar << " potion key "
             << last_press(hp_bar ? MacroKeyKind::HpPotion : MacroKeyKind::MpPotion, read_at);
        GB_INFO("%s", line.str().c_str());
    };
    for (const BarCheck::Disagreement& d : hp_dis)
        if (part.hp_dis_said < kDisagreementsSaid) {
            ++part.hp_dis_said;
            said_disagreement("HP", d, true);
        }
    for (const BarCheck::Disagreement& d : mp_dis)
        if (part.mp_dis_said < kDisagreementsSaid) {
            ++part.mp_dis_said;
            said_disagreement("MP", d, false);
        }
    // The MP caption read short of its leading 1, as all of 2026-11-08's run:
    // a picture of it, once a session, for the reader's fix (R9). Log only.
    if (!part.caption_kept)
        for (const BarCheck::Disagreement& d : mp_dis)
            if (figure_lost_leading_one(d.read.figure, d.packet.value)) {
                part.keep_mp_caption(vitals_, d.read.figure, d.packet.value, d.read.read_at_ms);
                break;
            }

    // ---- usable now: for the resting pace's 10 s, the minute's share, the log ----
    const VitalsUsable hp_in = part.usable_in(*this, true, s);
    const VitalsUsable mp_in = part.usable_in(*this, false, s);
    const bool hp_ok = vitals_usable(hp_in, now);
    const bool mp_ok = vitals_usable(mp_in, now);
    if (part.minute_from_ms == 0) part.minute_from_ms = now;
    if (part.tick_ms != 0 && now > part.tick_ms && now - part.tick_ms < 5'000) {
        if (part.hp_usable) part.hp_usable_ms += now - part.tick_ms;
        if (part.mp_usable) part.mp_usable_ms += now - part.tick_ms;
    }
    part.tick_ms = now;
    const auto switched = [&](const char* bar, bool was, bool is, const VitalsUsable& in) {
        if (was == is) return;
        if (is)
            GB_INFO("Vitals: %s goes by the packets -- the potions, the jobs' HP guards and the pace", bar);
        else
            GB_INFO("Vitals: %s goes by the screen again -- %s", bar, vitals_unusable_why(in, now));
    };
    switched("HP", part.hp_usable, hp_ok, hp_in);
    switched("MP", part.mp_usable, mp_ok, mp_in);
    part.hp_usable = hp_ok;
    part.mp_usable = mp_ok;
    if (!hp_ok || !mp_ok)
        part.usable_since_ms = 0;
    else if (part.usable_since_ms == 0)
        part.usable_since_ms = now;

    // A packet HP of 0 acts on nothing -- a death is the screen's -- and is
    // said beside what the screen shows, for a later plan to prove it by.
    const bool zero = hp_ok && v.hp == 0;
    if (zero && !part.zero_said) {
        const std::uint64_t death = death_seen_ms_.load();
        const float screen = vitals_.hp_percent();
        const std::string reads = screen >= 0.0f ? percent_text(screen) : std::string("nothing");
        const std::string panel = death != 0 ? "seen " + seconds_before(now, death) + " ago" : std::string("not seen");
        GB_INFO("Vitals: HP 0 in the packets (usable) -- the screen reads %s / the death panel %s", reads.c_str(),
                panel.c_str());
    }
    part.zero_said = zero;
}

SelfVitals GuiApp::gate_vitals(std::uint64_t now) {
    SelfVitals out;
    VitalsPart* part = vitals_part_.get();
    if (part == nullptr) return out;
    // With the bars not read nothing is checked, and nothing handed over: the
    // death block's packet path stays the packet bot's alone.
    if (!vitals_.running()) {
        part->hp_hand.end();
        part->mp_hand.end();
        return out;
    }
    // Nor with no macro running: a hand-over never lasts across a BẮT ĐẦU,
    // which would take it as a read of its own, from before its start.
    if (!macro_ || !macro_->running()) {
        part->hp_hand.end();
        part->mp_hand.end();
    }
    const VitalsPart::SelfNow s = VitalsPart::self_now(*this, now);
    // The read the screen would hand the macro now, as tick() works it out
    // below: none taken before the pace was last cut.
    const std::uint64_t last = vitals_.last_read_ms();
    const std::uint64_t screen_at = last >= vitals_.paced_from_ms() ? last : 0;
    if (vitals_usable(part->usable_in(*this, true, s), now)) {
        part->hp_hand.end();
        out.hp = BarCheck::percent(s.v.hp, s.v.max_hp);
    } else {
        out.hp = part->hp_hand.give(now, screen_at, MacroEngine::kVitalsStaleMs);
    }
    if (vitals_usable(part->usable_in(*this, false, s), now)) {
        part->mp_hand.end();
        out.mp = BarCheck::percent(s.v.mp, s.v.max_mp);
    } else {
        out.mp = part->mp_hand.give(now, screen_at, MacroEngine::kVitalsStaleMs);
    }
    return out;
}

bool GuiApp::packet_map_loading(std::uint64_t now) const {
    // On the game window's own process, by a capture started for it: the
    // server's map entry, and no report of yours since. Never a hold taken
    // off -- only one added (tick()'s in-world block).
    MapLoad in;
    in.on_window = capture_.running() && position_capture_ && position_link_.pid != 0 &&
                   position_link_.pid == pid_for_window(input_.target_window());
    if (in.on_window) {
        const PacketWorld::Stats ws = packet_world_.stats();
        in.entry_ms = ws.entry_ms;
        in.last_own_ms = ws.last_own_ms;
        in.last_server_ms = packet_self_.link(now).last_server_ms;
    }
    const bool loading = map_load_holds(in, now);
    if (VitalsPart* part = vitals_part_.get()) part->note_load(*this, now, loading, in.entry_ms);
    return loading;
}

std::uint64_t GuiApp::in_world_window_ms() const {
    // MacroEngine::kOutOfWorldAfterMs at the run's pace, as it always was;
    // wider while the reader rests, and for 3 s after it stops resting -- one
    // unreadable read at a second's pace is no reason to hold the macro.
    int longest = vitals_.period_ms();
    if (const VitalsPart* part = vitals_part_.get()) longest = std::max(longest, part->periods.longest(now_ms()));
    return world_window_ms(longest, MacroEngine::kOutOfWorldAfterMs);
}

int GuiApp::vitals_pace() const {
    // The run's pace while anything that acts on HP and MP, or presses a key,
    // is alive; vitals_idle_ms -- a second -- once nothing is. And while a
    // job runs with HP and MP both from the packets, checked and usable for
    // 10 s, no pet potion key in use, no critical hold, no look for the bars,
    // HP not at 0 on the screen and no death panel within a minute: resting,
    // at most a second (vitals_rest_period_ms). Any of that lost is the run's
    // pace at once, and the cut stamps paced_from_ms, so the macro waits for
    // a read taken after it -- while a bar that disagreed hands its read over.
    VitalsPart& part = VitalsPart::of(*this);
    const std::uint64_t now = now_ms();
    const int actual = vitals_.period_ms();
    if (!any_job_alive()) {
        part.note_pace(now, performance_.vitals_idle_ms, actual, VitalsPart::Pace::Idle, "");
        return performance_.vitals_idle_ms;
    }
    const VitalsRest in = part.rest_in(*this, now);
    const bool rest = vitals_rest_allowed(in, now);
    const int period = rest ? vitals_rest_period_ms(vitals_run_period(), performance_.vitals_idle_ms)
                            : vitals_run_period();
    part.note_pace(now, period, actual, rest ? VitalsPart::Pace::Rest : VitalsPart::Pace::Run, rest_why_not(in, now));
    return period;
}

std::string GuiApp::packet_vitals_text() const {
    // Any thread: the reviver's at a death panel, the black box, the HP / MP
    // tab. Copies only, each under its own lock.
    if (!capture_.running()) return "the packets: no capture running";
    const std::uint64_t now = now_ms();
    const VitalsPart::SelfNow s = VitalsPart::self_now(*this, now);
    const PacketSelf::Vitals& v = s.v;
    if (v.hp_ms == 0) return "the packets: no HP/MP frame since the capture started";
    const auto bar = [](std::uint32_t value, std::uint32_t max) {
        std::string out = std::to_string(value);
        if (max > 0) out += "/" + std::to_string(max) + " (" + percent_text(BarCheck::percent(value, max)) + ")";
        return out;
    };
    std::ostringstream out;
    out << "the packets: HP " << bar(v.hp, v.max_hp) << ", MP " << bar(v.mp, v.max_mp) << ", "
        << seconds_before(now, v.hp_ms) << " old" << (v.hp_epoch != s.epoch ? " (before a gap)" : "")
        << ", maxima from " << max_from_text(v.max_from);
    if (const VitalsPart* part = vitals_part_.get()) {
        BarCheck::State hs = BarCheck::State::Learning, ms = BarCheck::State::Learning;
        {
            std::lock_guard<std::mutex> lock(part->mutex);
            hs = part->hp.state();
            ms = part->mp.state();
        }
        const VitalsUsable hp_in = part->usable_in(*this, true, s);
        const VitalsUsable mp_in = part->usable_in(*this, false, s);
        out << "; checked against the bars: HP " << BarCheck::state_name(hs) << ", MP " << BarCheck::state_name(ms)
            << "; gone by: HP " << (vitals_usable(hp_in, now) ? "yes" : "no") << ", MP "
            << (vitals_usable(mp_in, now) ? "yes" : "no");
    }
    return out.str();
}

void GuiApp::note_vitals_minute(std::uint64_t now) {
    VitalsPart* part = vitals_part_.get();
    if (part == nullptr) return;
    BarCheck::Counts hc, mc;
    BarCheck::Progress hpg, mpg;
    {
        std::lock_guard<std::mutex> lock(part->mutex);
        hc = part->hp.take_counts();
        mc = part->mp.take_counts();
        hpg = part->hp.progress();
        mpg = part->mp.progress();
    }
    if (part->pace == VitalsPart::Pace::Rest && part->pace_ms != 0 && now > part->pace_ms) {
        part->rest_ms += now - part->pace_ms;
        part->pace_ms = now;
    }
    const std::uint64_t span = part->minute_from_ms != 0 && now > part->minute_from_ms ? now - part->minute_from_ms : 0;
    // A minute with nothing judged, no HP/MP frame and the pace not flipped
    // writes nothing: reads not judged for want of a frame say nothing new.
    const auto judged = [](const BarCheck::Counts& c) { return c.agree + c.disagree + c.between; };

    if (judged(hc) + judged(mc) + part->frames_seen + static_cast<std::uint64_t>(part->flips) > 0) {
        const auto counts = [](const BarCheck::Counts& c, bool named) {
            std::ostringstream o;
            if (named)
                o << "agree " << c.agree << " / disagree " << c.disagree << " / between " << c.between;
            else
                o << c.agree << "/" << c.disagree << "/" << c.between;
            o << " (exact " << c.exact << ", " << c.values
              << " values, lowest " << (c.lowest >= 0.0f ? percent_text(c.lowest) : std::string("-"));
            // Of the agreements, those 0x0051 reached after the read; of the
            // disagreements, those of the last one's HUD moment again.
            o << ", caught up " << c.caught_up << ", same moment " << c.same_episode;
            if (c.over_max) o << ", over the maximum " << c.over_max;
            const std::uint64_t not_judged = c.moving + c.stale + c.no_packet + c.no_maximum + c.no_figures + c.empty;
            if (not_judged)
                o << "; not judged: moving " << c.moving << ", stale " << c.stale << ", no packet " << c.no_packet
                  << ", no maximum " << c.no_maximum << ", no figures " << c.no_figures << ", empty " << c.empty;
            o << ")";
            return o.str();
        };
        const auto state = [](const BarCheck::Progress& p) {
            std::ostringstream o;
            switch (p.state) {
                case BarCheck::State::Trusted:
                    o << "trusted" << (p.since_ms != 0 ? " since " + clock_at(p.since_ms, false) : std::string());
                    break;
                case BarCheck::State::Doubt:
                    o << "in doubt (" << p.doubt_agreements << " of " << BarCheck::kDoubtClears << " in a row)";
                    break;
                case BarCheck::State::Learning:
                    o << "learning (" << p.agreements << " of " << p.needed << " agreements, " << p.values << " of "
                      << BarCheck::kTrustValues << " values)";
                    break;
                case BarCheck::State::Off:
                    o << "off for the session";
                    break;
            }
            if (p.distrusts) o << ", distrusted " << p.distrusts << "x";
            return o.str();
        };
        const auto share = [span](std::uint64_t ms) {
            return span == 0 ? std::string("-")
                             : std::to_string(static_cast<int>(std::min<std::uint64_t>(100, ms * 100 / span))) + "%";
        };
        const PacketSelf::Vitals v = packet_self_.vitals();
        std::ostringstream line;
        line << "Vitals check: HP " << counts(hc, true) << " " << state(hpg);
        if (hpg.state == BarCheck::State::Learning && !hpg.low_seen) line << ", none under 98% yet";
        line << "; MP " << counts(mc, false) << " " << state(mpg) << "; usable HP " << share(part->hp_usable_ms) << " MP "
             << share(part->mp_usable_ms) << " of the minute; the bars every " << vitals_.period_ms() << " ms (rest "
             << part->rest_ms / 1000 << " s, flips " << part->flips << "); last HP/MP frame "
             << (v.hp_ms != 0 ? seconds_before(now, v.hp_ms) + " ago" : std::string("never")) << "; maxima from "
             << max_from_text(v.max_from) << "; HP/MP frames " << part->frames_seen << " (" << part->same_ms
             << " in the same millisecond as the one before, " << part->same_ms_mp << " of them another MP)";
        if (packets_settings_.vitals == PacketUse::Off) line << "; [Packets] vitals=off: the screen decides";
        Logger::instance().write(LogLevel::Info, line.str());
    }
    if (part->loads > 0) {
        char held[32];
        std::snprintf(held, sizeof held, "%.1f s", static_cast<double>(part->held_ms) / 1000.0);
        Logger::instance().write(LogLevel::Info, "World: map loads by packets " + std::to_string(part->loads) +
                                                     " (held " + held + " in all), the HUD out first " +
                                                     std::to_string(part->hud_first));
    }

    part->minute_from_ms = now;
    part->hp_usable_ms = 0;
    part->mp_usable_ms = 0;
    part->frames_seen = 0;
    part->same_ms = 0;
    part->same_ms_mp = 0;
    part->hp_dis_said = 0;
    part->mp_dis_said = 0;
    part->rest_ms = 0;
    part->flips = 0;
    part->loads = 0;
    part->hud_first = 0;
    part->held_ms = 0;
}

} // namespace gobot
