#include "gobot/clr_bridge.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

// The handful of things hostfxr exports that are needed here, declared rather
// than included. The headers for these ship with the .NET SDK, which is not
// installed on this machine -- but the exports are a stable, documented part of
// the hosting contract, and three declarations is a smaller dependency than an
// SDK.
using char_t = wchar_t;
using hostfxr_handle = void*;

using hostfxr_initialize_fn = int(__cdecl*)(const char_t* runtime_config_path,
                                            const void* parameters,
                                            hostfxr_handle* host_context_handle);
using hostfxr_get_delegate_fn = int(__cdecl*)(hostfxr_handle host_context_handle,
                                              int type, void** delegate);
using hostfxr_close_fn = int(__cdecl*)(hostfxr_handle host_context_handle);

/// hdt_load_assembly_and_get_function_pointer, from the hosting enum.
constexpr int kLoadAssemblyAndGetFunctionPointer = 5;

using load_assembly_fn = int(__cdecl*)(const char_t* assembly_path,
                                       const char_t* type_name,
                                       const char_t* method_name,
                                       const char_t* delegate_type_name,
                                       void* reserved,
                                       void** delegate);

/// What every bridge method looks like from here.
using component_entry_fn = int(__cdecl*)(void* arg, std::int32_t arg_size);

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool exists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// The newest thing in a directory of version-named folders.
std::string newest_version_dir(const std::string& parent) {
    WIN32_FIND_DATAA find{};
    HANDLE h = FindFirstFileA((parent + "\\*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return {};

    std::string best;
    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string name = find.cFileName;
        if (name == "." || name == "..") continue;
        if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;

        // Compared by their numbers, so 10.0.12 sorts above 6.0.20 rather than
        // below it the way a string comparison would have it.
        auto parts = [](const std::string& v) {
            std::vector<int> out;
            std::size_t at = 0;
            while (at < v.size()) {
                std::size_t end = v.find('.', at);
                if (end == std::string::npos) end = v.size();
                out.push_back(std::atoi(v.substr(at, end - at).c_str()));
                at = end + 1;
            }
            return out;
        };
        if (best.empty() || parts(name) > parts(best)) best = name;
    } while (FindNextFileA(h, &find));

    FindClose(h);
    return best.empty() ? std::string{} : parent + "\\" + best;
}

} // namespace

struct ClrBridge::Impl {
    HMODULE        hostfxr = nullptr;
    hostfxr_handle context = nullptr;
    hostfxr_close_fn close = nullptr;
};

ClrBridge::ClrBridge() : impl_(new Impl) {}

ClrBridge::~ClrBridge() {
    if (impl_) {
        // The runtime itself is left running: .NET has no supported way to
        // unload it, and the process is about to end anyway.
        if (impl_->context && impl_->close) impl_->close(impl_->context);
        delete impl_;
    }
}

std::string ClrBridge::runtime_dir() {
    const char* candidates[] = {
        "C:\\Program Files\\dotnet",
        "C:\\Program Files (x86)\\dotnet",
    };
    for (const char* c : candidates)
        if (exists(std::string(c) + "\\host\\fxr")) return c;
    return {};
}

std::string ClrBridge::hostfxr_path() {
    const std::string root = runtime_dir();
    if (root.empty()) return {};
    const std::string dir = newest_version_dir(root + "\\host\\fxr");
    if (dir.empty()) return {};
    const std::string dll = dir + "\\hostfxr.dll";
    return exists(dll) ? dll : std::string{};
}

std::string ClrBridge::bridge_assembly() {
    return exe_relative("GoBotBridge.dll");
}

bool ClrBridge::start(std::string* trouble) {
    const auto fail = [&](const std::string& why) {
        if (trouble) *trouble = why;
        GB_WARN("ClrBridge: %s", why.c_str());
        return false;
    };

    if (entry_) return true;

    const std::string assembly = bridge_assembly();
    if (!exists(assembly))
        return fail("GoBotBridge.dll is not beside the executable. It is built "
                    "from bridge/GoBotBridge.cs -- see bridge/build.ps1.");

    const std::string config = assembly.substr(0, assembly.size() - 4) +
                               ".runtimeconfig.json";
    if (!exists(config))
        return fail("GoBotBridge.runtimeconfig.json is missing, so there is "
                    "nothing to tell .NET which runtime to start.");

    const std::string host = hostfxr_path();
    if (host.empty())
        return fail("No .NET runtime is installed; nothing of the other "
                    "helper's code can be run without one.");

    impl_->hostfxr = LoadLibraryA(host.c_str());
    if (!impl_->hostfxr) return fail("could not load " + host);

    auto initialise = reinterpret_cast<hostfxr_initialize_fn>(
        GetProcAddress(impl_->hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate = reinterpret_cast<hostfxr_get_delegate_fn>(
        GetProcAddress(impl_->hostfxr, "hostfxr_get_runtime_delegate"));
    impl_->close = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(impl_->hostfxr, "hostfxr_close"));
    if (!initialise || !get_delegate || !impl_->close)
        return fail("that hostfxr does not export the hosting functions");

    const std::wstring wide_config = widen(config);
    int rc = initialise(wide_config.c_str(), nullptr, &impl_->context);
    // Success is 0; 1 means a runtime was already up and is being reused, which
    // is also fine and is what happens on a second call.
    if ((rc != 0 && rc != 1) || impl_->context == nullptr) {
        char why[160];
        std::snprintf(why, sizeof why,
                      "starting the .NET runtime failed (0x%08x). The helper "
                      "wants .NET 6; check it is installed.", rc);
        return fail(why);
    }

    void* loader = nullptr;
    rc = get_delegate(impl_->context, kLoadAssemblyAndGetFunctionPointer, &loader);
    if (rc != 0 || !loader) {
        char why[128];
        std::snprintf(why, sizeof why, "could not get the assembly loader (0x%08x)", rc);
        return fail(why);
    }

    entry_ = loader;
    GB_INFO("ClrBridge: .NET runtime up, from %s", host.c_str());
    return true;
}

bool ClrBridge::call(const std::string& method, const std::string& input,
                     std::string& answer, std::string* trouble) {
    answer.clear();
    if (!entry_ && !start(trouble)) return false;

    const std::wstring assembly = widen(bridge_assembly());
    const std::wstring type = L"GoBot.Bridge, GoBotBridge";
    const std::wstring name = widen(method);

    void* fn = nullptr;
    const int rc = reinterpret_cast<load_assembly_fn>(entry_)(
        assembly.c_str(), type.c_str(), name.c_str(),
        nullptr,            // the default component entry point shape
        nullptr, &fn);
    if (rc != 0 || !fn) {
        char why[220];
        std::snprintf(why, sizeof why,
                      "could not resolve GoBot.Bridge.%s (0x%08x). The method "
                      "must be `public static int %s(IntPtr, int)`.",
                      method.c_str(), rc, method.c_str());
        if (trouble) *trouble = why;
        GB_WARN("ClrBridge: %s", why);
        return false;
    }

    // Asked twice when the first buffer was too small: the managed side
    // returns the size it wanted, not the size it wrote, so a long answer is
    // re-requested at the right length rather than silently cut off.
    std::vector<char> buffer(std::max<std::size_t>(16 * 1024, input.size() + 1024));
    for (int attempt = 0; attempt < 2; ++attempt) {
        // The request goes in the buffer the answer comes back in.
        std::fill(buffer.begin(), buffer.end(), '\0');
        if (!input.empty() && input.size() + 1 <= buffer.size())
            std::memcpy(buffer.data(), input.data(), input.size());

        const int wanted = reinterpret_cast<component_entry_fn>(fn)(
            buffer.data(), static_cast<std::int32_t>(buffer.size()));
        if (wanted < 0) {
            if (trouble) *trouble = "the bridge method reported failure";
            return false;
        }
        if (static_cast<std::size_t>(wanted) < buffer.size()) {
            answer.assign(buffer.data());
            return true;
        }
        buffer.assign(static_cast<std::size_t>(wanted) + 1024, '\0');
    }
    answer.assign(buffer.data());
    return true;
}

} // namespace gobot
