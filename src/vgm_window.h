// vgm_window.h - libvgm Simulation Playback Window
// Provides a dedicated ImGui window for VGM/VGZ file playback using libvgm.
// Reuses the file browser and folder history components from MidiPlayer.

#ifndef VGM_WINDOW_H
#define VGM_WINDOW_H

#include <string>
#include <windows.h>

namespace VgmWindow {

// ===== State =====
extern bool g_windowOpen;          // Whether the libvgm window is visible
extern bool g_initialized;         // Whether audio system is initialized

// ===== Scope mode state =====
bool GetScopeMode();               // Get current scope mode (true = scope, false = level meter)
float GetScopeBackgroundHeight();  // Get splitter bar position (oscilloscope background height)
void SetScopeBackgroundHeight(float height); // Set splitter bar position

// ===== Lifecycle =====
// Call once after ImGui context is created
void Init();
// Call once before ImGui context is destroyed
void Shutdown();

// ===== Per-frame =====
// Render as a floating popup window (no-op if !g_windowOpen)
void Render();
// Render inline inside an already-open ImGui window (for chip tab embedding)
void RenderInline();
// Sub-panel renderers for chip-tab layout
void RenderControls();       // Left pane: transport + info
void RenderPianoArea();      // Piano area placeholder
void RenderLevelMeterArea(); // Level meter placeholder
void RenderScopeArea();      // Oscilloscope area - multi-row display
void RenderStatusArea();     // Status area placeholder
void RenderFileBrowserPanel(); // Bottom-left: file browser
void RenderLogPanel();       // Bottom-right: log placeholder
void RenderChipAliasWindow(); // Floating chip alias settings window
void RenderScopeSettingsWindow(); // Floating scope settings window (non-modal)

// ===== File Loading =====
bool LoadFile(const char* path);
void UnloadFile();

// ===== Configuration =====
void SavePlayerState();  // Save player state and UI settings to config file

// ===== Dockable Tab Render =====
// Renders the full VGM tab content as a dockable ImGui window (title: "libvgm")
void RenderTab();

// ===== Playback Control (for tab-switch auto-pause/resume) =====
bool IsPlaying();   // Returns true if VGM is currently playing
void Pause();        // Pause playback (no-op if not playing)
void Resume();       // Resume playback (no-op if not paused)

} // namespace VgmWindow

#endif // VGM_WINDOW_H
