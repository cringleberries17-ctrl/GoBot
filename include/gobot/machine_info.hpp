#pragma once

#include <cstdint>
#include <string>

namespace gobot {

class IniFile;

/// The Direct3D device GoBot's window is drawn with, as gobot_gui's main.cpp
/// made it: handed to GuiApp before its start, which logs it ("GUI:").
struct RendererInfo {
    bool          known = false;        ///< a device was made, and this describes it
    bool          warp = false;         ///< WARP, Direct3D drawn by the CPU, not the driver's device
    unsigned      feature_level = 0;    ///< a D3D_FEATURE_LEVEL: 0xb000 is 11_0
    std::string   adapter;              ///< the adapter's description, UTF-8
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_mb = 0;     ///< its own video memory
    int           client_w = 0;         ///< the window's client area when the device was made
    int           client_h = 0;
    std::string   swap_effect;          ///< "DISCARD"
};

/// What this PC says about itself, read once as GoBot's window starts
/// (read_machine), and the window's renderer.
struct MachineSignals {
    RendererInfo  renderer;
    std::string   smbios_maker;         ///< SystemManufacturer, from the registry's copy of SMBIOS
    std::string   smbios_product;       ///< SystemProductName
    bool          hypervisor_bit = false;   ///< CPUID leaf 1, ECX bit 31
    std::string   hypervisor_vendor;    ///< CPUID leaf 0x40000000's 12 letters; empty without the bit
    int           logical_cpus = 0;
    std::uint64_t ram_mb = 0;
    std::string   windows;              ///< "10.0.19045"
    int           screen_w = 0;
    int           screen_h = 0;
    double        timer_ms = 0.0;       ///< Windows' clock tick now, 15.6 unless something raised it; 0 unknown
};

/// Microsoft's PCI vendor id: the Basic Render Driver -- what Windows draws
/// with when there is no display driver, a VM with no 3D among them -- and WARP.
constexpr std::uint32_t kVendorMicrosoft = 0x1414;
/// VMware's: the SVGA adapter of VMware Tools' display driver.
constexpr std::uint32_t kVendorVmware = 0x15AD;

/// Which of the signals of a VMware guest, or of a PC with no real graphics
/// card, are there (judge_machine). Any one turns light mode on when it is
/// left to "auto".
struct LightSignals {
    bool software_renderer = false;     ///< WARP, or the adapter is Microsoft's (0x1414)
    bool smbios_vmware = false;         ///< SystemManufacturer has "VMware", or the product starts with it
    bool cpuid_vmware = false;          ///< the hypervisor bit, and its vendor is "VMwareVMware"
    bool vmware_adapter = false;        ///< the adapter is VMware's (0x15AD)

    bool vmware() const { return smbios_vmware || cpuid_vmware || vmware_adapter; }
    bool any() const { return software_renderer || vmware(); }
    /// For the log: the signals that fired, or "none".
    std::string describe(const MachineSignals& m) const;
};

/// The signals in what was read. The hypervisor bit alone is not one: a PC
/// with Hyper-V, VBS or Memory integrity on sets it too, with the vendor
/// "Microsoft Hv", and that PC is no VM.
LightSignals judge_machine(const MachineSignals& m);

/// light_mode in [Simple]: left to GoBot, or set by the player's tick.
enum class LightSetting { Auto, On, Off };

/// "auto", "on" or "off", any case; true/false, yes/no and 1/0 for on and off.
/// Anything else is auto.
LightSetting parse_light_setting(const std::string& text);
const char* light_setting_name(LightSetting s);    ///< "auto", "on", "off"

/// Light mode, worked out.
struct LightDecision {
    bool        on = false;
    bool        by_auto = false;        ///< on because "auto" saw a signal
    std::string why;                    ///< English, for the log
    std::string why_vi;                 ///< the simple screen's line when auto turned it on; else empty
};

/// Off and on are the player's; auto is on when any signal fired.
LightDecision resolve_light_mode(LightSetting setting, const LightSignals& signals);

/// [Performance] and [Simple] light_mode in configs/app.ini, held to what
/// they may be.
struct PerformanceSettings {
    bool         gui_pacer = true;      ///< draw on demand (FramePacer); off is the loop before it
    int          vitals_idle_ms = 1000; ///< HP/MP read this often while nothing runs, 150..5000
    int          vitals_run_ms = 0;     ///< and while a job runs: 0 automatic (vitals_run_period), else 100..1000
    int          load_log_s = 60;       ///< a "Load:" line this often, 0 for none, else 10..3600
    LightSetting light = LightSetting::Auto;
};
PerformanceSettings performance_settings(const IniFile& app_ini);

/// HP and MP read this often while a job runs -- anything that acts on them
/// or presses a key -- as [Performance] vitals_run_ms leaves it: 0, as it
/// ships, is automatic, kVitalsRunMs, or kVitalsRunLightMs in light mode; any
/// other value is that, held to 100..1000. Light mode's 250 is the owner's
/// (2026-11-02), taken "if it does not hurt much": whatever counted the
/// reader's reads rather than time was made to count time (VitalsReader::
/// forget_after_misses, potion_pair_gap_ms), so that 250 behaves as 150 did.
constexpr int kVitalsRunMs = 150;
constexpr int kVitalsRunLightMs = 250;
int vitals_run_period(int vitals_run_ms, bool light);

/// The start's two lines: "GUI: ..." and "Machine: ...".
std::string describe_renderer(const RendererInfo& r);
std::string describe_machine(const MachineSignals& m, const LightSignals& s, LightSetting setting,
                             const LightDecision& light);
/// "11_0" for 0xb000.
std::string feature_level_name(unsigned level);

/// The real readers: the registry's SMBIOS, CPUID, the CPUs, the memory,
/// Windows' version, the screen and the clock tick. For gobot_gui alone;
/// the tests drive the judging above with values of their own.
MachineSignals read_machine(const RendererInfo& renderer);

} // namespace gobot
