// gui_renderer_impl.cpp - ImGui Render Function Implementations
// Included by gui_renderer.cpp via #include
// Extracted from ym2163_piano_gui_v11.cpp lines 2772-4594

#include "windows/spfm/spfm_manager.h"

// ===== RenderMIDIPlayer =====

void RenderMIDIPlayer() {
    ImGui::Text("MIDI Player");
    ImGui::Separator();

    float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
    if (ImGui::Button("Play", ImVec2(buttonWidth, 30))) {
        MidiPlayer::PlayMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(buttonWidth, 30))) {
        MidiPlayer::PauseMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(buttonWidth, 30))) {
        MidiPlayer::StopMIDI();
    }

    float navButtonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
    if (ImGui::Button("<< Prev", ImVec2(navButtonWidth, 25))) {
        MidiPlayer::PlayPreviousMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Next >>", ImVec2(navButtonWidth, 25))) {
        MidiPlayer::PlayNextMIDI();
    }

    ImGui::Checkbox("Auto-play next", &MidiPlayer::g_autoPlayNext);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically play next track when current finishes");
    }

    ImGui::SameLine();

    const char* modeText = MidiPlayer::g_isSequentialPlayback ? "Sequential" : "Random";
    if (ImGui::Button(modeText, ImVec2(85, 0))) {
        MidiPlayer::g_isSequentialPlayback = !MidiPlayer::g_isSequentialPlayback;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to toggle: Sequential (loop) / Random");
    }

    ImGui::Spacing();
    if (MidiPlayer::g_midiPlayer.isPlaying && !MidiPlayer::g_midiPlayer.isPaused) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Playing:");
    } else if (MidiPlayer::g_midiPlayer.isPaused) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused:");
    } else {
        ImGui::Text("Ready:");
    }

    ImGui::SameLine();

    if (!MidiPlayer::g_midiPlayer.currentFileName.empty()) {
        const char* filename = strrchr(MidiPlayer::g_midiPlayer.currentFileName.c_str(), '\\');
        if (!filename) filename = strrchr(MidiPlayer::g_midiPlayer.currentFileName.c_str(), '/');
        if (!filename) filename = MidiPlayer::g_midiPlayer.currentFileName.c_str();
        else filename++;
        ImGui::Text("%s", filename);
    } else {
        ImGui::TextDisabled("(no file loaded)");
    }

    // Progress bar with seek
    if (!MidiPlayer::g_midiPlayer.currentFileName.empty() && MidiPlayer::g_midiPlayer.midiFile.getEventCount(0) > 0) {
        smf::MidiEventList& track = MidiPlayer::g_midiPlayer.midiFile[0];
        int lastMidiTick = 0;
        if (track.size() > 0) {
            lastMidiTick = track[track.size() - 1].tick;
        }

            double microsPerTick = MidiPlayer::g_midiPlayer.tempo / (double)MidiPlayer::g_midiPlayer.ticksPerQuarterNote;
            double totalTimeMicros = (double)lastMidiTick * microsPerTick;

            float progress = (totalTimeMicros > 0) ? (float)(MidiPlayer::g_midiPlayer.accumulatedTime / totalTimeMicros) : 0.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            double currentTimeMicros = MidiPlayer::g_midiPlayer.accumulatedTime;
            int currentSeconds = (int)(currentTimeMicros / 1000000.0);
            int currentMinutes = currentSeconds / 60;
            currentSeconds %= 60;
            int totalSeconds = (int)(totalTimeMicros / 1000000.0);
            int totalMinutes = totalSeconds / 60;
            totalSeconds %= 60;
            char currentTimeStr[32], totalTimeStr[32];
            sprintf(currentTimeStr, "%02d:%02d", currentMinutes, currentSeconds);
            sprintf(totalTimeStr, "%02d:%02d", totalMinutes, totalSeconds);

            ImGui::Text("%s / %s", currentTimeStr, totalTimeStr);

            ImVec2 progressPos = ImGui::GetCursorScreenPos();
            ImVec2 progressSize = ImVec2(ImGui::GetContentRegionAvail().x, 20);
            ImGui::ProgressBar(progress, progressSize, "");

            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                ImVec2 mousePos = ImGui::GetMousePos();
                float clickPos = (mousePos.x - progressPos.x) / progressSize.x;
                if (clickPos < 0.0f) clickPos = 0.0f;
                if (clickPos > 1.0f) clickPos = 1.0f;

                int targetMidiTick = (int)(clickPos * lastMidiTick);
                int targetEventIndex = 0;
                for (int i = 0; i < (int)track.size(); i++) {
                    if (track[i].tick >= targetMidiTick) {
                        targetEventIndex = i;
                        break;
                    }
                }
                MidiPlayer::g_midiPlayer.currentTick = targetEventIndex;
                bool wasPlaying = MidiPlayer::g_midiPlayer.isPlaying && !MidiPlayer::g_midiPlayer.isPaused;
                YM2163::stop_all_notes();
                MidiPlayer::g_midiPlayer.activeNotes.clear();
                YM2163::ResetPianoKeyStates();
                QueryPerformanceCounter(&MidiPlayer::g_midiPlayer.lastPerfCounter);
                double ticksPerMicrosecond = (double)MidiPlayer::g_midiPlayer.ticksPerQuarterNote / MidiPlayer::g_midiPlayer.tempo;
                MidiPlayer::g_midiPlayer.accumulatedTime = targetMidiTick / ticksPerMicrosecond;
                auto seekNow = std::chrono::steady_clock::now();
                if (wasPlaying) {
                    MidiPlayer::g_midiPlayer.playStartTime = seekNow - std::chrono::microseconds((int)(targetMidiTick * microsPerTick));
                    MidiPlayer::g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                } else if (MidiPlayer::g_midiPlayer.isPaused) {
                    MidiPlayer::g_midiPlayer.playStartTime = seekNow - std::chrono::microseconds((int)(targetMidiTick * microsPerTick));
                    MidiPlayer::g_midiPlayer.pauseTime = seekNow;
                    MidiPlayer::g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                }
                // v11: Don't rebuild active notes after seek to avoid loud burst
                YM2163::log_command("Seek to progress: %.1f%% (time: %s)", clickPos * 100.0f, currentTimeStr);
            }
    } else {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
    }

    ImGui::Spacing();
    ImGui::Separator();
    // File browser
    ImGui::Text("File Browser");
    ImGui::Separator();

    if (ImGui::Button("<", ImVec2(25, 0))) MidiPlayer::NavigateBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(25, 0))) MidiPlayer::NavigateForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^", ImVec2(25, 0))) MidiPlayer::NavigateToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    if (!MidiPlayer::g_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = MidiPlayer::SplitPath(MidiPlayer::g_currentPath);

        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;

        for (size_t i = 0; i < segments.size(); i++) {
            if (i == 0) accumulatedPath = segments[i];
            else {
                if (accumulatedPath.back() != '\\') accumulatedPath += "\\";
                accumulatedPath += segments[i];
            }
            accumulatedPaths.push_back(accumulatedPath);
            ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
            float bw = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
            buttonWidths.push_back(bw);
        }

        ImVec2 separatorTextSize = ImGui::CalcTextSize(">");
        float separatorWidth = separatorTextSize.x + itemSpacingX * 2.0f;
        ImVec2 ellipsisTextSize = ImGui::CalcTextSize("...");
        float ellipsisButtonWidth = ellipsisTextSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        float ellipsisWidth = ellipsisButtonWidth + separatorWidth;
        float safeAvailWidth = availWidth - 10.0f;
        int firstVisibleSegment = (int)segments.size() - 1;
        float usedWidth = (segments.size() > 0) ? buttonWidths.back() : 0.0f;
        for (int i = (int)segments.size() - 2; i >= 0; i--) {
            float segmentWidth = buttonWidths[i] + separatorWidth;
            float neededEllipsis = (i > 0) ? ellipsisWidth : 0.0f;
            if (usedWidth + segmentWidth + neededEllipsis > safeAvailWidth) break;
            else { usedWidth += segmentWidth; firstVisibleSegment = i; }
        }

        ImVec2 barStartPos = ImGui::GetCursorScreenPos();
        float barHeight = ImGui::GetFrameHeight();
        ImGui::BeginGroup();

        if (firstVisibleSegment > 0) {
            if (ImGui::Button("...##ellipsis")) {
                MidiPlayer::g_pathEditMode = true;
                MidiPlayer::g_pathEditModeJustActivated = true;
                strcpy(MidiPlayer::g_pathInput, MidiPlayer::g_currentPath);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", MidiPlayer::g_currentPath);
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }

        for (size_t i = firstVisibleSegment; i < segments.size(); i++) {
            if (i > (size_t)firstVisibleSegment) {
                ImGui::SameLine(); ImGui::TextDisabled(">"); ImGui::SameLine();
            }
            std::string displayName = MidiPlayer::TruncateFolderName(segments[i], 20);
            char buttonId[256];
            snprintf(buttonId, sizeof(buttonId), "%s##seg%d", displayName.c_str(), (int)i);
            if (ImGui::Button(buttonId)) MidiPlayer::NavigateToPath(accumulatedPaths[i].c_str());
            if (displayName != segments[i] && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", segments[i].c_str());
        }

        ImGui::EndGroup();
        ImVec2 barEndPos = ImGui::GetItemRectMax();
        float usedBarWidth = barEndPos.x - barStartPos.x;
        float emptySpaceWidth = availWidth - usedBarWidth;
        if (emptySpaceWidth > 10.0f) {
            ImGui::SetCursorScreenPos(ImVec2(barEndPos.x, barStartPos.y));
            ImGui::InvisibleButton("##AddressBarEmptySpace", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                MidiPlayer::g_pathEditMode = true;
                MidiPlayer::g_pathEditModeJustActivated = true;
                strcpy(MidiPlayer::g_pathInput, MidiPlayer::g_currentPath);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##PathInput", MidiPlayer::g_pathInput, MAX_PATH,
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            MidiPlayer::NavigateToPath(MidiPlayer::g_pathInput);
            MidiPlayer::g_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            MidiPlayer::g_pathEditMode = false;
            MidiPlayer::g_pathEditModeJustActivated = false;
            strcpy(MidiPlayer::g_pathInput, MidiPlayer::g_currentPath);
        } else if (!MidiPlayer::g_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            MidiPlayer::g_pathEditMode = false;
            strcpy(MidiPlayer::g_pathInput, MidiPlayer::g_currentPath);
        }
        if (MidiPlayer::g_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            MidiPlayer::g_pathEditModeJustActivated = false;
        }
    }

    ImGui::BeginChild("FileList", ImVec2(-1, 0), true);

    std::string currentPathStr(MidiPlayer::g_currentPath);
    if (strlen(MidiPlayer::g_currentPath) > 0)
        MidiPlayer::g_pathScrollPositions[currentPathStr] = ImGui::GetScrollY();

    static std::string lastRestoredPath;
    if (currentPathStr != lastRestoredPath && MidiPlayer::g_pathScrollPositions.count(currentPathStr) > 0) {
        ImGui::SetScrollY(MidiPlayer::g_pathScrollPositions[currentPathStr]);
        lastRestoredPath = currentPathStr;
    }

    MidiPlayer::g_hoveredFileIndex = -1;
    for (int i = 0; i < (int)MidiPlayer::g_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = MidiPlayer::g_fileList[i];
        bool isSelected = (MidiPlayer::g_selectedFileIndex == i);
        bool isExitedFolder = (!MidiPlayer::g_lastExitedFolder.empty() && entry.isDirectory
                               && entry.name == MidiPlayer::g_lastExitedFolder);
        bool isPlayingPath = false;
        bool isPlayingFile = false;
        if (!MidiPlayer::g_currentPlayingFilePath.empty()) {
            if (entry.isDirectory) {
                std::string entryPath = entry.fullPath;
                if (entryPath.back() != '\\') entryPath += "\\";
                if (MidiPlayer::g_currentPlayingFilePath.find(entryPath) == 0) isPlayingPath = true;
            } else if (entry.fullPath == MidiPlayer::g_currentPlayingFilePath) {
                isPlayingFile = true;
            }
        }

        std::string label;
        if (entry.name == "..") label = "[UP] " + entry.name;
        else if (entry.isDirectory) label = "[DIR] " + entry.name;
        else label = entry.name;

        if (isExitedFolder) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        else if (isPlayingFile) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
        else if (isPlayingPath) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 1.0f, 1.0f));
        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        float availWidth = ImGui::GetContentRegionAvail().x;
        bool needsScrolling = textSize.x > availWidth;
        bool isHovered = false;

        if (needsScrolling) {
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            ImVec2 itemSize = ImVec2(availWidth, ImGui::GetTextLineHeightWithSpacing());
            ImGui::InvisibleButton(("##item" + std::to_string(i)).c_str(), itemSize);
            isHovered = ImGui::IsItemHovered();

            if (ImGui::IsItemClicked()) {
                MidiPlayer::g_selectedFileIndex = i;
                if (entry.name == "..") {
                    MidiPlayer::NavigateToParent();
                } else if (entry.isDirectory) {
                    MidiPlayer::g_lastExitedFolder.clear();
                    MidiPlayer::NavigateToPath(entry.fullPath.c_str());
                } else {
                    MidiPlayer::g_currentPlayingIndex = i;
                    MidiPlayer::g_currentPlayingFilePath = entry.fullPath;
                    YM2163::ResetAllYM2163Chips();
                    YM2163::InitializeAllChannels();
                    YM2163::stop_all_notes();
                    MidiPlayer::g_midiPlayer.activeNotes.clear();
                    YM2163::ResetPianoKeyStates();
                    if (MidiPlayer::LoadMIDIFile(entry.fullPath.c_str())) {
                        MidiPlayer::g_midiPlayer.currentTick = 0;
                        MidiPlayer::g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                        MidiPlayer::PlayMIDI();
                    }
                }
            }

            bool shouldScroll = (isSelected || isExitedFolder || isHovered);

            if (shouldScroll) {
            if (!MidiPlayer::g_textScrollStates.count(i)) {
                MidiPlayer::TextScrollState state;
                state.scrollOffset = 0.0f; state.scrollDirection = 1.0f;
                state.pauseTimer = 1.0f;
                state.lastUpdateTime = std::chrono::steady_clock::now();
                MidiPlayer::g_textScrollStates[i] = state;
            }
            MidiPlayer::TextScrollState& scrollState = MidiPlayer::g_textScrollStates[i];
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(now - scrollState.lastUpdateTime).count();
            scrollState.lastUpdateTime = now;

            if (scrollState.pauseTimer > 0.0f) scrollState.pauseTimer -= deltaTime;
            else {
                float scrollSpeed = 30.0f;
                scrollState.scrollOffset += scrollState.scrollDirection * scrollSpeed * deltaTime;
                float maxScroll = textSize.x - availWidth + 20.0f;
                if (scrollState.scrollOffset >= maxScroll) {
                    scrollState.scrollOffset = maxScroll; scrollState.scrollDirection = -1.0f; scrollState.pauseTimer = 1.0f;
                } else if (scrollState.scrollOffset <= 0.0f) {
                    scrollState.scrollOffset = 0.0f; scrollState.scrollDirection = 1.0f; scrollState.pauseTimer = 1.0f;
                }
            }
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (isPlayingFile) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), IM_COL32(40,80,50,255));
            else if (isSelected) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            drawList->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), true);
            drawList->AddText(ImVec2(cursorPos.x - scrollState.scrollOffset, cursorPos.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            drawList->PopClipRect();
            } else {
                // Not scrolling - draw static text and erase scroll state
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                if (isPlayingFile) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), IM_COL32(40,80,50,255));
                else if (isSelected) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
                else if (isHovered) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                drawList->AddText(cursorPos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                if (MidiPlayer::g_textScrollStates.count(i) > 0)
                    MidiPlayer::g_textScrollStates.erase(i);
            }
        } else {
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            ImVec2 itemSize = ImVec2(availWidth, ImGui::GetTextLineHeightWithSpacing());
            ImGui::InvisibleButton(("##item" + std::to_string(i)).c_str(), itemSize);
            isHovered = ImGui::IsItemHovered();

            if (ImGui::IsItemClicked()) {
                MidiPlayer::g_selectedFileIndex = i;
                if (entry.name == "..") {
                    MidiPlayer::NavigateToParent();
                } else if (entry.isDirectory) {
                    MidiPlayer::g_lastExitedFolder.clear();
                    MidiPlayer::NavigateToPath(entry.fullPath.c_str());
                } else {
                    MidiPlayer::g_currentPlayingIndex = i;
                    MidiPlayer::g_currentPlayingFilePath = entry.fullPath;
                    YM2163::ResetAllYM2163Chips();
                    YM2163::InitializeAllChannels();
                    YM2163::stop_all_notes();
                    MidiPlayer::g_midiPlayer.activeNotes.clear();
                    YM2163::ResetPianoKeyStates();
                    if (MidiPlayer::LoadMIDIFile(entry.fullPath.c_str())) {
                        MidiPlayer::g_midiPlayer.currentTick = 0;
                        MidiPlayer::g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                        MidiPlayer::PlayMIDI();
                    }
                }
            }
            isHovered = ImGui::IsItemHovered();

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (isPlayingFile) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), IM_COL32(40,80,50,255));
            else if (isSelected) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            drawList->AddText(cursorPos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }

        if (isHovered) MidiPlayer::g_hoveredFileIndex = i;
        if (isExitedFolder || isPlayingFile || isPlayingPath) ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

// ===== RenderPianoKeyboard =====

// Chip colors (matching RenderChannelStatus)
static const ImVec4 g_chipColors[4] = {
    ImVec4(0.0f, 1.0f, 0.5f, 1.0f),  // Slot0: Green
    ImVec4(0.5f, 0.5f, 1.0f, 1.0f),  // Slot1: Blue
    ImVec4(1.0f, 0.5f, 0.5f, 1.0f),  // Slot2: Red
    ImVec4(1.0f, 0.8f, 0.2f, 1.0f)   // Slot3: Orange
};

void RenderPianoKeyboard() {
    ImGui::BeginChild("Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    float whiteKeyWidth = 20.0f;
    float whiteKeyHeight = 100.0f;
    float blackKeyWidth = 12.0f;
    float blackKeyHeight = 60.0f;

    int c8WhiteKeys = YM2163::g_enableSlot3_2MHz ? 7 : 0;
    int totalWhiteKeys = 36 + c8WhiteKeys;
    float pianoWidth = totalWhiteKeys * whiteKeyWidth;
    float availWidth = ImGui::GetContentRegionAvail().x;
    float centerOffset = (availWidth > pianoWidth) ? (availWidth - pianoWidth) * 0.5f : 0.0f;

    int whiteKeyCount = 0;

    // Helper to get key color based on chip index and level
    auto getKeyColor = [&](int keyIdx, float level) -> ImU32 {
        int chipIdx = g_pianoKeyChipIndex[keyIdx];
        float velocityFactor = (g_pianoKeyVelocity[keyIdx] > 0)
            ? (g_pianoKeyVelocity[keyIdx] / 127.0f) : 1.0f;
        float intensity = level * velocityFactor;
        // Power curve to spread out high-velocity differences
        intensity = powf(intensity, 0.5f);
        if (chipIdx >= 0 && chipIdx < 4) {
            ImVec4 baseColor = g_chipColors[chipIdx];
            int r = (int)(baseColor.x * 255 * (0.15f + 0.85f * intensity));
            int g = (int)(baseColor.y * 255 * (0.15f + 0.85f * intensity));
            int b = (int)(baseColor.z * 255 * (0.15f + 0.85f * intensity));
            return IM_COL32(r, g, b, 255);
        }
        return IM_COL32((int)(30 + 225 * intensity), (int)(60 + 195 * intensity), 255, 255);
    };

    // Draw B2
    {
        int keyIdx = 0;
        float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
        float y = p.y;

        ImU32 color;
        if (g_pianoKeyPressed[keyIdx]) {
            color = getKeyColor(keyIdx, g_pianoKeyLevel[keyIdx]);
        } else {
            color = IM_COL32(255, 255, 255, 255);
        }

        draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
        draw_list->AddRect(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

        draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18), IM_COL32(0, 0, 0, 255), "B2");

        whiteKeyCount++;
    }

    // Draw C3-B7 white keys
    for (int octave = 1; octave <= 5; octave++) {
        for (int note = 0; note <= 11; note++) {
            if (YM2163::g_isBlackNote[note]) continue;

            int keyIdx = YM2163::get_key_index(octave, note);
            if (keyIdx < 0) continue;

            float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                color = getKeyColor(keyIdx, g_pianoKeyLevel[keyIdx]);
            } else {
                color = IM_COL32(255, 255, 255, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

            if (note == 0) {
                char label[8];
                sprintf(label, "C%d", octave + 2);
                draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18), IM_COL32(0, 0, 0, 255), label);
            }

            whiteKeyCount++;
        }
    }

    // Draw C8-B8 white keys (when Slot3 2MHz enabled)
    if (YM2163::g_enableSlot3_2MHz) {
        for (int note = 0; note <= 11; note++) {
            if (YM2163::g_isBlackNote[note]) continue;

            int keyIdx = YM2163::get_key_index(6, note);  // octave 6 = C8-B8
            if (keyIdx < 0) continue;

            float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                color = getKeyColor(keyIdx, g_pianoKeyLevel[keyIdx]);
            } else {
                color = IM_COL32(255, 250, 230, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y), ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

            if (note == 0) {
                draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18), IM_COL32(0, 0, 0, 255), "C8");
            }

            whiteKeyCount++;
        }
    }

    // Draw black keys
    whiteKeyCount = 1;
    for (int octave = 1; octave <= (YM2163::g_enableSlot3_2MHz ? 6 : 5); octave++) {
        for (int note = 0; note <= 11; note++) {
            if (!YM2163::g_isBlackNote[note]) continue;

            int keyIdx = YM2163::get_key_index(octave, note);
            if (keyIdx < 0) continue;

            // Calculate black key position relative to white keys
            int whiteKeyIdx = 0;
            if (note == 1)  whiteKeyIdx = 0;   // C#
            else if (note == 3)  whiteKeyIdx = 1;   // D#
            else if (note == 6)  whiteKeyIdx = 3;   // F#
            else if (note == 8)  whiteKeyIdx = 4;   // G#
            else if (note == 10) whiteKeyIdx = 5;   // A#

            float x = p.x + centerOffset + (whiteKeyCount + whiteKeyIdx) * whiteKeyWidth - blackKeyWidth / 2;
            float y = p.y;

            ImU32 color;
            if (keyIdx < 73 && g_pianoKeyPressed[keyIdx]) {
                float level = g_pianoKeyLevel[keyIdx];
                float velocityFactor = (g_pianoKeyVelocity[keyIdx] > 0)
                    ? (g_pianoKeyVelocity[keyIdx] / 127.0f) : 1.0f;
                float intensity = level * velocityFactor;
                // Power curve to spread out high-velocity differences
                intensity = powf(intensity, 0.5f);
                int chipIdx = g_pianoKeyChipIndex[keyIdx];
                if (chipIdx >= 0 && chipIdx < 4) {
                    ImVec4 baseColor = g_chipColors[chipIdx];
                    int r = (int)(baseColor.x * 255 * (0.1f + 0.7f * intensity));
                    int g = (int)(baseColor.y * 255 * (0.1f + 0.7f * intensity));
                    int b = (int)(baseColor.z * 255 * (0.1f + 0.7f * intensity));
                    color = IM_COL32(r, g, b, 255);
                } else {
                    color = IM_COL32((int)(20 + 180 * intensity), (int)(40 + 175 * intensity), 255, 255);
                }
            } else {
                if (octave == 6 && YM2163::g_enableSlot3_2MHz)
                    color = IM_COL32(30, 30, 20, 255);
                else
                    color = IM_COL32(0, 0, 0, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + blackKeyWidth, y + blackKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y), ImVec2(x + blackKeyWidth, y + blackKeyHeight), IM_COL32(128, 128, 128, 255));
        }
        whiteKeyCount += 7;
    }

    // SUS indicator
    if (YM2163::g_sustainPedalActive && YM2163::g_enableSustainPedal) {
        ImVec2 susPos = ImVec2(p.x + centerOffset + 10, p.y + whiteKeyHeight + 10);
        draw_list->AddText(susPos, IM_COL32(255, 200, 0, 255), "SUS");
    }

    ImGui::EndChild();
}

// ===== RenderLevelMeters =====

void RenderLevelMeters() {
    ImGui::BeginChild("LevelMeters", ImVec2(0, 0), true);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availWidth = ImGui::GetContentRegionAvail().x;
    float availHeight = ImGui::GetContentRegionAvail().y;

    float chipGroupWidth = availWidth / 4.0f;
    float boxPadding = 8.0f;
    float meterWidth = 18.0f;
    float rhythmMeterWidth = 25.0f;
    float spacing = 10.0f;
    float verticalSpacing = 15.0f;
    float slotLabelHeight = 25.0f;
    float melodyMeterHeight = (availHeight - slotLabelHeight - verticalSpacing - boxPadding * 3) * 0.5f;
    float rhythmMeterHeight = (availHeight - slotLabelHeight - verticalSpacing - boxPadding * 3) * 0.5f;

    auto levelToDBScale = [](float level) -> float {
        if (level <= 0.0f) return 0.0f;
        float db = 20.0f * log10f(level);
        if (db < -24.0f) db = -24.0f;
        return (db + 24.0f) / 24.0f;
    };

    auto getLevelColor = [](float level) -> ImU32 {
        if (level <= 0.0f) return IM_COL32(40, 40, 40, 255);  // Dark gray for empty

        // Blue (0.0) -> Green (0.33) -> Yellow (0.66) -> Red (1.0)
        if (level < 0.33f) {
            // Blue to Green
            float t = level / 0.33f;
            int r = 0;
            int g = (int)(100 + 155 * t);   // 100 to 255
            int b = (int)(255 - 155 * t);   // 255 to 100
            return IM_COL32(r, g, b, 255);
        } else if (level < 0.66f) {
            // Green to Yellow
            float t = (level - 0.33f) / 0.33f;
            int r = (int)(0 + 255 * t);    // 0 to 255
            int g = 255;
            int b = (int)(100 - 100 * t);  // 100 to 0
            return IM_COL32(r, g, b, 255);
        } else {
            // Yellow to Red
            float t = (level - 0.66f) / 0.34f;
            int r = 255;
            int g = (int)(255 - 155 * t);  // 255 to 100
            int b = 0;
            return IM_COL32(r, g, b, 255);
        }
    };

    const char* chipLabels[] = {"Slot0", "Slot1", "Slot2", "Slot3"};
    for (int chip = 0; chip < 4; chip++) {
        float chipX = p.x + chip * chipGroupWidth;
        draw_list->AddRect(
            ImVec2(chipX + 2, p.y + 2),
            ImVec2(chipX + chipGroupWidth - 2, p.y + availHeight - 2),
            IM_COL32(120, 120, 120, 255), 4.0f, 0, 2.0f);
        draw_list->AddText(ImVec2(chipX + 8, p.y + 8), IM_COL32(200, 200, 200, 255), chipLabels[chip]);

        float currentY = p.y + slotLabelHeight + boxPadding;

        // Melody meters
        float melodyTotalWidth = 4 * meterWidth + 3 * spacing;
        float melodyStartX = chipX + (chipGroupWidth - melodyTotalWidth) * 0.5f;
        for (int ch = 0; ch < 4; ch++) {
            int channelIndex = chip * 4 + ch;
            float meterX = melodyStartX + ch * (meterWidth + spacing);
            float meterY = currentY;
            float level = YM2163::g_channels[channelIndex].currentLevel;
            float displayLevel = levelToDBScale(level);

            // Background
            draw_list->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterWidth, meterY + melodyMeterHeight), IM_COL32(30, 30, 30, 255));
            // Border
            draw_list->AddRect(ImVec2(meterX, meterY), ImVec2(meterX + meterWidth, meterY + melodyMeterHeight), IM_COL32(100, 100, 100, 255));

            // Segmented bar from bottom up
            if (displayLevel > 0.01f) {
                float barHeight2 = melodyMeterHeight * displayLevel;
                float barY = meterY + melodyMeterHeight - barHeight2;
                int segments = 20;
                for (int i = 0; i < segments; i++) {
                    float segmentHeight = barHeight2 / segments;
                    float segmentY = barY + i * segmentHeight;
                    float segmentLevel = (float)(segments - i) / segments * displayLevel;
                    draw_list->AddRectFilled(
                        ImVec2(meterX + 1, segmentY),
                        ImVec2(meterX + meterWidth - 1, segmentY + segmentHeight),
                        getLevelColor(segmentLevel));
                }
            }

            // Channel label at top-left of meter
            char chLabel[4];
            snprintf(chLabel, sizeof(chLabel), "%d", ch);
            draw_list->AddText(ImVec2(meterX + 2, meterY + 2), IM_COL32(180, 180, 180, 255), chLabel);
        }
        // Rhythm meters
        float rhythmSectionY = currentY + melodyMeterHeight + verticalSpacing;
        float rhythmTotalWidth = 5 * rhythmMeterWidth + 4 * (spacing * 0.5f);
        float rhythmStartX = chipX + (chipGroupWidth - rhythmTotalWidth) * 0.5f;
        const char* drumLabels[] = {"BD", "HC", "SD", "HO", "HD"};
        for (int drum = 0; drum < 5; drum++) {
            float meterX = rhythmStartX + drum * (rhythmMeterWidth + spacing * 0.5f);
            float meterY = rhythmSectionY;
            float barHeight = rhythmMeterHeight;
            draw_list->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + rhythmMeterWidth, meterY + barHeight), IM_COL32(20, 20, 20, 255));
            draw_list->AddRect(ImVec2(meterX, meterY), ImVec2(meterX + rhythmMeterWidth, meterY + barHeight), IM_COL32(100, 100, 100, 255));

            {
                float displayLevel = YM2163::g_drumLevels[chip][drum];
                if (displayLevel > 0.01f) {
                    int segments = 20;
                    for (int i = segments - 1; i >= 0; i--) {
                        float segmentHeight = barHeight / segments;
                        float segmentY = meterY + i * segmentHeight;
                        float segmentLevel = (float)(segments - i) / segments * displayLevel;
                        draw_list->AddRectFilled(
                            ImVec2(meterX + 1, segmentY),
                            ImVec2(meterX + rhythmMeterWidth - 1, segmentY + segmentHeight),
                            getLevelColor(segmentLevel));
                    }
                }
            }
            draw_list->AddText(ImVec2(meterX + 1, meterY + 2), IM_COL32(180, 180, 180, 255), drumLabels[drum]);
        }
    }

    ImGui::EndChild();
}

// ===== RenderChannelStatus =====

void RenderChannelStatus() {
    ImVec4 slot0Color = ImVec4(0.0f, 1.0f, 0.5f, 1.0f);
    ImVec4 slot1Color = ImVec4(0.5f, 0.5f, 1.0f, 1.0f);
    ImVec4 slot2Color = ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
    ImVec4 slot3Color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
    ImVec4 slotDisabledColor = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    ImVec4 drumActiveColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 releaseColor = ImVec4(1.0f, 1.0f, 0.0f, 0.8f);
    const int RELEASE_DISPLAY_TIME_MS = 1000;

    float availWidth = ImGui::GetContentRegionAvail().x;
    float availHeight = ImGui::GetContentRegionAvail().y;
    float boxWidth = (availWidth / 2.0f - 5);
    float boxHeight = (availHeight / 2.0f - 5);

    auto now = std::chrono::steady_clock::now();

    auto renderChipBox = [&](int chipIndex, const char* childName, ImVec4 activeColor, bool isEnabled) {
        ImGui::BeginChild(childName, ImVec2(boxWidth, boxHeight), true);
        if (isEnabled)
            ImGui::TextColored(activeColor, "YM2163 Slot%d (used)", chipIndex);
        else
            ImGui::TextColored(slotDisabledColor, "YM2163 Slot%d (unused)", chipIndex);
        ImGui::Separator();

        int baseChannel = chipIndex * 4;
        for (int i = 0; i < 4; i++) {
            int ch = baseChannel + i;
            if (YM2163::g_channels[ch].active) {
                ImGui::TextColored(activeColor, "CH%d: %s%d", i,
                    YM2163::g_noteNames[YM2163::g_channels[ch].note],
                    YM2163::g_channels[ch].octave + 2);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[%s/%s/%s]",
                    g_timbreNames[YM2163::g_channels[ch].timbre],
                    g_envelopeNames[YM2163::g_channels[ch].envelope],
                    g_volumeNames[YM2163::g_channels[ch].volume]);
            } else {
                auto timeSinceRelease = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - YM2163::g_channels[ch].releaseTime).count();
                if (YM2163::g_channels[ch].hasBeenUsed && timeSinceRelease < RELEASE_DISPLAY_TIME_MS)
                    ImGui::TextColored(releaseColor, "CH%d: Release", i);
                else
                    ImGui::TextDisabled("CH%d: ---", i);
            }
        }
        ImGui::Separator();
        ImGui::Text("Drums:");
        ImGui::SameLine();
        for (int i = 0; i < 5; i++) {
            if (YM2163::g_drumActive[chipIndex][i])
                ImGui::TextColored(drumActiveColor, "%s", YM2163::g_drumNames[i]);
            else
                ImGui::TextDisabled("%s", YM2163::g_drumNames[i]);
            if (i < 4) ImGui::SameLine();
        }
        ImGui::EndChild();
    };

    renderChipBox(0, "Slot0Channels", slot0Color, true);
    ImGui::SameLine();
    renderChipBox(1, "Slot1Channels", slot1Color, YM2163::g_enableSecondYM2163);
    renderChipBox(2, "Slot2Channels", slot2Color, YM2163::g_enableThirdYM2163);
    ImGui::SameLine();
    renderChipBox(3, "Slot3Channels", slot3Color, YM2163::g_enableFourthYM2163);
}

// ===== RenderControls =====

void RenderControls() {
    ImGui::BeginChild("Controls", ImVec2(280, 0), true);
    ImGui::Text("Controls");
    ImGui::Separator();

    ImGui::Text("Octave: B=%d", g_currentOctave);
    if (g_currentOctave == 0) { ImGui::SameLine(); ImGui::TextDisabled("(B2 only)"); }
    else { ImGui::SameLine(); ImGui::Text("(C%d-B%d)", g_currentOctave + 2, g_currentOctave + 2); }
    if (ImGui::Button("Oct +") && g_currentOctave < 5) { YM2163::stop_all_notes(); g_currentOctave++; }
    ImGui::SameLine();
    if (ImGui::Button("Oct -") && g_currentOctave > 0) { YM2163::stop_all_notes(); g_currentOctave--; }

    ImGui::Spacing();
    ImGui::Text("Volume: %-15s", g_volumeNames[YM2163::g_currentVolume]);
    if (ImGui::Button("Vol +") && YM2163::g_currentVolume > 0) YM2163::g_currentVolume--;
    ImGui::SameLine();
    if (ImGui::Button("Vol -") && YM2163::g_currentVolume < 3) YM2163::g_currentVolume++;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("MIDI Control Mode");
    if (ImGui::RadioButton("Live Control", YM2163::g_useLiveControl)) YM2163::g_useLiveControl = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI playback uses UI Wave/Envelope settings\n(Ignores config file)");
    if (ImGui::RadioButton("Config Mode", !YM2163::g_useLiveControl)) {
        YM2163::g_useLiveControl = false;
        LoadInstrumentConfigToUI(g_selectedInstrument);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI playback uses config file settings\n(Wave/Envelope only affect keyboard play)");

    ImGui::Spacing();
    ImGui::Checkbox("Velocity Mapping", &YM2163::g_enableVelocityMapping);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Map MIDI velocity to 4-level volume\n(Enable for dynamic volume control)");

    if (YM2163::g_enableVelocityMapping) {
        ImGui::Indent(20.0f);
        ImGui::Checkbox("Dynamic Mapping", &YM2163::g_enableDynamicVelocityMapping);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Dynamic: analyze MIDI velocity distribution\nStatic: fixed thresholds (0dB:113-127, -6dB:64-112, -12dB:1-63)");
        }
        if (!YM2163::g_enableDynamicVelocityMapping) {
            ImGui::TextDisabled("Static thresholds:");
            ImGui::TextDisabled("  0dB: 113-127");
            ImGui::TextDisabled("  -6dB: 64-112");
            ImGui::TextDisabled("  -12dB: 1-63");
            ImGui::TextDisabled("  Mute: 0");
        }
        ImGui::Unindent(20.0f);
    }
    ImGui::Checkbox("Sustain Pedal", &YM2163::g_enableSustainPedal);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Map sustain pedal (CC64) to envelope:\nPedal Down: Fast, Pedal Up: Decay");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Checkbox("Global Media Keys", &MidiPlayer::g_enableGlobalMediaKeys)) {
        if (MidiPlayer::g_enableGlobalMediaKeys) MidiPlayer::RegisterGlobalMediaKeys();
        else MidiPlayer::UnregisterGlobalMediaKeys();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Capture global media keys:\nPlay/Pause, Next Track, Previous Track\nWorks even when window is not focused");

    ImGui::Checkbox("Auto-Skip Silence", &MidiPlayer::g_enableAutoSkipSilence);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically skip silence at the start of MIDI files\nJumps to the first note to avoid waiting");

    ImGui::Separator();
    ImGui::Text("YM2163 Chips");
    // Connect / Disconnect buttons
    {
        bool isYM = (SPFMManager::GetActiveChipType() == SPFMManager::CHIP_YM2163);
        if (isYM) {
            if (ImGui::Button("Disconnect##ym", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_NONE);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set chip type to None, send SPFM reset");
        } else {
            if (ImGui::Button("Connect##ym", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_YM2163);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch all slots to YM2163 mode");
        }
    }
    // Slot0 always on
    { bool alwaysOn = true; ImGui::BeginDisabled(); ImGui::Checkbox("Slot0 (1st @1MHz)", &alwaysOn); ImGui::EndDisabled(); }

    // Slot1
    if (ImGui::Checkbox("Slot1 (2nd @1MHz)", &YM2163::g_enableSecondYM2163)) {
        if (YM2163::g_ftHandle) {
            if (YM2163::g_enableSecondYM2163) YM2163::init_single_ym2163(1);
            else { for (int i = 4; i < 8; i++) if (YM2163::g_channels[i].active) YM2163::stop_note(i); }
        }
        Config::SaveSlotConfigToINI();
    }

    // Slot2
    if (ImGui::Checkbox("Slot2 (3rd @1MHz)", &YM2163::g_enableThirdYM2163)) {
        if (YM2163::g_ftHandle) {
            if (YM2163::g_enableThirdYM2163) YM2163::init_single_ym2163(2);
            else { for (int i = 8; i < 12; i++) if (YM2163::g_channels[i].active) YM2163::stop_note(i); }
        }
        Config::SaveSlotConfigToINI();
    }

    // Slot3 1MHz
    {
        bool slot3_1MHz = YM2163::g_enableFourthYM2163 && !YM2163::g_enableSlot3_2MHz;
        if (ImGui::Checkbox("Slot3 (4th @1MHz)", &slot3_1MHz)) {
            if (slot3_1MHz) {
                YM2163::g_enableFourthYM2163 = true; YM2163::g_enableSlot3_2MHz = false;
                if (YM2163::g_ftHandle) YM2163::init_single_ym2163(3);
            } else {
                YM2163::g_enableFourthYM2163 = false;
                for (int i = 12; i < 16; i++) if (YM2163::g_channels[i].active) YM2163::stop_note(i);
            }
            Config::SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable Slot3 as normal 1MHz chip\nPolyphony: +4 channels");
    }
    // Slot3 2MHz
    if (ImGui::Checkbox("Slot3 (4th @2MHz)", &YM2163::g_enableSlot3_2MHz)) {
        if (YM2163::g_enableSlot3_2MHz) {
            YM2163::g_enableFourthYM2163 = true;
            if (YM2163::g_ftHandle) YM2163::init_single_ym2163(3);
        } else {
            YM2163::g_enableFourthYM2163 = false;
            for (int i = 12; i < 16; i++) if (YM2163::g_channels[i].active) YM2163::stop_note(i);
        }
        Config::SaveSlotConfigToINI();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable Slot3 as 2MHz chip for C8-B8 octave\nDedicated to high-octave notes only");

    // Slot3 Overflow
    {
        bool overflow_enabled = YM2163::g_enableSlot3Overflow;
        if (ImGui::Checkbox("Slot3 Overflow (C4-B7)", &overflow_enabled)) {
            YM2163::g_enableSlot3Overflow = overflow_enabled;
            Config::SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "When enabled: overflow C4-B7 to Slot3 2MHz when normal channels >=80%% full\n"
            "When disabled: Slot3 2MHz only plays C8-B8 high octave");
    }

    // Slot3 2MHz Range
    ImGui::Text("Slot3 2MHz Range");
    {
        const char* rangeOptions[] = { "C8-B8 only", "C7-B8" };
        if (ImGui::Combo("##Slot3_2MHz_Range", &YM2163::g_slot3_2MHz_Range, rangeOptions, IM_ARRAYSIZE(rangeOptions)))
            Config::SaveSlotConfigToINI();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "C8-B8 only: Slot3 2MHz plays only C8-B8 high octave\n"
            "C7-B8: Slot3 2MHz plays both C7-B7 and C8-B8 (default)");
    }

    // All Slots button
    if (ImGui::Button("All Slots", ImVec2(-1, 0))) {
        YM2163::g_enableSecondYM2163 = true; YM2163::g_enableThirdYM2163 = true;
        YM2163::g_enableFourthYM2163 = true; YM2163::g_enableSlot3_2MHz = true;
        if (YM2163::g_ftHandle) {
            YM2163::init_single_ym2163(1); YM2163::init_single_ym2163(2); YM2163::init_single_ym2163(3);
        }
        Config::SaveSlotConfigToINI();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable all 4 slots\nSlot3 uses 2MHz mode for C8-B8");

    ImGui::Separator();
    // Instrument Editor
    ImGui::Text("Instrument Editor");
    static char instrumentPreview[128];
    if (Config::g_instrumentConfigs.count(g_selectedInstrument) > 0)
        snprintf(instrumentPreview, sizeof(instrumentPreview), "%d: %s", g_selectedInstrument, Config::g_instrumentConfigs[g_selectedInstrument].name.c_str());
    else
        snprintf(instrumentPreview, sizeof(instrumentPreview), "%d: (undefined)", g_selectedInstrument);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##InstrumentSelect", instrumentPreview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < 128; i++) {
            char label[128];
            if (Config::g_instrumentConfigs.count(i) > 0)
                snprintf(label, sizeof(label), "%d: %s", i, Config::g_instrumentConfigs[i].name.c_str());
            else
                snprintf(label, sizeof(label), "%d: (undefined)", i);
            bool isSelected = (g_selectedInstrument == i);
            if (ImGui::Selectable(label, isSelected)) {
                g_selectedInstrument = i;
                LoadInstrumentConfigToUI(i);
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f && g_selectedInstrument > 0) { g_selectedInstrument--; LoadInstrumentConfigToUI(g_selectedInstrument); }
        else if (wheel < 0.0f && g_selectedInstrument < 127) { g_selectedInstrument++; LoadInstrumentConfigToUI(g_selectedInstrument); }
    }

    float btnWidth = (ImGui::GetContentRegionAvail().x - 5.0f) / 2.0f;
    if (ImGui::Button("Load Config", ImVec2(btnWidth, 0))) LoadInstrumentConfigToUI(g_selectedInstrument);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load selected instrument config to UI");
    ImGui::SameLine();
    if (ImGui::Button("Save Config", ImVec2(btnWidth, 0))) SaveInstrumentConfig(g_selectedInstrument);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save current Wave/Envelope to selected instrument");

    ImGui::Spacing();
    if (ImGui::Button("Tuning", ImVec2(-1, 0))) MidiPlayer::g_showTuningWindow = !MidiPlayer::g_showTuningWindow;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open frequency tuning window");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Envelope");
    for (int i = 0; i < 4; i++) {
        if (ImGui::RadioButton(g_envelopeNames[i], YM2163::g_currentEnvelope == i))
            YM2163::g_currentEnvelope = i;
        if (i % 2 == 0 && i < 3) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Pedal Mode");
    if (ImGui::RadioButton("Disabled##PedalMode", YM2163::g_pedalMode == 0)) YM2163::g_pedalMode = 0;
    if (ImGui::RadioButton("Piano Pedal##PedalMode", YM2163::g_pedalMode == 1)) YM2163::g_pedalMode = 1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pedal Down: Fast envelope\nPedal Up: Decay envelope");
    if (ImGui::RadioButton("Organ Pedal##PedalMode", YM2163::g_pedalMode == 2)) YM2163::g_pedalMode = 2;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pedal Down: Slow envelope\nPedal Up: Medium envelope");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Timbre");
    for (int i = 1; i <= 5; i++) {
        if (ImGui::RadioButton(g_timbreNames[i], YM2163::g_currentTimbre == i))
            YM2163::g_currentTimbre = i;
        if (i % 2 == 1 && i < 5) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Drums (Numpad 1-5)");
    static const char* drumBtnNames[] = {"BD", "HC", "SDN", "HHO", "HHD"};
    static const uint8_t drumBtnBits[] = {0x10, 0x08, 0x04, 0x02, 0x01};
    for (int i = 0; i < 5; i++) {
        ImGui::PushID(i);
        if (ImGui::Button(drumBtnNames[i], ImVec2(45, 40)))
            YM2163::play_drum(drumBtnBits[i]);
        if (i < 4) ImGui::SameLine();
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::EndChild();
}

// ===== RenderLog =====

void RenderLog() {
    static bool g_logExpanded = false;

    if (ImGui::CollapsingHeader("Log", g_logExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        g_logExpanded = true;
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &g_autoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##Log")) {
            YM2163::ClearLogBuffer();
            g_logDisplayBuffer[0] = '\0';
            g_lastLogSize = 0;
        }

        const std::string& logBuf = YM2163::GetLogBuffer();
        size_t copyLen = (logBuf.length() < sizeof(g_logDisplayBuffer) - 1) ?
                         logBuf.length() : sizeof(g_logDisplayBuffer) - 1;
        memcpy(g_logDisplayBuffer, logBuf.c_str(), copyLen);
        g_logDisplayBuffer[copyLen] = '\0';

        bool log_changed = (logBuf.length() != g_lastLogSize);
        g_lastLogSize = logBuf.length();
        if (g_autoScroll && log_changed) g_logScrollToBottom = true;

        float logHeight = 150;
        ImGui::BeginChild("LogScrollRegion", ImVec2(0, logHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 text_size = ImGui::CalcTextSize(g_logDisplayBuffer, NULL, false, -1.0f);
        float line_height = ImGui::GetTextLineHeightWithSpacing();
        float min_visible_height = ImGui::GetContentRegionAvail().y;
        float input_height = (text_size.y > min_visible_height) ? text_size.y + line_height * 2 : min_visible_height;
        ImGui::InputTextMultiline("##LogText", g_logDisplayBuffer, sizeof(g_logDisplayBuffer),
            ImVec2(-1, input_height), ImGuiInputTextFlags_ReadOnly);
        if (g_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            g_logScrollToBottom = false;
        }
        ImGui::EndChild();
    } else {
        g_logExpanded = false;
    }

    ImGui::Spacing();
}

// ===== RenderTuningWindow =====

void RenderTuningWindow() {
    if (!MidiPlayer::g_showTuningWindow) return;

    ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Frequency Tuning", &MidiPlayer::g_showTuningWindow)) {
        ImGui::Text("Adjust YM2163 frequency values (FNUM) for each note");
        ImGui::Text("Range: 0-2047 | Mouse wheel: +/-10 per step");
        ImGui::Separator();
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 5.0f) / 2.0f;
        if (ImGui::Button("Load All Frequencies", ImVec2(btnWidth, 0))) {
            Config::LoadFrequenciesFromINI();
            YM2163::log_command("All frequencies loaded from INI");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load all frequency values from ym2163_config.ini");
        ImGui::SameLine();
        if (ImGui::Button("Save All Frequencies", ImVec2(btnWidth, 0))) {
            Config::SaveFrequenciesToINI();
            YM2163::log_command("All frequencies saved to INI");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save all frequency values to ym2163_config.ini");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Base Frequencies (C3-C6 octaves)");
        ImGui::Separator();
        for (int i = 0; i < 12; i++) {
            ImGui::PushID(i);
            ImGui::Text("%s:", YM2163::g_noteNames[i]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("", &YM2163::g_fnums[i], 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
                if (YM2163::g_fnums[i] < 0) YM2163::g_fnums[i] = 0;
                if (YM2163::g_fnums[i] > 2047) YM2163::g_fnums[i] = 2047;
                YM2163::log_command("Base Freq updated: %s = %d", YM2163::g_noteNames[i], YM2163::g_fnums[i]);
            }
            if (ImGui::IsItemHovered()) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    YM2163::g_fnums[i] += (int)(wheel * 10);
                    if (YM2163::g_fnums[i] < 0) YM2163::g_fnums[i] = 0;
                    if (YM2163::g_fnums[i] > 2047) YM2163::g_fnums[i] = 2047;
                    YM2163::log_command("Base Freq updated: %s = %d", YM2163::g_noteNames[i], YM2163::g_fnums[i]);
                }
            }
            if ((i + 1) % 6 != 0) ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "B2 Frequency (Lowest Note)");
        ImGui::Separator();
        ImGui::PushID(100);
        ImGui::Text("B2:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("", &YM2163::g_fnum_b2, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            if (YM2163::g_fnum_b2 < 0) YM2163::g_fnum_b2 = 0;
            if (YM2163::g_fnum_b2 > 2047) YM2163::g_fnum_b2 = 2047;
            YM2163::log_command("B2 Freq updated: B2 = %d", YM2163::g_fnum_b2);
        }
        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                YM2163::g_fnum_b2 += (int)(wheel * 10);
                if (YM2163::g_fnum_b2 < 0) YM2163::g_fnum_b2 = 0;
                if (YM2163::g_fnum_b2 > 2047) YM2163::g_fnum_b2 = 2047;
                YM2163::log_command("B2 Freq updated: B2 = %d", YM2163::g_fnum_b2);
            }
        }
        ImGui::PopID();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "C7 Octave Frequencies (C7-B7)");
        ImGui::Separator();
        for (int i = 0; i < 12; i++) {
            ImGui::PushID(200 + i);
            ImGui::Text("%s7:", YM2163::g_noteNames[i]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("", &YM2163::g_fnums_c7[i], 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
                if (YM2163::g_fnums_c7[i] < 0) YM2163::g_fnums_c7[i] = 0;
                if (YM2163::g_fnums_c7[i] > 2047) YM2163::g_fnums_c7[i] = 2047;
                YM2163::log_command("C7 Freq updated: %s7 = %d", YM2163::g_noteNames[i], YM2163::g_fnums_c7[i]);
            }
            if (ImGui::IsItemHovered()) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    YM2163::g_fnums_c7[i] += (int)(wheel * 10);
                    if (YM2163::g_fnums_c7[i] < 0) YM2163::g_fnums_c7[i] = 0;
                    if (YM2163::g_fnums_c7[i] > 2047) YM2163::g_fnums_c7[i] = 2047;
                    YM2163::log_command("C7 Freq updated: %s7 = %d", YM2163::g_noteNames[i], YM2163::g_fnums_c7[i]);
                }
            }
            if ((i + 1) % 6 != 0) ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::Spacing();
    }
    ImGui::End();
}

void RenderMIDIFolderHistory() {
    ImGui::Text("MIDI Folder History");
    ImGui::SameLine();
    if (ImGui::Button("Clear All##History"))
        MidiPlayer::ClearMIDIFolderHistory();
    ImGui::Separator();

    float historyHeight = ImGui::GetContentRegionAvail().y - 5;
    ImGui::BeginChild("HistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

    if (MidiPlayer::g_midiFolderHistory.empty()) {
        ImGui::TextDisabled("No MIDI folder history yet...");
        ImGui::TextDisabled("Navigate to folders containing MIDI files to build history.");
    } else {
        for (int i = 0; i < (int)MidiPlayer::g_midiFolderHistory.size(); i++) {
            const std::string& path = MidiPlayer::g_midiFolderHistory[i];
            ImGui::PushID(i);
            size_t lastSlash = path.find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
            if (ImGui::Selectable(folderName.c_str(), false))
                MidiPlayer::NavigateToPath(path.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove from history"))
                    MidiPlayer::RemoveMIDIFolderHistoryEntry(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}






















