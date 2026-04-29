// libvgm VGM Player with ImGui + DirectX 11
// Features: File browser, playback controls, VU meters, waveform visualization

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// libvgm includes
extern "C" {
#include "stdtype.h"
#include "audio/AudioStream.h"
#include "audio/AudioStructs.h"  // For ADRVSIG_DSOUND
}
#include "utils/DataLoader.h"
#include "utils/FileLoader.h"
#include "player/playera.hpp"
#include "player/vgmplayer.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"

// Chip visualization includes
#include "chip_visualizer.h"
#include "imgui_chip_windows.h"

// DirectSound driver-specific function
extern "C" UINT8 DSound_SetHWnd(void* drvObj, HWND hWnd);

// DirectX 11 globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Audio globals
static void* audDrv = nullptr;
static void* audDrvLog = nullptr;
static PlayerA* player = nullptr;

// Visualization globals
const int VU_METER_COUNT = 32;
const int WAVEFORM_SAMPLES = 256;
static float vuMeters[VU_METER_COUNT] = {0};
static float waveform[WAVEFORM_SAMPLES] = {0};
static float vuDecay = 0.8f;
static UINT32 fillBufferCallCount = 0;  // Debug counter
static UINT32 lastRenderedBytes = 0;    // Last render result

// File browser globals
static std::wstring currentFolder;
static std::vector<std::wstring> playlist;
static int currentTrackIndex = -1;
static char folderPathInput[512] = "";

// Playback state
static bool isPlaying = false;
static bool isPaused = false;
static float volume = 0.75f;

// Debug log
static ImGuiTextBuffer debugLog;
static bool showDebugLog = false;  // 默认隐藏调试日志
static bool autoScrollLog = true;

// Chip visualization globals
static YM2612Visualizer* ym2612Vis = nullptr;
static SN76489Visualizer* sn76489Vis = nullptr;
static YM2612Window* ym2612Window = nullptr;
static SN76489Window* sn76489Window = nullptr;

void AddDebugLog(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    debugLog.appendfv(fmt, args);
    debugLog.append("\n");
    va_end(args);
}

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper functions
void GetExePath(std::wstring& exePath);
void PopulateFileList(const std::wstring& folder);
bool PlayFile(int index);
void StopPlayback();
void UpdateVisualization();
UINT32 FillBuffer(void* drvObj, void* userParam, UINT32 bufSize, void* data);

// Convert wstring to UTF-8 string for ImGui
std::string WStringToString(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// Convert UTF-8 string to wstring
std::wstring StringToWString(const std::string& str)
{
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

void GetExePath(std::wstring& exePath)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    exePath = path;
}

void PopulateFileList(const std::wstring& folder)
{
    playlist.clear();
    currentFolder = folder;

    // Convert to UTF-8 for input buffer
    std::string folderUtf8 = WStringToString(folder);
    strncpy(folderPathInput, folderUtf8.c_str(), sizeof(folderPathInput) - 1);

    std::wstring searchPath = folder + L"\\*.*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring fileName = findData.cFileName;
            std::wstring ext = fileName.substr(fileName.find_last_of(L".") + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            if (ext == L"vgm" || ext == L"vgz" || ext == L"s98" ||
                ext == L"dro" || ext == L"gym")
            {
                playlist.push_back(fileName);
            }
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    std::sort(playlist.begin(), playlist.end());

    // Auto-select first file if available
    if (!playlist.empty())
    {
        currentTrackIndex = 0;
    }
    else
    {
        currentTrackIndex = -1;
    }
}

bool PlayFile(int index)
{
    AddDebugLog("=== PlayFile called: index=%d, playlist.size=%d ===", index, (int)playlist.size());

    if (index < 0 || index >= (int)playlist.size())
    {
        AddDebugLog("ERROR: Invalid index %d (playlist size: %d)", index, (int)playlist.size());
        return false;
    }

    AddDebugLog("Stopping previous playback...");
    StopPlayback();

    std::wstring filePath = currentFolder + L"\\" + playlist[index];
    AddDebugLog("Loading file: %S", filePath.c_str());

    // Convert to multi-byte for file loading
    int size_needed = WideCharToMultiByte(CP_ACP, 0, filePath.c_str(), -1, NULL, 0, NULL, NULL);
    std::string filePathMb(size_needed, 0);
    WideCharToMultiByte(CP_ACP, 0, filePath.c_str(), -1, &filePathMb[0], size_needed, NULL, NULL);

    AddDebugLog("FileLoader_Init...");
    DATA_LOADER* dLoad = FileLoader_Init(filePathMb.c_str());
    if (dLoad == nullptr)
    {
        AddDebugLog("ERROR: FileLoader_Init returned NULL!");
        return false;
    }

    AddDebugLog("DataLoader_Load...");
    UINT8 retVal = DataLoader_Load(dLoad);
    if (retVal)
    {
        DataLoader_Deinit(dLoad);
        AddDebugLog("ERROR: DataLoader_Load failed with code: %u", retVal);
        return false;
    }

    AddDebugLog("player->LoadFile...");
    retVal = player->LoadFile(dLoad);
    if (retVal)
    {
        DataLoader_Deinit(dLoad);
        AddDebugLog("ERROR: player->LoadFile failed with code: %u", retVal);
        return false;
    }

    AddDebugLog("player->Start...");
    retVal = player->Start();
    if (retVal)
    {
        player->UnloadFile();
        DataLoader_Deinit(dLoad);
        AddDebugLog("ERROR: player->Start failed with code: %u", retVal);
        return false;
    }

    currentTrackIndex = index;
    isPlaying = true;
    isPaused = false;

    // Create chip visualizers based on song devices
    AddDebugLog("Creating chip visualizers...");
    PlayerBase* basePlayer = player->GetPlayer();
    if (basePlayer) {
        VGMPlayer* vgmPlayer = dynamic_cast<VGMPlayer*>(basePlayer);
        if (vgmPlayer) {
            std::vector<PLR_DEV_INFO> devices;
            vgmPlayer->GetSongDeviceInfo(devices);

            // Clean up existing visualizers
            if (ym2612Window) { delete ym2612Window; ym2612Window = nullptr; }
            if (ym2612Vis) { delete ym2612Vis; ym2612Vis = nullptr; }
            if (sn76489Window) { delete sn76489Window; sn76489Window = nullptr; }
            if (sn76489Vis) { delete sn76489Vis; sn76489Vis = nullptr; }

            // Create visualizers for detected chips
            for (const auto& dev : devices) {
                AddDebugLog("  Device: type=0x%02X, instance=%d", dev.type, dev.instance);

                if (dev.type == 0x02 && dev.instance == 0) {  // YM2612
                    AddDebugLog("    Creating YM2612 visualizer");
                    ym2612Vis = new YM2612Visualizer();
                    ym2612Window = new YM2612Window(ym2612Vis);
                    ym2612Window->SetVisible(true);  // Auto-show
                }
                else if (dev.type == 0x00 && dev.instance == 0) {  // SN76489
                    AddDebugLog("    Creating SN76489 visualizer");
                    sn76489Vis = new SN76489Visualizer();
                    sn76489Window = new SN76489Window(sn76489Vis);
                    sn76489Window->SetVisible(true);  // Auto-show
                }
            }
        }
    }

    AddDebugLog("SUCCESS! Playback started. isPlaying=%d, State=0x%02X", isPlaying, player->GetState());
    return true;
}

void StopPlayback()
{
    if (player && isPlaying)
    {
        player->Stop();
        player->UnloadFile();
        isPlaying = false;
        isPaused = false;
    }

    // Clean up visualizers
    if (ym2612Window) { delete ym2612Window; ym2612Window = nullptr; }
    if (ym2612Vis) { delete ym2612Vis; ym2612Vis = nullptr; }
    if (sn76489Window) { delete sn76489Window; sn76489Window = nullptr; }
    if (sn76489Vis) { delete sn76489Vis; sn76489Vis = nullptr; }
}

UINT32 FillBuffer(void* drvObj, void* userParam, UINT32 bufSize, void* data)
{
    fillBufferCallCount++;

    if (!player || !isPlaying || isPaused)
    {
        memset(data, 0, bufSize);
        lastRenderedBytes = 0;
        return bufSize;
    }

    UINT32 renderedBytes = player->Render(bufSize, data);
    lastRenderedBytes = renderedBytes;

    // Update visualization
    INT16* samples = (INT16*)data;
    UINT32 sampleCount = renderedBytes / 4;  // 16-bit stereo = 4 bytes per sample frame

    // Update VU meters (frequency-like bands)
    for (int i = 0; i < VU_METER_COUNT; i++)
    {
        float sum = 0.0f;
        int start = (i * sampleCount) / VU_METER_COUNT;
        int end = ((i + 1) * sampleCount) / VU_METER_COUNT;

        for (int j = start; j < end && j < (int)sampleCount; j++)
        {
            float sample = abs(samples[j * 2]) / 32768.0f;
            sum += sample;
        }

        float avg = (end > start) ? (sum / (end - start)) : 0.0f;
        vuMeters[i] = fmax(vuMeters[i] * vuDecay, avg);
    }

    // Update waveform
    for (int i = 0; i < WAVEFORM_SAMPLES && i < (int)sampleCount; i++)
    {
        int idx = (i * sampleCount) / WAVEFORM_SAMPLES;
        waveform[i] = samples[idx * 2] / 32768.0f;
    }

    // Check if playback finished
    if (player->GetState() & PLAYSTATE_FIN)
    {
        // Auto-play next track
        if (currentTrackIndex + 1 < (int)playlist.size())
        {
            PlayFile(currentTrackIndex + 1);
        }
        else
        {
            StopPlayback();
        }
    }

    return renderedBytes;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Initialize COM for DirectSound (must use MULTITHREADED for audio)
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // DPI awareness - 禁用系统DPI缩放，使用固定大字体
    ImGui_ImplWin32_EnableDpiAwareness();

    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"libvgm ImGui Player", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"libvgm Player - ImGui + DirectX 11",
                                WS_OVERLAPPEDWINDOW, 100, 100,
                                1280, 800,
                                nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize DirectX 11
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 字体配置 - 加载Windows系统大字体，点对点清晰显示
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;  // 不过采样，保持点对点
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;  // 强制像素对齐

    // 尝试加载Windows系统字体，如果失败则使用默认字体
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, &fontConfig, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) {
        // 备选：Segoe UI
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f, &fontConfig);
    }
    if (!font) {
        // 最后备选：使用默认字体
        fontConfig.SizePixels = 20.0f;
        io.Fonts->AddFontDefault(&fontConfig);
    }

    // Setup ImGui style - VSCode dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // 完全禁用抗锯齿，实现像素完美点对点显示
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;

    // VSCode color theme
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);  // #1E1E1E
    colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);   // #252526
    colors[ImGuiCol_FrameBg] = ImVec4(0.24f, 0.24f, 0.26f, 1.00f);   // #3E3E42
    colors[ImGuiCol_Header] = ImVec4(0.04f, 0.28f, 0.44f, 1.00f);    // #094771
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.06f, 0.35f, 0.55f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.04f, 0.28f, 0.44f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.83f, 0.83f, 0.83f, 1.00f);      // #D4D4D4

    // Initialize backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialize audio
    Audio_Init();
    UINT32 drvCount = Audio_GetDriverCount();
    UINT32 drvID = (UINT32)-1;

    // Declare variables before any goto statements
    AUDIO_OPTS* opts = nullptr;
    std::wstring exePath;
    bool done = false;
    UINT8 startRet = 0;

    // Find DirectSound driver
    for (UINT32 i = 0; i < drvCount; i++)
    {
        AUDDRV_INFO* drvInfo;
        Audio_GetDriverInfo(i, &drvInfo);
        if (strcmp(drvInfo->drvName, "DirectSound") == 0)
        {
            drvID = i;
            break;
        }
    }

    if (drvID == (UINT32)-1)
    {
        MessageBoxA(hwnd, "DirectSound driver not found!", "Error", MB_ICONERROR);
        goto cleanup;
    }

    AudioDrv_Init(drvID, &audDrv);

    // Set HWND for DirectSound (CRITICAL!)
    AUDDRV_INFO* drvInfo;
    Audio_GetDriverInfo(drvID, &drvInfo);
    if (drvInfo->drvSig == ADRVSIG_DSOUND)
    {
        DSound_SetHWnd(AudioDrv_GetDrvData(audDrv), hwnd);
    }

    opts = AudioDrv_GetOptions(audDrv);
    opts->sampleRate = 44100;
    opts->numChannels = 2;
    opts->numBitsPerSmpl = 16;
    opts->usecPerBuf = 10000;
    opts->numBuffers = 10;

    AudioDrv_SetCallback(audDrv, FillBuffer, nullptr);
    startRet = AudioDrv_Start(audDrv, 0);
    AddDebugLog("=== Audio System Initialized ===");
    AddDebugLog("AudioDrv_Start returned: %u (0=OK)", startRet);

    if (startRet != 0)
    {
        char errMsg[256];
        sprintf(errMsg, "AudioDrv_Start FAILED with error code %u (0x%02X)\n\nDirectSound may not be properly initialized.\nPlease check audio settings.", startRet, startRet);
        AddDebugLog("ERROR: %s", errMsg);
        MessageBoxA(hwnd, errMsg, "Audio Driver Error", MB_ICONERROR);
    }

    AddDebugLog("DirectSound HWND set: %s", (drvInfo->drvSig == ADRVSIG_DSOUND) ? "YES" : "NO");
    AddDebugLog("Sample rate: %u Hz, Channels: %u, Bits: %u", opts->sampleRate, opts->numChannels, opts->numBitsPerSmpl);
    AddDebugLog("Buffer: %u usec × %u buffers", opts->usecPerBuf, opts->numBuffers);

    // Initialize player
    player = new PlayerA();
    player->SetOutputSettings(opts->sampleRate, opts->numChannels, opts->numBitsPerSmpl, 1024);
    player->SetLoopCount(2);
    player->SetMasterVolume((INT32)(0x10000 * volume));

    // Register player engines
    player->RegisterPlayerEngine(new VGMPlayer());
    player->RegisterPlayerEngine(new S98Player());
    player->RegisterPlayerEngine(new DROPlayer());
    player->RegisterPlayerEngine(new GYMPlayer());
    AddDebugLog("Player engines registered: VGM, S98, DRO, GYM");

    // Load files from current directory
    GetExePath(exePath);
    PopulateFileList(exePath);
    AddDebugLog("Loaded %d files from: %S", (int)playlist.size(), exePath.c_str());

    // Main loop
    done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("libvgm Player", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // Title
        ImGui::Text("libvgm Player - ImGui Edition");
        ImGui::Separator();

        // Layout: Left panel (file browser) + Right panel (visualization)
        float panelHeight = io.DisplaySize.y - 150;  // 为底部控制区预留150px
        ImGui::BeginChild("LeftPanel", ImVec2(350, panelHeight), true);
        {
            ImGui::Text("File Browser");
            ImGui::Separator();

            // Address bar
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##Path", folderPathInput, sizeof(folderPathInput), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::wstring newPath = StringToWString(folderPathInput);
                PopulateFileList(newPath);
            }
            ImGui::PopItemWidth();

            if (ImGui::Button("Open Folder", ImVec2(-1, 0)))
            {
                // Use native folder dialog (simplified)
                wchar_t path[MAX_PATH];
                BROWSEINFOW bi = {0};
                bi.lpszTitle = L"Select Folder";
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl != nullptr)
                {
                    if (SHGetPathFromIDListW(pidl, path))
                    {
                        PopulateFileList(path);
                    }
                    CoTaskMemFree(pidl);
                }
            }

            ImGui::Separator();

            // File list
            ImGui::BeginChild("FileList", ImVec2(0, 0), false);
            for (int i = 0; i < (int)playlist.size(); i++)
            {
                std::string fileName = WStringToString(playlist[i]);
                bool isSelected = (i == currentTrackIndex);

                // ImGui standard pattern: check double-click when Selectable is activated
                bool wasClicked = ImGui::Selectable(fileName.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick);

                if (wasClicked)
                {
                    currentTrackIndex = i;  // Always update selection on click

                    // Check if it was a double-click
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        AddDebugLog("*** DOUBLE-CLICK detected on file %d: %s ***", i, fileName.c_str());
                        PlayFile(i);
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel - Visualization
        ImGui::BeginChild("RightPanel", ImVec2(0, panelHeight), true);
        {
            ImGui::Text("Visualization");
            ImGui::Separator();

            // VU Meters
            ImGui::Text("VU Meters (32 channels)");
            float vuHeight = 80.0f;  // 减小高度
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, vuHeight);
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            float barWidth = canvasSize.x / VU_METER_COUNT;
            for (int i = 0; i < VU_METER_COUNT; i++)
            {
                float x = canvasPos.x + i * barWidth;
                float height = vuMeters[i] * vuHeight;

                // Color gradient: green -> yellow -> red
                ImU32 color;
                if (vuMeters[i] < 0.6f)
                    color = IM_COL32(0, 255, 0, 255);  // Green
                else if (vuMeters[i] < 0.8f)
                    color = IM_COL32(255, 255, 0, 255);  // Yellow
                else
                    color = IM_COL32(255, 0, 0, 255);  // Red

                drawList->AddRectFilled(
                    ImVec2(x + 1, canvasPos.y + vuHeight - height),
                    ImVec2(x + barWidth - 1, canvasPos.y + vuHeight),
                    color
                );
            }

            ImGui::Dummy(ImVec2(0, vuHeight + 5));  // 减小间距

            // Waveform
            ImGui::Text("Waveform");
            ImVec2 wavePos = ImGui::GetCursorScreenPos();
            ImVec2 waveSize = ImVec2(ImGui::GetContentRegionAvail().x, 60.0f);  // 减小高度

            drawList->AddRect(wavePos, ImVec2(wavePos.x + waveSize.x, wavePos.y + waveSize.y),
                            IM_COL32(100, 100, 100, 255));

            float centerY = wavePos.y + waveSize.y / 2;
            for (int i = 0; i < WAVEFORM_SAMPLES - 1; i++)
            {
                float x1 = wavePos.x + (i * waveSize.x) / WAVEFORM_SAMPLES;
                float x2 = wavePos.x + ((i + 1) * waveSize.x) / WAVEFORM_SAMPLES;
                float y1 = centerY - waveform[i] * waveSize.y / 2;
                float y2 = centerY - waveform[i + 1] * waveSize.y / 2;

                drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(0, 200, 255, 255));
            }

            ImGui::Dummy(ImVec2(0, waveSize.y + 5));  // 减小间距

            // Playback info
            ImGui::Separator();
            if (isPlaying && currentTrackIndex >= 0 && player)
            {
                std::string currentFile = WStringToString(playlist[currentTrackIndex]);
                ImGui::Text("Now Playing: %s", currentFile.c_str());

                double curTime = player->GetCurTime(PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE);
                double totalTime = player->GetTotalTime(PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE);
                UINT8 state = player->GetState();

                int curMin = (int)(curTime / 60);
                int curSec = (int)curTime % 60;
                int totMin = (int)(totalTime / 60);
                int totSec = (int)totalTime % 60;

                ImGui::Text("Time: %02d:%02d / %02d:%02d", curMin, curSec, totMin, totSec);

                // Progress bar
                float progress = (totalTime > 0) ? (float)(curTime / totalTime) : 0.0f;
                ImGui::ProgressBar(progress, ImVec2(-1, 0));
            }
            else
            {
                ImGui::Text("No file playing");
            }
        }
        ImGui::EndChild();

        // Bottom control panel
        ImGui::Separator();

        // Playback controls
        if (ImGui::Button("Prev", ImVec2(70, 25)))
        {
            if (currentTrackIndex > 0)
                PlayFile(currentTrackIndex - 1);
        }
        ImGui::SameLine();

        if (isPlaying && !isPaused)
        {
            if (ImGui::Button("Pause", ImVec2(70, 25)))
            {
                isPaused = true;
            }
        }
        else if (isPlaying && isPaused)
        {
            if (ImGui::Button("Resume", ImVec2(70, 25)))
            {
                isPaused = false;
            }
        }
        else
        {
            if (ImGui::Button("Play", ImVec2(70, 25)))
            {
                AddDebugLog("*** PLAY button clicked ***");
                if (currentTrackIndex >= 0)
                {
                    AddDebugLog("Playing currentTrackIndex=%d", currentTrackIndex);
                    PlayFile(currentTrackIndex);
                }
                else if (playlist.size() > 0)
                {
                    AddDebugLog("No selection, playing first file (index 0)");
                    PlayFile(0);
                }
                else
                {
                    AddDebugLog("ERROR: No files in playlist!");
                }
            }
        }
        ImGui::SameLine();

        if (ImGui::Button("Stop", ImVec2(70, 25)))
        {
            StopPlayback();
        }
        ImGui::SameLine();

        if (ImGui::Button("Next", ImVec2(70, 25)))
        {
            if (currentTrackIndex + 1 < (int)playlist.size())
                PlayFile(currentTrackIndex + 1);
        }
        ImGui::SameLine();

        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f"))
        {
            player->SetMasterVolume((INT32)(0x10000 * volume));
        }

        // Debug log window
        ImGui::Separator();
        ImGui::Checkbox("Show Debug Log", &showDebugLog);

        if (showDebugLog)
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear Log"))
            {
                debugLog.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy Log"))
            {
                const char* text = debugLog.c_str();
                size_t len = strlen(text);

                if (OpenClipboard(NULL))
                {
                    EmptyClipboard();
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len + 1);
                    if (hMem)
                    {
                        char* pMem = (char*)GlobalLock(hMem);
                        if (pMem)
                        {
                            memcpy(pMem, text, len + 1);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_TEXT, hMem);
                        }
                    }
                    CloseClipboard();
                    AddDebugLog(">>> Log copied to clipboard (%zu bytes)", len);
                }
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScrollLog);

            ImGui::BeginChild("DebugLogRegion", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);  // 减小高度到120
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

            const char* buf = debugLog.begin();
            const char* buf_end = debugLog.end();

            ImGuiListClipper clipper;
            clipper.Begin((int)((buf_end - buf) / 50)); // Rough estimate of lines

            // Display text with selectable capability
            ImGui::TextUnformatted(buf, buf_end);

            if (autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);

            ImGui::PopStyleVar();
            ImGui::EndChild();
        }

        ImGui::End();

        // Update and render chip visualization windows
        if (isPlaying && player) {
            PlayerBase* basePlayer = player->GetPlayer();
            if (basePlayer) {
                VGMPlayer* vgmPlayer = dynamic_cast<VGMPlayer*>(basePlayer);
                if (vgmPlayer) {
                    std::vector<PLR_DEV_INFO> devices;
                    vgmPlayer->GetSongDeviceInfo(devices);

                    // Find and update chip visualizers
                    for (const auto& dev : devices) {
                        // Update YM2612
                        if (dev.type == 0x02 && dev.instance == 0 && ym2612Vis) {
                            // Get chip device pointer using public accessor
                            VGMPlayer::CHIP_DEVICE* chipDev = vgmPlayer->GetChipDevice(dev.type, dev.instance);
                            if (chipDev && chipDev->base.defInf.dataPtr && chipDev->base.defInf.dataPtr->chipInf) {
                                ym2612Vis->Update(chipDev->base.defInf.dataPtr->chipInf);
                            }
                        }
                        // Update SN76489
                        else if (dev.type == 0x00 && dev.instance == 0 && sn76489Vis) {
                            VGMPlayer::CHIP_DEVICE* chipDev = vgmPlayer->GetChipDevice(dev.type, dev.instance);
                            if (chipDev && chipDev->base.defInf.dataPtr && chipDev->base.defInf.dataPtr->chipInf) {
                                sn76489Vis->Update(chipDev->base.defInf.dataPtr->chipInf);
                            }
                        }
                    }
                }
            }
        }

        // Render chip windows
        if (ym2612Window) {
            ym2612Window->Render();
        }
        if (sn76489Window) {
            sn76489Window->Render();
        }

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.12f, 0.12f, 0.12f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

cleanup:
    // Cleanup
    StopPlayback();

    if (player)
    {
        player->UnregisterAllPlayers();
        delete player;
    }

    if (audDrv)
    {
        AudioDrv_Stop(audDrv);
        AudioDrv_Deinit(&audDrv);
    }

    Audio_Deinit();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    CoUninitialize();

    return 0;
}

// DirectX 11 helper functions
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                createDeviceFlags, featureLevelArray, 2,
                                                D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, featureLevelArray, 2,
                                            D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
