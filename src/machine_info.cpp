#include "gobot/machine_info.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"

#include <windows.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gobot {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// Printable ASCII only, for a line in the log: CPUID's vendor is twelve raw
/// bytes, and a hypervisor may fill them with anything.
std::string printable(const std::string& s) {
    std::string out;
    for (const char c : s)
        if (c >= 0x20 && c < 0x7f) out += c;
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : ", ") + p;
    return out;
}

} // namespace

std::string feature_level_name(unsigned level) {
    if (level == 0) return "unknown";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u_%u", (level >> 12) & 0xFu, (level >> 8) & 0xFu);
    return buf;
}

LightSignals judge_machine(const MachineSignals& m) {
    LightSignals s;
    const RendererInfo& r = m.renderer;
    s.software_renderer = r.known && (r.warp || r.vendor_id == kVendorMicrosoft);
    s.vmware_adapter = r.known && r.vendor_id == kVendorVmware;
    // "VMware, Inc." as VMware writes it. An administrator can hide it
    // (smbios.reflectHost), and players who hide the VM from the game's
    // anti-cheat do: the renderer and the player's own tick cover that.
    s.smbios_vmware = lower(m.smbios_maker).find("vmware") != std::string::npos ||
                      lower(m.smbios_product).rfind("vmware", 0) == 0;
    // Only behind the hypervisor bit, which a PC with Hyper-V or Memory
    // integrity on sets too -- its vendor is "Microsoft Hv", and that is no
    // reason for light mode.
    s.cpuid_vmware = m.hypervisor_bit && m.hypervisor_vendor == "VMwareVMware";
    return s;
}

std::string LightSignals::describe(const MachineSignals& m) const {
    std::vector<std::string> parts;
    if (smbios_vmware)
        parts.push_back("SMBIOS \"" + printable(m.smbios_maker) + "\" / \"" + printable(m.smbios_product) + "\"");
    if (cpuid_vmware) parts.push_back("CPUID VMwareVMware");
    if (vmware_adapter) parts.push_back("VMware SVGA adapter");
    if (software_renderer)
        parts.push_back(m.renderer.warp ? "WARP device (no usable graphics driver)"
                                        : "Microsoft Basic Render Driver (no real graphics card)");
    return parts.empty() ? std::string("none") : join(parts);
}

LightSetting parse_light_setting(const std::string& text) {
    const std::string s = lower(trim(text));
    if (s == "on" || s == "true" || s == "yes" || s == "1") return LightSetting::On;
    if (s == "off" || s == "false" || s == "no" || s == "0") return LightSetting::Off;
    return LightSetting::Auto;
}

const char* light_setting_name(LightSetting s) {
    switch (s) {
        case LightSetting::On:  return "on";
        case LightSetting::Off: return "off";
        case LightSetting::Auto: break;
    }
    return "auto";
}

LightDecision resolve_light_mode(LightSetting setting, const LightSignals& signals) {
    LightDecision d;
    if (setting == LightSetting::Off) {
        d.why = "off as set";
        return d;
    }
    if (setting == LightSetting::On) {
        d.on = true;
        d.why = "on as set";
        return d;
    }
    if (!signals.any()) {
        d.why = "no signal";
        return d;
    }
    d.on = true;
    d.by_auto = true;
    std::vector<std::string> why;
    if (signals.vmware()) why.push_back("VMware guest");
    if (signals.software_renderer) why.push_back("no real graphics card");
    d.why = join(why);
    if (signals.vmware() && signals.software_renderer)
        d.why_vi = "Tự bật vì GoBot thấy đây là máy ảo VMware, không có card đồ hoạ thật.";
    else if (signals.vmware())
        d.why_vi = "Tự bật vì GoBot thấy đây là máy ảo VMware.";
    else
        d.why_vi = "Tự bật vì máy không có card đồ hoạ thật.";
    return d;
}

PerformanceSettings performance_settings(const IniFile& app_ini) {
    PerformanceSettings p;
    p.gui_pacer = app_ini.get_bool("Performance", "gui_pacer", p.gui_pacer);
    p.vitals_idle_ms = std::clamp(app_ini.get_int("Performance", "vitals_idle_ms", p.vitals_idle_ms), 150, 5000);
    // 0 is automatic (vitals_run_period); anything else is held to 100..1000.
    const int run = app_ini.get_int("Performance", "vitals_run_ms", p.vitals_run_ms);
    p.vitals_run_ms = run == 0 ? 0 : std::clamp(run, 100, 1000);
    const int load = app_ini.get_int("Performance", "load_log_s", p.load_log_s);
    p.load_log_s = load <= 0 ? 0 : std::clamp(load, 10, 3600);
    p.light = parse_light_setting(app_ini.get("Simple", "light_mode", "auto"));
    return p;
}

int vitals_run_period(int vitals_run_ms, bool light) {
    if (vitals_run_ms == 0) return light ? kVitalsRunLightMs : kVitalsRunMs;
    return std::clamp(vitals_run_ms, 100, 1000);
}

std::string describe_renderer(const RendererInfo& r) {
    if (!r.known) return "GUI: the Direct3D device was not described";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "GUI: Direct3D 11 %s device, feature level %s, adapter \"%s\" vendor %04x device %04x, "
                  "%llu MB, client %dx%d, swap %s",
                  r.warp ? "WARP (software)" : "hardware", feature_level_name(r.feature_level).c_str(),
                  printable(r.adapter).c_str(), static_cast<unsigned>(r.vendor_id),
                  static_cast<unsigned>(r.device_id), static_cast<unsigned long long>(r.dedicated_mb), r.client_w,
                  r.client_h, r.swap_effect.empty() ? "?" : r.swap_effect.c_str());
    return buf;
}

std::string describe_machine(const MachineSignals& m, const LightSignals& s, LightSetting setting,
                             const LightDecision& light) {
    std::string out = "Machine: ";
    out += s.vmware() ? "VMware guest" : s.software_renderer ? "no real graphics card" : "no VMware signal";
    out += " (signals: " + s.describe(m) + ")";
    // The hypervisor bit with another vendor is said but not counted: Hyper-V,
    // VBS or Memory integrity on the PC itself, or another kind of VM.
    if (m.hypervisor_bit && !s.cpuid_vmware)
        out += ", hypervisor \"" + printable(m.hypervisor_vendor) + "\" (not counted)";
    char buf[256];
    std::snprintf(buf, sizeof buf, ", %d logical CPUs, %llu MB RAM, Windows %s, screen %dx%d", m.logical_cpus,
                  static_cast<unsigned long long>(m.ram_mb), m.windows.empty() ? "?" : m.windows.c_str(),
                  m.screen_w, m.screen_h);
    out += buf;
    if (m.timer_ms > 0.0) {
        std::snprintf(buf, sizeof buf, ", timer %.1f ms", m.timer_ms);
        out += buf;
    }
    out += std::string(", light mode: ") + light_setting_name(setting) + " -> " + (light.on ? "on" : "off") + " (" +
           light.why + ")";
    return out;
}

MachineSignals read_machine(const RendererInfo& renderer) {
    MachineSignals m;
    m.renderer = renderer;

    // SMBIOS as Windows keeps a copy of it, readable without VMware Tools
    // and without administrator.
    const auto bios = [](const wchar_t* name) -> std::string {
        wchar_t text[256] = {};
        DWORD size = sizeof(text);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr,
                         text, &size) != ERROR_SUCCESS)
            return {};
        return wide_to_utf8(text);
    };
    m.smbios_maker = bios(L"SystemManufacturer");
    m.smbios_product = bios(L"SystemProductName");

#if defined(_M_X64) || defined(_M_IX86)
    int regs[4] = {};
    __cpuid(regs, 1);
    m.hypervisor_bit = ((static_cast<unsigned>(regs[2]) >> 31) & 1u) != 0;
    if (m.hypervisor_bit) {
        __cpuid(regs, 0x40000000);
        char vendor[12];
        std::memcpy(vendor, &regs[1], 4);
        std::memcpy(vendor + 4, &regs[2], 4);
        std::memcpy(vendor + 8, &regs[3], 4);
        m.hypervisor_vendor.assign(vendor, strnlen(vendor, sizeof vendor));
    }
#endif

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    m.logical_cpus = static_cast<int>(si.dwNumberOfProcessors);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) m.ram_mb = mem.ullTotalPhys / (1024ull * 1024ull);

    // Asked of ntdll: GetVersionEx tells a program without a manifest that
    // says otherwise that it runs on Windows 8.
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        using QueryTimerFn = LONG(WINAPI*)(ULONG*, ULONG*, ULONG*);
        if (const auto version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof(v);
            if (version(&v) == 0)
                m.windows = std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                            std::to_string(v.dwBuildNumber);
        }
        // The clock tick every wait ends on, in 100 ns: 156250 unless
        // something on the machine -- the game, a browser -- raised it, and
        // then GoBot's 10 ms waits wake a hundred times a second.
        if (const auto timer = reinterpret_cast<QueryTimerFn>(GetProcAddress(ntdll, "NtQueryTimerResolution"))) {
            ULONG coarsest = 0, finest = 0, current = 0;
            if (timer(&coarsest, &finest, &current) == 0 && current > 0) m.timer_ms = current / 10000.0;
        }
    }

    m.screen_w = GetSystemMetrics(SM_CXSCREEN);
    m.screen_h = GetSystemMetrics(SM_CYSCREEN);
    return m;
}

} // namespace gobot
