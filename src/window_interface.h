// window_interface.h - Chip Window Interface
// Defines the interface for chip-specific window modules
// Each window (YM2163, OPL3, Vgm) implements these functions
// Docking mode: each window manages its own ImGui::Begin/End

#pragma once

#include "imgui/imgui.h"

namespace ChipWindow {

// ===== Lifecycle =====
// Called once at application startup
void Init();

// Called once at application shutdown
void Shutdown();

// ===== Per-Frame =====
// Called every frame when this window is active (visible)
// Put hardware updates, playback updates here
void Update();

// Called every frame to render the window content
// Each window calls ImGui::Begin/End with its own title
void Render();

// ===== Window State =====
// Returns true if this window wants to capture keyboard input
// (prevents shortcuts from firing while typing)
bool WantsKeyboardCapture();

}  // namespace ChipWindow

// ===== Window IDs =====
// Used for tab switching
constexpr int CHIP_WINDOW_YM2163 = 0;
constexpr int CHIP_WINDOW_OPL3   = 1;
constexpr int CHIP_WINDOW_LIBVGM = 2;
constexpr int CHIP_WINDOW_GIGATRON = 3;
constexpr int CHIP_WINDOW_COUNT  = 4;
