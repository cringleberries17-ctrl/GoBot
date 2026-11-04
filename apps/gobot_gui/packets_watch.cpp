// The game's packets, watched all session (the packets plan, Slice 1): the
// "Packets:" line once a minute, the gaps in what the capture saw, and what
// the link to the server looks like by its frames.
//
// What starts, follows and stops the capture is gui_app.cpp's (positions_tick,
// position_reattach, position_job). This is the log's side of it. Nothing here
// acts on the game, and nothing here asks Windows for anything but the time of
// day: the capture's counts, the packet world and PacketSelf are each read as
// a copy under their own lock.

#include "gui_app.hpp"
#include "gobot/logger.hpp"

#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

namespace gobot {
namespace {

/// `ms` ago at `now`, in seconds with one decimal, for the log.
std::string seconds_ago(std::uint64_t now, std::uint64_t at) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s ago", static_cast<double>(now > at ? now - at : 0) / 1000.0);
    return buf;
}

/// The time of day as the log writes it, HH:MM:SS.
std::string time_of_day() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return "?";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

} // namespace

void GuiApp::note_capture_gaps(std::uint64_t now) {
    const PacketCapture::Stats cs = capture_.stats();
    // Another start, whoever made it -- this one's, the Capture tab's,
    // Autopilot's: its counts begin at 0, so a drop or a wrapper that would
    // not unpack since then is a gap like any other, compared below. Not the
    // server's resyncs: joining its stream mid-way costs one, and they are
    // measured from here.
    if (cs.starts != capture_starts_seen_) {
        capture_starts_seen_ = cs.starts;
        capture_seen_ = PacketCapture::Stats{};
        capture_seen_.server_resync_bytes = cs.server_resync_bytes;
        capture_since_ = time_of_day();
    }
    if (!capture_.running()) return;
    // Something of the stream was missed: frames Npcap dropped, a wrapper
    // from the server that would not unpack, or bytes of the server's stream
    // skipped to find the next frame. What PacketSelf keeps may then not be
    // the game's now, and it waits for the next frame of each.
    const char* why = nullptr;
    if (cs.pcap_dropped > capture_seen_.pcap_dropped)
        why = "frames dropped before the capture read them";
    else if (cs.unwrap.unpack_failed > capture_seen_.unwrap.unpack_failed)
        why = "a wrapper that would not unpack";
    else if (cs.server_resync_bytes > capture_seen_.server_resync_bytes)
        why = "bytes of the server's stream skipped";
    capture_seen_ = cs;
    if (why != nullptr) packet_self_.mark_gap(now, why);
}

std::string GuiApp::packet_link_text(std::uint64_t now) const {
    const PacketSelf::Link link = packet_self_.link(now);
    std::ostringstream out;
    out << "link: ticks " << link.ticks_last_min << "/min, last server frame "
        << (link.last_server_ms == 0 ? std::string("none yet") : seconds_ago(now, link.last_server_ms))
        << ", world-entry bursts " << link.bursts << "; 0x0014 " << link.selected << "/" << link.channel;
    return out.str();
}

void GuiApp::packets_minute(std::uint64_t now) {
    if (!capture_.running()) return;
    // Each part a copy, taken under its own lock; the line is built here.
    const PacketCapture::Stats cs = capture_.stats();
    const PacketSelf::Stats ss = packet_self_.stats();
    const PacketSelf::Link link = packet_self_.link(now);
    const PacketWorld::View v = packet_world_.view(now);
    const std::string kinds = frame_tally_.take_new_kinds();

    // What says something new: a minute like the one before writes nothing.
    // The ticks and the age of the last frame move every minute, so they
    // count only as alive or quiet.
    std::ostringstream steady;
    steady << cs.starts << '|' << ss.gaps << '|' << cs.pcap_dropped << '|' << cs.unwrap.unpack_failed << '|'
           << cs.server_resync_bytes << '|' << v.map_epoch << '|' << v.map << '/' << v.area << '|'
           << (v.me.known ? v.me.id : 0u) << '|' << link.bursts << '|' << link.selected << '/' << link.channel << '|'
           << (link.ticks_last_min > 0) << '|' << position_link_.pid << '|' << kinds;
    if (steady.str() != packets_said_) {
        packets_said_ = steady.str();
        std::ostringstream line;
        line << "Packets: capture since " << (capture_since_.empty() ? std::string("?") : capture_since_) << " on ";
        if (position_capture_ && position_link_.pid != 0)
            line << "pid " << position_link_.pid;
        else
            line << "a capture not started for positions";
        line << " (starts " << cs.starts << "; gaps " << ss.gaps << ": drops " << cs.pcap_dropped << ", unpack "
             << cs.unwrap.unpack_failed << ", server resync " << cs.server_resync_bytes << ")";
        line << ", map ";
        if (v.map_since_ms == 0)
            line << "not known";
        else
            line << v.map << "/" << v.area << " = " << place_text(v.map, v.area) << " since "
                 << seconds_ago(now, v.map_since_ms);
        line << ", your id ";
        if (v.me.known && v.me.id != 0) {
            char id[32];
            std::snprintf(id, sizeof id, "%u (0x%04X)", static_cast<unsigned>(v.me.id), static_cast<unsigned>(v.me.id));
            line << id;
        } else {
            line << "not known";
        }
        line << "; " << packet_link_text(now) << "; new kinds: " << (kinds.empty() ? std::string("none") : kinds);
        Logger::instance().write(LogLevel::Info, line.str());
    }

    // Each later part's own minute line, as it has something to say.
    note_vitals_minute(now);
    note_bag_minute(now);
    note_casts_minute(now);
}

void GuiApp::say_traffic_lost() {
    // The watchdog has said "No game traffic" already. What it means here:
    // the packet bot stopped, or -- the capture running all session -- the
    // game gone quiet under it.
    if (bot_running_)
        GB_WARN("Traffic stopped -- disabling bot");
    else
        GB_INFO("Positions: no game TCP for 15 s (the game closed, at the login, or disconnected)");
}

} // namespace gobot
