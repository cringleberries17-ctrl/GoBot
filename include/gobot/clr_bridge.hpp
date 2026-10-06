#pragma once

#include <string>

namespace gobot {

/// Runs .NET code from inside this C++ program.
///
/// There is one reason this exists: the other helper is a .NET assembly, and
/// everything it knows is in there. This project has read that assembly from
/// the outside for a while now -- its pictures, its resource names, its
/// feature list -- and reading is not the same as calling. This calls.
///
/// The route is .NET's own hosting API. `hostfxr.dll` is asked to start a
/// runtime against a configuration file, hand back a function that can load an
/// assembly and resolve a method in it, and from there a managed method is an
/// ordinary function pointer. The managed side is GoBotBridge.dll, a small
/// library compiled from bridge/GoBotBridge.cs, which reaches into the
/// helper's assembly by reflection and answers in UTF-8.
///
/// Every entry point has the same shape: it is given a buffer and its size,
/// and returns how many bytes it wanted. That is the signature .NET's hosting
/// API calls a component entry point, and sticking to it keeps the managed
/// side free of unsafe code, of the UnmanagedCallersOnly attribute, and of any
/// need for the .NET SDK -- which is not installed here. Only the compiler
/// inside Visual Studio's build tools is, and that is enough.
///
/// Nothing in GoBot depends on this. The runtime may be absent, the helper may
/// not be installed, and the bridge may fail to build; all of those leave a
/// bridge that reports `running() == false` and a program that works as before.
class ClrBridge {
public:
    ClrBridge();
    ~ClrBridge();

    ClrBridge(const ClrBridge&) = delete;
    ClrBridge& operator=(const ClrBridge&) = delete;

    /// Start a runtime and load the bridge. Says what went wrong if it did not.
    ///
    /// Starting more than once is harmless: .NET allows one runtime per
    /// process, so the second call returns the first one's answer.
    bool start(std::string* trouble = nullptr);
    bool running() const { return entry_ != nullptr; }

    /// Call a method on GoBot.Bridge by name, and collect what it writes.
    ///
    /// `input` is handed over in the same buffer the answer comes back in,
    /// which is how a method is told what to act on -- a point to click, a
    /// rectangle to read. The managed side returns how many bytes it wanted
    /// rather than how many it wrote, so an answer too big for the first
    /// buffer is asked for again at the right size rather than coming back
    /// quietly truncated.
    bool call(const std::string& method, const std::string& input,
              std::string& answer, std::string* trouble = nullptr);

    /// For methods that need telling nothing.
    bool call(const std::string& method, std::string& answer,
              std::string* trouble = nullptr) {
        return call(method, {}, answer, trouble);
    }

    /// Where the pieces are. Exposed because "it did not start" is nearly
    /// always one of these being somewhere else.
    static std::string runtime_dir();      ///< the installed .NET, or empty
    static std::string hostfxr_path();     ///< the hosting library, or empty
    static std::string bridge_assembly();  ///< GoBotBridge.dll, beside the exe

private:
    struct Impl;
    Impl* impl_ = nullptr;
    void* entry_ = nullptr;     ///< load_assembly_and_get_function_pointer
};

} // namespace gobot
