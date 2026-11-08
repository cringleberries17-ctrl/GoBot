#include "gobot/packet_eyes.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_place.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace gobot {

// A walk held from one look to the next is let go of and taken up again --
// a stop and a start report -- before the packets stop following it; and a
// step back, held 800 ms at most and the turn's tap more (Hunter::kTurnTapMs),
// is followed as long as it was before.
static_assert(Hunter::kWalkRenewMs + Hunter::kWalkRenewGapMs + 200 <= PacketWorld::kOwnReckonMs,
              "a held walk is renewed well inside the time a walk is followed with no report");
static_assert(PacketWorld::kOwnStepReckonMs + PacketHealth::kReportLateMs >= 800 + Hunter::kTurnTapMs,
              "the longest step back stays on the packets until its stop comes");

namespace {

/// How long ago `at` was at `now`. A frame can be stamped a moment after the
/// pass read its clock -- the capture thread runs beside it -- and is then
/// no time ago, not 584 million years.
std::uint64_t age(std::uint64_t at, std::uint64_t now) { return now > at ? now - at : 0; }

/// The middle of `v`: the mean of the two middle ones for an even count.
float median_of(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return v.size() % 2 == 1 ? v[mid] : (v[mid - 1] + v[mid]) / 2.0f;
}

/// The least-squares slope of world dx against screen dx over `pairs`, once
/// kScalePairs of them span kScaleSpanPx of the screen. With an intercept: a
/// constant offset between a plate's middle and its monster's point is not a
/// scale.
std::optional<double> scale_of(const std::vector<ViewComparison::Pair>& pairs) {
    if (pairs.size() < static_cast<std::size_t>(ViewComparison::kScalePairs)) return std::nullopt;
    int lo = pairs.front().screen_dx, hi = lo;
    double sum_s = 0.0, sum_w = 0.0;
    for (const ViewComparison::Pair& p : pairs) {
        lo = std::min(lo, p.screen_dx);
        hi = std::max(hi, p.screen_dx);
        sum_s += p.screen_dx;
        sum_w += static_cast<double>(p.world_dx);
    }
    if (hi - lo < ViewComparison::kScaleSpanPx) return std::nullopt;
    const double n = static_cast<double>(pairs.size());
    const double mean_s = sum_s / n, mean_w = sum_w / n;
    double ss = 0.0, sw = 0.0;
    for (const ViewComparison::Pair& p : pairs) {
        const double ds = p.screen_dx - mean_s;
        ss += ds * ds;
        sw += ds * (static_cast<double>(p.world_dx) - mean_w);
    }
    if (ss <= 0.0) return std::nullopt;
    return sw / ss;
}

/// A monster list -- Lọc quái -- has a say: its names are read off the
/// plates, and the packets carry none.
bool lists_in_use(const HunterConfig& cfg) {
    return !cfg.skip_names.empty() || !cfg.first_names.empty() || cfg.first_only;
}

/// Reasons that watching cannot get past either: said in Watch in place of
/// "watching only", since ticking the box would not help.
bool hard_reason(PacketWhy why) {
    return why == PacketWhy::NoNpcap || why == PacketWhy::NoCapture || why == PacketWhy::NoTraffic;
}

// The rules packet_health and world_fix share: the capture's, and yours.

/// The capture first: nothing past it means anything without one. InUse
/// when it has the game's traffic; otherwise why not.
PacketWhy capture_why(const PacketLink& link, std::uint64_t now) {
    if (!link.npcap) return PacketWhy::NoNpcap;
    if (!link.capturing) return PacketWhy::NoCapture;
    if (link.last_tcp_ms == 0 || age(link.last_tcp_ms, now) > PacketHealth::kTcpQuietMs) return PacketWhy::NoTraffic;
    return PacketWhy::InUse;
}

/// Another character's report a moment ago: a second client on this PC.
bool foreign_lately(const PacketWorld::Stats& s, std::uint64_t now) {
    return s.last_foreign_ms != 0 && age(s.last_foreign_ms, now) < PacketHealth::kForeignMs;
}

/// You, where the packets put you, not to be gone by. Not known, or moving
/// with no facing to follow: at once. A walk or a jump followed past its cap
/// -- the view's own_follow_ms on the ground: 3 s for a walk the hunter
/// holds, 0.6 s for any other -- is held where the working-out stopped
/// (uncertain); but the report that ends it comes a moment after the key is
/// up, so only once that report is kReportLateMs late. That moment matters at
/// a step back held 860 ms, past the 0.6 s of a walk the hunter does not
/// hold; at the landing of a jump past kAirReckonMs; and at a held walk let
/// go of near its 3 s.
bool you_lost(const PacketWorld::View& v, std::uint64_t now) {
    if (!v.me.known || (v.me.moving && v.me.facing == 0)) return true;
    if (v.me.uncertain) {
        const std::uint64_t followed = v.me.airborne ? PacketWorld::kAirReckonMs : v.own_follow_ms;
        if (age(v.me.report_ms, now) > followed + PacketHealth::kReportLateMs) return true;
    }
    return false;
}

} // namespace

Nameplate packet_plate(int x, int y) {
    // As the tests' plates are built (hunter_test.cpp): the bar's top at y,
    // and the name box above it centred on x -- so x() and y() are the point.
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    return n;
}

const char* packet_why_name(PacketWhy why) {
    switch (why) {
        case PacketWhy::InUse:      return "the packets steer";
        case PacketWhy::Off:        return "packet_positions=off";
        case PacketWhy::Watch:      return "watching only (packet_positions=watch)";
        case PacketWhy::NoNpcap:    return "Npcap is not installed, or its service is not running";
        case PacketWhy::NoCapture:  return "no capture is running";
        case PacketWhy::NoTraffic:  return "no game TCP for 3 s";
        case PacketWhy::ListInUse:  return "a monster list is in use, and the packets carry no names";
        case PacketWhy::NotMoved:   return "no report of yours since the capture began (walk or cast once)";
        case PacketWhy::Foreign:    return "another character's reports within 30 s";
        case PacketWhy::WrongSize:  return "frames of an unexpected size within 60 s";
        case PacketWhy::MapChanged: return "the map just changed, and its monsters are not settled";
        case PacketWhy::NoMonsters: return "no monster frames yet, or not for 6 s";
        case PacketWhy::Uncertain:  return "your position is uncertain: a move with no report for too long";
        case PacketWhy::Settling:   return "healthy, but not yet for 2 s";
    }
    return "?";
}

std::string position_line_vi(PacketWhy why) {
    const char* reason = "";
    switch (why) {
        case PacketWhy::InUse:      return "Vị trí: từ gói tin";
        case PacketWhy::Off:        reason = "đã tắt vị trí từ gói tin"; break;
        case PacketWhy::Watch:      reason = "gói tin chỉ được theo dõi (chưa đánh dấu Vị trí từ gói tin)"; break;
        case PacketWhy::NoNpcap:    reason = "chưa cài Npcap, hoặc dịch vụ Npcap chưa chạy"; break;
        case PacketWhy::NoCapture:
            reason = "GoBot không bắt gói tin (chưa bật được, hoặc đã bấm Stop capture)";
            break;
        case PacketWhy::NoTraffic:  reason = "đang bắt gói tin nhưng không thấy gói tin của game"; break;
        case PacketWhy::ListInUse:  reason = "đang dùng Lọc quái, mà gói tin không có tên quái"; break;
        case PacketWhy::NotMoved:
            reason = "nhân vật chưa di chuyển từ lúc bắt gói tin (đi một bước hoặc dùng một kỹ năng)";
            break;
        case PacketWhy::Foreign:    reason = "thấy gói tin của một nhân vật khác"; break;
        case PacketWhy::WrongSize:  reason = "gói tin khác thường, có thể game vừa cập nhật"; break;
        case PacketWhy::MapChanged: reason = "vừa đổi bản đồ"; break;
        case PacketWhy::NoMonsters: reason = "chưa đủ gói tin về quái quanh đây"; break;
        case PacketWhy::Uncertain:  reason = "nhân vật đang đi mà gói tin chưa báo vị trí"; break;
        case PacketWhy::Settling:   reason = "đang chờ gói tin ổn định"; break;
    }
    return std::string("Vị trí: từ màn hình — ") + reason;
}

PacketHealth packet_health(const PacketWorld::View& v, const PacketWorld::Stats& s, const PacketLink& link,
                           const HunterConfig& cfg, std::uint64_t now) {
    const auto no = [](PacketWhy why) {
        PacketHealth h;
        h.why = why;
        return h;
    };
    if (cfg.packet_positions == PacketPositions::Off) return no(PacketWhy::Off);
    if (cfg.packet_positions == PacketPositions::Watch) return no(PacketWhy::Watch);

    // The capture first: nothing past it means anything without one.
    const PacketWhy capture = capture_why(link, now);
    if (capture != PacketWhy::InUse) return no(capture);

    if (lists_in_use(cfg)) return no(PacketWhy::ListInUse);

    // Your position only from a report of yours: a map entry alone says where
    // the server put you, not that the client's reports reach the capture --
    // a filter without its UDP ports would leave you standing at the spawn.
    if (s.last_own_ms == 0) return no(PacketWhy::NotMoved);
    if (foreign_lately(s, now)) return no(PacketWhy::Foreign);
    if (s.last_wrong_size_ms != 0 && age(s.last_wrong_size_ms, now) < PacketHealth::kWrongSizeMs)
        return no(PacketWhy::WrongSize);

    // A map entry clears every monster; the table puts them all back within
    // about a second, or each slot's own events within a few. Begun mid-map,
    // with neither, the slots come one event at a time.
    if (!v.table_seen) {
        if (v.map_since_ms != 0) {
            if (age(v.map_since_ms, now) < PacketHealth::kSettleMs) return no(PacketWhy::MapChanged);
        } else if (v.first_monster_ms == 0 || age(v.first_monster_ms, now) < PacketHealth::kSettleMs) {
            return no(PacketWhy::NoMonsters);
        }
    }

    // You, where the packets put you: the screen once that is not to be gone
    // by (you_lost).
    if (you_lost(v, now)) return no(PacketWhy::Uncertain);

    PacketHealth ok;
    ok.ok = true;
    ok.why = PacketWhy::InUse;
    return ok;
}

WorldFix world_fix(const PacketWorld::View& v, const PacketWorld::Stats& s, const PacketLink& link,
                   std::uint64_t now) {
    WorldFix f;
    f.x = v.me.x;
    f.y = v.me.y;
    f.rx = v.me.report_x;
    f.ry = v.me.report_y;
    f.report_ms = v.me.report_ms;
    f.facing = v.me.said_facing;
    f.facing_ms = v.me.facing_ms;
    f.airborne = v.me.airborne;
    f.moving = v.me.moving;
    f.uncertain = v.me.uncertain;
    f.map_known = v.map_since_ms != 0;
    f.map = v.map;
    f.area = v.area;
    f.epoch = v.map_epoch;
    // From the view, which keeps it through a capture started again as it
    // keeps the epoch: the stats forget it, and an entry first looked at
    // after such a start would be measured from 0,0.
    f.entry_x = v.entry_x;
    f.entry_y = v.entry_y;
    // packet_health's capture and "you" rules, in its order. A frame of a
    // strange size only when it is one of your reports': a monster's or the
    // map's says nothing of where you are, and a minute of standing for one
    // would be a minute lost -- but a report of yours of another size is a
    // walk of yours unread, the fix standing where it began.
    const bool own_wrong_size =
        s.last_own_wrong_size_ms != 0 && age(s.last_own_wrong_size_ms, now) < PacketHealth::kWrongSizeMs;
    if (capture_why(link, now) != PacketWhy::InUse) f.state = WorldFix::State::NoCapture;
    else if (s.last_own_ms == 0)                     f.state = WorldFix::State::FirstReport;
    else if (foreign_lately(s, now) || own_wrong_size || you_lost(v, now)) f.state = WorldFix::State::Lost;
    else                                             f.state = WorldFix::State::Known;
    return f;
}

std::vector<MapTrust::Mob> packet_stands(const PacketWorld::View& v, std::size_t max) {
    // The monsters where their frame put them: a stop's speed is 0, a walk's
    // and a blow's are not, and a walk run out keeps its speed with a place
    // worked out. A spawn's is 0 too, 9-36 px above its floor: MapTrust takes
    // a monster's place only once two of its frames -- the same monster, by
    // its generation -- agree.
    std::vector<MapTrust::Mob> out;
    for (const PacketWorld::Mob& m : v.mobs) {
        if (out.size() >= max) break;
        if (m.dead || m.moving || m.speed > 0.0f) continue;
        MapTrust::Mob st;
        st.at = { static_cast<int>(std::lround(m.x)), static_cast<int>(std::lround(m.y)) };
        st.slot = m.slot;
        st.at_ms = m.event_ms;
        st.gen = m.gen;
        out.push_back(st);
    }
    return out;
}

// ---------------------------------------------------------------- the eyes

PacketEyes::PacketEyes(const PacketWorld& world, Screen screen, Inputs inputs)
    : world_(world), screen_(std::move(screen)), inputs_(std::move(inputs)) {}

Hunter::Eyes PacketEyes::eyes() {
    Hunter::Eyes e;
    e.frame = [this] { return frame(); };
    e.player = [this](Nameplate& out, bool* fresh) { return player(out, fresh); };
    e.mobs = [this] { return mobs(); };
    e.view = [this] { return view(); };
    e.where = [this](WorldFix& out) { return where(out); };
    // For the hunter's drops: the monsters' stops, and your take-offs.
    e.stands = [this] { return stands(); };
    e.takeoffs = inputs_.takeoffs;
    // For its jumps up (2026.11.05b): your take-offs by kind, your reports.
    e.air = inputs_.air;
    e.own_since = inputs_.own_since;
    // For "Tự tìm quái trong vùng": the whole map's monsters, placed.
    e.world = [this](std::vector<SeekMob>& out) { return world(out); };
    return e;
}

bool PacketEyes::world(std::vector<SeekMob>& out) {
    out.clear();
    if (source_.load() != Source::Packets || handing_over_.load()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // No floors from the map tab (not handed over yet, or its file doubtful):
    // every monster would be on no floor, and the seek would stand. Off
    // instead -- the hunter as 2026.11.05 and its jumps up.
    if (!seek_floors_) return false;
    out = seek_world_;
    return true;
}

PacketEyes::PlateCounts PacketEyes::plates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plates_;
}

bool PacketEyes::where(WorldFix& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = fix_;
    return fix_on_;
}

std::vector<MapTrust::Mob> PacketEyes::stands() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stands_;
}

std::uint64_t PacketEyes::clock() const { return inputs_.now ? inputs_.now() : now_ms(); }

// A hand-over to the screen is over long before the packets could steer again.
static_assert(PacketEyes::kHandOverMs < PacketEyes::kHealthyForMs,
              "a hand-over ends before the packets have been healthy long enough to take over again");

std::uint64_t PacketEyes::frame() {
    // Asked before the lock: the config is the hunter's, under its own lock,
    // and the capture's stats under the capture's. With the packets off,
    // nothing of theirs is asked at all.
    const std::uint64_t now = clock();
    const HunterConfig cfg = inputs_.config ? inputs_.config() : HunterConfig{};
    const bool watched = cfg.packet_positions != PacketPositions::Off;
    const PacketLink link = watched && inputs_.link ? inputs_.link() : PacketLink{};
    int w = 0, h = 0;
    const bool sized = watched && inputs_.view_size && inputs_.view_size(w, h) && w > 0 && h > 0;
    // The detector's count of pictures, under its own lock, at every pass
    // whatever steers: a switch back to the screen goes by how long ago it
    // last moved on, and a hand-over by its moving on since (kHandOverMs).
    const std::uint64_t pictures = screen_.frame ? screen_.frame() : 0;
    // The map file the monsters are placed on, under the GUI's own lock.
    const std::shared_ptr<const GameMapBook::Page> floors = watched && inputs_.floors ? inputs_.floors() : nullptr;

    bool from_packets = false;
    std::uint64_t number = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The time since the last pass goes to what it was spent on. A gap
        // longer than a second is hunting stopped and started again.
        if (passed_ && now > last_pass_ms_) {
            const std::uint64_t spent = std::min<std::uint64_t>(now - last_pass_ms_, 1000);
            if (source_.load() == Source::Packets) packets_ms_ += spent;
            else                                   screen_ms_ += spent;
            if (was_healthy_) healthy_ms_ += spent;
        }
        passed_ = true;
        last_pass_ms_ = now;
        if (sized) {
            box_w_ = w;
            box_h_ = h;
        }
        if (pictures != pictures_ || picture_new_ms_ == 0) {
            if (picture_new_ms_ != 0 && pictures != pictures_) picture_gap_ms_ = age(picture_new_ms_, now);
            pictures_ = pictures;
            picture_new_ms_ = now;
        }

        // One view of the world for the whole pass: you and the monsters from
        // the same moment, whatever the pass asks for after this. Judged as if
        // the packets were to steer, so that a watching hunt says how often
        // they could have.
        PacketWorld::View v;
        PacketHealth health;
        // Where you are in the world (world_fix), for the hunting spot: from
        // the same view, the packets steering or only watched.
        fix_ = WorldFix{};
        fix_on_ = watched;
        stands_.clear();
        if (watched) {
            // A walk of yours the hunter holds is followed 3 s with no report;
            // any other, 0.6 s, as every walk was before the held walk.
            const bool held = inputs_.walk_held_until && now <= inputs_.walk_held_until();
            v = world_.view(now, held ? PacketWorld::kOwnReckonMs : PacketWorld::kOwnStepReckonMs);
            HunterConfig judged = cfg;
            judged.packet_positions = PacketPositions::On;
            const PacketWorld::Stats s = world_.stats();
            health = packet_health(v, s, link, judged, now);
            fix_ = world_fix(v, s, link, now);
            stands_ = packet_stands(v, kStandsMax);
        }
        was_healthy_ = health.ok;

        // Back to the screen at once; on to the packets only once they have
        // been healthy for kHealthyForMs -- a switch costs the hunter what it
        // held by position, and one at every flicker would cost it every fight.
        const bool steer = health.ok && cfg.packet_positions == PacketPositions::On;
        if (!steer) {
            healthy_ = false;
            if (source_.load() == Source::Packets) {
                source_ = Source::Screen;
                ++switches_;
                // The detector may have been resting while the packets
                // steered, and its last picture be as old as its rest: the
                // hunter keeps the packets' last plates, in their view, until
                // it has one taken since (kHandOverMs). With pictures at its
                // own pace, the screen's plates at once, in another view. While
                // it rests, at once only with a picture no older than one at
                // its own pace would be: a walk moves the plates on meanwhile.
                const std::uint64_t fresh = picture_gap_ms_ > kScreenFreshMs
                                                ? static_cast<std::uint64_t>(MobDetector::kLookGapMs)
                                                : kScreenFreshMs;
                const bool hand_over = age(picture_new_ms_, now) > fresh;
                if (hand_over) {
                    handing_over_ = true;
                    handover_from_ms_ = now;
                    handover_pictures_ = pictures;
                    ++handovers_;
                } else {
                    ++view_;
                }
                const PacketWhy reason = cfg.packet_positions == PacketPositions::On      ? health.why
                                         : cfg.packet_positions == PacketPositions::Watch ? PacketWhy::Watch
                                                                                          : PacketWhy::Off;
                GB_INFO("Positions: from the screen now -- %s%s", packet_why_name(reason),
                        hand_over ? "; the packets' last plates kept until the detector's next picture" : "");
            }
        } else {
            if (!healthy_) {
                healthy_ = true;
                healthy_since_ms_ = now;
            }
            if (source_.load() == Source::Screen && age(healthy_since_ms_, now) >= kHealthyForMs) {
                source_ = Source::Packets;
                ++view_;
                ++switches_;
                GB_INFO("Positions: from the packets now, healthy for %llu ms",
                        static_cast<unsigned long long>(age(healthy_since_ms_, now)));
            }
        }

        // A hand-over ends at the detector's count two past its count at the
        // switch -- a picture taken wholly after it -- or kHandOverMs on: the
        // screen's plates from this pass, in another view.
        if (handing_over_.load()) {
            const bool newer = pictures > handover_pictures_ + 1;
            const std::uint64_t held = age(handover_from_ms_, now);
            if (newer || held >= kHandOverMs) {
                handing_over_ = false;
                ++view_;
                handover_ms_ += held;
                if (!newer) ++handovers_capped_;
                if (newer)
                    GB_INFO("Positions: the screen's plates from now -- the detector's first picture since the "
                            "switch, %llu ms after it", static_cast<unsigned long long>(held));
                else
                    GB_INFO("Positions: the screen's plates from now -- no picture since the switch came within "
                            "%llu ms", static_cast<unsigned long long>(kHandOverMs));
            }
        }

        PacketWhy reason = health.why;
        if (source_.load() == Source::Packets)                    reason = PacketWhy::InUse;
        else if (cfg.packet_positions == PacketPositions::Off)    reason = PacketWhy::Off;
        else if (cfg.packet_positions == PacketPositions::Watch)  reason = hard_reason(health.why) ? health.why
                                                                                                    : PacketWhy::Watch;
        else if (health.ok)                                       reason = PacketWhy::Settling;
        why_ = reason;

        if (source_.load() == Source::Packets) {
            make_plates(v, cfg, now, floors.get());
            from_packets = true;
            number = v.updates;
            packet_number_ = number;
        } else if (handing_over_.load()) {
            // The packets' last plates as they were, and their number: no new
            // picture for the hunter until the screen has a fresh one.
            from_packets = true;
            number = packet_number_;
        } else {
            // The screen's: no whole map to seek in, no plates of the packets.
            seek_world_.clear();
            seek_floors_ = false;
            plates_ = PlateCounts{};
        }
    }
    // The screen's own count, outside the lock: the detector's, under its own.
    if (from_packets) return number;
    return screen_.frame ? screen_.frame() : 0;
}

void PacketEyes::make_plates(const PacketWorld::View& v, const HunterConfig& cfg, std::uint64_t now,
                             const GameMapBook::Page* floors) {
    // World pixels laid over the screen's: packet_scale over both ways, and
    // packet_dy on the monsters' heights against you. The same for you and
    // for them, so a monster standing still stays where it is as you walk,
    // and the hunter's following it from one look to the next holds.
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const auto px = [scale](double world) { return static_cast<int>(std::lround(world / scale)); };
    me_ = packet_plate(px(v.me.x), px(v.me.y));
    me_.is_player = true;
    me_.is_self = true;
    me_.name = cfg.character_name;
    me_fresh_ = !v.me.uncertain;

    // About what the screen would show, as the player chose: the packets
    // name every monster of the map, and the hunter's "nearest with health
    // anywhere" would walk it end to end.
    const double half_w = kBoxFraction * box_w_;
    const double half_h = kBoxFraction * box_h_;
    // A living monster never reads as finished: "finished" is the hunter's
    // word for killed, and here a kill is known, not guessed from a bar. Its
    // HP is laid between just above dead_below and full rather than held up
    // at that floor: held there, the last few percent would never move the
    // bar, and with a Kiểu đánh a monster nearly dead would read as one the
    // attacks do not hurt (no_damage_ms) and be given up on. Full stays full,
    // and the order -- which is the most hurt -- stays as it was.
    const float alive_floor = std::min(1.0f, cfg.dead_below + 0.01f);
    mobs_.clear();
    seek_world_.clear();
    plates_ = PlateCounts{};
    plates_.half_w = static_cast<int>(std::lround(half_w));
    plates_.half_h = static_cast<int>(std::lround(half_h));
    plates_.floors = kPlatesOnFloors && floors != nullptr;
    seek_floors_ = floors != nullptr;
    for (const PacketWorld::Mob& m : v.mobs) {
        float hp = 0.0f;
        if (m.dead) {
            // At 0 a moment, so the hunter sees its target finished, counts
            // it and picks up after it; then gone, until its slot's spawn.
            if (age(m.dead_ms, now) > kDeadShownMs) continue;
        } else {
            const float left =
                m.max_hp > 0 ? std::min(1.0f, static_cast<float>(m.hp) / static_cast<float>(m.max_hp)) : 1.0f;
            // Worked from the top, so a monster at full reads exactly 1: the
            // hunter takes anything under 1 for wounded.
            hp = 1.0f - (1.0f - alive_floor) * (1.0f - left);
        }
        // Where it stands, by the map's file (mob_place): on its floor, a walk
        // run out followed on -- the seek's whole map always, the plates with
        // kPlatesOnFloors (2026.11.05b); a killed one where the blow found it.
        // Another player's blows, within the seek's own while (Q6).
        MapDot at;
        at.x = m.x;
        at.y = m.y;
        if (!m.dead) {
            ++plates_.alive;
            at = mob_place(floors, m, now, true);
            SeekMob s;
            s.slot = static_cast<std::int32_t>(m.slot);
            s.gen = m.gen;
            s.at = { static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y)) };
            s.floor_y = at.top;
            s.why = static_cast<std::uint8_t>(at.why);
            s.frame_age_ms = age(m.event_ms, now);
            s.hp = hp;
            s.hurt_by_other = v.actor_checked && m.hurt_by_other_ms > m.hurt_by_you_ms &&
                              age(m.hurt_by_other_ms, now) < kSeekOthersHurtMs;
            seek_world_.push_back(s);
        }
        const bool placed = plates_.floors && !m.dead;
        const int x = placed ? px(at.x) : px(static_cast<double>(m.x));
        const int y = (placed ? px(at.y) : px(static_cast<double>(m.y))) + cfg.packet_dy;
        if (std::abs(x - me_.x()) > half_w || std::abs(y - me_.y()) > half_h) continue;
        if (!m.dead) {
            ++plates_.in_view;
            if (placed && (at.why == DotWhy::Followed || at.why == DotWhy::Held || at.why == DotWhy::Projected))
                ++plates_.walking;
            if (placed && at.why == DotWhy::Projected) ++plates_.projected;
            if (placed && at.why == DotWhy::SetDown) ++plates_.set_down;
        }
        Nameplate n = packet_plate(x, y);
        n.hp = hp;
        // Which monster it is, for the hunter's hold on its target: its slot
        // and the slot's generation, which a respawn changes. And its blows:
        // when you last hurt it, and whether another player has since --
        // within kOthersHurtMs, and only once your id has been proven the
        // hitter's (View::actor_checked).
        constexpr std::uint64_t kOthersHurtMs = 5000;
        n.slot = static_cast<std::int32_t>(m.slot);
        n.gen = m.gen;
        n.hurt_ms = m.hurt_by_you_ms;
        n.actor_checked = v.actor_checked;
        n.hurt_by_other = v.actor_checked && m.hurt_by_other_ms > m.hurt_by_you_ms &&
                          age(m.hurt_by_other_ms, now) < kOthersHurtMs;
        mobs_.push_back(std::move(n));
    }
    MobDetector::nearest_first(mobs_, me_.x(), me_.y());
}

bool PacketEyes::player(Nameplate& out, bool* fresh) {
    // Through a hand-over, the packets' last plate of you: where you were a
    // moment ago, not seen just now.
    const bool handing = handing_over_.load();
    if (source_.load() == Source::Packets || handing) {
        std::lock_guard<std::mutex> lock(mutex_);
        out = me_;
        if (fresh) *fresh = me_fresh_ && !handing;
        return true;
    }
    if (screen_.player) return screen_.player(out, fresh);
    if (fresh) *fresh = false;
    return false;
}

std::vector<Nameplate> PacketEyes::mobs() {
    if (source_.load() == Source::Packets || handing_over_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_;
    }
    return screen_.mobs ? screen_.mobs() : std::vector<Nameplate>{};
}

void PacketEyes::restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    healthy_ = false;
    was_healthy_ = false;
    passed_ = false;
    mobs_.clear();
    // Another frame of reference than the hunter may last have seen: moved,
    // so a hunter that holds anything from before lets go of it -- the
    // packets' plates kept through a hand-over among them.
    const bool handing = handing_over_.exchange(false);
    if (source_.load() == Source::Packets || handing) {
        source_ = Source::Screen;
        ++view_;
    }
}

PacketEyes::Stats PacketEyes::stats() const {
    Stats s;
    s.packets_ms = packets_ms_.load();
    s.screen_ms = screen_ms_.load();
    s.healthy_ms = healthy_ms_.load();
    s.switches = switches_.load();
    s.handovers = handovers_.load();
    s.handovers_capped = handovers_capped_.load();
    s.handover_ms = handover_ms_.load();
    return s;
}

// ------------------------------------------------------ the detector's pace

// While it rests, your plate is looked for every kRestMs, and a look's time
// on top: remembered longer than that (MobDetector::player), it is not lost
// between two looks -- which is what the first assert proves. "Cannot find"
// waits as many looks as it did at kLookGapMs (MobDetector::sightings_grace_ms),
// not as many seconds.
static_assert(MobDetector::kPlayerMemoryMs > static_cast<std::uint64_t>(DetectorPace::kRestMs) + 400,
              "your plate is remembered across the gap between a resting detector's looks");
static_assert(DetectorPace::kRunMs == MobDetector::kLookGapMs && DetectorPace::kRestMs > DetectorPace::kRunMs &&
                  DetectorPace::kRestMs <= MobDetector::kMaxPeriodMs,
              "the detector rests at a longer wait than its own, and one it can be asked for");

bool detector_rest(const DetectorPace& p, const char** why) {
    const char* no = "";
    if (!p.hunting)
        no = "no hunt";
    else if (p.source != PacketEyes::Source::Packets || p.why != PacketWhy::InUse)
        no = "the screen steers again";
    else if (p.jump_test)
        no = "a jump key's Thử watches your plate";
    else if (p.monster_look)
        no = "Quái đang thấy reads the plates";
    else if (p.camera_panels)
        no = "the Radar or the movement panels are open";
    else if (p.walking)
        no = "a walk follows the camera";
    else if (p.rest_off)
        no = "detector_rest=off in app.ini [Packets]";
    if (why) *why = no;
    return *no == '\0';
}

// ------------------------------------------------------- watching the two

ViewComparison compare_views(const PacketWorld::View& v, std::uint64_t now, const Nameplate& me,
                             const std::vector<Nameplate>& plates, const ScreenRect& seen) {
    ViewComparison c;
    const PacketWorld::Me& you = v.me;
    // Only while you stand settled: a walk's or a jump's position is worked
    // out, not reported, and the screen's view may still be catching up with
    // a move just ended.
    if (!you.known || you.moving || you.airborne || you.uncertain) return c;
    if (age(you.report_ms, now) < ViewComparison::kSettledMs) return c;
    c.valid = true;
    c.offset_x = you.x - static_cast<float>(me.x());
    c.offset_y = you.y - static_cast<float>(me.y());

    // Walking monsters are left out of the pairs: their y is a guess between
    // events, 60-69 px out on a slope. They still count as being somewhere.
    std::vector<const PacketWorld::Mob*> standing, living;
    for (const PacketWorld::Mob& m : v.mobs) {
        if (m.dead) continue;
        living.push_back(&m);
        if (!m.moving) standing.push_back(&m);
    }
    const auto world_x = [&c](const Nameplate& n) { return static_cast<float>(n.x()) + c.offset_x; };
    const auto world_y = [&c](const Nameplate& n) { return static_cast<float>(n.y()) + c.offset_y; };

    // Every plate and standing monster near enough, then one to one, nearest
    // first: two plates side by side cannot both take the same monster.
    // (Not called "near": windows.h makes that word mean nothing.)
    struct Candidate {
        std::size_t plate;
        std::size_t mob;
        float       d;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < plates.size(); ++i) {
        const float wx = world_x(plates[i]), wy = world_y(plates[i]);
        for (std::size_t j = 0; j < standing.size(); ++j) {
            const float d = std::hypot(standing[j]->x - wx, standing[j]->y - wy);
            if (d <= ViewComparison::kPairPx) candidates.push_back(Candidate{ i, j, d });
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    std::vector<char> plate_taken(plates.size(), 0), mob_taken(standing.size(), 0);
    for (const Candidate& n : candidates) {
        if (plate_taken[n.plate] || mob_taken[n.mob]) continue;
        plate_taken[n.plate] = 1;
        mob_taken[n.mob] = 1;
        ViewComparison::Pair p;
        p.slot = standing[n.mob]->slot;
        p.screen_dx = plates[n.plate].x() - me.x();
        p.screen_dy = plates[n.plate].y() - me.y();
        p.world_dx = standing[n.mob]->x - you.x;
        p.world_dy = standing[n.mob]->y - you.y;
        c.pairs.push_back(p);
    }

    // What one sees and the other does not.
    for (std::size_t i = 0; i < plates.size(); ++i) {
        if (plate_taken[i]) continue;
        const float wx = world_x(plates[i]), wy = world_y(plates[i]);
        bool any = false;
        for (const PacketWorld::Mob* m : living)
            if (std::hypot(m->x - wx, m->y - wy) <= ViewComparison::kPairPx) any = true;
        if (!any) ++c.screen_only;
    }
    for (std::size_t j = 0; j < standing.size(); ++j) {
        if (mob_taken[j]) continue;
        const long sx = std::lround(standing[j]->x - c.offset_x);
        const long sy = std::lround(standing[j]->y - c.offset_y);
        if (seen.contains(static_cast<int>(sx), static_cast<int>(sy))) ++c.packets_only;
    }

    std::vector<float> rx, ry;
    for (const ViewComparison::Pair& p : c.pairs) {
        rx.push_back(p.residual_x());
        ry.push_back(p.residual_y());
    }
    c.median_x = median_of(rx);
    c.median_y = median_of(ry);
    c.scale = scale_of(c.pairs);
    return c;
}

ViewSummary summarize_views(const std::vector<ViewComparison>& looks) {
    ViewSummary s;
    s.looks = static_cast<int>(looks.size());
    std::vector<ViewComparison::Pair> all;
    for (const ViewComparison& c : looks) {
        if (!c.valid) continue;
        ++s.compared;
        all.insert(all.end(), c.pairs.begin(), c.pairs.end());
        s.screen_only += c.screen_only;
        s.packets_only += c.packets_only;
    }
    s.pairs = static_cast<int>(all.size());
    if (!all.empty()) {
        std::vector<float> rx, ry;
        for (const ViewComparison::Pair& p : all) {
            rx.push_back(p.residual_x());
            ry.push_back(p.residual_y());
        }
        s.median_x = median_of(rx);
        s.median_y = median_of(ry);
        std::sort(ry.begin(), ry.end());
        const auto centile = [&ry](double q) {
            const double at = q * static_cast<double>(ry.size() - 1);
            return ry[static_cast<std::size_t>(std::lround(at))];
        };
        s.spread_y = centile(0.9) - centile(0.1);
    }
    s.scale = scale_of(all);
    return s;
}

// ---------------------------------------------------- keeping it attached

bool should_reattach(const GameLink& before, const GameLink& now, std::uint64_t tcp_silent_ms,
                     std::uint64_t since_try_ms, const char** why) {
    if (why) *why = nullptr;
    const auto because = [why](const char* reason) {
        if (why) *why = reason;
        return true;
    };
    if (since_try_ms < GameLink::kReattachNewMs) return false;
    // No game, or a game with no connection -- its login pages, a reconnect
    // under way: nothing to build a capture from yet.
    if (now.pid == 0 || now.servers.empty()) return false;
    // A new game, or a new connection, a second after the last start: the
    // map entry that comes with it is the one look at the map there is.
    if (now.pid != before.pid) return because("the game's process changed");
    // Only what is new: a connection or a socket gone is still in the filter,
    // where it does no harm, and the game's own come and go.
    for (const Endpoint& e : now.servers)
        if (std::find(before.servers.begin(), before.servers.end(), e) == before.servers.end())
            return because("the game has a connection the capture was not built with");
    // The rest no sooner than kReattachEveryMs: a UDP socket opened later
    // costs nothing but the reports it carries, and a quiet TCP may only be
    // quiet.
    if (since_try_ms < GameLink::kReattachEveryMs) return false;
    for (const std::uint16_t port : now.udp_ports)
        if (std::find(before.udp_ports.begin(), before.udp_ports.end(), port) == before.udp_ports.end())
            return because("the game has a UDP port the capture was not built with");
    if (tcp_silent_ms >= GameLink::kReattachSilentMs) return because("no game TCP for 5 s");
    return false;
}

} // namespace gobot
