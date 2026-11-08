#include "gobot/map_view.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>

namespace gobot {
namespace {

int round_px(float v) { return static_cast<int>(std::lround(v)); }
int round_px(double v) { return static_cast<int>(std::lround(v)); }

} // namespace

KnownMap map_identity(const WorldFix& f, const MapTrust::Report& r, std::uint64_t trust_gen) {
    KnownMap id;
    id.epoch = f.epoch;
    id.trust_gen = trust_gen;
    id.report = r;
    if (f.state == WorldFix::State::NoCapture) return id;
    if (f.map_known) {
        // The entry names the place whatever is thought of its file.
        id.stem = zone_stem(GameMapBook::stem_of(f.map, f.area));
        id.by = id.stem.empty() ? KnownMap::By::None : KnownMap::By::Entry;
        id.file_doubtful = r.why == MapTrust::Why::Mismatch || r.why == MapTrust::Why::Distrusted;
        return id;
    }
    if (r.named && (r.why == MapTrust::Why::Trusted || r.why == MapTrust::Why::NoGates)) {
        id.stem = zone_stem(r.stem);
        id.by = id.stem.empty() ? KnownMap::By::None : KnownMap::By::Named;
    }
    return id;
}

KnownMap usable_identity(const KnownMap& id, std::uint64_t trust_gen_now, bool lose_epoch_on,
                         std::uint32_t lose_epoch, std::uint32_t live_epoch) {
    const bool stale = id.by == KnownMap::By::None || id.trust_gen != trust_gen_now ||
                       (id.by == KnownMap::By::Entry && lose_epoch_on && id.epoch == lose_epoch) ||
                       id.epoch != live_epoch;
    if (!stale) return id;
    KnownMap none;
    none.epoch = id.epoch;
    none.trust_gen = id.trust_gen;
    none.report = id.report;
    return none;
}

std::uint32_t lose_epoch_for(std::uint64_t now, std::uint64_t cross_ms, std::uint32_t cross_epoch,
                             std::uint32_t place_epoch) {
    const bool crossing = cross_ms != 0 && now >= cross_ms && now - cross_ms < kCrossKeepMs;
    return crossing ? cross_epoch : place_epoch;
}

MapViewResult MapViewJob::run(const PacketWorld::View& view, const PacketWorld::Stats& stats, const PacketLink& link,
                              std::uint64_t now, std::uint64_t trust_gen, int drop_gate_px, GameMapBook& book,
                              MapViewState& ws) {
    MapViewResult r;
    if (ws.book != &book) {
        ws.view_trust.set_book(&book);
        ws.book = &book;
    }

    // 1. Logged back in, or Tìm lại: what was named may be another map's.
    if (trust_gen > ws.lost_gen) {
        ws.view_trust.lose("logged back in");
        ws.lost_gen = trust_gen;
    }
    // 2. A capture started again, or no look for a while: the character may
    // have gone through a door unseen, so the tab's MapTrust lets go as at a
    // capture lost. An entry's map comes back with the next look; a named one
    // is named afresh.
    const bool reset = ws.ran && stats.resets != ws.last_resets;
    if (!ws.ran || reset || now > ws.last_job_ms + kGapMs) {
        const bool known = !ws.view_trust.stem().empty();
        MapTrust::Look gap;
        gap.now_ms = now;
        gap.capture = false;
        ws.view_trust.update(gap);
        r.capture_gap = ws.ran;
        if (ws.ran && known) {
            if (reset)
                GB_INFO("Map view: the capture started again -- the map is named afresh");
            else
                GB_DEBUG("Map view: not looked at for %llu ms -- the map is named afresh",
                         static_cast<unsigned long long>(now - ws.last_job_ms));
        }
    }
    ws.ran = true;
    ws.last_resets = stats.resets;
    ws.last_job_ms = now;

    // 3. The look, as the hunter's drops make theirs (hunter_drop.cpp).
    r.fix = world_fix(view, stats, link, now);
    const WorldFix& f = r.fix;
    MapTrust::Look look;
    look.now_ms = now;
    look.capture = f.state != WorldFix::State::NoCapture && f.state != WorldFix::State::FirstReport;
    look.settled = f.settled();
    look.rx = round_px(f.rx);
    look.ry = round_px(f.ry);
    look.report_ms = f.report_ms;
    look.map_known = f.map_known;
    look.map = f.map;
    look.area = f.area;
    look.epoch = f.epoch;
    look.entry_x = round_px(f.entry_x);
    look.entry_y = round_px(f.entry_y);
    if (look.capture) look.mobs = packet_stands(view, PacketEyes::kStandsMax);
    ws.view_trust.update(look, kWorkMs);

    // 4. Which map.
    r.id = map_identity(f, ws.view_trust.report(), trust_gen);
    r.data_dir_ok = !book.data_dir().empty();
    if (!r.data_dir_ok && !ws.said_no_data) {
        ws.said_no_data = true;
        GB_INFO("Map view: no game data folder found -- positions only");
    }
    if (r.id.by == KnownMap::By::None) return r;
    const std::string& stem = r.id.stem;

    // 5. Its file and sketch, once per map.
    bool fresh = false;
    long long built_ms = 0;
    if (stem != ws.map_stem) {
        const auto t0 = std::chrono::steady_clock::now();
        ws.map_stem = stem;
        ws.page = book.get(stem);
        ws.sketch = ws.page ? std::make_shared<const MapSketch>(build_sketch(stem, ws.page->floors)) : nullptr;
        if (ws.sketch) ++ws.sketches;
        ws.no_drop.reset();
        built_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        fresh = ws.said.insert(stem).second;
    }
    r.page = ws.page;
    r.sketch = ws.sketch;

    // 6. Its gates: a lock, no file.
    if (r.page) r.gates = r.page->gates;
    const std::vector<MapGate> learned = book.learned_gates(stem);
    r.learned_gates = learned.size();
    r.gates.insert(r.gates.end(), learned.begin(), learned.end());
    // And what jumps up learned of it (2026.11.05b): a lock, no file -- the
    // floors solid from below followed on its page, a few at most.
    r.no_jumps = book.no_jumps(stem);
    r.solid = book.solid_floors(stem);
    if (r.page && !r.solid.empty()) r.solid_runs = solid_marks(*r.page, r.solid);
    if (fresh) {
        if (r.sketch)
            GB_INFO("Map view: %s -- %dx%d px, %zu floor columns -> %zu runs / %zu points, %zu gates (+%zu learned), "
                    "%lld ms",
                    stem.c_str(), r.sketch->w_px, r.sketch->h_px, r.sketch->columns, r.sketch->runs.size(),
                    r.sketch->points(), r.gates.size() - r.learned_gates, r.learned_gates, built_ms);
        else if (r.data_dir_ok)
            GB_INFO("Map view: %s has no map file (data\\Map)", stem.c_str());
    }

    // 7. Where a drop is refused for a gate.
    if (r.page) {
        if (!ws.no_drop || ws.no_drop_stem != stem || ws.no_drop_gates != r.gates.size() ||
            ws.no_drop_px != drop_gate_px) {
            ws.no_drop = std::make_shared<const std::vector<MapRun>>(no_drop_marks(*r.page, r.gates, drop_gate_px));
            ws.no_drop_stem = stem;
            ws.no_drop_gates = r.gates.size();
            ws.no_drop_px = drop_gate_px;
        }
        r.no_drop = ws.no_drop;
        r.no_drop_px = drop_gate_px;
    }

    // 8. Its name, from the game's own list of places.
    if (!ws.table_tried) {
        ws.table_tried = true;
        const std::string path = ws.table_path ? ws.table_path() : std::string();
        if (!path.empty()) ws.table.load(path);
    }
    if (ws.name_stem != stem) {
        ws.name_stem = stem;
        ws.name.clear();
        for (const MapTable::Entry& e : ws.table.entries()) {
            if (e.stem() != stem) continue;
            ws.name = e.name;
            break;
        }
    }
    r.map_name = ws.name;
    return r;
}

// ------------------------------------------------------- the tab's rules

MapShows map_shows(const MapShowsIn& in) {
    using Why = MapTrust::Why;
    if (in.positions_off) return MapShows::PositionsOff;
    if (in.state == WorldFix::State::NoCapture) return MapShows::NoCapture;
    // An entry since the job's look: what it knew is of the map before.
    if (in.ran && in.id.epoch != in.live_epoch) return MapShows::Changing;
    const KnownMap& u = in.usable;
    const Why why = u.report.why;
    if (u.by == KnownMap::By::None) {
        if (why == Why::Naming || why == Why::Confirming) return MapShows::Naming;
        if (why == Why::Ambiguous) return MapShows::Ambiguous;
        if (u.report.named && (why == Why::Mismatch || why == Why::Distrusted)) return MapShows::NamedDoubtful;
        if (!in.data_dir_ok || why == Why::NoFiles) return MapShows::NoGame;
        return MapShows::NothingYet;
    }
    if (!in.data_dir_ok) return MapShows::NoGame;
    if (!in.page) return MapShows::NoFile;
    if (u.file_doubtful) return MapShows::EntryDoubtful;
    if (why == Why::NoGates) return MapShows::NoGates;
    return u.by == KnownMap::By::Entry ? MapShows::ByEntry : MapShows::ByName;
}

bool map_drawn(MapShows s) {
    return s == MapShows::EntryDoubtful || s == MapShows::NoGates || s == MapShows::ByEntry || s == MapShows::ByName;
}

MapEdit map_edit(bool book_loaded, bool book_bad, const KnownMap& usable, MapShows shows) {
    if (book_bad) return MapEdit::BookBad;
    if (!book_loaded) return MapEdit::BookLoading;
    // Nothing places the character: the map last known may be gone by.
    if (shows == MapShows::PositionsOff || shows == MapShows::NoCapture) return MapEdit::MapUnknown;
    if (shows == MapShows::Changing) return MapEdit::Changing;
    if (shows == MapShows::NoGame) return MapEdit::NoGame;
    if (usable.by == KnownMap::By::None || usable.stem.empty()) return MapEdit::MapUnknown;
    return MapEdit::Ok;
}

bool zone_here_ok(const KnownMap& usable, const GameMapBook::Page* page, MapPoint at) {
    return usable.by != KnownMap::By::Named || (!usable.report.own_off && page != nullptr && page->on_floor(at));
}

bool spot_keep_ok(const KnownMap& usable) { return usable.by != KnownMap::By::Named || !usable.report.own_off; }

ZoneSend zone_send(const KnownMap& usable, const HuntZone& z, const GameMapBook::Page* page, bool have_settled,
                   std::uint32_t settled_epoch, MapPoint settled) {
    if (usable.by == KnownMap::By::None || usable.stem.empty()) return ZoneSend::NoMap;
    // With a file to go by, the zone on its floors and within the map; with a
    // doubtful one or none, within what a map can be.
    const GameMapBook::Page* floors = usable.file_doubtful ? nullptr : page;
    if (!zone_valid(z, zone_limits(floors, usable.file_doubtful), floors)) return ZoneSend::DoesNotFit;
    // A name is had from points that fit more than one place at first: sent
    // once this entry's own settled report is on the named file's floors and
    // none of yours is off them.
    if (usable.by == KnownMap::By::Named &&
        (usable.report.own_off || !have_settled || settled_epoch != usable.epoch || page == nullptr ||
         !page->on_floor(settled)))
        return ZoneSend::NotSure;
    return ZoneSend::Send;
}

ZoneCard zone_card(const ZoneCardIn& in, const Hunter::Status& hs) {
    if (!in.hunt_enabled) return ZoneCard::HuntOff;
    if (in.book_bad) return ZoneCard::BookBad;
    if (!in.book_loaded) return ZoneCard::BookLoading;
    if (in.stem.empty()) return in.hunting && in.spot && in.book_any ? ZoneCard::MapUnknown : ZoneCard::NoMap;
    if (!in.zone) {
        if (!hs.spot_set) return ZoneCard::Empty;
        return hs.spot_from_map ? ZoneCard::Other : ZoneCard::SessionSpot;
    }
    if (!in.spot) return ZoneCard::SpotOff;
    if (!in.hunting) return ZoneCard::NotHunting;
    if (hs.spot_set && hs.spot_from_map && hs.spot_stem == in.stem) {
        if (hs.spot_suspended) return ZoneCard::Suspended;
        return hs.spot_why == Hunter::SpotWhy::None && hs.spot_outside_px > 0 ? ZoneCard::OutsideArea
                                                                              : ZoneCard::InUse;
    }
    if (hs.zone_wait != Hunter::ZoneWait::None && hs.zone_wait_stem == in.stem)
        return hs.zone_wait == Hunter::ZoneWait::Outside ? ZoneCard::Outside : ZoneCard::Waiting;
    if (in.send == ZoneSend::DoesNotFit) return ZoneCard::DoesNotFit;
    if (in.send == ZoneSend::NotSure) return ZoneCard::Waiting;
    return ZoneCard::Other;
}

// ------------------------------------------------- where a dot is drawn

MapDot mob_dot(const GameMapBook::Page* page, const PacketWorld::Mob& m, std::uint64_t now) {
    if (m.dead) {
        MapDot d;
        d.x = m.x;
        d.y = m.y;
        const std::uint64_t age = now > m.dead_ms ? now - m.dead_ms : 0;
        if (m.dead_ms == 0 || age >= kMapDeadShownMs) d.why = DotWhy::Hidden;
        return d;
    }
    // The placing is mob_place's, the hunter's plates' and the seek's own: on
    // the tab, a walk run out is held where the world holds it (2026.11.05).
    return mob_place(page, m, now, false);
}

MapDot you_dot(const GameMapBook::Page* page, const WorldFix& fix, double x, double y) {
    MapDot d;
    d.x = x;
    d.y = y;
    if (page == nullptr || fix.state != WorldFix::State::Known || fix.airborne || !fix.moving) return d;
    if (const std::optional<MapDot> on = along_floor(*page, fix.rx, fix.ry, x)) return *on;
    return d;
}

bool map_shows_mobs(MapShows s) { return s != MapShows::PositionsOff && s != MapShows::NoCapture; }

std::vector<MapDot> mob_dots(const GameMapBook::Page* page, const PacketWorld::View& v, MapShows shows,
                             std::uint64_t now) {
    std::vector<MapDot> dots;
    if (!map_shows_mobs(shows)) return dots;
    dots.reserve(v.mobs.size());
    for (const PacketWorld::Mob& m : v.mobs) dots.push_back(mob_dot(page, m, now));
    return dots;
}

const GameMapBook::Page* dot_floors(const GameMapBook::Page* page, MapShows shows) {
    return shows == MapShows::EntryDoubtful ? nullptr : page;
}

bool mob_grey(const HunterConfig& cfg, const Hunter::Status& hs, bool area_on, bool you, double you_x,
              double you_y, const PacketWorld::Mob& m, bool target) {
    if (!area_on || m.dead) return false;
    // The one fought let go of only kSpotKeepPx past the edge, as Spot::inside
    // has it in Hunter::mark_out_of_area.
    const double keep = target ? static_cast<double>(Hunter::kSpotKeepPx) : 0.0;
    const bool in_area = m.x >= hs.spot_left - keep && m.x <= hs.spot_right + keep && m.y >= hs.spot_top - keep &&
                         m.y <= hs.spot_bottom + keep;
    if (in_area) return false;
    if (hs.spot_back_first) return true;
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    // As the hunter has the plate: packet_dy on the height (PacketEyes::make_plates).
    const bool reach = you && Hunter::in_reach(cfg, round_px((m.x - you_x) / scale),
                                               round_px((m.y - you_y) / scale) + cfg.packet_dy);
    return !reach;
}

// ------------------------------------- the jumps up and the seek (2026.11.05b)

std::optional<JumpArrow> jump_arrow(const Hunter::Status& hs, std::uint64_t now) {
    JumpArrow a;
    // The one under way: from its take-off column, onto its floor.
    if (hs.jump_phase != Hunter::JumpUpPhase::Idle) {
        a.kind = hs.jump_kind;
        a.take = { hs.jump_x, hs.jump_feet_y };
        a.land = { hs.jump_land_x, hs.jump_land_top - 1 };
        return a;
    }
    // The last that would have gone, while fresh.
    if (hs.jump_would_ms == 0 || now < hs.jump_would_ms || now - hs.jump_would_ms > kJumpArrowFreshMs)
        return std::nullopt;
    a.kind = hs.jump_would_plan.kind;
    a.take = hs.jump_would_plan.take();
    a.land = hs.jump_would_plan.land();
    a.would = true;
    return a;
}

const Hunter::Status::SeekGrey* seek_grey_of(const Hunter::Status& hs, const PacketWorld::Mob& m) {
    if (!hs.seek_active || m.dead) return nullptr;
    if (hs.seek_on && hs.seek_slot >= 0 && m.slot == static_cast<std::uint32_t>(hs.seek_slot) && m.gen == hs.seek_gen)
        return nullptr;
    for (const Hunter::Status::SeekGrey& g : hs.seek_grey)
        if (g.slot == m.slot && g.gen == m.gen) return &g;
    return nullptr;
}

std::vector<MapRun> solid_marks(const GameMapBook::Page& p, std::span<const MapPoint> on) {
    std::vector<MapRun> out;
    for (const MapPoint& s : on) {
        const MapTrail left = p.follow(s.x, s.y, -1);
        const MapTrail right = p.follow(s.x, s.y, 1);
        if (left.pts.empty() && right.pts.empty()) continue;
        MapRun r;
        r.attr = 1;
        // Left to right: the left trail backwards, then the right one past
        // the point both start at.
        for (auto it = left.pts.rbegin(); it != left.pts.rend(); ++it) r.pts.push_back(*it);
        for (std::size_t i = left.pts.empty() ? 0 : 1; i < right.pts.size(); ++i) r.pts.push_back(right.pts[i]);
        r.x0 = r.x1 = r.pts.front().x;
        r.y0 = r.y1 = r.pts.front().y;
        for (const MapPoint& q : r.pts) {
            r.x0 = std::min(r.x0, q.x);
            r.x1 = std::max(r.x1, q.x);
            r.y0 = std::min(r.y0, q.y);
            r.y1 = std::max(r.y1, q.y);
        }
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace gobot
