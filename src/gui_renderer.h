// gui_renderer.h - ImGui UI Rendering Module
// Handles all ImGui render functions, keyboard input, and DX11 device management

#ifndef GUI_RENDERER_H
#define GUI_RENDERER_H

#include <windows.h>
#include <d3d11.h>
#include <string>

// ===== DX11 Device Globals =====
extern ID3D11Device*            g_pd3dDevice;
extern ID3D11DeviceContext*     g_pd3dDeviceContext;
extern IDXGISwapChain*          g_pSwapChain;
extern bool                     g_SwapChainOccluded;
extern UINT                     g_ResizeWidth;
extern UINT                     g_ResizeHeight;
extern ID3D11RenderTargetView*  g_mainRenderTargetView;

// ===== UI State Globals =====
extern int  g_currentOctave;
extern bool g_keyStates[256];
extern bool g_autoScroll;
extern char g_logDisplayBuffer[32768];
extern size_t g_lastLogSize;
extern bool g_logScrollToBottom;
extern int  g_selectedInstrument;

// ===== Piano Key State =====
extern bool g_pianoKeyPressed[73];
extern int  g_pianoKeyVelocity[73];
extern bool g_pianoKeyFromKeyboard[73];
extern bool g_pianoKeyOnSlot3_2MHz[73];
extern int   g_pianoKeyChipIndex[73];  // Which chip is playing this key (0-3)
extern float g_pianoKeyLevel[73];      // Visual intensity for each key (0.0-1.0), driven by channel level

// ===== Name Tables (read-only, defined in gui_renderer.cpp) =====
extern const char* g_timbreNames[];
extern const char* g_envelopeNames[];
extern const char* g_volumeNames[];

// ===== DX11 Device Management =====
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();

// ===== Keyboard Input =====
void HandleKeyPress(int vk);
void HandleKeyRelease(int vk);

// ===== Window Procedure =====
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ===== ImGui Render Functions =====
void RenderMIDIPlayer();
void RenderPianoKeyboard();
void RenderLevelMeters();
void RenderChannelStatus();
void RenderControls();
void RenderLog();
void RenderTuningWindow();
void RenderMIDIFolderHistory();

// ===== Config UI Helpers =====
void SaveInstrumentConfig(int instrument);
void LoadInstrumentConfigToUI(int instrument);

#endif // GUI_RENDERER_H
