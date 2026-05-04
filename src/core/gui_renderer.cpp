// gui_renderer.cpp - ImGui UI Rendering Module Implementation

#include "gui_renderer.h"
#include "windows/ym2163/chip_control.h"
#include "config_manager.h"
#include "midi/midi_player.h"
#include "windows/sn76489/sn76489_window.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ===== DX11 Global Definitions =====

ID3D11Device*            g_pd3dDevice           = nullptr;
ID3D11DeviceContext*     g_pd3dDeviceContext     = nullptr;
IDXGISwapChain*          g_pSwapChain            = nullptr;
bool                     g_SwapChainOccluded     = false;
UINT                     g_ResizeWidth           = 0;
UINT                     g_ResizeHeight          = 0;
ID3D11RenderTargetView*  g_mainRenderTargetView  = nullptr;

// ===== UI State Global Definitions =====

int  g_currentOctave        = 2;
bool g_keyStates[256]       = {0};
bool g_autoScroll           = true;
char g_logDisplayBuffer[32768] = {0};
size_t g_lastLogSize        = 0;
bool g_logScrollToBottom    = false;
int  g_selectedInstrument   = 0;

// ===== Piano Key State =====

bool g_pianoKeyPressed[73]      = {false};
int  g_pianoKeyVelocity[73]     = {0};
bool g_pianoKeyFromKeyboard[73] = {false};
bool g_pianoKeyOnSlot3_2MHz[73] = {false};
int  g_pianoKeyChipIndex[73]    = {-1};  // Which chip is playing this key (-1 = none)
float g_pianoKeyLevel[73]       = {0.0f}; // Visual intensity (0.0-1.0)

// ===== Name Tables =====

const char* g_timbreNames[]   = { "", "String", "Organ", "Clarinet", "Piano", "Harpsichord" };
const char* g_envelopeNames[] = { "Decay", "Fast", "Medium", "Slow" };
const char* g_volumeNames[]   = { "0dB", "-6dB", "-12dB", "Mute" };

// ===== Keyboard Mapping =====

typedef struct { int vk; int note; int octave_offset; } KeyMapping;

static KeyMapping g_keyMappings[] = {
    {'Z', 0, 0}, {'X', 2, 0}, {'C', 4, 0}, {'V', 5, 0}, {'B', 7, 0}, {'N', 9, 0}, {'M', 11, 0},
    {VK_OEM_COMMA, 0, 1}, {VK_OEM_PERIOD, 2, 1}, {VK_OEM_2, 4, 1},
    {'S', 1, 0}, {'D', 3, 0}, {'G', 6, 0}, {'H', 8, 0}, {'J', 10, 0}, {'K', 1, 1}, {'L', 3, 1},
    {'Q', 0, 1}, {'W', 2, 1}, {'E', 4, 1}, {'R', 5, 1}, {'T', 7, 1}, {'Y', 9, 1}, {'U', 11, 1},
    {'I', 0, 2}, {'O', 2, 2}, {'P', 4, 2}, {VK_OEM_4, 5, 2}, {VK_OEM_6, 7, 2},
    {'2', 1, 1}, {'3', 3, 1}, {'5', 6, 1}, {'6', 8, 1}, {'7', 10, 1},
    {'8', 1, 2}, {'9', 3, 2}, {'0', 6, 2},
};
static const int g_numKeyMappings = sizeof(g_keyMappings) / sizeof(KeyMapping);

// ===== Config UI Helpers =====

void SaveInstrumentConfig(int instrument) {
    if (instrument < 0 || instrument > 127) return;
    char section[32];
    sprintf(section, "Instrument_%d", instrument);
    const char* envelopeStr = g_envelopeNames[YM2163::g_currentEnvelope];
    const char* waveStr     = g_timbreNames[YM2163::g_currentTimbre];
    WritePrivateProfileStringA(section, "Envelope", envelopeStr, Config::g_midiConfigPath);
    WritePrivateProfileStringA(section, "Wave",     waveStr,     Config::g_midiConfigPath);
    if (Config::g_instrumentConfigs.count(instrument) > 0) {
        Config::g_instrumentConfigs[instrument].envelope = YM2163::g_currentEnvelope;
        Config::g_instrumentConfigs[instrument].wave     = YM2163::g_currentTimbre;
    }
    YM2163::log_command("Saved Instrument %d: %s, %s", instrument, waveStr, envelopeStr);
}

void LoadInstrumentConfigToUI(int instrument) {
    if (instrument < 0 || instrument > 127) return;
    if (Config::g_instrumentConfigs.count(instrument) > 0) {
        Config::InstrumentConfig& config = Config::g_instrumentConfigs[instrument];
        YM2163::g_currentEnvelope = config.envelope;
        YM2163::g_currentTimbre   = config.wave;
        YM2163::log_command("Loaded Instrument %d (%s): %s, %s",
            instrument, config.name.c_str(),
            g_timbreNames[YM2163::g_currentTimbre],
            g_envelopeNames[YM2163::g_currentEnvelope]);
    }
}

// ===== Keyboard Input =====

void HandleKeyPress(int vk) {
    if (g_keyStates[vk]) return;
    g_keyStates[vk] = true;

    if (MidiPlayer::g_isInputActive) return;

    if (vk == VK_PRIOR && g_currentOctave < 5) {
        YM2163::stop_all_notes(); g_currentOctave++; return;
    } else if (vk == VK_NEXT && g_currentOctave > 0) {
        YM2163::stop_all_notes(); g_currentOctave--; return;
    }
    if (vk == VK_UP && YM2163::g_currentVolume > 0)   { YM2163::g_currentVolume--; return; }
    if (vk == VK_DOWN && YM2163::g_currentVolume < 3) { YM2163::g_currentVolume++; return; }
    if (vk >= VK_F1 && vk <= VK_F4) { YM2163::g_currentEnvelope = vk - VK_F1; return; }
    if (vk >= VK_F5 && vk <= VK_F9) { YM2163::g_currentTimbre   = vk - VK_F5 + 1; return; }
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD5) {
        static const uint8_t drumBits[5] = {0x10, 0x08, 0x04, 0x02, 0x01};
        YM2163::play_drum(drumBits[vk - VK_NUMPAD1]); return;
    }

    for (int i = 0; i < g_numKeyMappings; i++) {
        if (g_keyMappings[i].vk == vk) {
            int note   = g_keyMappings[i].note;
            int octave = g_currentOctave + g_keyMappings[i].octave_offset;
            bool valid = (octave == 0 && note == 11) || (octave >= 1 && octave <= 5);
            if (valid) {
                int channel = YM2163::find_free_channel();
                if (channel >= 0) {
                    YM2163::play_note(channel, note, octave);
                    int keyIdx = YM2163::get_key_index(octave, note);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx]     = true;
                        g_pianoKeyFromKeyboard[keyIdx] = true;
                        g_pianoKeyVelocity[keyIdx]    = 96;
                    }
                }
            }
            break;
        }
    }
}

void HandleKeyRelease(int vk) {
    g_keyStates[vk] = false;
    for (int i = 0; i < g_numKeyMappings; i++) {
        if (g_keyMappings[i].vk == vk) {
            int note   = g_keyMappings[i].note;
            int octave = g_currentOctave + g_keyMappings[i].octave_offset;
            bool valid = (octave == 0 && note == 11) || (octave >= 1 && octave <= 5);
            if (valid) {
                int channel = YM2163::find_channel_playing(note, octave);
                if (channel >= 0) {
                    YM2163::stop_note(channel);
                    int keyIdx = YM2163::get_key_index(octave, note);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyFromKeyboard[keyIdx] = false;
                        g_pianoKeyVelocity[keyIdx]     = 0;
                        // Don't clear pianoKeyPressed/pianoKeyChipIndex here
                        // - let UpdateChannelLevels fade out via g_pianoKeyLevel
                    }
                }
            }
            break;
        }
    }
}

// ===== DX11 Device Management =====

bool CreateDeviceD3D(HWND hWnd) {
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
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain)           { g_pSwapChain->Release();           g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)    { g_pd3dDeviceContext->Release();    g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)           { g_pd3dDevice->Release();           g_pd3dDevice = nullptr; }
}

// ===== Window Procedure =====

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth  = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_ENTERSIZEMOVE:
            MidiPlayer::g_isWindowDragging = true;
            SetTimer(hWnd, TIMER_MIDI_UPDATE, 16, NULL);
            return 0;

        case WM_EXITSIZEMOVE:
            MidiPlayer::g_isWindowDragging = false;
            KillTimer(hWnd, TIMER_MIDI_UPDATE);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_MIDI_UPDATE && MidiPlayer::g_isWindowDragging) {
                MidiPlayer::UpdateMIDIPlayback();
                YM2163::UpdateDrumStates();
                YM2163::CleanupStuckChannels();
            }
            return 0;

        case WM_HOTKEY:
            if (MidiPlayer::g_enableGlobalMediaKeys) {
                switch (wParam) {
                    case HK_PLAY_PAUSE:
                        if (MidiPlayer::g_midiPlayer.isPlaying) {
                            if (MidiPlayer::g_midiPlayer.isPaused) MidiPlayer::PlayMIDI();
                            else MidiPlayer::PauseMIDI();
                        } else {
                            MidiPlayer::PlayMIDI();
                        }
                        break;
                    case HK_NEXT_TRACK: MidiPlayer::PlayNextMIDI();     break;
                    case HK_PREV_TRACK: MidiPlayer::PlayPreviousMIDI(); break;
                }
            }
            return 0;

        case WM_KEYDOWN:
            HandleKeyPress((int)wParam);
            return 0;

        case WM_KEYUP:
            HandleKeyRelease((int)wParam);
            return 0;

        case WM_DESTROY:
            Config::SaveFrequenciesToINI();
            Config::SaveSlotConfigToINI();
            if (MidiPlayer::g_enableGlobalMediaKeys)
                MidiPlayer::UnregisterGlobalMediaKeys();
            SN76489Window::Shutdown();
            YM2163::DisconnectHardware();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===== Render Function Implementations =====
// NOTE: The full ImGui render logic for RenderMIDIPlayer, RenderPianoKeyboard,
// RenderLevelMeters, RenderChannelStatus, RenderControls, RenderLog, and
// RenderTuningWindow is large (~1700 lines). These implementations are extracted
// from ym2163_piano_gui_v11.cpp lines 2772-4460.
// Include the render implementations from the detail file:
#include "gui_renderer_impl.cpp"


