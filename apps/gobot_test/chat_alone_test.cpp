// gobot_test -- Tự chat's "Chỉ khi chưa có tổ đội": the tick on each line, how
// it is kept in chat.ini and carried through an edit of the box, and which
// line goes when the player is in a party.
//
// The party is a pretend one: the worker's look at the party HUD is a value
// the test sets. Nothing is typed or pressed anywhere.

#include "gobot/auto_chat.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

/// A file in this run's own temp folder, which main.cpp makes and removes.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string ticks_text(const std::vector<bool>& t) {
    std::string s;
    for (const bool b : t) s += b ? '1' : '0';
    return s;
}

void test_file() {
    section("Tự chat: each line's \"Chỉ khi chưa có tổ đội\" in chat.ini (PA-A1)");
    const std::string path = temp_path("gobot_chat_alone.ini");
    write_all(path, "# mine\n[Chat]\nenabled=false\n\n[Lines]\n# the lines\nline1=old\n");
    check(ChatSettings::save_lines(path, { "moi nguoi vao to doi", "ban do 2", "hello" }, { true, false, true }),
          "three lines written, the first and the last ticked");
    ChatSettings s;
    const bool loaded = s.load(path);
    check(loaded && s.lines.size() == 3 && ticks_text(s.only_alone) == "101",
          "and read back with their ticks: " + ticks_text(s.only_alone));
    const std::string text = read_all(path);
    check(text.find("line1_only_alone=true") != std::string::npos &&
              text.find("line2_only_alone") == std::string::npos &&
              text.find("line3_only_alone=true") != std::string::npos,
          "written as lineN_only_alone=true beside its line, and only where ticked");
    check(text.find("# mine") != std::string::npos && text.find("# the lines") != std::string::npos &&
              text.find("\r\n") == std::string::npos,
          "the comments stay, and the file keeps its LF line ends");
    check(s.alone_only(0) && !s.alone_only(1) && s.alone_only(2) && !s.alone_only(3) && !s.alone_only(-1),
          "alone_only says the same, and no for a line that is not there");

    // Written again with fewer lines: the old ticks go with the old lines.
    check(ChatSettings::save_lines(path, { "hello" }, { false }), "written again, one line, not ticked");
    ChatSettings t;
    const std::string again = read_all(path);
    check(t.load(path) && t.lines.size() == 1 && ticks_text(t.only_alone) == "0" &&
              again.find("_only_alone") == std::string::npos,
          "no tick left behind for a line that is gone, nor moved onto another");
    check(ChatSettings::save_lines(path, { "a", "b" }), "the lines-only save");
    ChatSettings u;
    check(u.load(path) && ticks_text(u.only_alone) == "00", "writes no tick at all");

    // A file from before the tick, or one written by hand.
    write_all(path, "[Chat]\nenabled=true\n[Lines]\nline1=one\nline2=two\n");
    ChatSettings old;
    check(old.load(path) && ticks_text(old.only_alone) == "00", "a file without ticks: every line unticked");
    write_all(path, "[Chat]\n[Lines]\nline1=one\nline1_only_alone=TRUE\nline3=three\nline3_only_alone=1\n"
                    "line2_only_alone=true\n");
    ChatSettings hand;
    const bool by_hand = hand.load(path);
    check(by_hand && hand.lines.size() == 2 && ticks_text(hand.only_alone) == "11",
          "ticks by hand, in any case and for a line with a gap before it; one for a line that is not "
          "there ticks nothing: " + ticks_text(hand.only_alone));
    DeleteFileA(path.c_str());

#ifdef GOBOT_SOURCE_DIR
    ChatSettings shipped;
    const bool read = shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/chat.ini");
    bool none = true;
    for (const bool b : shipped.only_alone) none = none && !b;
    // Nor a tick written for a line yet to come: no lineN_only_alone key at
    // the start of a line, only the comment that explains it.
    const std::string file = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/chat.ini");
    bool keyed = false;
    for (std::size_t at = 0; at < file.size();) {
        const std::size_t end = file.find('\n', at);
        const std::string line = file.substr(at, end == std::string::npos ? std::string::npos : end - at);
        keyed = keyed || (line.rfind("line", 0) == 0 && line.find("_only_alone") != std::string::npos);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    check(read && none && !keyed, "the shipped chat.ini ticks no line");
#endif
}

void test_carry() {
    section("Tự chat: a line keeps its tick by its words when the box is edited (PA-A1)");
    const std::vector<std::string> before = { "A moi vao to doi", "B ban do", "C chao" };
    const std::vector<bool> ticks = { true, false, true };
    check(ticks_text(ChatSettings::carry_ticks(before, ticks, before)) == "101", "nothing changed: the same ticks");
    check(ticks_text(ChatSettings::carry_ticks(before, ticks, { "C chao", "A moi vao to doi", "B ban do" })) == "110",
          "the lines moved round: each tick moves with its line");
    check(ticks_text(ChatSettings::carry_ticks(before, ticks, { "A moi vao to doi nhe", "B ban do", "C chao" })) ==
              "001",
          "a line's words changed: that line starts unticked");
    check(ticks_text(ChatSettings::carry_ticks(before, ticks, { "D moi", "A moi vao to doi" })) == "01",
          "a new line unticked, a kept one ticked wherever it went");
    check(ticks_text(ChatSettings::carry_ticks({ "x", "x" }, { false, true }, { "x", "x" })) == "01" &&
              ticks_text(ChatSettings::carry_ticks({ "x", "y", "x" }, { true, false, false }, { "x", "x" })) == "10",
          "the same words twice: each keeps its own tick, its own place first");
    check(ChatSettings::carry_ticks(before, {}, before).size() == 3, "no ticks before: as many, none ticked");
}

/// The line turns of a run, on a pretend clock, as chat_loop takes them: the
/// line whose turn it is, ticked or not; for a ticked one the party HUD's
/// word; then sent, passed over or put off. Returns which lines went.
struct Run {
    ChatSettings settings;
    std::vector<bool> typeable;
    std::string sent;               ///< the lines sent, by letter
    int looks = 0;                  ///< party HUD reads
    int holds = 0;                  ///< "auto chat" holds and mouse locks taken
    std::uint64_t shortest_gap = ~0ull;     ///< between two turns of one ticked line put off
    std::uint64_t waited_ms = 0;            ///< the last postpone

    std::string go(bool alone, std::uint64_t for_ms) {
        ChatSchedule schedule(7);
        std::uint64_t now = 1'000'000;
        schedule.start(now, settings);
        std::vector<std::uint64_t> last(settings.lines.size(), 0);
        for (const std::uint64_t until = now + for_ms; now < until; now += 100) {
            if (!schedule.due(now)) continue;
            const int index = schedule.turn();
            if (settings.alone_only(index)) {
                ++looks;
                const auto i = static_cast<std::size_t>(index);
                if (last[i] != 0) shortest_gap = std::min(shortest_gap, now - last[i]);
                last[i] = now;
            }
            const AloneTurn turn = settings.alone_only(index)
                                       ? alone_turn(settings.only_alone, typeable, index, alone)
                                       : AloneTurn::Send;
            if (turn == AloneTurn::Skip) { schedule.skip(now); continue; }
            if (turn == AloneTurn::Wait) { schedule.postpone(now, kAloneWaitMs); waited_ms = kAloneWaitMs; continue; }
            ++holds;                // the mouse and the hold, only for a line that goes
            sent += settings.lines[static_cast<std::size_t>(index)];
            schedule.sent(now);
        }
        return sent;
    }
};

Run two_lines(bool a_ticked, bool b_ticked) {
    Run r;
    r.settings.interval_s = 30;
    r.settings.jitter_pct = 0;
    r.settings.first_delay_s = 0;
    r.settings.lines = { "A", "B" };
    r.settings.only_alone = { a_ticked, b_ticked };
    r.typeable = { true, true };
    return r;
}

void test_turns() {
    section("Tự chat: a ticked line in a party, alone, and not read (PA-A2, PA-A3)");
    // [A ticked, B]: in a party A is passed over and B goes, for as long as
    // the party lasts; alone A goes in its turn.
    {
        Run r = two_lines(true, false);
        const std::string went = r.go(false, 100'000);
        check(went.find('A') == std::string::npos && !went.empty() && went.front() == 'B',
              "in a party: A passed over, B sent next -- sent " + went);
        check(r.holds == static_cast<int>(went.size()),
              "and only the lines sent took the hold and the mouse: " + std::to_string(r.holds));
    }
    {
        Run r = two_lines(true, false);
        const std::string went = r.go(true, 100'000);
        check(went.size() >= 3 && went.substr(0, 3) == "ABA", "alone: A sent in its turn -- sent " + went);
    }
    {
        // The HUD not read is not "alone": a missed advert is better than one
        // sent in a party.
        const std::vector<bool> ticks = { true, false };
        check(alone_turn(ticks, { true, true }, 0, false) == AloneTurn::Skip,
              "the HUD unread: A passed over, as in a party");
        check(alone_turn(ticks, { true, true }, 1, false) == AloneTurn::Send, "B, unticked, goes whatever the HUD");
    }
    {
        // Every line ticked, in a party: each put off a minute, never passed
        // round and round a second apart.
        Run r = two_lines(true, true);
        const std::string went = r.go(false, 300'000);
        check(went.empty() && r.waited_ms == kAloneWaitMs && r.shortest_gap >= 60'000 && r.looks <= 6,
              "all ticked, in a party: nothing sent, put off 60 s at a time (" + std::to_string(r.looks) +
                  " looks in 5 minutes, shortest gap " + std::to_string(r.shortest_gap / 1000) + " s)");
        check(r.holds == 0, "and no hold or mouse taken for any of it");
    }
    {
        // The only unticked line has nothing left to type: no line to go
        // instead, so the ticked one waits a minute rather than spinning.
        const std::vector<bool> ticks = { true, false };
        check(alone_turn(ticks, { true, false }, 0, false) == AloneTurn::Wait,
              "the only unticked line has nothing to type: the ticked one waits");
        check(alone_turn({ false, false }, { true, true }, 0, false) == AloneTurn::Send &&
                  alone_turn({}, { true }, 0, false) == AloneTurn::Send,
              "an unticked line goes, party or not");
    }
}

void test_unseen_party() {
    section("Tự chat: a ticked line with the party's panels out of view (F-RF-1)");
    check(alone_look(true, true) == AloneLook::Read, "the game in front, its party's panels in view: read");
    check(alone_look(false, true) == AloneLook::Unread,
          "in front, a window over the panels or the game past the screen's edge: no reading, as a HUD not read");
    check(alone_look(true, false) == AloneLook::KeepTurn && alone_look(false, false) == AloneLook::KeepTurn,
          "behind another window: the turn kept, as every line waits then");

    // [A ticked, B], a line every 30 s, as chat_loop takes the turns: a
    // ticked line's look first, then the gate every line goes through -- the
    // game in front -- then sent.
    struct Seen {
        bool trusted = true, focused = true, alone = true;
    };
    const auto run = [](const std::function<Seen(std::uint64_t)>& seen, std::uint64_t for_ms) {
        ChatSettings s;
        s.interval_s = 30;
        s.jitter_pct = 0;
        s.first_delay_s = 0;
        s.lines = { "A", "B" };
        s.only_alone = { true, false };
        const std::vector<bool> typeable = { true, true };
        ChatSchedule schedule(7);
        const std::uint64_t start = 1'000'000;
        std::uint64_t now = start;
        schedule.start(now, s);
        std::string sent;
        for (const std::uint64_t until = now + for_ms; now < until; now += 100) {
            if (!schedule.due(now)) continue;
            const int index = schedule.turn();
            const Seen w = seen(now - start);
            if (s.alone_only(index)) {
                const AloneLook look = alone_look(w.trusted, w.focused);
                if (look == AloneLook::KeepTurn) {
                    schedule.postpone(now, 5000);
                    continue;
                }
                const AloneTurn turn = alone_turn(s.only_alone, typeable, index, look == AloneLook::Read && w.alone);
                if (turn == AloneTurn::Skip) { schedule.skip(now); continue; }
                if (turn == AloneTurn::Wait) { schedule.postpone(now, kAloneWaitMs); continue; }
            }
            if (!w.focused) {           // chat_gate: no line with the game behind another window
                schedule.postpone(now, 5000);
                continue;
            }
            sent += s.lines[static_cast<std::size_t>(index)];
            schedule.sent(now);
        }
        return sent;
    };
    {
        // The game in front, and a window kept on top over its left side the
        // whole time: B goes at every turn of its own, A is passed over. Its
        // turn kept instead, nothing at all would go.
        const std::string went = run([](std::uint64_t) { return Seen{ false, true, true }; }, 200'000);
        check(went.find('A') == std::string::npos && went.size() >= 3 && went.find_first_not_of('B') ==
                                                                              std::string::npos,
              "the panels never in view with the game in front: A passed over, B sent -- sent " + went);
    }
    {
        // Behind another window for a minute, then in front with the panels
        // in view and the player alone: nothing went meanwhile, and A, whose
        // turn it was, goes first.
        const std::string went = run(
            [](std::uint64_t t) { return t < 60'000 ? Seen{ false, false, true } : Seen{ true, true, true }; },
            100'000);
        check(!went.empty() && went.front() == 'A',
              "the game behind another window: nothing sent, A's turn kept, A sent first once it is in front -- "
              "sent " + went);
    }
}

/// Draw `pic` onto `f` at `x`, `y`, its see-through pixels left out.
void blit(Frame& f, const Frame& pic, int x, int y) {
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) continue;
            const std::uint8_t* s = &pic.pixels[(static_cast<std::size_t>(py) * pic.width + px) * 4];
            if (s[3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
            std::copy_n(s, 3, d);
            d[3] = 255;
        }
}

void test_dialog_names() {
    section("Tự chat: what a dialog that keeps Enter back is called (the 2A tester's problem 14)");
    TemplateStore store;
    if (!store.load_best(exe_relative("configs/templates.ini")) || !ChatDialogs::can_check(store)) {
        std::printf("  [skip] the dialogs' pictures are not installed here\n");
        return;
    }
    Frame plain;
    plain.width = 1280;
    plain.height = 720;
    plain.pixels.assign(static_cast<std::size_t>(1280) * 720 * 4, 255);
    for (std::size_t i = 0; i < plain.pixels.size(); i += 4) plain.pixels[i] = plain.pixels[i + 1] = 30;
    const ChatDialogs dialogs;
    // The invitation's picture is only the pair of buttons every question
    // wears: the revive question was told the player an invitation was up.
    struct Case {
        const char* picture;
        const char* called;
    };
    for (const Case& c : { Case{ "grabbed/hoisinh_confirm", "hồi sinh" },
                           Case{ "grabbed/del_item_confirm_1", "thùng rác" } }) {
        const Frame* pic = store.get(c.picture);
        if (pic == nullptr) {
            std::printf("  [skip] no %s here\n", c.picture);
            continue;
        }
        Frame f = plain;
        blit(f, *pic, 500, 300);
        const std::string said = dialogs.up(f, store);
        check(said == c.called, std::string(c.picture) + " up: \"" + said + "\", never an invitation");
    }
    if (const Frame* pair = store.get("grabbed/party_invite_buttons")) {
        Frame f = plain;
        blit(f, *pair, 540, 560);
        const std::string said = dialogs.up(f, store);
        check(!said.empty(), "the invitation's buttons alone: still keep Enter back -- \"" + said + "\"");
    }
}

} // namespace

void test_chat_alone(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_file();
    test_carry();
    test_turns();
    test_unseen_party();
    test_dialog_names();
}
