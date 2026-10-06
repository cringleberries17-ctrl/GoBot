// gobot_gui — Win32 + Direct3D 11 host for the ImGui front-end.
//
// Standard ImGui boilerplate: create a window, spin up a D3D11 device and
// swap chain, pump messages, draw. The application logic lives in GuiApp.

#include "gui_app.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <tchar.h>

#include <cstdio>

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
    D3D_FEATURE_LEVEL got;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &g_swapchain, &g_device, &got, &g_context);

    // Fall back to WARP: some VMs and remote sessions have no usable GPU, and
    // a software device beats refusing to start.
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &g_swapchain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;

    create_rtv();
    return true;
}

void cleanup_device() {
    release_rtv();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
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

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"GoBot",
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

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    gobot::GuiApp app;
    // Npcap cannot open an adapter without elevation, and the failure surfaces
    // much later as a mysteriously empty interface list. Surfaced as a banner
    // inside the window rather than a modal before it: a dialog that blocks
    // before anything is on screen reads as a crash when it is dismissed.
    app.set_elevated(running_as_admin());
    // The radar needs to know where this window is: the game refuses to render
    // itself into a bitmap, so frames come off the screen and anything covering
    // the game is read instead of it.
    app.set_main_window(hwnd);
    app.initialise();

    const ImVec4 clear(0.06f, 0.06f, 0.07f, 1.00f);
    bool done = false;

    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
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
        app.tick();

        // Minimised or fully covered: stop rendering, which is all that is
        // actually wasted when nobody can see the window.
        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resize_w != 0 && g_resize_h != 0) {
            release_rtv();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.draw();

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        const float rgba[4] = { clear.x, clear.y, clear.z, clear.w };
        g_context->ClearRenderTargetView(g_rtv, rgba);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // vsync on: this is a control panel, not a game. Spinning the GPU at
        // hundreds of frames a second would steal time from the machine
        // running the actual game.
        const HRESULT hr = g_swapchain->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
