// gobot_gui — Win32 + Direct3D 11 host for the ImGui front-end.
//
// Standard ImGui boilerplate: create a window, spin up a D3D11 device and
// swap chain, pump messages, draw. The application logic lives in GuiApp.

#include "gui_app.hpp"
#include "gobot/common.hpp"
#include "gobot/dpi.hpp"
#include "gobot/frame_pacer.hpp"
#include "gobot/machine_info.hpp"
#include "gobot/util.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <tchar.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

// Declared in imgui_impl_win32.cpp; not in the public header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_occluded = false;
UINT                    g_resize_w = 0, g_resize_h = 0;
/// The device made, for GoBot's log ("GUI:"): create_device fills it.
gobot::RendererInfo     g_renderer;
/// What reached the window since the loop last looked, for the FramePacer:
/// someone using it (gobot::is_interactive_message), and WM_PAINT. Noted in
/// wnd_proc rather than in the loop's PeekMessageW, because much of it --
/// activation, focus, size, the cursor, DPI -- is sent, not posted, and
/// never comes out of the queue. All on this one thread.
bool                    g_input = false;
bool                    g_paint = false;
/// A combo or popup was open at the end of the last frame.
bool                    g_popup_open = false;

void cleanup_device();

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void release_rtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &g_swapchain, &g_device, &got, &g_context);

    // Fall back to WARP: some VMs and remote sessions have no usable GPU, and
    // a software device beats refusing to start. On any failure, not only
    // DXGI_ERROR_UNSUPPORTED: a VM's 3D driver that is there but broken fails
    // in other ways, and GoBot then would not start at all.
    bool warp = false;
    if (FAILED(hr)) {
        cleanup_device();       // whatever the failed attempt left
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &g_swapchain, &g_device, &got, &g_context);
        warp = SUCCEEDED(hr);
    }
    if (FAILED(hr)) return false;

    create_rtv();

    // What was made, for the log: a VMware guest with no 3D gets the
    // hardware device of the Basic Render Driver, which draws on the CPU just
    // as WARP does, and every frame of this window is then the CPU's work.
    g_renderer = gobot::RendererInfo{};
    g_renderer.known = true;
    g_renderer.warp = warp;
    g_renderer.feature_level = static_cast<unsigned>(got);
    g_renderer.swap_effect = "DISCARD";
    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                g_renderer.adapter = gobot::wide_to_utf8(desc.Description);
                g_renderer.vendor_id = desc.VendorId;
                g_renderer.device_id = desc.DeviceId;
                g_renderer.dedicated_mb = static_cast<std::uint64_t>(desc.DedicatedVideoMemory) / (1024u * 1024u);
            }
            adapter->Release();
        }
        dxgi->Release();
    }
    RECT client{};
    if (GetClientRect(hwnd, &client)) {
        g_renderer.client_w = client.right - client.left;
        g_renderer.client_h = client.bottom - client.top;
    }
    return true;
}

/// Waits up to `ms` for anything for this thread: input, or a message posted
/// or sent. Input in the queue that was looked at but not taken counts too
/// (MWMO_INPUTAVAILABLE), so a click never waits for the timeout.
void wait_for_input(std::uint32_t ms) {
    if (ms == 0) return;
    MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void cleanup_device() {
    release_rtv();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (gobot::is_interactive_message(msg)) g_input = true;
    else if (msg == WM_PAINT) g_paint = true;

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = static_cast<UINT>(LOWORD(lp));
            g_resize_h = static_cast<UINT>(HIWORD(lp));
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // swallow alt-menu
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// Dark theme with squarer corners — closer to the original client's look
/// than ImGui's rounded default.
void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 0.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(7, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.CellPadding       = ImVec2(6, 4);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.11f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.22f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.26f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_MenuBarBg]       = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.28f, 0.36f, 0.48f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.32f, 0.42f, 0.56f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.27f, 0.33f, 0.43f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.33f, 0.41f, 0.53f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.28f, 0.36f, 0.48f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);
    c[ImGuiCol_TableHeaderBg]   = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]   = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_Separator]       = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.40f, 0.52f, 0.68f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.45f, 0.75f, 0.95f, 1.00f);
}

bool running_as_admin() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = sizeof(e);
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size))
            elevated = e.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

/// Faces with Vietnamese in them, for the simple screen.
///
/// ImGui's built-in font covers ASCII and nothing more, so every "ế" and "ộ"
/// would come out as a question mark. Every Windows this runs on has Segoe UI,
/// with Tahoma and Arial behind it. Glyphs are rasterised as they are first
/// drawn -- ImGui 1.92 and its DX11 backend build the atlas on demand -- so
/// there are no ranges to list.
///
/// The built-in font is added first and stays the default. The full window is
/// laid out for it, and a different face under it would move every column.
struct Faces {
    ImFont* text = nullptr;
    ImFont* strong = nullptr;
};

Faces load_faces(ImGuiIO& io) {
    io.Fonts->AddFontDefault();

    char windows[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(windows, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::string dir = std::string(windows) + "\\Fonts\\";

    const auto first_of = [&](std::initializer_list<const char*> files) -> ImFont* {
        for (const char* file : files) {
            const std::string path = dir + file;
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig cfg;
            cfg.Flags |= ImFontFlags_NoLoadError;
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &cfg)) return f;
        }
        return nullptr;
    };

    Faces faces;
    faces.text   = first_of({ "segoeui.ttf", "tahoma.ttf", "arial.ttf" });
    faces.strong = first_of({ "seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf" });
    if (!faces.strong) faces.strong = faces.text;
    return faces;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Before anything loads a DLL. GoBot runs as administrator from a folder a
    // player made, and Windows looks for a DLL named without its path in the
    // program's own folder first: an xinput1_4.dll (Dear ImGui asks for it by
    // name) or a wpcap.dll left beside gobot_gui.exe would run as
    // administrator. From here on such DLLs come from System32 and Npcap's
    // folder alone. The ones this program imports load before this line;
    // /DEPENDENTLOADFLAG in CMakeLists.txt holds those to System32.
    gobot::harden_dll_search();

    // Real screen pixels for everything GoBot does to the game (gobot/dpi.hpp):
    // at 125% display scale Windows handed GoBot resampled frames and scaled
    // sizes, and nothing it looks for was found. Before any window exists. This
    // thread alone keeps the pixels Windows scales, because the window below is
    // laid out in them; the work below switches to real ones for its length.
    gobot::become_dpi_aware();
    gobot::set_thread_dpi(gobot::DpiScope::Mode::Scaled);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GoBotWindow";
    RegisterClassExW(&wc);

    // Portrait, and the same shape as the client this replaces: 975 x 1164.
    //
    // Taken from that client's own saved layout rather than chosen. It is the
    // right shape for the job and the reason is the game: the game runs in a
    // 1024x768 window, so a bot window that is tall and narrow stands beside
    // it on an ordinary screen, while a wide one has to overlap it -- and
    // anything overlapping the game breaks the screen capture that everything
    // here depends on.
    //
    // Capped to what the screen can actually show, since 1164 tall does not
    // fit on a 1080p display once the taskbar has had its share.
    const int wanted_w = 975, wanted_h = 1164;
    const int screen_h = GetSystemMetrics(SM_CYMAXIMIZED);
    const int height = screen_h > 0 ? std::min(wanted_h, screen_h) : wanted_h;

    // The version in the title, the way GoC shows its own ("GoC - 0.5.8.6"):
    // the first thing to quote when reporting a problem.
    const std::wstring title = gobot::utf8_to_wide("GoBot - " + gobot::version_shown());
    HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(),
                              WS_OVERLAPPEDWINDOW,
                              100, 20, wanted_w, height,
                              nullptr, nullptr, inst, nullptr);

    if (!create_device(hwnd)) {
        cleanup_device();
        UnregisterClassW(wc.lpszClassName, inst);
        MessageBoxW(nullptr, L"Failed to create a Direct3D 11 device.",
                    L"GoBot", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    static std::string ini_path = gobot::exe_relative("gobot_gui.ini");
    io.IniFilename = ini_path.c_str();

    apply_style();
    const Faces faces = load_faces(io);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    gobot::GuiApp app;
    app.set_fonts(faces.text, faces.strong);
    // Npcap cannot open an adapter without elevation, and the failure surfaces
    // much later as a mysteriously empty interface list. Surfaced as a banner
    // inside the window rather than a modal before it: a dialog that blocks
    // before anything is on screen reads as a crash when it is dismissed.
    app.set_elevated(running_as_admin());
    // The radar needs to know where this window is: the game refuses to render
    // itself into a bitmap, so frames come off the screen and anything covering
    // the game is read instead of it.
    app.set_main_window(hwnd);
    // The device just made, for the start's "GUI:" line and light mode's
    // look at the machine: GoBot's log opens only once it starts.
    app.set_renderer(g_renderer);
    {
        const gobot::DpiScope pixels(gobot::DpiScope::Mode::RealPixels);
        app.initialise();
    }

    const ImVec4 clear(0.06f, 0.06f, 0.07f, 1.00f);
    bool done = false;

    // When a frame is drawn (gobot/frame_pacer.hpp), and what the loop did,
    // counted for the Load line (GuiApp::LoopCounters).
    gobot::FramePacer pacer;
    gobot::GuiApp::LoopCounters& counters = app.loop_counters();
    LARGE_INTEGER qpc_hz{};
    QueryPerformanceFrequency(&qpc_hz);
    const auto ms_since = [&qpc_hz](const LARGE_INTEGER& from) {
        LARGE_INTEGER to{};
        QueryPerformanceCounter(&to);
        return qpc_hz.QuadPart > 0 ? static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 /
                                         static_cast<double>(qpc_hz.QuadPart)
                                   : 0.0;
    };

    while (!done) {
        // When this pass began: the pacer's wait is counted from here, so the
        // loop comes round within FramePacer::kMaxWaitMs of it however long
        // the pass took.
        const std::uint64_t woke = gobot::now_ms();
        MSG msg;
        // The W versions, by name. The window is a Unicode one (RegisterClassExW,
        // CreateWindowW), but UNICODE is not defined, so the bare names were the
        // A versions, and those pass WM_CHAR through the ANSI code page: "Tuấn"
        // typed into Tên nhân vật became "Tu?n". ImGui takes UTF-16 from a
        // Unicode window (IsWindowUnicode).
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || app.wants_quit()) break;

        // Before the occlusion check, not after it.
        //
        // tick() is where the global hotkeys are read, and the moment they
        // matter most is when the game is full screen on top of this window —
        // which is exactly when the window is occluded. Skipping it there
        // meant the panic key stopped working whenever there was something to
        // panic about.
        {
            const gobot::DpiScope pixels(gobot::DpiScope::Mode::RealPixels);
            app.tick();
        }
        ++counters.passes;

        // How frames are paced, asked every pass: "Nhẹ máy" can be ticked
        // meanwhile. With [Performance] gui_pacer=off, the loop as it was
        // before the pacer: a frame every pass, held to the screen's refresh
        // by vsync.
        const gobot::GuiApp::Pacing pacing = app.pacing();

        // Minimised or fully covered: stop rendering, which is all that is
        // actually wasted when nobody can see the window.
        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ++counters.occluded_skips;
            if (pacing.pacer_on)
                wait_for_input(gobot::FramePacer::tick_wait_ms(woke, gobot::now_ms()));
            else
                Sleep(10);
            continue;
        }
        g_occluded = false;

        // Draw only when a frame is due. Whatever the answer, the pass above
        // has run, and the wait below is never longer than
        // FramePacer::kMaxWaitMs: the keys tick() reads are read as often at
        // 1 frame a second as at 30.
        if (pacing.pacer_on) {
            gobot::FramePacer::Pass pass;
            pass.now_ms = woke;
            pass.input = g_input;
            pass.paint = g_paint;
            g_input = g_paint = false;
            pass.foreground = GetForegroundWindow() == hwnd;
            // Only with GoBot in front: ImGui keeps a text box active and a
            // combo open when the window loses the focus, and one left so
            // behind the game would hold the interactive rate for as long as
            // the bot runs.
            pass.want_interactive = pass.foreground && (pacing.want_interactive || io.WantTextInput ||
                                                        g_popup_open || ImGui::IsAnyMouseDown());
            pass.minimised = IsIconic(hwnd) != FALSE;
            pass.redraw = app.take_redraw();
            pass.rates = pacing.rates;
            if (!pacer.decide(pass)) {
                wait_for_input(pacer.wait_ms(gobot::now_ms()));
                continue;
            }
        }

        if (g_resize_w != 0 && g_resize_h != 0) {
            release_rtv();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }

        LARGE_INTEGER built_from{};
        QueryPerformanceCounter(&built_from);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // What the panels do to the game -- looks, clicks, finding and moving
        // windows -- in real pixels; ImGui itself asks Windows for nothing here.
        {
            const gobot::DpiScope pixels(gobot::DpiScope::Mode::RealPixels);
            app.draw();
        }
        g_popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        const float rgba[4] = { clear.x, clear.y, clear.z, clear.w };
        g_context->ClearRenderTargetView(g_rtv, rgba);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const double build_ms = ms_since(built_from);

        // This is a control panel, not a game: spinning the GPU at hundreds of
        // frames a second would steal time from the machine running the
        // actual game. With the pacer, it sets the pace and Present does not
        // wait for the screen; without it, vsync holds the loop to the
        // screen's refresh.
        LARGE_INTEGER presented_from{};
        QueryPerformanceCounter(&presented_from);
        const HRESULT hr = g_swapchain->Present(pacing.pacer_on ? 0 : 1, 0);
        counters.frame(build_ms, ms_since(presented_from));
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);

        if (pacing.pacer_on) wait_for_input(pacer.wait_ms(gobot::now_ms()));
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    // GuiApp's destructor, at the return, stops every job in real pixels too.
    gobot::set_thread_dpi(gobot::DpiScope::Mode::RealPixels);
    return 0;
}
