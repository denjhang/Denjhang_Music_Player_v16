// spfm_window.cpp - SPFM Hardware Management Window

#include "spfm_window.h"
#include "spfm_manager.h"
#include "windows/ym2163/chip_control.h"
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
    ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
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

    // Slot table
    SPFMManager::ChipType activeType = SPFMManager::GetActiveChipType();
    if (ImGui::BeginTable("##spfmslots", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Chip Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < SPFMManager::MAX_SLOTS; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Slot %d", i);

            ImGui::TableSetColumnIndex(1);
            int current = static_cast<int>(SPFMManager::GetSlotType(i));
            ImGui::PushID(i);
            const char* items[] = { "YM2163", "SN76489", "YM2413", "AY8910", "Disabled" };
            if (ImGui::Combo("##chiptype", &current, items, 5)) {
                SPFMManager::ChipType newType;
                switch (current) {
                    case 0: newType = SPFMManager::CHIP_YM2163; break;
                    case 1: newType = SPFMManager::CHIP_SN76489; break;
                    case 2: newType = SPFMManager::CHIP_YM2413; break;
                    case 3: newType = SPFMManager::CHIP_AY8910; break;
                    default: newType = SPFMManager::CHIP_NONE; break;
                }
                if (newType != activeType) {
                    SwitchToChipType(newType);
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(2);
            bool enabled = SPFMManager::g_device.slots[i].enabled;
            if (enabled) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Active");
            } else {
                ImGui::TextDisabled("---");
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();

    // Connect / Disconnect buttons
    {
        float w = (ImGui::GetContentRegionAvail().x - 15.0f) / 4.0f;
        if (ImGui::Button("YM2163##spfm", ImVec2(w, 0))) {
            SwitchToChipType(SPFMManager::CHIP_YM2163);
        }
        ImGui::SameLine();
        if (ImGui::Button("SN76489##spfm", ImVec2(w, 0))) {
            SwitchToChipType(SPFMManager::CHIP_SN76489);
        }
        ImGui::SameLine();
        if (ImGui::Button("YM2413##spfm", ImVec2(w, 0))) {
            SwitchToChipType(SPFMManager::CHIP_YM2413);
        }
        ImGui::SameLine();
        if (ImGui::Button("AY8910##spfm", ImVec2(w, 0))) {
            SwitchToChipType(SPFMManager::CHIP_AY8910);
        }
    }
    {
        if (ImGui::Button("Disconnect##spfm", ImVec2(-1, 0))) {
            SwitchToChipType(SPFMManager::CHIP_NONE);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set chip type to None, send SPFM reset");
    }

    ImGui::Spacing();

    // Reset button
    if (SPFMManager::IsConnected()) {
        if (ImGui::Button("Reset##spfmreset", ImVec2(-1, 0))) {
            SPFMManager::SendReset();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Send SPFM reset command (0xFE)");
    }

    ImGui::End();
}

} // namespace SPFMWindow
