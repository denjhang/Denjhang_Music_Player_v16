// ImGui Chip Visualization Windows Implementation

#include "imgui_chip_windows.h"
#include <cstdio>
#include <cstring>

// =============================================================================
// YM2612 Window Implementation
// =============================================================================

YM2612Window::YM2612Window(YM2612Visualizer* vis)
    : visualizer(vis)
    , zoom(1)
    , showRegisters(false)
    , showFMParams(false)
{
    pianoRoll.SetKeyRange(24, 96);  // C1 to C7
    pianoRoll.SetSize(ImVec2(0, 150));
}

void YM2612Window::Render() {
    if (!isVisible || !visualizer) return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("YM2612 (OPN2)", &isVisible);

    const YM2612Params& params = visualizer->GetParams();

    // Toolbar
    RenderToolbar();

    ImGui::Separator();

    // Piano Roll
    RenderPianoRoll();

    ImGui::Separator();

    // Channel Information Table
    RenderChannelTable();

    // Optional sections
    if (showFMParams) {
        ImGui::Separator();
        RenderFMParameters();
    }

    if (showRegisters) {
        ImGui::Separator();
        RenderRegisters();
    }

    // PCM channels (if XGM)
    if (params.pcm[0].active || params.pcm[1].active || params.pcm[2].active || params.pcm[3].active) {
        ImGui::Separator();
        RenderPCMChannels();
    }

    ImGui::End();
}

void YM2612Window::RenderToolbar() {
    if (ImGui::Button("1x")) zoom = 1;
    ImGui::SameLine();
    if (ImGui::Button("2x")) zoom = 2;
    ImGui::SameLine();

    ImGui::Checkbox("FM Params", &showFMParams);
    ImGui::SameLine();
    ImGui::Checkbox("Registers", &showRegisters);

    const YM2612Params& params = visualizer->GetParams();
    if (params.ch3SpecialMode) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "CH3 Special Mode");
    }
    if (params.dacEnabled) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0, 1, 1, 1), "DAC Enabled");
    }
}

void YM2612Window::RenderPianoRoll() {
    const YM2612Params& params = visualizer->GetParams();
    int channelCount = params.ch3SpecialMode ? 9 : 6;
    pianoRoll.Render(params.channels, channelCount, "YM2612 Piano Roll");
}

void YM2612Window::RenderChannelTable() {
    const YM2612Params& params = visualizer->GetParams();

    if (ImGui::BeginTable("YM2612 Channels", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("Note");
        ImGui::TableSetupColumn("Freq (Hz)");
        ImGui::TableSetupColumn("Volume");
        ImGui::TableSetupColumn("Pan");
        ImGui::TableSetupColumn("Algo");
        ImGui::TableSetupColumn("FB");
        ImGui::TableSetupColumn("Key");
        ImGui::TableHeadersRow();

        int channelCount = params.ch3SpecialMode ? 9 : 6;
        for (int ch = 0; ch < channelCount; ch++) {
            const ChannelParams& channel = params.channels[ch];

            ImGui::TableNextRow();

            // Channel number
            ImGui::TableNextColumn();
            if (ch < 6) {
                ImGui::Text("FM%d", ch + 1);
            } else {
                ImGui::Text("OP%d", ch - 5);  // CH3 operators
            }

            // Note
            ImGui::TableNextColumn();
            if (channel.note >= 0) {
                ImGui::Text("%s%d", GetNoteName(channel.note), GetNoteOctave(channel.note));
            } else {
                ImGui::TextDisabled("---");
            }

            // Frequency
            ImGui::TableNextColumn();
            if (channel.frequency > 0.0f) {
                ImGui::Text("%.1f", channel.frequency);
            } else {
                ImGui::TextDisabled("---");
            }

            // Volume (as progress bar)
            ImGui::TableNextColumn();
            float volumeFraction = channel.volume / 127.0f;
            ImGui::ProgressBar(volumeFraction, ImVec2(-1, 0));

            // Pan
            ImGui::TableNextColumn();
            const char* panStr[] = {"L", "C", "R", "LR"};
            ImGui::Text("%s", panStr[channel.pan & 0x03]);

            // Algorithm & Feedback (FM channels only)
            ImGui::TableNextColumn();
            if (ch < 6) {
                ImGui::Text("%d", params.fmChannels[ch].algorithm);
            } else {
                ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            if (ch < 6) {
                ImGui::Text("%d", params.fmChannels[ch].feedback);
            } else {
                ImGui::TextDisabled("-");
            }

            // Key On state
            ImGui::TableNextColumn();
            if (channel.keyOn) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "ON");
            } else {
                ImGui::TextDisabled("OFF");
            }
        }

        ImGui::EndTable();
    }
}

void YM2612Window::RenderFMParameters() {
    const YM2612Params& params = visualizer->GetParams();

    if (ImGui::CollapsingHeader("FM Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int ch = 0; ch < 6; ch++) {
            char label[32];
            snprintf(label, sizeof(label), "FM Channel %d", ch + 1);

            if (ImGui::TreeNode(label)) {
                const YM2612Params::FMChannel& fmCh = params.fmChannels[ch];

                ImGui::Text("Algorithm: %d, Feedback: %d", fmCh.algorithm, fmCh.feedback);
                ImGui::Text("F-Num: 0x%03X, Block: %d", fmCh.fnum, fmCh.block);
                ImGui::Text("AMS: %d, FMS: %d", fmCh.ams, fmCh.fms);

                ImGui::Separator();

                // Operators table
                if (ImGui::BeginTable("Operators", 5, ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn("OP");
                    ImGui::TableSetupColumn("TL");
                    ImGui::TableSetupColumn("MUL");
                    ImGui::TableSetupColumn("DT");
                    ImGui::TableSetupColumn("KS");
                    ImGui::TableHeadersRow();

                    for (int op = 0; op < 4; op++) {
                        const YM2612Params::FMChannel::Operator& oper = fmCh.operators[op];

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", op + 1);

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", oper.tl);

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", oper.mul);

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", oper.dt);

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", oper.ks);
                    }

                    ImGui::EndTable();
                }

                ImGui::TreePop();
            }
        }
    }
}

void YM2612Window::RenderRegisters() {
    if (ImGui::CollapsingHeader("Registers")) {
        ImGui::TextDisabled("Register view not yet implemented");
        // TODO: Implement hex dump of chip->REG[2][0x100]
    }
}

void YM2612Window::RenderPCMChannels() {
    const YM2612Params& params = visualizer->GetParams();

    if (ImGui::CollapsingHeader("PCM Channels (XGM)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("PCM", 4, ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Ch");
            ImGui::TableSetupColumn("Active");
            ImGui::TableSetupColumn("Sample");
            ImGui::TableSetupColumn("Volume");
            ImGui::TableHeadersRow();

            for (int i = 0; i < 4; i++) {
                const YM2612Params::PCMChannel& pcm = params.pcm[i];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("PCM%d", i + 1);

                ImGui::TableNextColumn();
                if (pcm.active) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "YES");
                } else {
                    ImGui::TextDisabled("NO");
                }

                ImGui::TableNextColumn();
                if (pcm.active) {
                    ImGui::Text("0x%02X", pcm.sample);
                } else {
                    ImGui::TextDisabled("--");
                }

                ImGui::TableNextColumn();
                if (pcm.active) {
                    ImGui::ProgressBar(pcm.volume / 255.0f, ImVec2(-1, 0));
                } else {
                    ImGui::ProgressBar(0.0f, ImVec2(-1, 0));
                }
            }

            ImGui::EndTable();
        }
    }
}

// =============================================================================
// SN76489 Window Implementation
// =============================================================================

SN76489Window::SN76489Window(SN76489Visualizer* vis)
    : visualizer(vis)
    , zoom(1)
    , showRegisters(false)
{
    pianoRoll.SetKeyRange(24, 84);  // C1 to C6 (PSG has lower range)
    pianoRoll.SetSize(ImVec2(0, 120));
}

void SN76489Window::Render() {
    if (!isVisible || !visualizer) return;

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("SN76489 (PSG)", &isVisible);

    const SN76489Params& params = visualizer->GetParams();

    // Toolbar
    RenderToolbar();

    ImGui::Separator();

    // Piano Roll
    RenderPianoRoll();

    ImGui::Separator();

    // Channel Table
    RenderChannelTable();

    // Noise info
    ImGui::Separator();
    RenderNoiseInfo();

    // Registers
    if (showRegisters) {
        ImGui::Separator();
        RenderRegisters();
    }

    ImGui::End();
}

void SN76489Window::RenderToolbar() {
    if (ImGui::Button("1x")) zoom = 1;
    ImGui::SameLine();
    if (ImGui::Button("2x")) zoom = 2;
    ImGui::SameLine();

    ImGui::Checkbox("Registers", &showRegisters);

    const SN76489Params& params = visualizer->GetParams();
    if (params.isNGPMode) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "NGP Mode");
    }
}

void SN76489Window::RenderPianoRoll() {
    const SN76489Params& params = visualizer->GetParams();
    pianoRoll.Render(params.channels, 4, "SN76489 Piano Roll");
}

void SN76489Window::RenderChannelTable() {
    const SN76489Params& params = visualizer->GetParams();

    if (ImGui::BeginTable("SN76489 Channels", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Note");
        ImGui::TableSetupColumn("Freq (Hz)");
        ImGui::TableSetupColumn("Volume");
        ImGui::TableSetupColumn("Pan");
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < 4; ch++) {
            const ChannelParams& channel = params.channels[ch];

            ImGui::TableNextRow();

            // Channel number
            ImGui::TableNextColumn();
            ImGui::Text("Ch%d", ch);

            // Type
            ImGui::TableNextColumn();
            if (ch < 3) {
                ImGui::Text("Tone");
            } else {
                ImGui::Text("Noise");
            }

            // Note
            ImGui::TableNextColumn();
            if (ch < 3 && channel.note >= 0) {
                ImGui::Text("%s%d", GetNoteName(channel.note), GetNoteOctave(channel.note));
            } else {
                ImGui::TextDisabled("---");
            }

            // Frequency
            ImGui::TableNextColumn();
            if (ch < 3 && channel.frequency > 0.0f) {
                ImGui::Text("%.1f", channel.frequency);
            } else {
                ImGui::TextDisabled("---");
            }

            // Volume
            ImGui::TableNextColumn();
            float volumeFraction = channel.volume / 120.0f;
            ImGui::ProgressBar(volumeFraction, ImVec2(-1, 0));

            // Pan
            ImGui::TableNextColumn();
            const char* panStr[] = {"L", "C", "R", "LR"};
            ImGui::Text("%s", panStr[channel.pan & 0x03]);
        }

        ImGui::EndTable();
    }
}

void SN76489Window::RenderNoiseInfo() {
    const SN76489Params& params = visualizer->GetParams();

    if (ImGui::CollapsingHeader("Noise Channel", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Type: %s", params.noise.type ? "White Noise" : "Periodic");
        ImGui::Text("Frequency Mode: %d", params.noise.freqMode);
        ImGui::Text("Shift Rate: %d", params.noise.shiftRate);
    }
}

void SN76489Window::RenderRegisters() {
    const SN76489Params& params = visualizer->GetParams();

    if (ImGui::CollapsingHeader("Registers")) {
        for (int i = 0; i < 8; i++) {
            ImGui::Text("Reg[%d]: 0x%04X", i, params.registers[i]);
        }

        ImGui::Separator();
        ImGui::Text("GG Stereo: 0x%02X", params.ggStereo);
    }
}

// =============================================================================
// Register Viewer Window Implementation
// =============================================================================

RegisterViewerWindow::RegisterViewerWindow(const char* name, int regCount)
    : chipDataPtr(nullptr)
    , registerCount(regCount)
    , chipName(name)
{
    registerData = new uint8_t[regCount];
    prevRegisterData = new uint8_t[regCount];
    memset(registerData, 0, regCount);
    memset(prevRegisterData, 0, regCount);
}

RegisterViewerWindow::~RegisterViewerWindow() {
    delete[] registerData;
    delete[] prevRegisterData;
}

void RegisterViewerWindow::SetChipData(void* dataPtr, uint8_t* regs) {
    chipDataPtr = dataPtr;
    if (regs) {
        memcpy(prevRegisterData, registerData, registerCount);
        memcpy(registerData, regs, registerCount);
    }
}

void RegisterViewerWindow::Render() {
    if (!isVisible) return;

    char title[64];
    snprintf(title, sizeof(title), "%s Registers", chipName);

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin(title, &isVisible);

    RenderHexDump();

    ImGui::End();
}

void RegisterViewerWindow::RenderHexDump() {
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;

    if (ImGui::BeginTable("RegHex", 17, flags)) {
        // Header
        ImGui::TableSetupColumn("Addr");
        for (int i = 0; i < 16; i++) {
            char label[4];
            snprintf(label, sizeof(label), "%02X", i);
            ImGui::TableSetupColumn(label);
        }
        ImGui::TableHeadersRow();

        // Data rows
        for (int row = 0; row < (registerCount + 15) / 16; row++) {
            ImGui::TableNextRow();

            // Address column
            ImGui::TableNextColumn();
            ImGui::Text("%04X", row * 16);

            // Data columns
            for (int col = 0; col < 16; col++) {
                int addr = row * 16 + col;
                ImGui::TableNextColumn();

                if (addr < registerCount) {
                    uint8_t value = registerData[addr];
                    bool changed = (value != prevRegisterData[addr]);

                    if (changed) {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "%02X", value);
                    } else {
                        ImGui::Text("%02X", value);
                    }
                }
            }
        }

        ImGui::EndTable();
    }
}
