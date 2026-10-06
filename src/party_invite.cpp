#include "gobot/party_invite.hpp"

#include <algorithm>
#include <cctype>

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

} // namespace gobot
