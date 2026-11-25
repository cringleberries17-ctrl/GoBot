#pragma once

#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"

#include <string>
#include <vector>

namespace gobot {

/// Finds a line of an NPC's menu on screen by its words (2026.11.19).
///
/// The game draws its NPC menus -- a Teleporter's places, Thợ Mỏ's "Shop", Cổ
/// Thụ's "Ngọc Thụ Thần - Level trên 160" -- in a TrueType face, not in the
/// bitmap font GlyphFont reads. GoC finds those rows the same way as here
/// (go_client 0x140102400): the words drawn with data\font\WOD.ttf -- Arial
/// when it is missing -- the screen cut at grey 150, and the two matched,
/// 0.70 needed. GoC's WOD.ttf is read where GoC is installed, never copied;
/// without it Arial and Tahoma are tried, at a few sizes each, bold and not,
/// bright words on a dark box and dark on a light one.
///
/// Nothing of it has been seen against the live client: each miss keeps its
/// picture (the worker's debug folder) and says the best it came to.
class MenuText {
public:
    struct Hit {
        bool        found = false;
        ScreenRect  at;             ///< the words, in frame coordinates
        float       score = 0.0f;   ///< the best score, found or not
        int         px = 0;         ///< the size that did best
        bool        bold = false;
        bool        dark = false;   ///< dark words on a light box
        std::string face;           ///< the face that did best, for the log
    };

    static constexpr float kMinScore = 0.70f;
    static constexpr int   kCut = 150;

    /// The best place `utf8` is written in `area` of `f`.
    static Hit find(const Frame& f, const ScreenRect& area, const std::string& utf8, float min_score = kMinScore);

    /// `utf8` drawn white on black in `face` at `px`, cut to its ink with one
    /// pixel round it; empty when nothing was drawn. For the tests and for
    /// find.
    static Frame render(const std::string& utf8, const std::string& face, int px, bool bold);

    /// The faces tried, in order: WOD.ttf's own family when GoC's file is
    /// there, then Arial and Tahoma.
    static std::vector<std::string> faces();

    /// A TrueType file's family name (its 'name' table, id 1), or empty.
    static std::string ttf_family(const std::string& path);
};

} // namespace gobot
