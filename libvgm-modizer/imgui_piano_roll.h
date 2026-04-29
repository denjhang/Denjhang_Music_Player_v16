// Piano Roll Visualization Component for ImGui
// Draws a piano keyboard with active notes highlighted

#pragma once

#include "imgui/imgui.h"
#include "chip_params.h"

class PianoRoll {
public:
    PianoRoll();

    // Render piano roll with given channel parameters
    void Render(const ChannelParams* channels, int channelCount, const char* label = "Piano Roll");

    // Set the range of keys to display
    void SetKeyRange(int minNote, int maxNote);

    // Set colors for each channel
    void SetChannelColors(const ImVec4* colors, int count);

    // Set size
    void SetSize(ImVec2 size) { renderSize = size; }

private:
    int minNote;     // Minimum MIDI note to display
    int maxNote;     // Maximum MIDI note to display
    ImVec2 renderSize;  // Size of the piano roll

    ImVec4 channelColors[16];  // Colors for up to 16 channels
    int colorCount;

    // Draw the piano keyboard background
    void DrawKeyboard(ImDrawList* drawList, ImVec2 pos, ImVec2 size);

    // Draw active notes on the keyboard
    void DrawActiveNotes(ImDrawList* drawList, ImVec2 pos, ImVec2 size,
                         const ChannelParams* channels, int channelCount);

    // Get color for a specific channel and key type
    ImVec4 GetNoteColor(int channel, bool isBlackKey);

    // Get Y position for a given MIDI note
    float GetNoteYPosition(int note, float totalHeight);
};
