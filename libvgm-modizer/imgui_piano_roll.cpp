// Piano Roll Visualization Component Implementation

#include "imgui_piano_roll.h"
#include <algorithm>
#include <cstdio>

// Default channel colors (similar to MDPlayer)
static const ImVec4 DEFAULT_COLORS[] = {
    ImVec4(0.0f, 0.8f, 1.0f, 0.8f),   // Ch0: Cyan
    ImVec4(1.0f, 0.4f, 0.4f, 0.8f),   // Ch1: Red
    ImVec4(0.4f, 1.0f, 0.4f, 0.8f),   // Ch2: Green
    ImVec4(1.0f, 1.0f, 0.4f, 0.8f),   // Ch3: Yellow
    ImVec4(1.0f, 0.4f, 1.0f, 0.8f),   // Ch4: Magenta
    ImVec4(0.4f, 0.4f, 1.0f, 0.8f),   // Ch5: Blue
    ImVec4(1.0f, 0.6f, 0.2f, 0.8f),   // Ch6: Orange
    ImVec4(0.6f, 0.2f, 1.0f, 0.8f),   // Ch7: Purple
    ImVec4(0.2f, 1.0f, 1.0f, 0.8f),   // Ch8: Bright Cyan
};

PianoRoll::PianoRoll()
    : minNote(24)    // C1
    , maxNote(96)    // C7
    , renderSize(0, 0)
    , colorCount(0)
{
    // Initialize with default colors
    SetChannelColors(DEFAULT_COLORS, sizeof(DEFAULT_COLORS) / sizeof(DEFAULT_COLORS[0]));
}

void PianoRoll::SetKeyRange(int min, int max) {
    minNote = std::max(0, min);
    maxNote = std::min(127, max);
}

void PianoRoll::SetChannelColors(const ImVec4* colors, int count) {
    colorCount = std::min(count, 16);
    for (int i = 0; i < colorCount; i++) {
        channelColors[i] = colors[i];
    }
}

void PianoRoll::Render(const ChannelParams* channels, int channelCount, const char* label) {
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    if (renderSize.x <= 0) renderSize.x = availSize.x;
    if (renderSize.y <= 0) renderSize.y = 150.0f;

    ImGui::BeginChild(label, renderSize, true);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    // Draw keyboard background
    DrawKeyboard(drawList, canvasPos, canvasSize);

    // Draw active notes
    DrawActiveNotes(drawList, canvasPos, canvasSize, channels, channelCount);

    ImGui::EndChild();
}

void PianoRoll::DrawKeyboard(ImDrawList* drawList, ImVec2 pos, ImVec2 size) {
    int noteRange = maxNote - minNote + 1;
    float keyHeight = size.y / noteRange;

    // Draw background
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            IM_COL32(40, 40, 40, 255));

    // Draw keyboard keys
    for (int note = minNote; note <= maxNote; note++) {
        float y = GetNoteYPosition(note, size.y);
        bool isBlack = IsBlackKey(note);

        ImVec2 keyPos(pos.x, pos.y + y);
        ImVec2 keySize;

        if (isBlack) {
            // Black keys are narrower and offset
            keySize = ImVec2(size.x * 0.6f, keyHeight);
            ImU32 color = IM_COL32(20, 20, 20, 255);
            drawList->AddRectFilled(keyPos, ImVec2(keyPos.x + keySize.x, keyPos.y + keySize.y), color);
        } else {
            // White keys span full width
            keySize = ImVec2(size.x, keyHeight);
            ImU32 color = IM_COL32(200, 200, 200, 255);
            drawList->AddRectFilled(keyPos, ImVec2(keyPos.x + keySize.x, keyPos.y + keySize.y), color);

            // Draw border
            drawList->AddRect(keyPos, ImVec2(keyPos.x + keySize.x, keyPos.y + keySize.y),
                            IM_COL32(100, 100, 100, 255));
        }

        // Draw note name on C notes
        if (note % 12 == 0) {
            char noteName[8];
            snprintf(noteName, sizeof(noteName), "%s%d", GetNoteName(note), GetNoteOctave(note));
            ImVec2 textPos(keyPos.x + 2, keyPos.y);
            drawList->AddText(textPos, IM_COL32(0, 0, 0, 180), noteName);
        }
    }
}

void PianoRoll::DrawActiveNotes(ImDrawList* drawList, ImVec2 pos, ImVec2 size,
                                const ChannelParams* channels, int channelCount) {
    if (!channels) return;

    int noteRange = maxNote - minNote + 1;
    float keyHeight = size.y / noteRange;

    // Draw active notes for each channel
    for (int ch = 0; ch < channelCount; ch++) {
        const ChannelParams& channel = channels[ch];

        if (!channel.keyOn || channel.note < 0) continue;
        if (channel.note < minNote || channel.note > maxNote) continue;

        float y = GetNoteYPosition(channel.note, size.y);
        bool isBlack = IsBlackKey(channel.note);

        // Calculate note rectangle
        ImVec2 notePos(pos.x, pos.y + y);
        ImVec2 noteSize;

        if (isBlack) {
            noteSize = ImVec2(size.x * 0.6f, keyHeight);
        } else {
            noteSize = ImVec2(size.x, keyHeight);
        }

        // Get color for this channel
        ImVec4 color = GetNoteColor(ch, isBlack);

        // Calculate brightness based on volume (0-127)
        float volumeFactor = channel.volume / 127.0f;
        color.x *= (0.5f + 0.5f * volumeFactor);
        color.y *= (0.5f + 0.5f * volumeFactor);
        color.z *= (0.5f + 0.5f * volumeFactor);

        // Draw note highlight
        ImU32 noteColor = ImGui::ColorConvertFloat4ToU32(color);
        drawList->AddRectFilled(notePos, ImVec2(notePos.x + noteSize.x, notePos.y + noteSize.y),
                                noteColor);

        // Draw border
        drawList->AddRect(notePos, ImVec2(notePos.x + noteSize.x, notePos.y + noteSize.y),
                         IM_COL32(255, 255, 255, 200), 0.0f, 0, 2.0f);

        // Draw channel number
        char chLabel[4];
        snprintf(chLabel, sizeof(chLabel), "%d", ch);
        ImVec2 textPos(notePos.x + 4, notePos.y + keyHeight * 0.3f);
        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), chLabel);
    }
}

ImVec4 PianoRoll::GetNoteColor(int channel, bool isBlackKey) {
    if (channel < 0 || channel >= colorCount) {
        return ImVec4(0.7f, 0.7f, 0.7f, 0.8f);  // Default gray
    }

    ImVec4 color = channelColors[channel];

    // Darken for black keys
    if (isBlackKey) {
        color.x *= 0.7f;
        color.y *= 0.7f;
        color.z *= 0.7f;
    }

    return color;
}

float PianoRoll::GetNoteYPosition(int note, float totalHeight) {
    int noteRange = maxNote - minNote + 1;
    float keyHeight = totalHeight / noteRange;

    // Inverted Y axis (lower notes at bottom)
    int noteIndex = maxNote - note;
    return noteIndex * keyHeight;
}
