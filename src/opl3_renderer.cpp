// opl3_renderer.cpp - OPL3 (YMF262) UI rendering
// Ported from YM2163 Piano v12. Hardware not yet connected - UI placeholders only.

#include "opl3_renderer.h"
#include "gui_renderer.h"
#include "midi_player.h"
#include "imgui/imgui.h"

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <string.h>

// ===== OPL3 File Browser State (independent from MIDI/VGM tabs) =====
static char  opl3_currentPath[MAX_PATH]   = {0};
static char  opl3_pathInput[MAX_PATH]     = {0};
static std::vector<std::string> opl3_folderHistory;
static std::vector<MidiPlayer::FileEntry> opl3_fileList;
static int   opl3_selectedFileIndex      = -1;
static bool  opl3_pathEditMode           = false;
static bool  opl3_pathEditModeJustActivated = false;
static std::map<std::string, float> opl3_pathScrollPositions;
static std::string opl3_lastExitedFolder;
static std::string opl3_currentPlayingFilePath;
static std::map<int, MidiPlayer::TextScrollState> opl3_textScrollStates;
static int   opl3_hoveredFileIndex       = -1;
static int   opl3_currentPlayingIndex    = -1;
static std::vector<std::string> opl3_navHistory;
static int   opl3_navPos                 = -1;
static bool  opl3_navigating             = false;
static bool  opl3_autoPlayNext           = true;
static bool  opl3_isSequentialPlayback   = true;
static const char* k_opl3HistoryFile     = "opl3_config.ini";

static void OPL3_GetExeDir(char* out, int maxLen) {
    GetModuleFileNameA(NULL, out, maxLen);
    char* slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
}

static void OPL3_SaveFolderHistory() {
    char exeDir[MAX_PATH];
    OPL3_GetExeDir(exeDir, MAX_PATH);
    char histPath[MAX_PATH];
    snprintf(histPath, MAX_PATH, "%s%s", exeDir, k_opl3HistoryFile);
    WritePrivateProfileStringA("Opl3FolderHistory", "CurrentPath", opl3_currentPath, histPath);
    int count = 0;
    for (const auto& h : opl3_folderHistory) {
        char key[16];
        sprintf(key, "History%d", count++);
        WritePrivateProfileStringA("Opl3FolderHistory", key, h.c_str(), histPath);
    }
    WritePrivateProfileStringA("Opl3FolderHistory", "Count", std::to_string(count).c_str(), histPath);
}

static void OPL3_LoadFolderHistory() {
    char exeDir[MAX_PATH];
    OPL3_GetExeDir(exeDir, MAX_PATH);
    char histPath[MAX_PATH];
    snprintf(histPath, MAX_PATH, "%s%s", exeDir, k_opl3HistoryFile);
    GetPrivateProfileStringA("Opl3FolderHistory", "CurrentPath", "", opl3_currentPath, MAX_PATH, histPath);
    strncpy(opl3_pathInput, opl3_currentPath, MAX_PATH);
    int count = GetPrivateProfileIntA("Opl3FolderHistory", "Count", 0, histPath);
    for (int i = 0; i < count; i++) {
        char key[16], line[MAX_PATH];
        sprintf(key, "History%d", i);
        GetPrivateProfileStringA("Opl3FolderHistory", key, "", line, MAX_PATH, histPath);
        if (strlen(line) > 0)
            opl3_folderHistory.push_back(line);
    }
    if (opl3_currentPath[0] == 0)
        GetCurrentDirectoryA(MAX_PATH, opl3_currentPath);
}

static void OPL3_RefreshFileList() {
    opl3_fileList.clear();
    opl3_textScrollStates.clear();
    if (strlen(opl3_currentPath) > 3) {
        MidiPlayer::FileEntry parent;
        parent.name = ".."; parent.fullPath = ""; parent.isDirectory = true;
        opl3_fileList.push_back(parent);
    }
    std::string searchPath = std::string(opl3_currentPath) + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    std::vector<MidiPlayer::FileEntry> dirs, files;
    do {
        if (strcmp(fd.cFileName, ".") == 0) continue;
        MidiPlayer::FileEntry e;
        e.name = fd.cFileName;
        e.fullPath = std::string(opl3_currentPath) + "\\" + fd.cFileName;
        e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (e.isDirectory) dirs.push_back(e);
        else {
            // Accept MIDI files
            const char* dot = strrchr(fd.cFileName, '.');
            if (dot && (_stricmp(dot,".mid")==0 || _stricmp(dot,".midi")==0))
                files.push_back(e);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    std::sort(dirs.begin(), dirs.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b){ return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::sort(files.begin(), files.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b){ return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    for (auto& d : dirs)  opl3_fileList.push_back(d);
    for (auto& f : files) opl3_fileList.push_back(f);
}

static void OPL3_NavigateTo(const char* path) {
    char canon[MAX_PATH];
    if (_fullpath(canon, path, MAX_PATH) == nullptr)
        snprintf(canon, MAX_PATH, "%s", path);
    snprintf(opl3_currentPath, MAX_PATH, "%s", canon);
    snprintf(opl3_pathInput,   MAX_PATH, "%s", canon);
    opl3_selectedFileIndex = -1;
    opl3_textScrollStates.clear();
    if (!opl3_navigating) {
        if (opl3_navPos < (int)opl3_navHistory.size() - 1)
            opl3_navHistory.erase(opl3_navHistory.begin() + opl3_navPos + 1, opl3_navHistory.end());
        opl3_navHistory.push_back(canon);
        opl3_navPos++;
    }
    opl3_navigating = false;
    // Add to folder history (avoid duplicates)
    auto it = std::find(opl3_folderHistory.begin(), opl3_folderHistory.end(), std::string(canon));
    if (it != opl3_folderHistory.end()) opl3_folderHistory.erase(it);
    opl3_folderHistory.insert(opl3_folderHistory.begin(), canon);
    if ((int)opl3_folderHistory.size() > 20) opl3_folderHistory.resize(20);
    OPL3_RefreshFileList();
    OPL3_SaveFolderHistory();
}

static void OPL3_NavBack() {
    if (opl3_navPos > 0) {
        opl3_navPos--;
        opl3_navigating = true;
        OPL3_NavigateTo(opl3_navHistory[opl3_navPos].c_str());
    }
}

static void OPL3_NavForward() {
    if (opl3_navPos < (int)opl3_navHistory.size() - 1) {
        opl3_navPos++;
        opl3_navigating = true;
        OPL3_NavigateTo(opl3_navHistory[opl3_navPos].c_str());
    }
}

static void OPL3_NavToParent() {
    char parentPath[MAX_PATH];
    strncpy(parentPath, opl3_currentPath, MAX_PATH);
    int len = (int)strlen(parentPath);
    while (len > 0 && parentPath[len-1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        opl3_lastExitedFolder = std::string(lastSlash + 1);
        *lastSlash = '\0';
        OPL3_NavigateTo(parentPath);
    }
}

// ===== OPL3 State =====
static int  opl3_currentOctave           = 2;
static int  opl3_currentTimbre           = 4;
static int  opl3_currentEnvelope         = 1;
static int  opl3_currentVolume           = 0;
static bool opl3_enableVelocityMapping   = true;
static bool opl3_enableSustainPedal      = true;
static bool opl3_enableAutoSkipSilence   = true;
static bool opl3_pianoKeyPressed[73]     = {false};

static const char* opl3_timbreNames[] = {
    "", "String", "Organ", "Clarinet", "Piano", "Harpsichord"
};
static const char* opl3_envelopeNames[] = {
    "Decay", "Fast", "Medium", "Slow"
};
static const char* opl3_volumeNames[] = {
    "0dB", "-6dB", "-12dB", "Mute"
};

// ===== Render Functions =====

void RenderOPL3Controls() {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "YMF262 (OPL3)");
    ImGui::Separator();
    ImGui::TextDisabled("[Hardware not yet connected]");
    ImGui::Spacing();

    ImGui::Text("Timbre:");
    for (int i = 1; i <= 5; i++) {
        if (i > 1) ImGui::SameLine();
        bool sel = (opl3_currentTimbre == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s##opl3t%d", opl3_timbreNames[i], i);
        if (ImGui::Button(lbl)) opl3_currentTimbre = i;
        if (sel) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Text("Envelope:");
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool sel = (opl3_currentEnvelope == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s##opl3e%d", opl3_envelopeNames[i], i);
        if (ImGui::Button(lbl)) opl3_currentEnvelope = i;
        if (sel) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Text("Volume:");
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool sel = (opl3_currentVolume == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        char lbl[32]; snprintf(lbl, sizeof(lbl), "%s##opl3v%d", opl3_volumeNames[i], i);
        if (ImGui::Button(lbl)) opl3_currentVolume = i;
        if (sel) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Octave: %d", opl3_currentOctave);
    ImGui::SameLine();
    if (ImGui::Button("-##opl3oct") && opl3_currentOctave > 0) opl3_currentOctave--;
    ImGui::SameLine();
    if (ImGui::Button("+##opl3oct") && opl3_currentOctave < 7) opl3_currentOctave++;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Checkbox("Velocity Mapping##opl3", &opl3_enableVelocityMapping);
    ImGui::Checkbox("Sustain Pedal##opl3",    &opl3_enableSustainPedal);
    ImGui::Checkbox("Auto-Skip Silence##opl3", &opl3_enableAutoSkipSilence);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "OPL3 hardware support");
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "coming in future version.");
}

void RenderOPL3PianoKeyboard() {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    int numWhiteKeys = 52;
    float whiteKeyWidth = availW / numWhiteKeys;
    if (whiteKeyWidth < 8.0f) whiteKeyWidth = 8.0f;
    float whiteKeyHeight = ImGui::GetContentRegionAvail().y - 4;
    if (whiteKeyHeight < 40.0f) whiteKeyHeight = 40.0f;
    float blackKeyHeight = whiteKeyHeight * 0.6f;

    static const bool isBlack[] = {false,true,false,true,false,false,true,false,true,false,true,false};
    int wkIdx = 0;
    for (int octave = 0; octave <= 7; octave++) {
        for (int note = 0; note < 12; note++) {
            if (isBlack[note]) continue;
            int keyIdx = octave * 12 + note;
            float x = p.x + wkIdx * whiteKeyWidth;
            ImU32 col = opl3_pianoKeyPressed[keyIdx % 73] ?
                IM_COL32(100,220,100,255) : IM_COL32(255,255,255,255);
            draw_list->AddRectFilled(ImVec2(x,p.y), ImVec2(x+whiteKeyWidth-1,p.y+whiteKeyHeight), col);
            draw_list->AddRect(ImVec2(x,p.y), ImVec2(x+whiteKeyWidth,p.y+whiteKeyHeight), IM_COL32(0,0,0,255));
            wkIdx++;
        }
    }
    wkIdx = 0;
    int wkTotal = 0;
    for (int octave = 0; octave <= 7; octave++) {
        for (int note = 0; note < 12; note++) {
            if (!isBlack[note]) { wkTotal++; continue; }
            float x = p.x + (wkTotal - 1) * whiteKeyWidth + whiteKeyWidth * 0.6f;
            float bw = whiteKeyWidth * 0.7f;
            draw_list->AddRectFilled(ImVec2(x,p.y), ImVec2(x+bw,p.y+blackKeyHeight), IM_COL32(20,20,20,255));
            draw_list->AddRect(ImVec2(x,p.y), ImVec2(x+bw,p.y+blackKeyHeight), IM_COL32(0,0,0,255));
        }
    }
    (void)wkIdx;
    (void)blackKeyHeight;
    ImGui::Dummy(ImVec2(availW, whiteKeyHeight));
}

void RenderOPL3LevelMeters() {
    ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "OPL3 Level Meters (not active)");
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y - 4;
    int numMeters = 18;
    float meterW = (availW - (numMeters-1)*2) / numMeters;
    for (int i = 0; i < numMeters; i++) {
        float mx = p.x + i*(meterW+2);
        draw_list->AddRectFilled(ImVec2(mx,p.y), ImVec2(mx+meterW,p.y+availH), IM_COL32(30,30,30,255));
        draw_list->AddRect(ImVec2(mx,p.y), ImVec2(mx+meterW,p.y+availH), IM_COL32(80,80,80,255));
        char lbl[4]; snprintf(lbl,sizeof(lbl),"%d",i+1);
        draw_list->AddText(ImVec2(mx+1,p.y+2), IM_COL32(120,120,120,255), lbl);
    }
    ImGui::Dummy(ImVec2(availW, availH));
}

void RenderOPL3ChannelStatus() {
    ImGui::TextColored(ImVec4(0.4f,1.0f,0.6f,1.0f), "OPL3 Channel Status");
    ImGui::Separator();
    ImGui::TextDisabled("18 melody channels + 5 percussion channels");
    ImGui::Spacing();
    for (int i = 0; i < 18; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Ch%02d: ---##opl3ch%d", i+1, i);
        ImGui::TextDisabled("%s", label);
        if (i % 6 != 5) ImGui::SameLine(0, 10);
    }
    ImGui::NewLine();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "OPL3 not yet connected.");
}

void RenderOPL3MIDIPlayer() {
    // Initialize on first call
    static bool s_opl3BrowserInit = false;
    if (!s_opl3BrowserInit) {
        OPL3_LoadFolderHistory();
        OPL3_RefreshFileList();
        s_opl3BrowserInit = true;
    }

    ImGui::Text("OPL3 MIDI Player");
    ImGui::Separator();

    float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
    if (ImGui::Button("Play##opl3", ImVec2(buttonWidth, 30)))  MidiPlayer::PlayMIDI();
    ImGui::SameLine();
    if (ImGui::Button("Pause##opl3", ImVec2(buttonWidth, 30))) MidiPlayer::PauseMIDI();
    ImGui::SameLine();
    if (ImGui::Button("Stop##opl3", ImVec2(buttonWidth, 30)))  MidiPlayer::StopMIDI();

    float navButtonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
    if (ImGui::Button("<< Prev##opl3", ImVec2(navButtonWidth, 25))) MidiPlayer::PlayPreviousMIDI();
    ImGui::SameLine();
    if (ImGui::Button("Next >>##opl3", ImVec2(navButtonWidth, 25))) MidiPlayer::PlayNextMIDI();

    ImGui::Checkbox("Auto-play next##opl3", &opl3_autoPlayNext);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically play next track when current finishes");
    ImGui::SameLine();
    const char* modeText = opl3_isSequentialPlayback ? "Sequential" : "Random";
    if (ImGui::Button(modeText, ImVec2(85, 0)))
        opl3_isSequentialPlayback = !opl3_isSequentialPlayback;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to toggle: Sequential (loop) / Random");

    ImGui::Spacing();
    if (MidiPlayer::g_midiPlayer.isPlaying && !MidiPlayer::g_midiPlayer.isPaused)
        ImGui::TextColored(ImVec4(0.0f,1.0f,0.0f,1.0f), "Playing:");
    else if (MidiPlayer::g_midiPlayer.isPaused)
        ImGui::TextColored(ImVec4(1.0f,1.0f,0.0f,1.0f), "Paused:");
    else
        ImGui::Text("Ready:");
    ImGui::SameLine();
    if (!MidiPlayer::g_midiPlayer.currentFileName.empty()) {
        const char* fn = strrchr(MidiPlayer::g_midiPlayer.currentFileName.c_str(), '\\');
        if (!fn) fn = strrchr(MidiPlayer::g_midiPlayer.currentFileName.c_str(), '/');
        ImGui::Text("%s", fn ? fn+1 : MidiPlayer::g_midiPlayer.currentFileName.c_str());
    } else {
        ImGui::TextDisabled("(no file loaded)");
    }

    // Progress bar with seek (same as MIDI tab)
    if (!MidiPlayer::g_midiPlayer.currentFileName.empty() && MidiPlayer::g_midiPlayer.midiFile.getEventCount(0) > 0) {
        smf::MidiEventList& track = MidiPlayer::g_midiPlayer.midiFile[0];
        int lastMidiTick = (track.size() > 0) ? track[track.size()-1].tick : 0;
        double microsPerTick = MidiPlayer::g_midiPlayer.tempo / (double)MidiPlayer::g_midiPlayer.ticksPerQuarterNote;
        double totalTimeMicros = (double)lastMidiTick * microsPerTick;
        float progress = (totalTimeMicros > 0) ? (float)(MidiPlayer::g_midiPlayer.accumulatedTime / totalTimeMicros) : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        double currentTimeMicros = MidiPlayer::g_midiPlayer.accumulatedTime;
        int curSec = (int)(currentTimeMicros/1000000.0); int curMin = curSec/60; curSec %= 60;
        int totSec = (int)(totalTimeMicros/1000000.0); int totMin = totSec/60; totSec %= 60;
        char curStr[32], totStr[32];
        sprintf(curStr, "%02d:%02d", curMin, curSec);
        sprintf(totStr, "%02d:%02d", totMin, totSec);
        ImGui::Text("%s / %s", curStr, totStr);
        ImGui::ProgressBar(progress, ImVec2(ImGui::GetContentRegionAvail().x, 20), "");
    } else {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // File browser
    ImGui::Text("File Browser");
    ImGui::Separator();

    // Navigation buttons
    if (ImGui::Button("<##opl3", ImVec2(25, 0))) OPL3_NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">##opl3", ImVec2(25, 0))) OPL3_NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^##opl3", ImVec2(25, 0))) OPL3_NavToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    // Breadcrumb path bar
    if (!opl3_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = MidiPlayer::SplitPath(opl3_currentPath);
        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;
        for (size_t i = 0; i < segments.size(); i++) {
            if (i == 0) accumulatedPath = segments[i];
            else { if (accumulatedPath.back() != '\\') accumulatedPath += "\\"; accumulatedPath += segments[i]; }
            accumulatedPaths.push_back(accumulatedPath);
            ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
            float bw2 = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
            buttonWidths.push_back(bw2);
        }
        float separatorWidth = ImGui::CalcTextSize(">").x + itemSpacingX * 2.0f;
        float ellipsisBW = ImGui::CalcTextSize("...").x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        float ellipsisWidth = ellipsisBW + separatorWidth;
        float safeAvailWidth = availWidth - 10.0f;
        int firstVisibleSegment = (int)segments.size() - 1;
        float usedWidth = (segments.size() > 0) ? buttonWidths.back() : 0.0f;
        for (int i = (int)segments.size() - 2; i >= 0; i--) {
            float sw = buttonWidths[i] + separatorWidth;
            float ne = (i > 0) ? ellipsisWidth : 0.0f;
            if (usedWidth + sw + ne > safeAvailWidth) break;
            else { usedWidth += sw; firstVisibleSegment = i; }
        }
        ImVec2 barStartPos = ImGui::GetCursorScreenPos();
        float barHeight = ImGui::GetFrameHeight();
        ImGui::BeginGroup();
        if (firstVisibleSegment > 0) {
            if (ImGui::Button("...##opl3ellipsis")) { opl3_pathEditMode = true; opl3_pathEditModeJustActivated = true; strncpy(opl3_pathInput, opl3_currentPath, MAX_PATH); }
            ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##opl3seg" + std::to_string(i);
            if (ImGui::Button(btnId.c_str())) OPL3_NavigateTo(accumulatedPaths[i].c_str());
            if (i < (int)segments.size() - 1) { ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine(); }
        }
        ImGui::EndGroup();
        ImVec2 barEndPos = ImGui::GetItemRectMax();
        float emptySpaceWidth = barStartPos.x + availWidth - barEndPos.x;
        if (emptySpaceWidth > 0) {
            ImGui::SameLine();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(barEndPos.x, barStartPos.y), ImVec2(barEndPos.x + emptySpaceWidth, barEndPos.y),
                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::InvisibleButton("##opl3pathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) { opl3_pathEditMode = true; opl3_pathEditModeJustActivated = true; strncpy(opl3_pathInput, opl3_currentPath, MAX_PATH); }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##opl3PathInput", opl3_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
            OPL3_NavigateTo(opl3_pathInput); opl3_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            opl3_pathEditMode = false; opl3_pathEditModeJustActivated = false;
            strncpy(opl3_pathInput, opl3_currentPath, MAX_PATH);
        } else if (!opl3_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            opl3_pathEditMode = false;
            strncpy(opl3_pathInput, opl3_currentPath, MAX_PATH);
        }
        if (opl3_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            opl3_pathEditModeJustActivated = false;
        }
    }

    // Folder history dropdown
    if (!opl3_folderHistory.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##opl3Hist", "History", ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)opl3_folderHistory.size(); i++) {
                size_t lastSlash = opl3_folderHistory[i].find_last_of("\\/");
                std::string folderName = (lastSlash != std::string::npos) ? opl3_folderHistory[i].substr(lastSlash+1) : opl3_folderHistory[i];
                if (ImGui::Selectable(folderName.c_str(), false)) OPL3_NavigateTo(opl3_folderHistory[i].c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opl3_folderHistory[i].c_str());
            }
            ImGui::EndCombo();
        }
    }

    // File list with full MIDI-style scrolling text
    ImGui::BeginChild("OPL3FileList", ImVec2(-1, 0), true);

    std::string opl3PathStr(opl3_currentPath);
    if (strlen(opl3_currentPath) > 0)
        opl3_pathScrollPositions[opl3PathStr] = ImGui::GetScrollY();
    static std::string opl3_lastRestoredPath;
    if (opl3PathStr != opl3_lastRestoredPath && opl3_pathScrollPositions.count(opl3PathStr) > 0) {
        ImGui::SetScrollY(opl3_pathScrollPositions[opl3PathStr]);
        opl3_lastRestoredPath = opl3PathStr;
    }

    opl3_hoveredFileIndex = -1;
    for (int i = 0; i < (int)opl3_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = opl3_fileList[i];
        bool isSelected = (opl3_selectedFileIndex == i);
        bool isExitedFolder = (!opl3_lastExitedFolder.empty() && entry.isDirectory && entry.name == opl3_lastExitedFolder);
        bool isPlayingPath = false;
        if (!opl3_currentPlayingFilePath.empty() && entry.isDirectory) {
            std::string ep = entry.fullPath;
            if (!ep.empty() && ep.back() != '\\') ep += "\\";
            if (opl3_currentPlayingFilePath.find(ep) == 0) isPlayingPath = true;
        }
        bool isPlayingFile = (!entry.isDirectory && entry.fullPath == opl3_currentPlayingFilePath);

        std::string label;
        if (entry.name == "..") label = "[UP] " + entry.name;
        else if (entry.isDirectory) label = "[DIR] " + entry.name;
        else label = entry.name;

        if (isExitedFolder) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        else if (isPlayingPath || isPlayingFile) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.5f, 1.0f));

        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        float availWidth = ImGui::GetContentRegionAvail().x;
        bool needsScrolling = textSize.x > availWidth;
        bool isHovered = false;

        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImVec2 itemSize = ImVec2(availWidth, ImGui::GetTextLineHeightWithSpacing());
        ImGui::InvisibleButton(("##opl3item" + std::to_string(i)).c_str(), itemSize);
        isHovered = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
            opl3_selectedFileIndex = i;
            if (entry.name == "..") {
                OPL3_NavToParent();
            } else if (entry.isDirectory) {
                opl3_lastExitedFolder.clear();
                OPL3_NavigateTo(entry.fullPath.c_str());
            } else {
                opl3_currentPlayingIndex = i;
                opl3_currentPlayingFilePath = entry.fullPath;
                MidiPlayer::LoadMIDIFile(entry.fullPath.c_str());
                MidiPlayer::PlayMIDI();
            }
        }

        if (needsScrolling && (isSelected || isExitedFolder || isHovered)) {
            if (!opl3_textScrollStates.count(i)) {
                MidiPlayer::TextScrollState st;
                st.scrollOffset = 0.0f; st.scrollDirection = 1.0f; st.pauseTimer = 1.0f;
                st.lastUpdateTime = std::chrono::steady_clock::now();
                opl3_textScrollStates[i] = st;
            }
            MidiPlayer::TextScrollState& ss = opl3_textScrollStates[i];
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - ss.lastUpdateTime).count();
            ss.lastUpdateTime = now;
            if (ss.pauseTimer > 0.0f) ss.pauseTimer -= dt;
            else {
                ss.scrollOffset += ss.scrollDirection * 30.0f * dt;
                float maxScroll = textSize.x - availWidth + 20.0f;
                if (ss.scrollOffset >= maxScroll) { ss.scrollOffset = maxScroll; ss.scrollDirection = -1.0f; ss.pauseTimer = 1.0f; }
                else if (ss.scrollOffset <= 0.0f) { ss.scrollOffset = 0.0f; ss.scrollDirection = 1.0f; ss.pauseTimer = 1.0f; }
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (isSelected) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x+availWidth, cursorPos.y+itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x+availWidth, cursorPos.y+itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            dl->PushClipRect(cursorPos, ImVec2(cursorPos.x+availWidth, cursorPos.y+itemSize.y), true);
            dl->AddText(ImVec2(cursorPos.x - ss.scrollOffset, cursorPos.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            dl->PopClipRect();
        } else {
            if (needsScrolling && opl3_textScrollStates.count(i)) opl3_textScrollStates.erase(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (isSelected) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x+availWidth, cursorPos.y+itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x+availWidth, cursorPos.y+itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            dl->AddText(cursorPos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }

        if (isHovered) opl3_hoveredFileIndex = i;
        if (isExitedFolder || isPlayingPath || isPlayingFile) ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

void RenderOPL3Log() {
    RenderLog();
}
