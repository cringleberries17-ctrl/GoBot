// Your casts and buffs from the packets, paired with the macro's presses (the
// packets plan, items F and H): log only. See gobot/cast_match.hpp.

#include "gobot/cast_match.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/packet_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace gobot {
namespace {

constexpr std::size_t kHeader = 8;      // frame bytes before the payload

/// `p` is `opcode` from your client over UDP, read straight off a datagram,
/// of `size` bytes. Its size is checked last, so that a frame of the other
/// side is never counted as one of the wrong size.
FrameFit client_udp_fit(const GamePacket& p, std::uint16_t opcode, std::size_t size) {
    if (p.opcode != opcode) return FrameFit::OtherOpcode;
    if (p.from_server || !p.udp || p.depth != 0) return FrameFit::WrongSide;
    if (p.payload.size() + kHeader != size) return FrameFit::WrongSize;
    return FrameFit::Taken;
}

bool taken(FrameFit f, FrameFit* fit) {
    if (fit) *fit = f;
    return f == FrameFit::Taken;
}

/// `ms` in seconds with one decimal: "0.4 s".
std::string seconds_text(std::uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(ms) / 1000.0);
    return buf;
}

std::string key_text(std::uint32_t vk) { return "key " + InputManager::key_name(vk); }

/// `ms` in seconds with two decimals: "0.74 s".
std::string seconds2_text(std::uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f s", static_cast<double>(ms) / 1000.0);
    return buf;
}

/// A span between a cast and a press: in ms under 10 s, in seconds past it.
std::string span_text(std::uint64_t ms) {
    return ms < 10'000 ? std::to_string(ms) + " ms" : seconds_text(ms);
}

/// `at_ms` as `clock` writes it, or the clock's own ms.
std::string time_text(CastClockText clock, std::uint64_t at_ms) {
    return clock != nullptr ? clock(at_ms) : "t=" + std::to_string(at_ms) + " ms";
}

/// The nearest-rank `pct`th percentile of `v`, which is not empty.
std::uint64_t percentile(std::vector<std::uint64_t> v, std::size_t pct) {
    std::sort(v.begin(), v.end());
    const std::size_t rank = std::max<std::size_t>(1, (v.size() * pct + 99) / 100);
    return v[std::min(rank, v.size()) - 1];
}

} // namespace

// ---------------------------------------------------------------- readers

std::optional<CastFrame> read_cast(const GamePacket& p, FrameFit* fit) {
    if (!taken(client_udp_fit(p, 0x0031, 92), fit)) return std::nullopt;
    ByteReader r(p.payload);
    // A failed read poisons the reader, so one ok() at the end covers them all.
    auto u8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.u8(); };
    auto u16_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u16(); };
    auto u32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u32(); };
    CastFrame c;
    c.id = u32_at(12);
    c.x = u16_at(16);
    c.y = u16_at(18);
    c.skill = u32_at(20);
    c.kind = u8_at(24);
    c.named = u8_at(26) == 2;
    // Padded with 0xFFFF when no target is named.
    const std::uint16_t slot = u16_at(28);
    if (c.named) c.slot = slot;
    c.counter = u32_at(88);
    if (!r.ok()) return std::nullopt;
    return c;
}

std::optional<BuffTimeFrame> read_buff_time(const GamePacket& p, FrameFit* fit) {
    if (!self_frames_detail::fits(p, 0x0077, false, false, 24, fit)) return std::nullopt;
    ByteReader r(p.payload);
    BuffTimeFrame b;
    r.seek(12 - kHeader);
    b.skill = r.u32();
    b.value = r.u32();
    if (!r.ok()) return std::nullopt;
    return b;
}

std::optional<BuffStatFrame> read_buff_stat(const GamePacket& p, FrameFit* fit) {
    if (!self_frames_detail::fits(p, 0x005A, false, true, 32, fit)) return std::nullopt;
    ByteReader r(p.payload);
    BuffStatFrame b;
    r.seek(12 - kHeader);
    b.skill = r.u32();
    r.seek(20 - kHeader);
    b.change = r.i32();
    b.value = r.u32();
    if (!r.ok()) return std::nullopt;
    return b;
}

FrameFit buff_follow_fit(const GamePacket& p) { return client_udp_fit(p, 0x002E, 72); }

// -------------------------------------------------------------- CastMatch

void CastMatch::on_packets(const std::vector<GamePacket>& all, const PacketWorld& world) {
    for (const GamePacket& p : all) {
        if (p.opcode != 0x0031) continue;
        FrameFit fit = FrameFit::OtherOpcode;
        const std::optional<CastFrame> c = read_cast(p, &fit);
        if (!c) {
            if (fit == FrameFit::WrongSize) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++minute_.wrong_size;
            }
            continue;
        }
        // A named target where the world has it at the cast's own time, not
        // when GoBot gets round to it: a walker goes some 100 px a second.
        // Asked under the world's lock, before this one is taken.
        bool known = false;
        int dx = 0, dy = 0;
        if (c->named) {
            const PacketWorld::View v = world.view(p.timestamp_ms);
            if (const PacketWorld::Mob* m = v.mob(c->slot)) {
                known = true;
                dx = static_cast<int>(std::lround(std::fabs(static_cast<double>(m->x) - c->x)));
                dy = static_cast<int>(std::lround(std::fabs(static_cast<double>(m->y) - c->y)));
            }
        }
        add(p.timestamp_ms, *c, known, dx, dy);
    }
}

void CastMatch::set_run(bool macro_on, bool hunter_on, std::uint64_t capture_start) {
    // The capture thread's alone, as the three are: only the count is read by
    // another (add, under no lock of its).
    if (macro_on == run_macro_ && hunter_on == run_hunter_ && capture_start == run_capture_) return;
    run_macro_ = macro_on;
    run_hunter_ = hunter_on;
    run_capture_ = capture_start;
    run_.fetch_add(1);
}

void CastMatch::add(std::uint64_t at_ms, const CastFrame& c, bool reach_known, int dx, int dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    Cast cast;
    cast.at_ms = at_ms;
    cast.frame = c;
    cast.reach_known = reach_known;
    cast.dx = dx;
    cast.dy = dy;
    cast.seq = ++added_;
    cast.held = held_.load();
    cast.run = run_.load();
    ++minute_.casts;
    switch (c.kind) {
        case 1:  ++minute_.attacks; break;
        case 2:  ++minute_.buffs; break;
        default: ++minute_.other_kinds; break;
    }
    if (c.named) {
        ++minute_.named;
        if (!reach_known) ++minute_.named_unplaced;
    }
    if (reach_known) {
        auto it = std::find_if(reach_.begin(), reach_.end(), [&c](const Reach& r) { return r.skill == c.skill; });
        if (it == reach_.end() && reach_.size() < kSkills) {
            Reach r;
            r.skill = c.skill;
            reach_.push_back(r);
            it = reach_.end() - 1;
        }
        if (it != reach_.end()) {
            ++it->casts;
            it->most_dx = std::max(it->most_dx, dx);
            it->most_dy = std::max(it->most_dy, dy);
        }
    }
    casts_.push_back(cast);
    while (casts_.size() > kCasts) {
        // Gone before any press could take it: no press of the macro's did.
        const Cast& old = casts_.front();
        if (!old.settled && old.vk == 0) ++minute_.unpaired;
        casts_.pop_front();
    }
}

CastMatch::Key* CastMatch::key_of(std::uint32_t vk) {
    for (Key& k : keys_)
        if (k.vk == vk) return &k;
    if (keys_.size() >= kKeys) return nullptr;
    keys_.emplace_back();
    keys_.back().vk = vk;
    return &keys_.back();
}

void CastMatch::vote(Key& k, std::uint32_t skill) {
    k.votes.push_back(skill);
    if (k.votes.size() > kVotes) k.votes.pop_front();
    k.top_skill = 0;
    k.top_votes = 0;
    for (const std::uint32_t s : k.votes) {
        const int n = static_cast<int>(std::count(k.votes.begin(), k.votes.end(), s));
        if (n > k.top_votes) {
            k.top_votes = n;
            k.top_skill = s;
        }
    }
    k.pairs = static_cast<int>(k.votes.size());
    k.skill = k.pairs >= kLearnPairs && k.top_votes * 100 >= kLearnPct * k.pairs ? k.top_skill : 0;
}

void CastMatch::pair(const std::vector<Press>& presses, const std::vector<std::uint32_t>& buff_keys,
                     std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t until = now > kSettleMs ? now - kSettleMs : 0;
    if (until <= pair_from_) return;

    // Oldest first: an earlier press takes its cast before a later one looks.
    std::vector<Press> due;
    for (const Press& p : presses)
        if (p.vk != 0 && p.at_ms >= pair_from_ && p.at_ms < until) due.push_back(p);
    std::stable_sort(due.begin(), due.end(), [](const Press& a, const Press& b) { return a.at_ms < b.at_ms; });

    for (const Press& p : due) {
        // Kept, any key, for the presses either side of a cast once it is
        // settled: oldest first, as each call's come after the last's.
        pressed_.push_back(p);
        if (pressed_.size() > kPresses) pressed_.pop_front();
        Key* k = key_of(p.vk);
        if (k == nullptr) continue;
        k->buff = std::find(buff_keys.begin(), buff_keys.end(), p.vk) != buff_keys.end();
        ++k->pressed;
        ++k->pressed_all;
        if (k->buff) {
            // Its burst: joined within kBurstGapMs of the last press, else
            // the one before judged and a new one begun.
            const auto open = std::find_if(open_bursts_.begin(), open_bursts_.end(),
                                           [&p](const Burst& b) { return b.vk == p.vk; });
            if (open != open_bursts_.end() && p.at_ms <= open->last_ms + kBurstGapMs) {
                open->last_ms = p.at_ms;
                ++open->presses;
            } else {
                if (open != open_bursts_.end()) {
                    const Burst done = *open;
                    open_bursts_.erase(open);
                    judge(done);
                }
                Burst b;
                b.vk = p.vk;
                b.first_ms = b.last_ms = p.at_ms;
                b.presses = 1;
                open_bursts_.push_back(b);
            }
        }
        const std::uint64_t from = p.at_ms > kEarlyMs ? p.at_ms - kEarlyMs : 0;
        const std::uint64_t to = p.at_ms + kPairMs;
        for (Cast& c : casts_) {
            if (c.vk != 0 || c.settled || c.at_ms < from || c.at_ms > to) continue;
            c.vk = p.vk;
            c.press_ms = p.at_ms;
            ++k->cast;
            ++k->cast_all;
            switch (c.frame.kind) {
                case 1:  ++k->attacks; break;
                case 2:  ++k->buffs; break;
                default: ++k->other_kinds; break;
            }
            vote(*k, c.frame.skill);
            Paired paired;
            paired.press_ms = p.at_ms;
            paired.cast_ms = c.at_ms;
            paired.skill = c.frame.skill;
            k->recent.push_back(paired);
            if (k->recent.size() > kRecent) k->recent.erase(k->recent.begin());
            break;
        }
    }
    pair_from_ = until;

    // A burst no press can join any more -- every press up to `until` taken,
    // and kBurstGapMs gone by after its last -- is judged: its casts, up to
    // kPairMs after its last press, have come by now.
    for (auto it = open_bursts_.begin(); it != open_bursts_.end();) {
        if (it->last_ms + kBurstGapMs >= until) {
            ++it;
            continue;
        }
        const Burst done = *it;
        it = open_bursts_.erase(it);
        judge(done);
    }

    // A cast every press that could have taken it has been paired by: one
    // none took came with no press of the macro's -- the hunter's attack, a
    // key pressed by hand.
    for (Cast& c : casts_) {
        if (c.settled || c.at_ms + kEarlyMs >= pair_from_) continue;
        c.settled = true;
        if (c.vk == 0) ++minute_.unpaired;
        settle(c);
    }
}

void CastMatch::settle(Cast& c) {
    // GoBot's presses either side of it: each one up to pair_from_ has been
    // handed over by now, in the order they went down.
    for (const Press& p : pressed_) {
        if (p.at_ms <= c.at_ms) {
            c.before_ms = p.at_ms;
            c.before_vk = p.vk;
            continue;
        }
        c.after_ms = p.at_ms;
        c.after_vk = p.vk;
        break;
    }
    // How long after GoBot's newest key-down the cast came: the game's
    // latency, and what a pairing by the newest press would need. Only
    // within the pairing's own window -- further back, the cast was no
    // press's of GoBot's.
    if (c.before_ms != 0 && c.at_ms - c.before_ms <= kPairMs) {
        if (minute_.delays.size() < kCasts) minute_.delays.push_back(c.at_ms - c.before_ms);
    } else {
        ++minute_.no_press_before;
    }
    if (c.vk != 0) return;
    // No press of GoBot's near it either side and no hold: by hand or another
    // tool's -- GoBot open beside your own play -- counted only. (Before it,
    // at it, after: neither difference wraps.)
    const bool pressed_before = c.before_ms != 0 && c.at_ms - c.before_ms <= kLoneNearMs;
    const bool pressed_after = c.after_ms != 0 && c.after_ms - c.at_ms <= kLoneNearMs;
    if (!c.held && !pressed_before && !pressed_after) {
        ++minute_.lone_far;
        return;
    }
    if (minute_.lone.size() >= kLoneSaid) {
        ++minute_.lone_more;
        return;
    }
    Lone l;
    l.at_ms = c.at_ms;
    l.skill = c.frame.skill;
    l.kind = c.frame.kind;
    l.held = c.held;
    l.before_ms = c.before_ms;
    l.before_vk = c.before_vk;
    l.after_ms = c.after_ms;
    l.after_vk = c.after_vk;
    l.looked_ms = pair_from_;
    minute_.lone.push_back(l);
}

void CastMatch::judge(Burst b) {
    // Any cast of kind 2 in its window, whichever press took it: a buff key's
    // check of its own, which takes no cast from the pairing.
    const std::uint64_t from = b.first_ms > kEarlyMs ? b.first_ms - kEarlyMs : 0;
    const std::uint64_t to = b.last_ms + kPairMs;
    for (const Cast& c : casts_) {
        if (c.frame.kind != 2 || c.at_ms < from || c.at_ms > to) continue;
        b.seen = true;
        b.cast_ms = c.at_ms;
        b.skill = c.frame.skill;
        break;
    }
    // Its key is kept already -- found, never added: pair() holds a pointer
    // into keys_ meanwhile.
    for (Key& k : keys_) {
        if (k.vk != b.vk) continue;
        ++k.bursts;
        if (b.seen) ++k.bursts_seen;
        break;
    }
    judged_.push_back(b);
    // Read once a second; a GoBot whose log is not taken keeps no more.
    while (judged_.size() > kKeys) judged_.erase(judged_.begin());
}

std::vector<CastMatch::Burst> CastMatch::take_bursts() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Burst> out;
    out.swap(judged_);
    return out;
}

std::uint64_t CastMatch::pair_from() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pair_from_;
}

std::uint32_t CastMatch::learned(std::uint32_t vk) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Key& k : keys_)
        if (k.vk == vk) return k.skill;
    return 0;
}

std::vector<CastMatch::Key> CastMatch::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_;
}

std::vector<CastMatch::Cast> CastMatch::casts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Cast>(casts_.begin(), casts_.end());
}

CastMatch::Minute CastMatch::take_minute() {
    std::lock_guard<std::mutex> lock(mutex_);
    Minute out = minute_;
    out.keys = keys_;
    out.reach = reach_;

    // The minute's casts are those of the ring that came since the last
    // call: each one's gap from the cast before it, and from its own skill's
    // last cast, either maybe from the minute before.
    using LastCast = std::pair<std::uint32_t, std::uint64_t>;      // a skill, its last cast
    std::vector<LastCast> last_of;
    const Cast* before = nullptr;
    for (const Cast& c : casts_) {
        auto own = std::find_if(last_of.begin(), last_of.end(),
                                [&c](const LastCast& s) { return s.first == c.frame.skill; });
        if (c.seq > minute_seq_) {
            // A gap only within a run: across a stop and start, or a capture
            // started again, it is no pause of the run's -- counted apart.
            if (before != nullptr && c.at_ms >= before->at_ms) {
                if (c.run == before->run) {
                    const std::uint64_t gap = c.at_ms - before->at_ms;
                    out.gaps.push_back(gap);
                    if (gap > out.longest_ms || out.gaps.size() == 1) {
                        out.longest_ms = gap;
                        out.longest_at_ms = c.at_ms;
                    }
                } else {
                    ++out.gaps_left_out;
                }
            }
            auto s = std::find_if(out.skills.begin(), out.skills.end(),
                                  [&c](const SkillCasts& k) { return k.skill == c.frame.skill; });
            if (s == out.skills.end() && out.skills.size() < kSkills) {
                SkillCasts k;
                k.skill = c.frame.skill;
                out.skills.push_back(k);
                s = out.skills.end() - 1;
            }
            if (s != out.skills.end()) {
                ++s->casts;
                if (own != last_of.end() && c.at_ms >= own->second) {
                    const std::uint64_t gap = c.at_ms - own->second;
                    s->shortest_ms = s->recasts == 0 ? gap : std::min(s->shortest_ms, gap);
                    ++s->recasts;
                }
            }
        }
        if (own != last_of.end())
            own->second = c.at_ms;
        else if (last_of.size() < kCasts)
            last_of.emplace_back(c.frame.skill, c.at_ms);
        before = &c;
    }
    std::stable_sort(out.skills.begin(), out.skills.end(),
                     [](const SkillCasts& a, const SkillCasts& b) { return a.casts > b.casts; });
    if (!casts_.empty()) minute_seq_ = casts_.back().seq;

    minute_ = Minute{};
    for (Key& k : keys_) {
        k.pressed = 0;
        k.cast = 0;
        k.bursts = 0;
        k.bursts_seen = 0;
    }
    return out;
}

// -------------------------------------------------------------- BuffWatch

BuffWatch::Skill* BuffWatch::skill_of(std::uint32_t skill) {
    for (Skill& s : minute_.skills)
        if (s.skill == skill) return &s;
    // A layout read wrong would name a new "skill" in every frame.
    if (minute_.skills.size() >= kSkills) return nullptr;
    minute_.skills.emplace_back();
    minute_.skills.back().skill = skill;
    return &minute_.skills.back();
}

void BuffWatch::on_packets(const std::vector<GamePacket>& all) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const GamePacket& p : all) {
        const std::uint64_t t = p.timestamp_ms;
        FrameFit fit = FrameFit::OtherOpcode;
        switch (p.opcode) {
        case 0x0031:
            // Only its kind, here: CastMatch counts the casts.
            if (const auto c = read_cast(p)) {
                if (c->kind == 2) {
                    ++minute_.buff_casts;
                    last_buff_cast_ms_ = t;
                }
            }
            break;
        case 0x0077:
            if (const auto b = read_buff_time(p, &fit)) {
                if (Skill* s = skill_of(b->skill)) {
                    ++s->times;
                    s->time_value = b->value;
                    s->time_ms = t;
                } else {
                    ++minute_.other_skills;
                }
            }
            break;
        case 0x005A:
            if (const auto b = read_buff_stat(p, &fit)) {
                if (Skill* s = skill_of(b->skill)) {
                    ++s->stats;
                    s->change = b->change;
                    s->value = b->value;
                    s->stat_ms = t;
                } else {
                    ++minute_.other_skills;
                }
            }
            break;
        case 0x002E:
            fit = buff_follow_fit(p);
            if (fit == FrameFit::Taken) {
                ++minute_.follows;
                if (last_buff_cast_ms_ != 0 && t >= last_buff_cast_ms_ && t - last_buff_cast_ms_ <= kFollowMs)
                    ++minute_.follows_after;
            }
            break;
        default:
            break;
        }
        if (fit == FrameFit::WrongSize) ++minute_.wrong_size;
    }
}

BuffWatch::Minute BuffWatch::take_minute() {
    std::lock_guard<std::mutex> lock(mutex_);
    Minute out = minute_;
    minute_.buff_casts = 0;
    minute_.follows = 0;
    minute_.follows_after = 0;
    minute_.other_skills = 0;
    minute_.wrong_size = 0;
    for (Skill& s : minute_.skills) {
        s.times = 0;
        s.stats = 0;
    }
    return out;
}

// ------------------------------------------------------------ the log lines

std::vector<BuffSeen> pair_buffs(const std::vector<CastMatch::Key>& keys, const BuffWatch::Minute& m) {
    std::vector<BuffSeen> out;
    for (const BuffWatch::Skill& s : m.skills) {
        if (s.times == 0 && s.stats == 0) continue;
        BuffSeen b;
        b.skill = s.skill;
        b.timed = s.times > 0;
        b.time_value = s.time_value;
        b.stat = s.stats > 0;
        b.change = s.change;
        b.value = s.value;
        // The key learned to cast it: a buff key before any other.
        const CastMatch::Key* key = nullptr;
        for (const CastMatch::Key& k : keys)
            if (k.skill == s.skill && (key == nullptr || (k.buff && !key->buff))) key = &k;
        if (key != nullptr) {
            b.vk = key->vk;
            // Its last press, paired with a cast of this skill, before the
            // 0x0077 and within kBuffAfterMs of it: oldest first, so the last
            // that fits is the one.
            if (b.timed)
                for (const CastMatch::Paired& p : key->recent)
                    if (p.skill == s.skill && p.press_ms <= s.time_ms && s.time_ms - p.press_ms <= kBuffAfterMs)
                        b.after_press_ms = static_cast<std::int64_t>(s.time_ms - p.press_ms);
        }
        out.push_back(b);
    }
    return out;
}

std::string casts_line(const CastMatch::Minute& m) {
    bool pressed = false;
    for (const CastMatch::Key& k : m.keys)
        if (k.pressed > 0) pressed = true;
    if (m.casts == 0 && m.unpaired == 0 && !pressed) return {};

    std::ostringstream out;
    out << "Casts (packets): " << m.casts << " this minute (attacks " << m.attacks << ", buffs " << m.buffs;
    if (m.other_kinds > 0) out << ", other kinds " << m.other_kinds;
    out << ", named " << m.named << ")";
    for (const CastMatch::Key& k : m.keys) {
        if (k.pressed == 0) continue;
        out << "; " << key_text(k.vk) << (k.buff ? " (buff)" : "") << ": " << k.pressed << " pressed, " << k.cast
            << " cast";
        if (k.skill != 0)
            out << " -> skill " << k.skill << " (" << k.top_votes << "/" << k.pairs << ")";
        else if (k.top_skill != 0)
            out << " -> not learned yet (" << k.top_skill << " " << k.top_votes << "/" << k.pairs << ")";
        if (k.buff && k.kind_mismatch())
            out << ", kinds: attack " << k.attacks << ", buff " << k.buffs << ", other " << k.other_kinds
                << " -- a buff key's casts should be kind 2";
        else if (k.buff && k.buffs > 0)
            out << ", kind 2";
        else if (k.kind_mismatch())
            out << ", kind 2 (a buff's) " << k.buffs << " times";
        // A buff key's own check, whichever key the pairing gave the cast to.
        if (k.buff && k.bursts > 0)
            out << ", a kind-2 cast with " << k.bursts_seen << " of its " << k.bursts << " burst"
                << (k.bursts == 1 ? "" : "s");
    }
    out << "; no macro key " << m.unpaired;
    if (!m.reach.empty()) {
        out << "; reach seen";
        bool first = true;
        for (const CastMatch::Reach& r : m.reach) {
            out << (first ? " " : " / ") << r.skill << ": |dx| <= " << r.most_dx << ", |dy| <= " << r.most_dy << " ("
                << r.casts << ")";
            first = false;
        }
    }
    if (m.named_unplaced > 0) out << "; named targets not in the packets' world " << m.named_unplaced;
    if (m.wrong_size > 0) out << "; 0x0031 of another size " << m.wrong_size;
    return out.str();
}

std::string cast_gaps_line(const CastMatch::Minute& m, CastClockText clock) {
    if (m.gaps.empty() && m.delays.empty() && m.no_press_before == 0 && m.skills.empty()) return {};

    std::ostringstream out;
    out << "Cast gaps (packets): ";
    if (m.gaps.empty()) {
        out << "no gap between two casts";
    } else {
        const auto over = [&m](std::uint64_t ms) {
            return std::count_if(m.gaps.begin(), m.gaps.end(), [ms](std::uint64_t g) { return g > ms; });
        };
        out << m.gaps.size() << (m.gaps.size() == 1 ? " gap" : " gaps") << " -- p50 "
            << seconds2_text(percentile(m.gaps, 50)) << ", p90 " << seconds2_text(percentile(m.gaps, 90))
            << ", longest " << seconds2_text(m.longest_ms) << " ending at " << time_text(clock, m.longest_at_ms)
            << "; over 1.0 s " << over(1000) << ", over 1.5 s " << over(1500);
    }
    if (m.gaps_left_out > 0)
        out << "; " << m.gaps_left_out << (m.gaps_left_out == 1 ? " gap" : " gaps")
            << " across a stop and start or a capture restart, left out";
    // How long after GoBot's newest key-down each cast came.
    out << "; newest press to cast ";
    if (m.delays.empty())
        out << "none";
    else
        out << "p10 " << percentile(m.delays, 10) << " ms, p50 " << percentile(m.delays, 50) << " ms, p90 "
            << percentile(m.delays, 90) << " ms (" << m.delays.size() << (m.delays.size() == 1 ? " cast" : " casts")
            << ")";
    if (m.no_press_before > 0)
        out << ", " << m.no_press_before << " with no press of GoBot's in the " << CastMatch::kPairMs << " ms before";
    // Each skill: how often, and how soon again after its own cast -- a
    // cooldown it has is never shorter.
    if (!m.skills.empty()) {
        out << "; by skill";
        bool first = true;
        for (const CastMatch::SkillCasts& s : m.skills) {
            out << (first ? " " : " / ") << s.skill << " " << s.casts << (s.casts == 1 ? " cast" : " casts");
            if (s.recasts > 0) out << ", shortest re-cast " << seconds2_text(s.shortest_ms);
            first = false;
        }
    }
    return out.str();
}

std::string lone_cast_line(const CastMatch::Lone& c, CastClockText clock) {
    std::ostringstream out;
    out << "Cast (packets) with no GoBot key at " << time_text(clock, c.at_ms) << ": skill " << c.skill << ", kind "
        << static_cast<unsigned>(c.kind) << "; GoBot's last press ";
    if (c.before_ms != 0)
        out << span_text(c.at_ms - c.before_ms) << " before it (" << key_text(c.before_vk) << ")";
    else
        out << "none kept";
    out << ", the first after it ";
    if (c.after_ms != 0)
        out << span_text(c.after_ms - c.at_ms) << " after (" << key_text(c.after_vk) << ")";
    else
        out << "none in the " << span_text(c.looked_ms > c.at_ms ? c.looked_ms - c.at_ms : 0) << " after";
    out << (c.held ? "; the keys held for a job then" : "; no hold then");
    return out.str();
}

std::string burst_line(const CastMatch::Burst& b) {
    std::ostringstream out;
    out << "Buff (packets): " << key_text(b.vk) << " -- ";
    if (b.seen && b.cast_ms >= b.first_ms)
        out << "a kind-2 cast of yours, skill " << b.skill << ", " << b.cast_ms - b.first_ms
            << " ms after its first press";
    else if (b.seen)
        out << "a kind-2 cast of yours, skill " << b.skill << ", " << b.first_ms - b.cast_ms
            << " ms before its first press";
    else
        out << "no kind-2 cast of yours from " << CastMatch::kEarlyMs << " ms before its first press to "
            << CastMatch::kPairMs << " ms after its last: maybe not cast";
    out << " (" << b.presses << (b.presses == 1 ? " press" : " presses") << ")";
    return out.str();
}

std::string buffs_line(const std::vector<BuffSeen>& seen, const BuffWatch::Minute& m,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& intervals_s) {
    if (seen.empty() && m.buff_casts == 0 && m.follows == 0 && m.other_skills == 0 && m.wrong_size == 0)
        return {};

    std::ostringstream out;
    out << "Buffs (packets): ";
    for (const BuffSeen& b : seen) {
        if (b.vk != 0)
            out << key_text(b.vk) << " -> " << b.skill << ":";
        else
            out << "skill " << b.skill << " (no key learned for it):";
        if (b.timed) {
            out << " 0x0077 " << b.time_value;
            if (b.after_press_ms >= 0)
                out << " " << seconds_text(static_cast<std::uint64_t>(b.after_press_ms)) << " after the press";
        }
        if (b.stat)
            out << (b.timed ? ";" : "") << " 0x005A " << (b.change >= 0 ? "+" : "") << b.change << " -> " << b.value;
        if (b.vk != 0)
            for (const auto& iv : intervals_s)
                if (iv.first == b.vk) {
                    out << "; macro interval " << iv.second << " s";
                    break;
                }
        out << "; ";
    }
    out << "buff casts " << m.buff_casts << ", 0x002E " << m.follows << " (" << m.follows_after
        << " after a buff cast)";
    if (m.other_skills > 0) out << "; frames naming other skills " << m.other_skills;
    if (m.wrong_size > 0) out << "; of another size " << m.wrong_size;
    return out.str();
}

} // namespace gobot
