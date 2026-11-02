// gobot_test -- how heavy GoBot is on the PC: when its window draws a frame
// (FramePacer), and light mode ("Nhẹ máy (máy ảo / máy yếu)") -- the signals
// of a VMware guest or a PC with no real graphics card, and how the setting
// resolves.
//
// The decisions only, on a clock and a machine of the test's own: no window
// is made, nothing is read from the registry or the CPU, nothing is drawn.

#include "gobot/config.hpp"
#include "gobot/frame_pacer.hpp"
#include "gobot/machine_info.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

constexpr std::uint64_t kT0 = 5'000'000;
/// Windows' clock tick, as the loop wakes on it: 15.6 ms, taken as 16.
constexpr std::uint64_t kTick = 16;

/// Frames `p` draws from `from` up to `to`, a pass every `step` ms, with
/// `base` as every pass's state. One decide() per statement: each counts.
int frames_over(FramePacer& p, FramePacer::Pass base, std::uint64_t from, std::uint64_t to,
                std::uint64_t step = kTick) {
    int n = 0;
    for (std::uint64_t t = from; t < to; t += step) {
        base.now_ms = t;
        const bool drew = p.decide(base);
        if (drew) ++n;
    }
    return n;
}

/// A pacer that has drawn its first frame at kT0 and been idle since: the
/// state it is in nearly always.
FramePacer settled(const FramePacer::Pass& base) {
    FramePacer p;
    FramePacer::Pass first = base;
    first.now_ms = kT0;
    (void)p.decide(first);
    return p;
}

void test_pacer_rates() {
    section("GoBot's window: when a frame is drawn (FramePacer)");

    FramePacer::Pass fg;
    fg.foreground = true;
    fg.rates = FramePacer::normal_rates();

    {
        FramePacer p;
        fg.now_ms = kT0;
        check(p.decide(fg), "the first pass draws at once, with nothing asked");
    }
    {
        // Two seconds of nothing, then a click.
        FramePacer p = settled(fg);
        const int idle = frames_over(p, fg, kT0 + kTick, kT0 + 2000);
        FramePacer::Pass click = fg;
        click.input = true;
        click.now_ms = kT0 + 2000;
        const bool at_once = p.decide(click);
        check(at_once && idle >= 8 && idle <= 12,
              "a click after two idle seconds draws in the same pass, not at the idle rate's next frame -- " +
                  std::to_string(idle) + " idle frames before it");
    }
    {
        // A click, then nothing: the interactive rate for a second, then the
        // idle one while GoBot is in front.
        FramePacer p = settled(fg);
        FramePacer::Pass click = fg;
        click.input = true;
        click.now_ms = kT0 + 5000;
        (void)p.decide(click);
        const int first_second = frames_over(p, fg, kT0 + 5000 + kTick, kT0 + 6000);
        const bool fast = p.interactive();
        const int next_two = frames_over(p, fg, kT0 + 6000 + kTick, kT0 + 8000);
        check(first_second >= 27 && first_second <= 33 && fast,
              "for a second after the last input: 30 frames a second, on Windows' 15.6 ms clock tick too -- " +
                  std::to_string(first_second));
        check(next_two >= 9 && next_two <= 11 && !p.interactive(),
              "then 5 a second while GoBot is in front and nobody touches it -- " + std::to_string(next_two) +
                  " in two seconds");
    }
    {
        FramePacer::Pass bg = fg;
        bg.foreground = false;
        FramePacer p = settled(bg);
        const int ten = frames_over(p, bg, kT0 + kTick, kT0 + 10'000);
        check(ten >= 19 && ten <= 21, "2 a second with another window in front -- " + std::to_string(ten) +
                                          " in ten seconds");
    }
    {
        // Light mode: 20, 3 and 1.
        FramePacer::Pass lfg = fg;
        lfg.rates = FramePacer::light_rates();
        FramePacer p = settled(lfg);
        FramePacer::Pass click = lfg;
        click.input = true;
        click.now_ms = kT0 + 100;
        (void)p.decide(click);
        const int used = frames_over(p, lfg, kT0 + 100 + kTick, kT0 + 1100);
        const int idle = frames_over(p, lfg, kT0 + 1100 + kTick, kT0 + 4100);
        FramePacer::Pass lbg = lfg;
        lbg.foreground = false;
        const int behind = frames_over(p, lbg, kT0 + 4100, kT0 + 14'100);
        const FrameRates light = FramePacer::light_rates();
        check(light.interactive == 20 && light.idle == 3 && light.background == 1 && used >= 17 && used <= 21 &&
                  idle >= 8 && idle <= 10 && behind >= 9 && behind <= 11,
              "light mode: 20 a second while used, 3 in front, 1 behind -- " + std::to_string(used) + ", " +
                  std::to_string(idle) + " in 3 s, " + std::to_string(behind) + " in 10 s");
        const FrameRates normal = FramePacer::normal_rates();
        check(normal.interactive == 30 && normal.idle == 5 && normal.background == 2,
              "and 30, 5 and 2 for everybody else");
    }
    {
        // Typing, a combo open, a button held, a key box waiting: as long as
        // that lasts, not for a second.
        FramePacer::Pass typing = fg;
        typing.want_interactive = true;
        FramePacer p = settled(typing);
        const int five = frames_over(p, typing, kT0 + kTick, kT0 + 5000);
        check(five >= 140 && five <= 165,
              "while the window wants it -- typing, a popup, a key box -- 30 a second with no input at all -- " +
                  std::to_string(five) + " in five seconds");
    }
    {
        // Input on every pass, as a mouse moving over the window: never more
        // than the interactive rate, whatever asks.
        FramePacer::Pass busy = fg;
        busy.input = true;
        busy.redraw = true;
        busy.paint = true;
        FramePacer p = settled(busy);
        const int ticked = frames_over(p, busy, kT0 + 1, kT0 + 1001, kTick);
        const int every_ms = frames_over(p, busy, kT0 + 1001, kT0 + 2001, 1);
        check(ticked <= 32 && every_ms <= 36,
              "input, paints and redraws on every pass draw at the interactive rate at most -- " +
                  std::to_string(ticked) + " a second on the clock tick, " + std::to_string(every_ms) +
                  " on a 1 ms clock");
    }
}

void test_pacer_events() {
    section("GoBot's window: a frame asked for, minimised, and the wait (FramePacer)");

    FramePacer::Pass bg;
    bg.rates = FramePacer::normal_rates();
    {
        // Behind the game, a frame 100 ms ago, a status line changes: one
        // frame for it, then the background rate again.
        FramePacer p = settled(bg);
        FramePacer::Pass ask = bg;
        ask.redraw = true;
        ask.now_ms = kT0 + 100;
        const bool drawn = p.decide(ask);
        const int after = frames_over(p, bg, kT0 + 100 + kTick, kT0 + 500);
        check(drawn && after == 0, "a redraw asked for draws one frame at once, and only one -- " +
                                       std::to_string(after) + " more before the background rate's next");
    }
    {
        FramePacer p = settled(bg);
        FramePacer::Pass paint = bg;
        paint.paint = true;
        paint.now_ms = kT0 + 200;
        const bool drawn = p.decide(paint);
        const int after = frames_over(p, bg, kT0 + 200 + kTick, kT0 + 600);
        check(drawn && after == 0 && !p.interactive(),
              "so does WM_PAINT, without the interactive rate after it");
    }
    {
        // Minimised: nothing, whatever comes; the click that came is drawn
        // when the window is back.
        FramePacer::Pass mini = bg;
        mini.minimised = true;
        FramePacer p = settled(bg);
        FramePacer::Pass click = mini;
        click.input = true;
        click.now_ms = kT0 + 100;
        const bool clicked = p.decide(click);
        mini.redraw = true;
        const int hidden = frames_over(p, mini, kT0 + 200, kT0 + 5000);
        FramePacer::Pass covered = bg;
        covered.occluded = true;
        const int covered_n = frames_over(p, covered, kT0 + 5000, kT0 + 8000);
        FramePacer::Pass back = bg;
        back.now_ms = kT0 + 8000;
        const bool shown = p.decide(back);
        check(!clicked && hidden == 0 && covered_n == 0 && shown,
              "minimised or covered: no frame at all, a click and redraws included -- and the first pass back "
              "draws");
    }
    {
        // The wait: never more than 10 ms from when the loop woke, at any
        // rate, so tick() -- F12, the potions -- comes round on every clock
        // tick.
        bool held = true;
        std::uint32_t longest = 0;
        for (int light = 0; light < 2; ++light) {
            for (int fore = 0; fore < 2; ++fore) {
                for (int hide = 0; hide < 2; ++hide) {
                    FramePacer::Pass s = bg;
                    s.rates = light ? FramePacer::light_rates() : FramePacer::normal_rates();
                    s.foreground = fore != 0;
                    s.minimised = hide != 0;
                    FramePacer p = settled(s);
                    for (std::uint64_t t = kT0 + 1; t < kT0 + 3000; t += 7) {
                        s.now_ms = t;
                        (void)p.decide(s);
                        for (std::uint64_t spent = 0; spent <= 12; spent += 3) {
                            const std::uint32_t w = p.wait_ms(t + spent);
                            if (w > longest) longest = w;
                            held = held && w <= FramePacer::kMaxWaitMs &&
                                   w <= FramePacer::tick_wait_ms(t, t + spent);
                        }
                    }
                }
            }
        }
        check(held && longest == FramePacer::kMaxWaitMs && FramePacer::kMaxWaitMs <= 10,
              "the loop never waits more than 10 ms from its wake, at 1 frame a second as at 30, minimised or not "
              "-- the longest " + std::to_string(longest) + " ms");
        check(FramePacer::tick_wait_ms(kT0, kT0) == 10 && FramePacer::tick_wait_ms(kT0, kT0 + 4) == 6 &&
                  FramePacer::tick_wait_ms(kT0, kT0 + 10) == 0 && FramePacer::tick_wait_ms(kT0, kT0 + 25) == 0 &&
                  FramePacer::tick_wait_ms(kT0 + 5, kT0) == 10,
              "and what the pass took counts against it: 4 ms of work leaves 6 to wait, 25 leaves none");
    }
    {
        // Right after a frame, behind the game: the wait is the full 10 ms, and
        // with the next frame 3 ms off, 3.
        FramePacer p = settled(bg);
        const std::uint32_t right_after = p.wait_ms(kT0 + 1);
        FramePacer::Pass s = bg;
        s.now_ms = kT0 + 489;
        (void)p.decide(s);
        const std::uint32_t near_due = p.wait_ms(kT0 + 489);
        check(right_after == 9 && near_due == 3,
              "the wait ends when the next frame is due if that is sooner -- " + std::to_string(right_after) +
                  " ms, then " + std::to_string(near_due));
    }
    {
        bool used = true;
        for (const unsigned m : { unsigned(WM_MOUSEMOVE), unsigned(WM_LBUTTONDOWN), unsigned(WM_LBUTTONUP),
                                  unsigned(WM_MOUSEWHEEL), unsigned(WM_KEYDOWN), unsigned(WM_KEYUP),
                                  unsigned(WM_CHAR), unsigned(WM_SYSKEYDOWN), unsigned(WM_IME_STARTCOMPOSITION),
                                  unsigned(WM_IME_COMPOSITION), unsigned(WM_IME_ENDCOMPOSITION),
                                  unsigned(WM_INPUTLANGCHANGE), unsigned(WM_MOUSELEAVE), unsigned(WM_NCMOUSEMOVE),
                                  unsigned(WM_SETCURSOR), unsigned(WM_CAPTURECHANGED), unsigned(WM_ACTIVATE),
                                  unsigned(WM_SETFOCUS), unsigned(WM_KILLFOCUS), unsigned(WM_SIZE),
                                  unsigned(WM_DPICHANGED) })
            used = used && is_interactive_message(m);
        bool not_used = true;
        for (const unsigned m : { unsigned(WM_TIMER), unsigned(WM_PAINT), unsigned(WM_GETTEXT),
                                  unsigned(WM_GETTEXTLENGTH), unsigned(WM_NCHITTEST), unsigned(WM_GETOBJECT),
                                  unsigned(WM_NULL) })
            not_used = not_used && !is_interactive_message(m);
        check(used && not_used,
              "input is mouse, keys, WM_CHAR, the IME's messages, mouse-leave, the non-client mouse, the cursor, "
              "capture, activation, focus, size and DPI -- not a timer, a paint, or a query nobody's hand sent");
    }
}

MachineSignals plain_pc() {
    MachineSignals m;
    m.renderer.known = true;
    m.renderer.vendor_id = 0x10DE;      // NVIDIA
    m.renderer.adapter = "NVIDIA GeForce GTX 1060";
    m.smbios_maker = "Dell Inc.";
    m.smbios_product = "OptiPlex 7070";
    return m;
}

void test_light_signals() {
    section("Light mode: a VMware guest or no real graphics card (machine_info)");

    check(!judge_machine(plain_pc()).any(), "a PC with an NVIDIA card and Dell's SMBIOS: no signal");
    {
        MachineSignals hv = plain_pc();
        hv.hypervisor_bit = true;
        hv.hypervisor_vendor = "Microsoft Hv";
        check(!judge_machine(hv).any(),
              "the hypervisor bit with \"Microsoft Hv\" -- Hyper-V, VBS or Memory integrity on the PC itself -- is "
              "no signal");
        MachineSignals hidden = plain_pc();
        hidden.hypervisor_vendor = "VMwareVMware";
        check(!judge_machine(hidden).any(), "nor \"VMwareVMware\" without the bit that vouches for it");
        MachineSignals vm = plain_pc();
        vm.hypervisor_bit = true;
        vm.hypervisor_vendor = "VMwareVMware";
        const LightSignals s = judge_machine(vm);
        check(s.cpuid_vmware && s.vmware() && s.any() && !s.software_renderer,
              "the bit and \"VMwareVMware\": a VMware guest");
    }
    {
        MachineSignals maker = plain_pc();
        maker.smbios_maker = "VMware, Inc.";
        maker.smbios_product = "VMware20,1";
        MachineSignals product = plain_pc();
        product.smbios_maker = "";
        product.smbios_product = "VMware Virtual Platform";
        MachineSignals elsewhere = plain_pc();
        elsewhere.smbios_product = "Not VMware";
        check(judge_machine(maker).smbios_vmware && judge_machine(product).smbios_vmware &&
                  !judge_machine(elsewhere).smbios_vmware,
              "SMBIOS: \"VMware, Inc.\" as the maker, or a product that starts with VMware -- not one that only "
              "has it inside");
    }
    {
        MachineSignals warp = plain_pc();
        warp.renderer.warp = true;
        MachineSignals basic = plain_pc();
        basic.renderer.vendor_id = kVendorMicrosoft;
        basic.renderer.adapter = "Microsoft Basic Render Driver";
        MachineSignals svga = plain_pc();
        svga.renderer.vendor_id = kVendorVmware;
        svga.renderer.adapter = "VMware SVGA 3D";
        MachineSignals unknown = plain_pc();
        unknown.renderer.known = false;
        unknown.renderer.vendor_id = kVendorMicrosoft;
        const LightSignals sv = judge_machine(svga);
        check(judge_machine(warp).software_renderer && judge_machine(basic).software_renderer &&
                  !judge_machine(basic).vmware() && sv.vmware_adapter && sv.vmware() && !sv.software_renderer &&
                  !judge_machine(unknown).any(),
              "the window's device: WARP or Microsoft's adapter (0x1414) is no real graphics card, VMware's "
              "(0x15AD) a VMware guest; a device not described says nothing");
    }
    {
        LightSignals none;
        LightSignals vm;
        vm.smbios_vmware = true;
        LightSignals soft;
        soft.software_renderer = true;
        LightSignals both = vm;
        both.software_renderer = true;
        const LightDecision off = resolve_light_mode(LightSetting::Off, both);
        const LightDecision on = resolve_light_mode(LightSetting::On, none);
        const LightDecision auto_vm = resolve_light_mode(LightSetting::Auto, vm);
        const LightDecision auto_soft = resolve_light_mode(LightSetting::Auto, soft);
        const LightDecision auto_both = resolve_light_mode(LightSetting::Auto, both);
        const LightDecision auto_none = resolve_light_mode(LightSetting::Auto, none);
        check(!off.on && !off.by_auto && off.why_vi.empty(), "off beats every signal");
        check(on.on && !on.by_auto && on.why_vi.empty(), "on is on with no signal, and no line saying why");
        check(auto_vm.on && auto_vm.by_auto && auto_vm.why_vi.find("máy ảo VMware") != std::string::npos &&
                  auto_soft.on && auto_soft.why_vi.find("card đồ hoạ") != std::string::npos &&
                  auto_soft.why_vi.find("VMware") == std::string::npos && auto_both.why_vi.find("VMware") !=
                  std::string::npos && auto_both.why_vi.find("card đồ hoạ") != std::string::npos,
              "auto with a signal is on, and the screen's line says which: a VMware guest, no real graphics card, "
              "or both");
        check(!auto_none.on && !auto_none.by_auto, "auto with none is off");
    }
    {
        check(parse_light_setting("auto") == LightSetting::Auto && parse_light_setting(" ON ") == LightSetting::On &&
                  parse_light_setting("Off") == LightSetting::Off && parse_light_setting("true") == LightSetting::On &&
                  parse_light_setting("0") == LightSetting::Off && parse_light_setting("") == LightSetting::Auto &&
                  parse_light_setting("maybe") == LightSetting::Auto,
              "light_mode: auto, on or off in any case, true/false and 1/0 too; anything else is auto");
        check(std::string(light_setting_name(LightSetting::Auto)) == "auto" &&
                  std::string(light_setting_name(LightSetting::On)) == "on" &&
                  std::string(light_setting_name(LightSetting::Off)) == "off",
              "and written back as auto, on and off");
    }
    {
        // The two start lines.
        MachineSignals vm = plain_pc();
        vm.renderer.vendor_id = kVendorVmware;
        vm.renderer.device_id = 0x0405;
        vm.renderer.adapter = "VMware SVGA 3D";
        vm.renderer.feature_level = 0xb000;
        vm.renderer.dedicated_mb = 128;
        vm.renderer.client_w = 959;
        vm.renderer.client_h = 1125;
        vm.renderer.swap_effect = "DISCARD";
        vm.smbios_maker = "VMware, Inc.";
        vm.logical_cpus = 2;
        vm.ram_mb = 4096;
        vm.windows = "10.0.19045";
        vm.screen_w = 1920;
        vm.screen_h = 1080;
        vm.timer_ms = 15.625;
        const LightSignals s = judge_machine(vm);
        const LightDecision d = resolve_light_mode(LightSetting::Auto, s);
        const std::string gui = describe_renderer(vm.renderer);
        const std::string machine = describe_machine(vm, s, LightSetting::Auto, d);
        check(gui.rfind("GUI: Direct3D 11 hardware device, feature level 11_0, adapter \"VMware SVGA 3D\" vendor 15ad "
                        "device 0405, 128 MB, client 959x1125, swap DISCARD", 0) == 0,
              "GUI: the device, its feature level, adapter, ids, memory, size and swap effect -- " + gui);
        check(machine.rfind("Machine: VMware guest (signals: SMBIOS \"VMware, Inc.\"", 0) == 0 &&
                  machine.find("VMware SVGA adapter") != std::string::npos &&
                  machine.find("2 logical CPUs, 4096 MB RAM, Windows 10.0.19045, screen 1920x1080") !=
                      std::string::npos &&
                  machine.find("light mode: auto -> on (VMware guest)") != std::string::npos,
              "Machine: the signals that fired, the CPUs, memory, Windows, screen and light mode -- " + machine);
        MachineSignals hv = plain_pc();
        hv.hypervisor_bit = true;
        hv.hypervisor_vendor = "Microsoft Hv";
        const LightSignals hs = judge_machine(hv);
        const std::string plain = describe_machine(hv, hs, LightSetting::Auto, resolve_light_mode(LightSetting::Auto, hs));
        check(plain.find("no VMware signal (signals: none)") != std::string::npos &&
                  plain.find("hypervisor \"Microsoft Hv\" (not counted)") != std::string::npos &&
                  plain.find("light mode: auto -> off (no signal)") != std::string::npos,
              "and a PC with Hyper-V on says its hypervisor, not counted -- " + plain);
        check(feature_level_name(0xb000) == "11_0" && feature_level_name(0xa100) == "10_1" &&
                  feature_level_name(0) == "unknown" &&
                  describe_renderer(RendererInfo{}).find("not described") != std::string::npos,
              "feature levels by name, and a device not described said so");
    }
}

void test_performance_settings() {
    section("[Performance] and light_mode in app.ini");

    const PerformanceSettings none = performance_settings(IniFile{});
    check(none.gui_pacer && none.vitals_idle_ms == 1000 && none.load_log_s == 60 && none.light == LightSetting::Auto,
          "an app.ini without them: the pacer on, HP read every second when idle, a Load line a minute, light mode "
          "auto");
    {
        IniFile ini;
        ini.set("Performance", "gui_pacer", "off");
        ini.set("Performance", "vitals_idle_ms", "150");
        ini.set("Performance", "load_log_s", "0");
        ini.set("Simple", "light_mode", "on");
        const PerformanceSettings p = performance_settings(ini);
        check(!p.gui_pacer && p.vitals_idle_ms == 150 && p.load_log_s == 0 && p.light == LightSetting::On,
              "each read as written: gui_pacer=off, vitals_idle_ms=150 (as before), load_log_s=0 (none), "
              "light_mode=on");
    }
    {
        IniFile low;
        low.set("Performance", "vitals_idle_ms", "20");
        low.set("Performance", "load_log_s", "3");
        IniFile high;
        high.set("Performance", "vitals_idle_ms", "60000");
        high.set("Performance", "load_log_s", "999999");
        IniFile negative;
        negative.set("Performance", "load_log_s", "-5");
        const PerformanceSettings l = performance_settings(low);
        const PerformanceSettings h = performance_settings(high);
        const PerformanceSettings n = performance_settings(negative);
        check(l.vitals_idle_ms == 150 && h.vitals_idle_ms == 5000 && l.load_log_s == 10 && h.load_log_s == 3600 &&
                  n.load_log_s == 0,
              "held to what they may be: vitals_idle_ms 150..5000, load_log_s 0 or 10..3600");
    }
#ifdef GOBOT_SOURCE_DIR
    IniFile shipped;
    const bool read = shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini");
    const PerformanceSettings s = performance_settings(shipped);
    check(read && shipped.has_key("Performance", "gui_pacer") && shipped.has_key("Performance", "vitals_idle_ms") &&
              shipped.has_key("Performance", "load_log_s") && shipped.has_key("Simple", "light_mode") &&
              s.gui_pacer && s.vitals_idle_ms == 1000 && s.load_log_s == 60 && s.light == LightSetting::Auto &&
              shipped.get("Simple", "light_mode") == "auto",
          "the shipped app.ini: gui_pacer=on, vitals_idle_ms=1000, load_log_s=60 and light_mode=auto");
#endif
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

void test_pacer_wiring() {
    section("GoBot's window: the pacer and light mode wired in (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string host = source("/apps/gobot_gui/main.cpp");
    if (host.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto npos = std::string::npos;
    const std::size_t main_at = host.find("int WINAPI wWinMain(");
    const std::string win_main = main_at == npos ? std::string() : host.substr(main_at);
    const std::size_t renderer = win_main.find("app.set_renderer(g_renderer);");
    const std::size_t start = win_main.find("app.initialise();");
    const std::size_t scope = start == npos ? npos
                                            : win_main.rfind("gobot::DpiScope pixels(gobot::DpiScope::Mode::RealPixels);",
                                                             start);
    check(renderer != npos && scope != npos && renderer < scope,
          "main.cpp: the device is handed over before GoBot starts, outside the real-pixels block around it");
    const std::size_t tick = win_main.find("app.tick();");
    const std::size_t decide = win_main.find("pacer.decide(pass)");
    const std::size_t draw = win_main.find("app.draw();");
    check(tick != npos && decide != npos && draw != npos && tick < decide && decide < draw &&
              win_main.find("MsgWaitForMultipleObjectsEx(") == npos &&
              host.find("MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);") != npos &&
              win_main.find("wait_for_input(pacer.wait_ms(gobot::now_ms()));") != npos,
          "every pass runs tick() before the pacer is asked, and waits for input or the pacer's time -- never "
          "longer than it allows");
    check(win_main.find("g_swapchain->Present(pacing.pacer_on ? 0 : 1, 0);") != npos &&
              win_main.find("Sleep(10);") != npos,
          "with the pacer off, the loop as before: vsync, and 10 ms asleep while covered");
    const std::size_t failed = host.find("if (FAILED(hr)) {");
    const std::size_t warp = failed == npos ? npos : host.find("D3D_DRIVER_TYPE_WARP", failed);
    check(failed != npos && warp != npos && warp - failed < 300 && host.find("hr == DXGI_ERROR_UNSUPPORTED") == npos,
          "WARP is tried on any failure of the hardware device, not only DXGI_ERROR_UNSUPPORTED");
    check(host.find("if (gobot::is_interactive_message(msg)) g_input = true;") != npos,
          "what reaches the window is noted in wnd_proc, sent messages included");

    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::size_t matching = gui.find("GB_INFO(\"Template matching: %s\"");
    const std::size_t perf = gui.find("load_performance(app_ini);");
    check(matching != npos && perf != npos && matching < perf && perf - matching < 600,
          "gui_app.cpp: the GUI and Machine lines straight after the Template matching line");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::size_t refocus = view.find("ImGui::Checkbox(\"Luôn đưa game lên trước khi bot đang chạy\"");
    const std::size_t row = refocus == npos ? npos : view.find("simple_light_row();", refocus);
    check(refocus != npos && row != npos && row - refocus < 1500 &&
              view.find("ImGui::Checkbox(\"Nhẹ máy (máy ảo / máy yếu)\", &light)") != npos &&
              view.find("save_simple_option(\"light_mode\", light_setting_name(performance_.light));") != npos,
          "simple_view.cpp: \"Nhẹ máy (máy ảo / máy yếu)\" right under \"Luôn đưa game lên trước\", written to "
          "[Simple] light_mode as on or off");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_pacer(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_pacer_rates();
    test_pacer_events();
    test_light_signals();
    test_performance_settings();
    test_pacer_wiring();
}
