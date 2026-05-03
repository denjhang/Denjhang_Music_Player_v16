// main.cpp - Denjhang's Music Player v16 Entry Point
// Modular window architecture with ImGui DockSpace + Viewports:
//   Tab: YM2163 (DSG) - YM2163Window module
//   Tab: YMF262 (OPL3) - OPL3Window module
//   Tab: libvgm simulation player - VgmWindow module
//   Tab: SN76489 (DCSG) - SN76489Window module (FTDI/SPFM hardware)

#include <windows.h>
#include <d3d11.h>
#include <chrono>

#include "gui_renderer.h"
#include "chip_control.h"
#include "config_manager.h"
#include "midi_player.h"
#include "vgm_window.h"
#include "opl3_renderer.h"
#include "chip_window_ym2163.h"
#include "opl3_window.h"
#include "gigatron_window.h"
#include "sn76489_window.h"
#include "ym2413_window.h"
#include "spfm_manager.h"
#include "spfm_window.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

// ===== Visibility-based auto-pause =====
bool g_midiUserPaused = false;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // Set DPI awareness
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(void*);
        SetProcessDpiAwarenessContext_t func =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (func) {
            func((void*)-4);
        } else {
            HMODULE shcore = LoadLibraryA("shcore.dll");
            if (shcore) {
                typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
                SetProcessDpiAwareness_t func2 =
                    (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
                if (func2) func2(2);
                FreeLibrary(shcore);
            }
        }
        FreeLibrary(user32);
    }

    // Initialize config
    Config::InitConfigPaths();
    Config::LoadFrequenciesFromINI();
    Config::LoadSlotConfigFromINI();
    Config::LoadMIDIConfig();

    // Create window
    HICON hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(100));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), hIcon, nullptr, nullptr, nullptr,
        L"DenjhangMusicPlayer", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName,
        L"Denjhang's Music Player v16 - Quad YM2163 + OPL3 + libvgm",
        WS_OVERLAPPEDWINDOW, 100, 100, 1400, 900,
        nullptr, nullptr, wc.hInstance, nullptr);

    MidiPlayer::g_mainWindow = hwnd;
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Save imgui.ini next to executable for reliable layout persistence
    {
        wchar_t wbuf[MAX_PATH];
        GetModuleFileNameW(NULL, wbuf, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
        if (lastSlash) { wcscpy(lastSlash + 1, L"imgui.ini"); }
        char inipath[MAX_PATH];
        snprintf(inipath, sizeof(inipath), "%S", wbuf);
        io.IniFilename = strdup(inipath);
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_DockingEnable
                    | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDecoration = false;  // Show OS title bar on detached windows
    io.FontGlobalScale = 1.0f;
    io.FontAllowUserScaling = false;

    // When viewports are enabled, tweak WindowRounding/WindowBg
    // so that platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Tab colors to match original taskbar style
    style.Colors[ImGuiCol_Tab]        = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.3f, 0.6f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TabActive]  = ImVec4(0.2f, 0.5f, 0.9f, 1.0f);

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, &fontConfig,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", 20.0f, &fontConfig,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) {
        fontConfig.SizePixels = 20.0f;
        font = io.Fonts->AddFontDefault(&fontConfig);
    }
    if (font) {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;
        mergeConfig.OversampleH = 1;
        mergeConfig.OversampleV = 1;
        mergeConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 20.0f, &mergeConfig,
            io.Fonts->GetGlyphRangesKorean());
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msgothic.ttc", 20.0f, &mergeConfig,
            io.Fonts->GetGlyphRangesJapanese());
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    MidiPlayer::InitializeFileBrowser();
    VgmWindow::Init();
    if (!YM2163::g_useLiveControl)
        LoadInstrumentConfigToUI(g_selectedInstrument);

    // Initialize channel states before hardware connection
    YM2163::InitializeAllChannels();

    // SPFM Manager: auto-connect FTDI device, load chip type config
    SPFMManager::Init();
    SPFMWindow::Init();

    GigatronWindow::Init();
    SN76489Window::Init();
    YM2413Window::Init();
    if (MidiPlayer::g_enableGlobalMediaKeys)
        MidiPlayer::RegisterGlobalMediaKeys();

    // Check if imgui.ini exists (if not, this is first launch → apply default dock layout)
    DWORD attrib = GetFileAttributesA(io.IniFilename);
    bool s_dockLayoutInit = (attrib != INVALID_FILE_ATTRIBUTES);

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10); continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }

        // ===== Update all windows every frame =====
        SPFMManager::Update();
        SPFMWindow::Update();
        YM2163Window::Update();
        OPL3Window::Update();
        GigatronWindow::Update();
        SN76489Window::Update();
    YM2413Window::Update();
        // VgmWindow handles its own updates internally (via RenderInline)
        // But we still need to run MIDI playback updates
        MidiPlayer::UpdateMIDIPlayback();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ===== DockSpace host window =====
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##DockSpaceHost", nullptr,
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus);
        ImGui::PopStyleVar(3);

        // Menu bar with app title
        if (ImGui::BeginMenuBar()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Denjhang's Music Player");
            ImGui::EndMenuBar();
        }

        ImGuiID dockSpaceID = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockSpaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

        // Initial layout: dock all windows as tabs (only on first launch, when imgui.ini doesn't exist yet)
        if (!s_dockLayoutInit) {
            s_dockLayoutInit = true;
            ImGui::DockBuilderRemoveNode(dockSpaceID);
            ImGui::DockBuilderAddNode(dockSpaceID, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderDockWindow("YM2163(DSG)", dockSpaceID);
            ImGui::DockBuilderDockWindow("YMF262(OPL3)", dockSpaceID);
            ImGui::DockBuilderDockWindow("libvgm", dockSpaceID);
            ImGui::DockBuilderDockWindow("Gigatron", dockSpaceID);
            ImGui::DockBuilderDockWindow("SN76489(DCSG)", dockSpaceID);
    ImGui::DockBuilderDockWindow("YM2413(OPLL)", dockSpaceID);
            ImGui::DockBuilderDockWindow("SPFM", dockSpaceID);
            ImGui::DockBuilderFinish(dockSpaceID);
        }

        ImGui::End();

        // ===== Render all dockable windows =====
        SPFMWindow::Render();
        YM2163Window::Render();
        OPL3Window::Render();
        VgmWindow::RenderTab();
        GigatronWindow::Render();
        SN76489Window::Render();
    YM2413Window::Render();

        // Update keyboard capture state from active window
        MidiPlayer::g_isInputActive = YM2163Window::WantsKeyboardCapture();

        // ===== Render =====
        ImGui::Render();
        const float clear_color[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport rendering
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        Sleep(16);
    }

    // Cleanup (hardware disconnect already done in WM_DESTROY)
    VgmWindow::Shutdown();
    GigatronWindow::Shutdown();
    SPFMWindow::Shutdown();
    YM2413Window::Shutdown();
    SPFMManager::Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
