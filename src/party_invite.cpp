#include "gobot/party_invite.hpp"
#include "gobot/bag.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/party_chat.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace gobot {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// The ASCII letter behind an accented one.
///
/// Vietnamese puts up to two marks on a vowel and the client's text is full of
/// them -- "Yêu cầu của". Two things make this necessary rather than tidy.
/// The config file has to be editable by somebody who cannot type ố, and the
/// bitmap reader gets a mark wrong far more often than it gets the letter
/// wrong, so a comparison that ignores marks is a comparison that works.
char base_letter(unsigned int cp) {
    // The precomposed Vietnamese block, which is laid out one vowel at a time.
    if (cp >= 0x1EA0 && cp <= 0x1EB7) return 'a';
    if (cp >= 0x1EB8 && cp <= 0x1EC7) return 'e';
    if (cp >= 0x1EC8 && cp <= 0x1ECB) return 'i';
    if (cp >= 0x1ECC && cp <= 0x1EE3) return 'o';
    if (cp >= 0x1EE4 && cp <= 0x1EF1) return 'u';
    if (cp >= 0x1EF2 && cp <= 0x1EF9) return 'y';

    switch (cp) {
        case 0x0102: case 0x0103: return 'a';    // Ăă
        case 0x0110: case 0x0111: return 'd';    // Đđ
        case 0x0128: case 0x0129: return 'i';    // Ĩĩ
        case 0x0168: case 0x0169: return 'u';    // Ũũ
        case 0x01A0: case 0x01A1: return 'o';    // Ơơ
        case 0x01AF: case 0x01B0: return 'u';    // Ưư
        default: break;
    }

    if (cp >= 0x00C0 && cp <= 0x00C5) return 'a';
    if (cp >= 0x00C8 && cp <= 0x00CB) return 'e';
    if (cp >= 0x00CC && cp <= 0x00CF) return 'i';
    if (cp >= 0x00D2 && cp <= 0x00D6) return 'o';
    if (cp >= 0x00D9 && cp <= 0x00DC) return 'u';
    if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
    if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
    if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
    if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';
    if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
    if (cp == 0x00DD || cp == 0x00FD || cp == 0x00FF) return 'y';
    return 0;
}

/// Wait `ms`, asking `stop` every 50 ms; false when it said stop. Against the
/// clock rather than by adding up the slices, which Windows rounds up to its
/// tick each.
bool nap(int ms, const std::function<bool()>& stop) {
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(std::max(0, ms));
    for (;;) {
        if (stop && stop()) return false;
        const auto left = until - clock::now();
        if (left <= clock::duration::zero()) return true;
        std::this_thread::sleep_for(std::min<clock::duration>(left, std::chrono::milliseconds(50)));
    }
}

} // namespace

std::string PartyInvite::fold(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (c < 0x80) {
            if (!std::isspace(c))
                out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }

        // Decode one UTF-8 character. Anything malformed is skipped rather
        // than guessed at: a reader that produced it has already failed.
        unsigned int cp = 0;
        std::size_t len = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        else                         { ++i; continue; }

        if (i + len > text.size()) break;
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char n = static_cast<unsigned char>(text[i + k]);
            if ((n & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (n & 0x3Fu);
        }
        i += len;
        if (!ok) continue;

        const char letter = base_letter(cp);
        if (letter != 0) out.push_back(letter);
    }
    return out;
}

void PartyInvite::Settings::load(const IniFile& ini) {
    picture    = ini.get("Invite", "picture", picture);
    score      = ini.get_float("Invite", "score", score);
    accept_dx  = ini.get_int("Invite", "accept_dx", accept_dx);
    accept_dy  = ini.get_int("Invite", "accept_dy", accept_dy);
    decline_dx = ini.get_int("Invite", "decline_dx", decline_dx);
    decline_dy = ini.get_int("Invite", "decline_dy", decline_dy);
    name_dx    = ini.get_int("Invite", "name_dx", name_dx);
    name_dy    = ini.get_int("Invite", "name_dy", name_dy);
    name_w     = ini.get_int("Invite", "name_w", name_w);
    name_h     = ini.get_int("Invite", "name_h", name_h);
    prefix     = ini.get("Invite", "prefix", prefix);
    settle_ms  = std::max(0, ini.get_int("Invite", "settle_ms", settle_ms));
}

void PartyInvite::Settings::save(IniFile& ini) const {
    ini.set("Invite", "picture", picture);
    ini.set_float("Invite", "score", score);
    ini.set_int("Invite", "accept_dx", accept_dx);
    ini.set_int("Invite", "accept_dy", accept_dy);
    ini.set_int("Invite", "decline_dx", decline_dx);
    ini.set_int("Invite", "decline_dy", decline_dy);
    ini.set_int("Invite", "name_dx", name_dx);
    ini.set_int("Invite", "name_dy", name_dy);
    ini.set_int("Invite", "name_w", name_w);
    ini.set_int("Invite", "name_h", name_h);
    ini.set("Invite", "prefix", prefix);
    ini.set_int("Invite", "settle_ms", settle_ms);
}

std::string PartyInvite::name_in(const std::string& line, const std::string& prefix) {
    const std::string text = trim(line);
    if (text.empty()) return {};

    std::size_t from = 0;
    if (!prefix.empty()) {
        // Found by walking the line a character at a time and folding what has
        // been seen so far, so that "của" in the client matches "cua" in a
        // config file somebody typed. Doing it the obvious way -- folding the
        // whole line and searching that -- finds the prefix at an index into
        // the FOLDED string, which is not where it is in the real one, and the
        // name then gets cut in the wrong place.
        const std::string want = fold(prefix);
        if (want.empty()) return {};

        std::size_t at = std::string::npos;
        std::string seen;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            seen = fold(text.substr(0, i));
            if (seen.size() >= want.size() &&
                seen.compare(seen.size() - want.size(), want.size(), want) == 0) {
                at = i;
                break;
            }
        }
        if (at == std::string::npos) return {};
        from = at;
    }

    std::string who = trim(text.substr(from));

    // The client ends the sentence with a full stop. Drop it, and anything
    // else trailing that is punctuation rather than part of a name.
    while (!who.empty() && (who.back() == '.' || who.back() == ',' ||
                            who.back() == '!' || who.back() == '?' ||
                            who.back() == ':' || who.back() == ';'))
        who.pop_back();

    // Spaces are dropped rather than split on.
    //
    // A player name has no space in it, so any space on this line came from
    // the reader: a live invitation from "ExampleWin" was read as
    // "Exampley in", the W coming out as two characters with a gap between
    // them. Splitting on the space yields "Exampley" and throws away the rest;
    // joining yields "Exampleyin", which is one letter out of ten wrong and
    // still matches the real name well enough to check against a list.
    std::string joined;
    for (const char c : who)
        if (c != ' ' && c != '\t') joined.push_back(c);

    return joined;
}

PartyInvite::Seen PartyInvite::look(const Frame& frame, const TemplateStore& store,
                                    const GlyphFont* font) const {
    Seen out;
    if (!frame.valid()) return out;

    const Frame* picture = store.get(settings_.picture);
    if (picture == nullptr || !picture->valid()) return out;

    const auto hit = TemplateMatcher::find(
        frame, ScreenRect{ 0, 0, frame.width, frame.height }, *picture,
        settings_.score);
    if (!hit.found) return out;

    out.up    = true;
    out.at    = hit.at;
    out.score = hit.score;

    out.accept_x  = hit.at.x + settings_.accept_dx;
    out.accept_y  = hit.at.y + settings_.accept_dy;
    out.decline_x = hit.at.x + settings_.decline_dx;
    out.decline_y = hit.at.y + settings_.decline_dy;

    if (font == nullptr || !font->loaded()) return out;

    // The line of text, clamped into the frame: the dialog sits at the bottom
    // of the screen and the box above it can start outside a short capture.
    ScreenRect box;
    box.x = std::max(0, hit.at.x + settings_.name_dx);
    box.y = std::max(0, hit.at.y + settings_.name_dy);
    box.w = std::min(settings_.name_w, frame.width - box.x);
    box.h = std::min(settings_.name_h, frame.height - box.y);
    if (box.w <= 0 || box.h <= 0) return out;

    out.line = font->read_line(frame, box);
    out.who  = name_in(out.line, settings_.prefix);
    return out;
}

PartyInvite::Verdict PartyInvite::decide(const Seen& seen, bool bin_asking,
                                         const Party::Settings& rules) {
    Verdict v;
    if (!seen.up) {
        v.why = "no invitation on screen";
        return v;
    }
    // The footer picture matches the bin's quantity dialog at 0.90, and the
    // drop and revive questions at 0.86. A line with no name after the prefix
    // ("của") is no invitation's, whatever accept_all says.
    if (bin_asking) {
        v.why = "it is the trash bin's own dialog";
        return v;
    }
    if (seen.who.empty()) {
        v.why = "its line does not read as an invitation's";
        return v;
    }

    const bool name_matters =
        !rules.accept_all || (rules.use_blacklist && !rules.blacklist.empty());
    if (name_matters && !rules.may_accept(seen.who)) {
        v.answer = Answer::Decline;
        v.why = "the rules in party.ini say no";
        return v;
    }
    v.answer = Answer::Accept;
    v.why = "the rules in party.ini allow it";
    return v;
}

bool PartyInvite::same_invitation(const Seen& first, const Seen& again,
                                  bool bin_asking_again) {
    if (!first.up || !again.up || bin_asking_again) return false;
    if (first.who.empty() || again.who.empty()) return false;
    if (fold(first.who) != fold(again.who)) return false;
    return std::abs(again.at.x - first.at.x) <= 3 && std::abs(again.at.y - first.at.y) <= 3;
}

bool PartyInvite::answer(InputManager& input, const std::function<bool(Frame&)>& grab,
                         const TemplateStore& store, const GlyphFont* font, const Seen& seen,
                         bool accept, const std::function<bool()>& stop,
                         std::string* why) const {
    const auto said = [why](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    // The pause before accepting, which a stop cuts short.
    if (!nap(accept ? settings_.settle_ms : 0, stop)) return said("stopped before the click");
    if (font == nullptr || !font->loaded())
        return said("there is no bitmap font to read the name again with");

    // The screen a moment later is not the one this was decided on. Answered
    // only if a new look shows the same invitation, from the same player, and
    // the bin has not put up a dialog of its own meanwhile.
    Frame now;
    if (!grab || !grab(now) || !now.valid()) return said("no new look at the game");
    const Seen again = look(now, store, font);
    const bool bin = again.up && bag::bin_asking(now, store);
    if (!same_invitation(seen, again, bin))
        return said("the dialog changed, moved or went before the click");

    InputManager::GameClick how = InputManager::GameClick::Failed;
    const bool sent = accept
        ? input.click_on_game(again.accept_x, again.accept_y, false, stop, &how)
        : input.click_on_game(again.decline_x, again.decline_y, false, stop, &how);
    if (!sent)
        return said(std::string("the click was not made: ") + InputManager::game_click_name(how));
    return true;
}

const char* InviteReply::how_name(How h) {
    switch (h) {
        case How::None: return "none";
        case How::Closed: return "closed";
        case How::NotClosed: return "not closed";
        case How::NotClicked: return "not clicked";
        case How::Stopped: return "stopped";
    }
    return "?";
}

InviteReply::Result InviteReply::watch(const TemplateStore& store, const GlyphFont* font, const Hands& hands,
                                       int watch_ms, int first_gone_ms, int clicks) {
    const int click_count = std::max(1, clicks);
    Result r;
    const auto stopped = [&] { return hands.stop && hands.stop(); };
    const auto whole = [](const Frame& f) { return ScreenRect{ 0, 0, f.width, f.height }; };
    // The time gone by, as the naps have it: a pretend game's waits are its
    // naps, and the looks between them are quick. And by the clock, when the
    // hands have one (the review of 2026.11.14b): on a 1-CPU guest each look
    // -- a grab and a whole-screen search -- takes a good part of a second,
    // and the game's mouse is held all the while.
    int waited = 0;
    const auto nap = [&](int ms) {
        waited += ms;
        return hands.nap ? hands.nap(ms) : true;
    };
    const std::uint64_t began = hands.now_ms ? hands.now_ms() : 0;
    const auto clock_ms = [&]() -> int {
        return hands.now_ms ? static_cast<int>(hands.now_ms() - began) : 0;
    };
    if (!hands.grab || !hands.click) return r;

    // 1. A lone Xác Nhận anywhere -- bag::lone_ok, which never takes an
    //    invitation's Xác Nhận, nor the bin's or the revive question's: Hủy Bỏ
    //    stands right of theirs, not the box's frame -- on two looks kLookMs
    //    apart at one place: a box caught for one frame is none.
    bool have_first = false, confirmed = false;
    ScreenRect first;
    int first_at = 0;
    for (;;) {
        if (stopped()) {
            r.how = How::Stopped;
            return r;
        }
        Frame f;
        ScreenRect label;
        if (hands.grab(f) && f.valid() && bag::lone_ok(f, store, whole(f), &label)) {
            const bool same = have_first && std::abs(label.x - first.x) <= kSlack &&
                              std::abs(label.y - first.y) <= kSlack;
            if (same && waited - first_at >= kLookMs) {
                r.label = label;
                r.seen_after_ms = first_at;
                r.shot = std::move(f);
                confirmed = true;
                break;
            }
            if (!same) {
                have_first = true;
                first = label;
                first_at = waited;
            }
        } else {
            have_first = false;
        }
        // The clock's end leaves a box seen on this look its one look more,
        // to be taken or not: a short watch on a slow guest is two looks.
        if (waited >= watch_ms || (clock_ms() >= watch_ms && !have_first)) break;
        if (!nap(kLookMs)) {
            r.how = How::Stopped;
            return r;
        }
    }
    if (!confirmed) return r;

    // 2. Its words, for the log only: the rows of the box's text, placed from
    //    its label as grabbed/dialog_ok has them (label at 149,57, text from
    //    20,12, as the stall watch reads a notice).
    if (font != nullptr && font->loaded()) {
        for (int row = 0; row < 3; ++row) {
            const int x0 = std::max(0, r.label.x - 129), y0 = std::max(0, r.label.y - 45 + row * 14);
            const ScreenRect at{ x0, y0, std::min(158, r.shot.width - x0), std::min(15, r.shot.height - y0) };
            if (at.w <= 0 || at.h <= 0) continue;
            const std::string text = font->read_line(r.shot, at);
            if (!text.empty()) r.words += (r.words.empty() ? "" : " ") + text;
        }
    }

    // 3. Its Xác Nhận clicked in the middle, the pointer moved off under it --
    //    a lit button is not lone_ok's -- and the box seen gone: two looks
    //    kLookMs apart, within kFirstGoneMs of the first click (kReactMs since
    //    2026-11-15, the owner's 30 s for slow PCs; kGoneMs, 2.5 s, before) and
    //    kGoneMs of the second, that find neither its label nor the message's
    //    picture (bag::notice_up, which a lit button does not fool). Once
    //    more, a pixel aside, on a fresh look that finds its label; no third
    //    time. The party worker's own watch (2026.11.17b) is one click and
    //    kCloseHoldMs: it clicks again on a later pass, the mouse let go.
    int first_click_at = -1, first_click_waited = 0;
    for (int attempt = 0; attempt < click_count; ++attempt) {
        const int gone_ms = attempt == 0 ? first_gone_ms : kGoneMs;
        if (stopped()) {
            r.how = How::Stopped;
            return r;
        }
        const int cx = r.label.x + r.label.w / 2 + attempt, cy = r.label.y + r.label.h / 2;
        if (!hands.click(cx, cy)) {
            r.how = stopped() ? How::Stopped : How::NotClicked;
            return r;
        }
        ++r.clicks;
        if (hands.move) hands.move(cx, r.label.y + r.label.h + kParkPx);
        int since = 0, gone_at = -1;
        // By the clock too; a look that saw it gone gets its second all the
        // same, so that a box gone on a slow guest is not called still up.
        const int clicked_at = clock_ms();
        if (attempt == 0) {
            first_click_at = clicked_at;
            first_click_waited = waited;
        }
        // From the first click, by the clock when there is one, else by the
        // naps: what Result::gone_after_ms says.
        const auto from_first = [&] {
            return hands.now_ms ? clock_ms() - first_click_at : waited - first_click_waited;
        };
        int gone_seen_at = -1;
        while (since < gone_ms && (clock_ms() - clicked_at < gone_ms || gone_at >= 0)) {
            // Every nap asks the stop (Hands::nap is false once stopped): a
            // wait of kFirstGoneMs holds the game's mouse, and a revive waits
            // for it.
            if (!nap(kLookMs)) {
                r.how = How::Stopped;
                return r;
            }
            since += kLookMs;
            Frame h;
            if (!hands.grab(h) || !h.valid()) continue;
            if (bag::lone_ok(h, store, whole(h)) || bag::notice_up(h, store)) {
                gone_at = -1;
                continue;
            }
            if (gone_at < 0) {
                gone_at = since;
                gone_seen_at = from_first();
            } else if (since - gone_at >= kLookMs) {
                r.how = How::Closed;
                r.gone_after_ms = gone_seen_at;
                return r;
            }
        }
        if (attempt + 1 >= click_count) break;
        Frame h;
        ScreenRect label;
        if (!hands.grab(h) || !h.valid() || !bag::lone_ok(h, store, whole(h), &label)) break;
        r.label = label;
    }
    r.how = How::NotClosed;
    return r;
}

bool InviteTries::same(const std::string& a, const std::string& b) {
    const std::string fa = PartyInvite::fold(a), fb = PartyInvite::fold(b);
    if (fa.empty() || fb.empty()) return false;
    return fa == fb || PartyChat::alike(fa, fb) >= kNameAlike;
}

std::string InviteTries::key_of(const std::string& who) const {
    // The name as first kept: a letter read differently next time is still
    // the same player's count and hold.
    for (const auto& [key, n] : fails_)
        if (same(key, who)) return key;
    for (const auto& [key, until] : held_until_)
        if (same(key, who)) return key;
    return who;
}

void InviteTries::sent(const std::string& who, std::uint64_t now_ms, int size_before) {
    if (who.empty()) return;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return same(p.who, who); }),
                   pending_.end());
    // Nothing else out: the joins credited so far concern no invite left.
    if (pending_.empty()) credits_.clear();
    Pending p;
    p.who = who;
    p.sent_ms = now_ms;
    p.size_before = std::max(0, size_before);
    p.credits_before = credits_.size();
    pending_.push_back(p);
}

int InviteTries::credited_since(const Pending& p) const {
    int n = 0;
    for (std::size_t i = p.credits_before; i < credits_.size(); ++i) {
        const Credit& c = credits_[i];
        // One sent after p's: its member came after p's HUD was read. One
        // sent before p's: its member is in p's size_before already when
        // that size is more than its own was -- it joined first.
        if (c.sent_ms > p.sent_ms || c.size_before >= p.size_before) ++n;
    }
    return n;
}

bool InviteTries::due(std::uint64_t now_ms) const {
    for (const Pending& p : pending_)
        if (now_ms >= p.sent_ms && now_ms - p.sent_ms >= kJoinWaitMs) return true;
    return false;
}

std::vector<InviteTries::Outcome> InviteTries::judge(const Hud& hud, std::uint64_t now_ms) {
    std::vector<Outcome> out;
    // Oldest first, so that one more member seen goes to the invite that
    // went out first.
    std::vector<Pending> ready, left;
    for (const Pending& p : pending_) {
        const bool is_due = now_ms >= p.sent_ms && now_ms - p.sent_ms >= kJoinWaitMs;
        // A look from before GoBot's last change to the party: a later one,
        // a pass or two on -- unless it has been too long already.
        const bool wait_more = hud.stale && now_ms - p.sent_ms < kJoinWaitMs + kStaleWaitMs;
        (is_due && !wait_more ? ready : left).push_back(p);
    }
    pending_ = left;
    std::sort(ready.begin(), ready.end(), [](const Pending& a, const Pending& b) { return a.sent_ms < b.sent_ms; });
    for (const Pending& p : ready) {
        // Members seen now beyond those before, less those already credited
        // to another invite out at once that this one's size_before did not
        // show yet.
        const int credited = credited_since(p);
        const bool read_both = hud.size > 0 && p.size_before > 0;
        if (read_both && hud.size - p.size_before - credited >= 1) {
            credits_.push_back(Credit{ p.sent_ms, p.size_before });
            const std::string key = key_of(p.who);
            fails_.erase(key);
            held_until_.erase(key);
            Outcome o;
            o.who = p.who;
            o.joined = true;
            o.why = "the party HUD shows " + std::to_string(hud.size) + " member(s), " +
                    std::to_string(p.size_before) + " before the invite";
            out.push_back(o);
            continue;
        }
        std::string why;
        if (hud.size <= 0)
            why = "the party HUD could not be read " + std::to_string(kJoinWaitMs / 1000) + " s after the invite";
        else if (p.size_before <= 0)
            why = "the party HUD was not read before the invite, so a join cannot be told";
        else
            why = "no new member on the party HUD " + std::to_string(kJoinWaitMs / 1000) + " s after the invite (" +
                  std::to_string(p.size_before) + " before, " + std::to_string(hud.size) + " now)";
        out.push_back(failed(p.who, now_ms, why));
    }
    return out;
}

InviteTries::Outcome InviteTries::failed(const std::string& who, std::uint64_t now_ms, const std::string& why) {
    Outcome o;
    o.who = who;
    o.why = why;
    if (who.empty()) return o;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return same(p.who, who); }),
                   pending_.end());
    const std::string key = key_of(who);
    const int n = ++fails_[key];
    o.fails = n;
    o.hold_ms = n >= kFailsForLong ? kFailHoldLongMs : kFailHoldMs;
    std::uint64_t& until = held_until_[key];
    until = std::max(until, now_ms + o.hold_ms);
    return o;
}

bool InviteTries::held(const std::string& who, std::uint64_t now_ms, std::uint64_t* left_ms) const {
    std::uint64_t longest = 0;
    for (const auto& [key, until] : held_until_)
        if (now_ms < until && same(key, who)) longest = std::max(longest, until - now_ms);
    if (left_ms != nullptr) *left_ms = longest;
    return longest > 0;
}

int InviteTries::fails(const std::string& who) const {
    for (const auto& [key, n] : fails_)
        if (same(key, who)) return n;
    return 0;
}

bool InviteTries::waiting(const std::string& who) const {
    for (const Pending& p : pending_)
        if (same(p.who, who)) return true;
    return false;
}

} // namespace gobot
