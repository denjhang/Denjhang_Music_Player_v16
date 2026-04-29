// VGM Player with Win32 GDI GUI and File Browser
// Features: File browser, playback controls, VU meters, waveform visualization

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <vector>
#include <string>

#include "player/playerbase.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/vgmplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/playera.hpp"
#include "audio/AudioStream.h"

extern "C" {
#include "utils/FileLoader.h"
#include "emu/EmuStructs.h"
}

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")

// Global constants
const wchar_t* WINDOW_CLASS = L"VGMPlayerGUI";
const wchar_t* WINDOW_TITLE = L"libvgm Player - GDI GUI";
const int WINDOW_WIDTH = 1000;
const int WINDOW_HEIGHT = 600;
const int TIMER_ID_RENDER = 1;
const int RENDER_FPS = 30; // 30 FPS for GDI
const int VU_METER_COUNT = 32;

// VSCode dark theme colors
const COLORREF COLOR_BG_DARK = RGB(30, 30, 30);      // #1E1E1E
const COLORREF COLOR_BG_SIDEBAR = RGB(37, 37, 38);   // #252526
const COLORREF COLOR_TEXT_LIGHT = RGB(212, 212, 212); // #D4D4D4
const COLORREF COLOR_SELECTED = RGB(9, 71, 113);     // #094771
const COLORREF COLOR_BORDER = RGB(62, 62, 66);       // #3E3E42

// Control IDs
const int ID_LISTVIEW_FILES = 1001;
const int ID_BTN_OPEN_FOLDER = 1002;
const int ID_BTN_PLAY = 1003;
const int ID_BTN_PAUSE = 1004;
const int ID_BTN_STOP = 1005;
const int ID_BTN_PREV = 1006;
const int ID_BTN_NEXT = 1007;
const int ID_SLIDER_POSITION = 1008;
const int ID_SLIDER_VOLUME = 1009;
const int ID_STATIC_VIZ = 1010;
const int ID_EDIT_PATH = 1011;

// Global variables
HWND g_hWnd = NULL;
HWND g_hListView = NULL;
HWND g_hEditPath = NULL; // Path address bar
HWND g_hStaticViz = NULL; // Static control for visualization
HWND g_hBtnOpenFolder = NULL;
HWND g_hBtnPlay = NULL;
HWND g_hBtnPause = NULL;
HWND g_hBtnStop = NULL;
HWND g_hBtnPrev = NULL;
HWND g_hBtnNext = NULL;
HWND g_hSliderPosition = NULL;
HWND g_hSliderVolume = NULL;

// GDI resources
HDC g_hdcMem = NULL;
HBITMAP g_hbmMem = NULL;
HBITMAP g_hbmOld = NULL;
HBRUSH g_hBrushDark = NULL;
HBRUSH g_hBrushSidebar = NULL;

// Audio and player
void* g_audDrv = NULL;
PlayerA* g_player = NULL;
bool g_isPlaying = false;
bool g_isPaused = false;
std::wstring g_currentFolder;
std::vector<std::wstring> g_playlist;
int g_currentTrack = -1;
UINT32 g_masterVol = 0xC000; // 75% volume to prevent clipping

// Visualization data
float g_vuLevels[VU_METER_COUNT] = {0};
std::vector<float> g_waveformData(256, 0.0f);
UINT32 g_waveformPos = 0;

// Function declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK VizWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CreateControls(HWND hwnd);
void CreateVizBuffers(HWND hwnd);
void CleanupVizBuffers();
void RenderVisualization();
void DrawVUMeters(HDC hdc, int x, int y, int width, int height);
void DrawWaveform(HDC hdc, int x, int y, int width, int height);
void GetExePath(std::wstring& exePath);
void OpenFolderDialog();
void PopulateFileList(const std::wstring& folder);
void PlaySelectedFile();
void PlayFile(const std::wstring& filePath);
void StopPlayback();
void PausePlayback();
void ResumePlayback();
void PlayNext();
void PlayPrev();
void UpdatePositionSlider();
void SeekToPosition(UINT32 samplePos);
void SetVolume(UINT8 volumePercent);
UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data);

// WinMain entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow)
{
    // Initialize COM
    CoInitialize(NULL);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    // Create dark theme brushes
    g_hBrushDark = CreateSolidBrush(COLOR_BG_DARK);
    g_hBrushSidebar = CreateSolidBrush(COLOR_BG_SIDEBAR);

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushDark; // Dark background
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassEx(&wc);

    // Register visualization window class
    WNDCLASSEX wcViz = {};
    wcViz.cbSize = sizeof(WNDCLASSEX);
    wcViz.style = CS_HREDRAW | CS_VREDRAW;
    wcViz.lpfnWndProc = VizWndProc;
    wcViz.hInstance = hInstance;
    wcViz.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcViz.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcViz.lpszClassName = L"VGMPlayerViz";
    RegisterClassEx(&wcViz);

    // Create window
    g_hWnd = CreateWindowEx(
        0,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    if (g_hWnd == NULL)
        return 0;

    // Create controls
    CreateControls(g_hWnd);

    // Load files from current directory
    std::wstring exePath;
    GetExePath(exePath);
    PopulateFileList(exePath);

    // Show window
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // Start render timer (30 FPS)
    SetTimer(g_hWnd, TIMER_ID_RENDER, 1000 / RENDER_FPS, NULL);

    // Message loop
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    KillTimer(g_hWnd, TIMER_ID_RENDER);
    CleanupVizBuffers();
    if (g_hBrushDark) DeleteObject(g_hBrushDark);
    if (g_hBrushSidebar) DeleteObject(g_hBrushSidebar);
    CoUninitialize();

    return (int)msg.wParam;
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DESTROY:
        StopPlayback();
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID_RENDER)
        {
            RenderVisualization();
            if (g_isPlaying && !g_isPaused)
            {
                UpdatePositionSlider();

                // Check if track finished
                if (g_player && (g_player->GetState() & PLAYSTATE_FIN))
                {
                    PlayNext();
                }
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BTN_OPEN_FOLDER:
            OpenFolderDialog();
            break;
        case ID_BTN_PLAY:
            if (g_isPaused)
                ResumePlayback();
            else
                PlaySelectedFile();
            break;
        case ID_BTN_PAUSE:
            if (g_isPlaying)
                PausePlayback();
            break;
        case ID_BTN_STOP:
            StopPlayback();
            break;
        case ID_BTN_PREV:
            PlayPrev();
            break;
        case ID_BTN_NEXT:
            PlayNext();
            break;
        }
        return 0;

    case WM_NOTIFY:
    {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->idFrom == ID_LISTVIEW_FILES && pnmh->code == NM_DBLCLK)
        {
            PlaySelectedFile();
        }
        return 0;
    }

    case WM_HSCROLL:
        if ((HWND)lParam == g_hSliderVolume)
        {
            UINT8 vol = (UINT8)SendMessage(g_hSliderVolume, TBM_GETPOS, 0, 0);
            SetVolume(vol);
        }
        else if ((HWND)lParam == g_hSliderPosition && !g_isPlaying)
        {
            UINT32 pos = (UINT32)SendMessage(g_hSliderPosition, TBM_GETPOS, 0, 0);
            SeekToPosition(pos);
        }
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Visualization window procedure
LRESULT CALLBACK VizWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        CreateVizBuffers(hwnd);
        return 0;

    case WM_DESTROY:
        CleanupVizBuffers();
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_hdcMem && g_hbmMem)
        {
            // Blit from memory DC to window
            RECT rc;
            GetClientRect(hwnd, &rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_hdcMem, 0, 0, SRCCOPY);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Create GUI controls
void CreateControls(HWND hwnd)
{
    // Path address bar (read-only edit control)
    g_hEditPath = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        NULL,
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_READONLY | ES_AUTOHSCROLL,
        10, 10, 280, 24,
        hwnd, (HMENU)ID_EDIT_PATH, NULL, NULL
    );

    // File list (left panel)
    g_hListView = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEW,
        NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        10, 40, 300, 420,
        hwnd, (HMENU)ID_LISTVIEW_FILES, NULL, NULL
    );

    // Set ListView colors to dark theme
    ListView_SetBkColor(g_hListView, COLOR_BG_SIDEBAR);
    ListView_SetTextBkColor(g_hListView, COLOR_BG_SIDEBAR);
    ListView_SetTextColor(g_hListView, COLOR_TEXT_LIGHT);

    LVCOLUMN lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 280;
    lvc.pszText = (LPWSTR)L"Filename";
    ListView_InsertColumn(g_hListView, 0, &lvc);

    // Visualization area (right panel)
    g_hStaticViz = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        L"VGMPlayerViz",
        NULL,
        WS_CHILD | WS_VISIBLE,
        320, 10, 660, 450,
        hwnd, (HMENU)ID_STATIC_VIZ, NULL, NULL
    );

    // Open folder button
    g_hBtnOpenFolder = CreateWindow(
        L"BUTTON", L"Open Folder",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 470, 100, 30,
        hwnd, (HMENU)ID_BTN_OPEN_FOLDER, NULL, NULL
    );

    // Playback control buttons
    g_hBtnPrev = CreateWindow(
        L"BUTTON", L"<< Prev",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        320, 470, 80, 30,
        hwnd, (HMENU)ID_BTN_PREV, NULL, NULL
    );

    g_hBtnPlay = CreateWindow(
        L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        410, 470, 80, 30,
        hwnd, (HMENU)ID_BTN_PLAY, NULL, NULL
    );

    g_hBtnPause = CreateWindow(
        L"BUTTON", L"Pause",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        500, 470, 80, 30,
        hwnd, (HMENU)ID_BTN_PAUSE, NULL, NULL
    );

    g_hBtnStop = CreateWindow(
        L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        590, 470, 80, 30,
        hwnd, (HMENU)ID_BTN_STOP, NULL, NULL
    );

    g_hBtnNext = CreateWindow(
        L"BUTTON", L"Next >>",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        680, 470, 80, 30,
        hwnd, (HMENU)ID_BTN_NEXT, NULL, NULL
    );

    // Position slider
    g_hSliderPosition = CreateWindow(
        TRACKBAR_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        320, 510, 660, 30,
        hwnd, (HMENU)ID_SLIDER_POSITION, NULL, NULL
    );
    SendMessage(g_hSliderPosition, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
    SendMessage(g_hSliderPosition, TBM_SETPOS, TRUE, 0);

    // Volume slider
    g_hSliderVolume = CreateWindow(
        TRACKBAR_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        320, 550, 200, 30,
        hwnd, (HMENU)ID_SLIDER_VOLUME, NULL, NULL
    );
    SendMessage(g_hSliderVolume, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessage(g_hSliderVolume, TBM_SETPOS, TRUE, 75);
}

// Create visualization buffers
void CreateVizBuffers(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    RECT rc;
    GetClientRect(hwnd, &rc);

    g_hdcMem = CreateCompatibleDC(hdc);
    g_hbmMem = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    g_hbmOld = (HBITMAP)SelectObject(g_hdcMem, g_hbmMem);

    ReleaseDC(hwnd, hdc);
}

// Cleanup visualization buffers
void CleanupVizBuffers()
{
    if (g_hdcMem)
    {
        if (g_hbmOld) SelectObject(g_hdcMem, g_hbmOld);
        if (g_hbmMem) DeleteObject(g_hbmMem);
        DeleteDC(g_hdcMem);
        g_hdcMem = NULL;
        g_hbmMem = NULL;
        g_hbmOld = NULL;
    }
}

// Render visualization
void RenderVisualization()
{
    if (!g_hdcMem || !g_hStaticViz) return;

    RECT rc;
    GetClientRect(g_hStaticViz, &rc);

    // Clear background
    HBRUSH hBrushBlack = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(g_hdcMem, &rc, hBrushBlack);

    // Draw VU meters (top half)
    int vuHeight = rc.bottom / 2 - 40;
    DrawVUMeters(g_hdcMem, 10, 10, rc.right - 20, vuHeight);

    // Draw waveform (bottom half)
    int waveY = rc.bottom / 2 + 10;
    int waveHeight = rc.bottom - waveY - 40;
    DrawWaveform(g_hdcMem, 10, waveY, rc.right - 20, waveHeight);

    // Draw current track info
    if (g_player && g_currentTrack >= 0)
    {
        wchar_t infoText[256];
        const wchar_t* filename = g_playlist[g_currentTrack].c_str();
        const wchar_t* filenameOnly = wcsrchr(filename, L'\\');
        if (filenameOnly) filenameOnly++;
        else filenameOnly = filename;

        double curTime = g_player->GetCurTime(PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE);
        double totalTime = g_player->GetTotalTime(PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE);
        UINT32 curSec = (UINT32)curTime;
        UINT32 totalSec = (UINT32)totalTime;

        swprintf(infoText, 256, L"%s | %02u:%02u / %02u:%02u",
            filenameOnly,
            curSec / 60, curSec % 60,
            totalSec / 60, totalSec % 60
        );

        SetTextColor(g_hdcMem, RGB(255, 255, 255));
        SetBkMode(g_hdcMem, TRANSPARENT);
        RECT textRect = {10, rc.bottom - 30, rc.right - 10, rc.bottom - 10};
        DrawText(g_hdcMem, infoText, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Invalidate to trigger repaint
    InvalidateRect(g_hStaticViz, NULL, FALSE);
}

// Draw VU meters
void DrawVUMeters(HDC hdc, int x, int y, int width, int height)
{
    int barWidth = width / VU_METER_COUNT;

    for (int i = 0; i < VU_METER_COUNT; i++)
    {
        float level = g_vuLevels[i];
        int barHeight = (int)(level * height);

        // Choose color based on level
        COLORREF color;
        if (level > 0.8f)
            color = RGB(255, 0, 0); // Red
        else if (level > 0.6f)
            color = RGB(255, 255, 0); // Yellow
        else
            color = RGB(0, 200, 0); // Green

        HBRUSH hBrush = CreateSolidBrush(color);
        RECT barRect = {
            x + i * barWidth,
            y + height - barHeight,
            x + (i + 1) * barWidth - 2,
            y + height
        };
        FillRect(hdc, &barRect, hBrush);
        DeleteObject(hBrush);
    }
}

// Draw waveform
void DrawWaveform(HDC hdc, int x, int y, int width, int height)
{
    HPEN hPenBlue = CreatePen(PS_SOLID, 2, RGB(50, 100, 200));
    HPEN hPenOld = (HPEN)SelectObject(hdc, hPenBlue);

    int midY = y + height / 2;

    // Draw center line
    HPEN hPenGray = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
    SelectObject(hdc, hPenGray);
    MoveToEx(hdc, x, midY, NULL);
    LineTo(hdc, x + width, midY);
    DeleteObject(hPenGray);

    // Draw waveform
    SelectObject(hdc, hPenBlue);
    for (size_t i = 0; i < g_waveformData.size() - 1; i++)
    {
        int x1 = x + (int)(i * width / g_waveformData.size());
        int x2 = x + (int)((i + 1) * width / g_waveformData.size());
        int y1 = midY + (int)(g_waveformData[i] * (height / 2));
        int y2 = midY + (int)(g_waveformData[i + 1] * (height / 2));

        MoveToEx(hdc, x1, y1, NULL);
        LineTo(hdc, x2, y2);
    }

    SelectObject(hdc, hPenOld);
    DeleteObject(hPenBlue);
}

// Get executable directory path
void GetExePath(std::wstring& exePath)
{
    wchar_t path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);

    // Remove filename to get directory
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash)
    {
        *lastSlash = L'\0';
    }

    exePath = path;
}

// Open folder dialog
void OpenFolderDialog()
{
    wchar_t folderPath[MAX_PATH] = {0};

    BROWSEINFO bi = {};
    bi.hwndOwner = g_hWnd;
    bi.pszDisplayName = folderPath;
    bi.lpszTitle = L"Select VGM folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (pidl)
    {
        SHGetPathFromIDList(pidl, folderPath);
        CoTaskMemFree(pidl);

        g_currentFolder = folderPath;
        PopulateFileList(g_currentFolder);
    }
}

// Populate file list
void PopulateFileList(const std::wstring& folder)
{
    ListView_DeleteAllItems(g_hListView);
    g_playlist.clear();

    // Update path address bar
    g_currentFolder = folder;
    SetWindowText(g_hEditPath, folder.c_str());

    std::wstring searchPath = folder + L"\\*.*";
    WIN32_FIND_DATA findData;
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE)
    {
        int itemIndex = 0;
        do
        {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::wstring filename = findData.cFileName;
                std::wstring ext = filename.substr(filename.find_last_of(L".") + 1);

                if (_wcsicmp(ext.c_str(), L"vgm") == 0 ||
                    _wcsicmp(ext.c_str(), L"vgz") == 0 ||
                    _wcsicmp(ext.c_str(), L"s98") == 0 ||
                    _wcsicmp(ext.c_str(), L"dro") == 0 ||
                    _wcsicmp(ext.c_str(), L"gym") == 0)
                {
                    std::wstring fullPath = folder + L"\\" + filename;
                    g_playlist.push_back(fullPath);

                    LVITEM lvi = {};
                    lvi.mask = LVIF_TEXT;
                    lvi.iItem = itemIndex;
                    lvi.iSubItem = 0;
                    lvi.pszText = (LPWSTR)filename.c_str();
                    ListView_InsertItem(g_hListView, &lvi);

                    itemIndex++;
                }
            }
        } while (FindNextFile(hFind, &findData));

        FindClose(hFind);
    }
}

// Play selected file
void PlaySelectedFile()
{
    int selectedIndex = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
    if (selectedIndex >= 0 && selectedIndex < (int)g_playlist.size())
    {
        g_currentTrack = selectedIndex;
        PlayFile(g_playlist[g_currentTrack]);
    }
}

// Play file
void PlayFile(const std::wstring& filePath)
{
    StopPlayback();

    int len = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, NULL, 0, NULL, NULL);
    char* filePathUtf8 = new char[len];
    WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, filePathUtf8, len, NULL, NULL);

    DATA_LOADER* dLoad = FileLoader_Init(filePathUtf8);
    delete[] filePathUtf8;

    if (dLoad == NULL) return;

    UINT8 retVal = DataLoader_Load(dLoad);
    if (retVal)
    {
        DataLoader_Deinit(dLoad);
        return;
    }

    g_player = new PlayerA();
    g_player->RegisterPlayerEngine(new VGMPlayer());
    g_player->RegisterPlayerEngine(new S98Player());
    g_player->RegisterPlayerEngine(new DROPlayer());
    g_player->RegisterPlayerEngine(new GYMPlayer());

    retVal = g_player->LoadFile(dLoad);
    DataLoader_Deinit(dLoad);

    if (retVal)
    {
        delete g_player;
        g_player = NULL;
        return;
    }

    g_player->SetSampleRate(44100);
    g_player->SetOutputSettings(44100, 2, 16, 0);
    g_player->SetMasterVolume(g_masterVol);
    g_player->SetLoopCount(2);
    g_player->Start();

    if (!g_audDrv)
    {
        UINT8 result = Audio_Init();
        if (result) return;

        UINT32 drvCount = Audio_GetDriverCount();
        UINT32 dsoundID = (UINT32)-1;
        for (UINT32 i = 0; i < drvCount; i++)
        {
            AUDDRV_INFO* drvInfo;
            Audio_GetDriverInfo(i, &drvInfo);
            if (drvInfo->drvSig == ADRVSIG_DSOUND)
            {
                dsoundID = i;
                break;
            }
        }

        if (dsoundID == (UINT32)-1) return;

        result = AudioDrv_Init(dsoundID, &g_audDrv);
        if (result) return;

        AUDIO_OPTS* opts = AudioDrv_GetOptions(g_audDrv);
        opts->sampleRate = 44100;
        opts->numChannels = 2;
        opts->numBitsPerSmpl = 16;
        opts->usecPerBuf = 10000;
        opts->numBuffers = 10;

        result = AudioDrv_Start(g_audDrv, 0);
        if (result)
        {
            AudioDrv_Deinit(&g_audDrv);
            return;
        }

        AudioDrv_SetCallback(g_audDrv, FillBuffer, NULL);
    }

    g_isPlaying = true;
    g_isPaused = false;
    EnableWindow(g_hBtnPlay, TRUE);
    EnableWindow(g_hBtnPause, TRUE);
    EnableWindow(g_hBtnStop, TRUE);
}

// Stop playback
void StopPlayback()
{
    if (g_audDrv)
    {
        AudioDrv_Stop(g_audDrv);
        AudioDrv_Deinit(&g_audDrv);
        g_audDrv = NULL;
    }

    if (g_player)
    {
        g_player->Stop();
        delete g_player;
        g_player = NULL;
    }

    g_isPlaying = false;
    g_isPaused = false;

    memset(g_vuLevels, 0, sizeof(g_vuLevels));
    for (size_t i = 0; i < g_waveformData.size(); i++)
        g_waveformData[i] = 0.0f;

    EnableWindow(g_hBtnPause, FALSE);
    EnableWindow(g_hBtnStop, FALSE);
}

void PausePlayback()
{
    if (g_audDrv && g_isPlaying && !g_isPaused)
    {
        AudioDrv_Pause(g_audDrv);
        g_isPaused = true;
    }
}

void ResumePlayback()
{
    if (g_audDrv && g_isPlaying && g_isPaused)
    {
        AudioDrv_Resume(g_audDrv);
        g_isPaused = false;
    }
}

void PlayNext()
{
    if (g_playlist.empty()) return;

    g_currentTrack++;
    if (g_currentTrack >= (int)g_playlist.size())
        g_currentTrack = 0;

    ListView_SetItemState(g_hListView, g_currentTrack, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hListView, g_currentTrack, FALSE);

    PlayFile(g_playlist[g_currentTrack]);
}

void PlayPrev()
{
    if (g_playlist.empty()) return;

    g_currentTrack--;
    if (g_currentTrack < 0)
        g_currentTrack = (int)g_playlist.size() - 1;

    ListView_SetItemState(g_hListView, g_currentTrack, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hListView, g_currentTrack, FALSE);

    PlayFile(g_playlist[g_currentTrack]);
}

void UpdatePositionSlider()
{
    if (!g_player) return;

    UINT32 curSample = g_player->GetCurPos(PLAYPOS_SAMPLE);
    double totalTime = g_player->GetTotalTime(PLAYTIME_LOOP_INCL);
    UINT32 totalSamples = (UINT32)(totalTime * 44100);

    if (totalSamples > 0)
    {
        UINT32 pos = (curSample * 1000) / totalSamples;
        SendMessage(g_hSliderPosition, TBM_SETPOS, TRUE, pos);
    }
}

void SeekToPosition(UINT32 samplePos)
{
    if (!g_player) return;
    g_player->Seek(PLAYPOS_SAMPLE, samplePos);
}

void SetVolume(UINT8 volumePercent)
{
    g_masterVol = (volumePercent * 0x10000) / 100;
    if (g_player)
        g_player->SetMasterVolume(g_masterVol);
}

UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data)
{
    if (!g_player) return 0;

    UINT32 renderedBytes = g_player->Render(bufSize, data);
    INT16* buffer = (INT16*)data;
    UINT32 sampleCount = renderedBytes / (2 * sizeof(INT16));

    for (UINT32 i = 0; i < sampleCount && i < 256; i++)
    {
        float leftSample = buffer[i * 2] / 32768.0f;
        float rightSample = buffer[i * 2 + 1] / 32768.0f;
        float avgSample = (leftSample + rightSample) / 2.0f;

        g_waveformData[g_waveformPos] = avgSample;
        g_waveformPos = (g_waveformPos + 1) % g_waveformData.size();
    }

    for (int band = 0; band < VU_METER_COUNT; band++)
    {
        float peak = 0.0f;
        UINT32 startSample = (band * sampleCount) / VU_METER_COUNT;
        UINT32 endSample = ((band + 1) * sampleCount) / VU_METER_COUNT;

        for (UINT32 i = startSample; i < endSample; i++)
        {
            float left = abs(buffer[i * 2]) / 32768.0f;
            float right = abs(buffer[i * 2 + 1]) / 32768.0f;
            float max = (left > right) ? left : right;
            if (max > peak) peak = max;
        }

        g_vuLevels[band] = g_vuLevels[band] * 0.8f + peak * 0.2f;
    }

    return renderedBytes;
}
