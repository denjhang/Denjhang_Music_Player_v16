// opl3_window.cpp - OPL3 (YMF262) Chip Window Module Implementation
// Handles rendering and updates for the OPL3 tab

#include "opl3_window.h"
#include "opl3_renderer.h"
#include "gui_renderer.h"
#include "midi_player.h"

#include "imgui/imgui.h"

namespace OPL3Window {

// ===== Lifecycle =====

void Init() {
    // OPL3 initialization if needed
}

void Shutdown() {
    // OPL3 cleanup if needed
}

// ===== Per-Frame Update =====

void Update() {
    // OPL3-specific updates (currently none)
    // MIDI playback is handled by shared MidiPlayer::UpdateMIDIPlayback()
}

// ===== Render =====

void Render() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("YMF262(OPL3)")) { ImGui::End(); return; }

    // Left pane - controls
    ImGui::BeginChild("OPL3_LeftPane", ImVec2(300, 0), true);
    RenderOPL3Controls();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane - main content
    ImGui::BeginChild("OPL3_RightPane", ImVec2(0, 0), false);

    float pianoHeight      = 150;
    float levelMeterHeight = 200;
    float statusAreaWidth  = 560;
    float topSectionHeight = pianoHeight + levelMeterHeight;

    // Top section: Piano + Level Meters + Status
    ImGui::BeginGroup();
    ImGui::BeginChild("OPL3_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderOPL3PianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("OPL3_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderOPL3LevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("OPL3_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderOPL3ChannelStatus();
    ImGui::EndChild();

    // Bottom section: MIDI Player + Log
    ImGui::BeginChild("OPL3_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderOPL3MIDIPlayer();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("OPL3_BottomRight", ImVec2(0, 0), true);
    RenderOPL3Log();
    RenderMIDIFolderHistory();
    ImGui::EndChild();

    ImGui::EndChild();  // OPL3_RightPane
    ImGui::End();
}

bool WantsKeyboardCapture() {
    return ImGui::IsAnyItemActive();
}

}  // namespace OPL3Window
