// The name-plate reader resting while the packets steer (the packets plan,
// item O).
//
// With the game's packets steering the hunter, the detector's plates are not
// what it fights by: they are only laid over the packets' for the Positions
// line. Yet the detector went on looking every 140 ms -- 5.7 screen grabs a
// second, and 9 CPU s a minute against the HP/MP reader's 2.8, in the hunt of
// 2026-11-02. So while the packets steer and are healthy, and nothing else
// reads the plates or the frames they come with, it looks every 600 ms.
// Anything else -- the screen steering again, a "Thử", "Quái đang thấy", the
// Radar or the movement panels, a walk, detector_rest=off -- has it back at
// its own 140 ms within the tick, the wait it is in cut short.
//
// Decided at every tick (detector_rest), so neither the hunt's start and end
// nor the detector's own need say anything of it. And at a switch back to the
// screen, PacketEyes keeps the packets' last plates for the hunter until the
// woken detector has a picture taken since (PacketEyes::kHandOverMs): the
// hunter never fights by a frame as old as the rest.
//
// Nothing here waits: set_period_ms only stores the wait and, for a shorter
// one, wakes the detector's thread.

#include "gui_app.hpp"
#include "gobot/logger.hpp"

namespace gobot {

void GuiApp::detector_pace_tick(std::uint64_t) {
    DetectorPace p;
    p.hunting = hunting_;
    if (packet_eyes_) {
        p.source = packet_eyes_->source();
        p.why = packet_eyes_->why();
    }
    p.jump_test = jump_test_running();
    p.monster_look = monster_look_running();
    p.camera_panels = camera_panels_open();
    p.walking = navigator_ && navigator_->running();
    p.rest_off = packets_settings_.detector_rest == PacketUse::Off;

    // Its "Cannot find" a plain line while the packets steer the hunt, which
    // then has no need of your plate; a warning otherwise.
    detector_.set_packets_steer(p.hunting && p.source == PacketEyes::Source::Packets && p.why == PacketWhy::InUse);

    // A detector that is not running is set back to its own wait, so that it
    // starts at it next time, whatever starts it.
    const char* why = "the detector stopped";
    const bool rest = detector_running_ && detector_rest(p, &why);
    const int period = rest ? DetectorPace::kRestMs : DetectorPace::kRunMs;
    if (period == detector_period_ms_) return;
    const int was = detector_period_ms_;
    detector_period_ms_ = period;
    detector_.set_period_ms(period);
    // Each switch said once; not the first setting, which is the wait it
    // always had.
    if (rest)
        GB_INFO("Detector: every %d ms while the packets steer", period);
    else if (was > DetectorPace::kRunMs)
        GB_INFO("Detector: every %d ms -- %s", period, why);
}

} // namespace gobot
