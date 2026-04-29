// opl3_window.h - OPL3 (YMF262) Chip Window Module
// Handles rendering and updates for the OPL3 tab

#pragma once

namespace OPL3Window {

// ===== ChipWindow Interface Implementation =====
void Init();
void Shutdown();
void Update();
void Render();  // Manages own ImGui::Begin/End, title: "YMF262(OPL3)"
bool WantsKeyboardCapture();

}  // namespace OPL3Window
