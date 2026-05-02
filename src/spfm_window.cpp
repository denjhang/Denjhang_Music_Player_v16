// spfm_window.cpp - SPFM Hardware Management Window

#include "spfm_window.h"
#include "spfm_manager.h"
#include "chip_control.h"
#include "sn76489_window.h"
#include "imgui/imgui.h"

namespace SPFMWindow {

static bool s_lastChipType = SPFMManager::CHIP_NONE;

void Init() {
    s_lastChipType = SPFMManager::GetActiveChipType();
}

void Shutdown() {}

void Update() {
    // Detect chip type changes and notify modules
    SPFMManager::ChipType currentType = SPFMManager::GetActiveChipType();
    if (currentType != s_lastChipType) {
        s_lastChipType = currentType;

        if (currentType == SPFMManager::CHIP_YM2163) {
            YM2163::g_hardwareConnected = SPFMManager::IsConnected();
            YM2163::ym2163_init();
        }
    }
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin("SPFM");

    if (!visible) { ImGui::End(); return; }

    // Device info
    ImGui::Text("FTDI Device");
    ImGui::Separator();

    if (SPFMManager::IsConnected()) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", SPFMManager::g_device.description);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No device");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Card Slots");
    ImGui::Separator();

    // Slot list
    for (int i = 0; i < SPFMManager::MAX_SLOTS; i++) {
        SPFMManager::ChipType ct = SPFMManager::GetSlotType(i);
        const char* typeName = (ct == SPFMManager::CHIP_YM2163) ? "YM2163"
                             : (ct == SPFMManager::CHIP_SN76489) ? "SN76489"
                             : "---";
        bool enabled = SPFMManager::g_device.slots[i].enabled;

        ImGui::PushID(i);
        ImGui::Text("Slot %d:", i);
        ImGui::SameLine(60);
        if (enabled) {
            ImGui::Text("%s", typeName);
        } else {
            ImGui::TextDisabled("%s", typeName);
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Chip type selection (Phase 1: all slots same type)
    ImGui::Text("Chip Type");
    ImGui::Spacing();

    SPFMManager::ChipType activeType = SPFMManager::GetActiveChipType();
    bool isYM = (activeType == SPFMManager::CHIP_YM2163);
    bool isSN = (activeType == SPFMManager::CHIP_SN76489);

    if (ImGui::RadioButton("YM2163", isYM)) {
        SPFMManager::SetAllSlots(SPFMManager::CHIP_YM2163);
        // Notify YM2163 module
        YM2163::g_hardwareConnected = SPFMManager::IsConnected();
        YM2163::g_manualDisconnect = false;
        if (SPFMManager::IsConnected()) {
            SPFMManager::SendReset();
            Sleep(200);
            YM2163::ym2163_init();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("All 4 slots: YM2163 (FM synthesis)");

    ImGui::SameLine();
    if (ImGui::RadioButton("SN76489", isSN)) {
        SPFMManager::SetAllSlots(SPFMManager::CHIP_SN76489);
        // SN76489 module will pick up via SyncConnectionState in Update()
        if (SPFMManager::IsConnected()) {
            SPFMManager::SendReset();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("All 4 slots: SN76489 (DCSG/PSG)");

    ImGui::Spacing();

    // Reset button
    if (SPFMManager::IsConnected()) {
        if (ImGui::Button("Reset##spfmreset", ImVec2(-1, 0))) {
            SPFMManager::SendReset();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Send SPFM reset command (0xFE)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Phase 1: Single device, unified chip type");
    ImGui::TextDisabled("Future: Multi-device, mixed chip types per slot");

    ImGui::End();
}

} // namespace SPFMWindow
