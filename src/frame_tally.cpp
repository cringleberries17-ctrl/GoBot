#include "gobot/frame_tally.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace gobot {
namespace {

/// The order the classes are written in.
constexpr char kClasses[] = { 'S', 'W', 'U', 'C', 'D' };

std::uint32_t key_of(char cls, std::uint16_t opcode) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(cls)) << 16) | opcode;
}
char class_in(std::uint32_t key) { return static_cast<char>((key >> 16) & 0xFF); }
std::uint16_t opcode_in(std::uint32_t key) { return static_cast<std::uint16_t>(key & 0xFFFF); }

std::string hex4(std::uint16_t opcode) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", static_cast<unsigned>(opcode));
    return buf;
}

/// One class's kinds, most frames first, then by opcode.
std::vector<std::pair<std::uint16_t, std::uint64_t>> ranked(const std::map<std::uint32_t, std::uint64_t>& counts,
                                                            char cls) {
    std::vector<std::pair<std::uint16_t, std::uint64_t>> out;
    for (const auto& [key, n] : counts)
        if (class_in(key) == cls && n > 0) out.emplace_back(opcode_in(key), n);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return out;
}

} // namespace

char FrameTally::class_of(const GamePacket& p) {
    if (p.from_server) {
        if (p.udp) return 'U';
        return p.depth == 0 ? 'S' : 'W';
    }
    return p.udp ? 'D' : 'C';
}

void FrameTally::add(const std::vector<GamePacket>& all) {
    if (all.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const GamePacket& p : all) {
        const std::uint32_t key = key_of(class_of(p), p.opcode);
        ++counts_[key];
        ++counted_;
        if (seen_.insert(key).second) new_kinds_.push_back(key);
    }
}

std::string FrameTally::top_key() const {
    std::string key;
    for (const char cls : kClasses) {
        auto r = ranked(counts_, cls);
        if (r.size() > kTopKinds) r.resize(kTopKinds);
        // As a set: two kinds trading places is not a change.
        std::vector<std::uint16_t> ops;
        for (const auto& [op, n] : r) ops.push_back(op);
        std::sort(ops.begin(), ops.end());
        key += cls;
        for (const std::uint16_t op : ops) key += hex4(op) + ",";
        key += ';';
    }
    return key;
}

bool FrameTally::top_changed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (counted_ == 0) return false;
    return top_key() != top_at_take_;
}

std::string FrameTally::take(std::size_t per_class) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Room is always kept for the mark that says the text was cut.
    const std::string mark = " ...";
    std::string out;
    bool cut = false;
    const auto fits = [&out, &mark](const std::string& s) { return out.size() + s.size() + mark.size() <= kMaxText; };
    for (const char cls : kClasses) {
        if (cut) break;
        const auto r = ranked(counts_, cls);
        if (r.empty()) continue;
        const std::string head = std::string(out.empty() ? "" : " | ") + cls;
        if (!fits(head)) {
            cut = true;
            break;
        }
        out += head;
        std::size_t shown = 0;
        std::uint64_t rest_frames = 0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            if (i < per_class && !cut) {
                const std::string piece = " " + hex4(r[i].first) + ":" + std::to_string(r[i].second);
                if (fits(piece)) {
                    out += piece;
                    ++shown;
                    continue;
                }
                cut = true;
            }
            rest_frames += r[i].second;
        }
        if (!cut && shown < r.size()) {
            const std::string rest =
                " +" + std::to_string(r.size() - shown) + " kinds (" + std::to_string(rest_frames) + ")";
            if (fits(rest)) out += rest;
            else            cut = true;
        }
    }
    if (cut) out += mark;
    // What the game was sending, as of a take that had anything in it: an
    // empty one says nothing of that.
    if (counted_ != 0) top_at_take_ = top_key();
    counts_.clear();
    counted_ = 0;
    return out;
}

std::string FrameTally::take_new_kinds(std::size_t most) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (std::size_t i = 0; i < new_kinds_.size() && i < most; ++i)
        out += std::string(out.empty() ? "" : ", ") + class_in(new_kinds_[i]) + " " + hex4(opcode_in(new_kinds_[i]));
    if (new_kinds_.size() > most) out += " and " + std::to_string(new_kinds_.size() - most) + " more";
    new_kinds_.clear();
    return out;
}

std::uint64_t FrameTally::counted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counted_;
}

} // namespace gobot
