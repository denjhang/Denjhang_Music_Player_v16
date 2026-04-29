// ym2163_window.cpp - YM2163 Chip Window Module Implementation
// Handles rendering and updates for the YM2163 tab

#include "chip_window_ym2163.h"
#include "chip_control.h"
#include "gui_renderer.h"
#include "midi_player.h"
#include "config_manager.h"

#include "imgui/imgui.h"

namespace YM2163Window {

static bool s_wasVisible = false;
static bool s_midiAutoPaused = false;

// ===== Lifecycle =====

void Init() {
    YM2163::InitializeAllChannels();
    YM2163::ConnectHardware();
}

void Shutdown() {
    // Hardware disconnection handled in main cleanup
}

// ===== Per-Frame Update =====

void Update() {
    YM2163::CheckHardwareAutoConnect();
    YM2163::UpdateDrumStates();
    YM2163::CleanupStuckChannels();
    YM2163::UpdateChannelLevels();
    YM2163::UpdateDrumLevels();
}

// ===== Render =====

void Render() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin("YM2163(DSG)");

    // Visibility-based auto-pause/resume
    // visible==true means the tab/window is active; false means collapsed or inactive tab
    if (s_wasVisible && !visible) {
        // Became hidden: auto-pause MIDI
        if (MidiPlayer::g_midiPlayer.isPlaying && !MidiPlayer::g_midiPlayer.isPaused) {
            MidiPlayer::g_midiPlayer.isPaused = true;
            MidiPlayer::g_midiPlayer.pauseTime = std::chrono::steady_clock::now();
            YM2163::stop_all_notes();
            s_midiAutoPaused = true;
        }
    }
    if (!s_wasVisible && visible && s_midiAutoPaused) {
        // Became visible: auto-resume
        if (MidiPlayer::g_midiPlayer.isPlaying && MidiPlayer::g_midiPlayer.isPaused && !g_midiUserPaused) {
            MidiPlayer::PlayMIDI();
        }
        s_midiAutoPaused = false;
    }
    s_wasVisible = visible;

    if (!visible) { ImGui::End(); return; }

    // Left pane - controls
    ImGui::BeginChild("LeftPane", ImVec2(300, 0), true);
    RenderControls();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane - main content
    ImGui::BeginChild("RightPane", ImVec2(0, 0), false);

    float pianoHeight      = 150;
    float levelMeterHeight = 200;
    float statusAreaWidth  = 560;
    float topSectionHeight = pianoHeight + levelMeterHeight;

    // Top section: Piano + Level Meters + Status
    ImGui::BeginGroup();
    ImGui::BeginChild("PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderLevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderChannelStatus();
    ImGui::EndChild();

    // Bottom section: MIDI Player + Log
    ImGui::BeginChild("BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderMIDIPlayer();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("BottomRight", ImVec2(0, 0), true);
    RenderLog();
    RenderMIDIFolderHistory();
    ImGui::EndChild();

    ImGui::EndChild();  // RightPane
    ImGui::End();

    // Tuning window (popup)
    RenderTuningWindow();
}

bool WantsKeyboardCapture() {
    return ImGui::IsAnyItemActive();
}

}  // namespace YM2163Window
