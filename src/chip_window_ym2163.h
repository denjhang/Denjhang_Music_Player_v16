// ym2163_window.h - YM2163 Chip Window Module
// Handles rendering and updates for the YM2163 tab

#pragma once

namespace YM2163Window {

// ===== ChipWindow Interface Implementation =====
void Init();
void Shutdown();
void Update();
void Render();  // Manages own ImGui::Begin/End, title: "YM2163(DSG)"
bool WantsKeyboardCapture();

}  // namespace YM2163Window
