// sn76489_window.cpp - SN76489 (DCSG) Hardware Control Window
// Controls real SN76489 chip via FTDI/SPFM Light interface

#include "sn76489_window.h"
#include "sn76489/spfm.h"
#include "sn76489/sn76489.h"
#include "chip_control.h"
#include "chip_window_ym2163.h"
#include "imgui/imgui.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <math.h>

namespace SN76489Window {

// ============ Constants ============
static const int SN_SAMPLE_RATE = 44100;
static const int BUF_SAMPLES = 1024;

// Channel colors
static ImU32 kChColors[4] = {
    IM_COL32(80, 220, 80, 255),   // Ch0: green
    IM_COL32(80, 140, 255, 255),  // Ch1: blue
    IM_COL32(255, 80, 80, 255),   // Ch2: red
    IM_COL32(255, 180, 60, 255)   // Ch3: orange (noise)
};
static const char* kChNames[4] = { "Tone0", "Tone1", "Tone2", "Noise" };

// ============ State ============
static bool s_connected = false;

// Auto-connect state
static bool s_manualDisconnect = false;
static int s_connectRetries = 0;
static const int MAX_RETRIES = 10;
static const int CHECK_INTERVAL_MS = 2000;
static LARGE_INTEGER s_lastCheckTime;
static LARGE_INTEGER s_perfFreq;

// Test state
static bool s_testRunning = false;
static int s_testType = 0;  // 0=scale, 1=arpeggio, 2=chord, 3=volume sweep, 4=noise
static int s_testStep = 0;
static double s_testStepMs = 0.0;
static LARGE_INTEGER s_testStartTime;

// Channel state for display
static uint8_t s_tonePeriodLo[3] = {0, 0, 0};
static uint8_t s_tonePeriodHi[3] = {0, 0, 0};
static uint8_t s_vol[4] = {15, 15, 15, 15}; // 0=max, 15=mute
static uint8_t s_noiseType = 0;
static uint8_t s_noiseFreq = 0;
static bool s_noiseUseCh2 = false;  // true=noise freq from Ch2 tone, false=shift freq control
static uint16_t s_fullPeriod[3] = {0, 0, 0};

// Log
static std::string s_log;

static void DcLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_log += buf;
    if (s_log.size() > 64000) {
        s_log = s_log.substr(s_log.size() - 32000);
    }
}

// ============ Connection ============
static void sn76489_mute_all(void);
static void sn76489_set_noise(uint8_t ntype, uint8_t shift_freq);
static void sn76489_set_tone(uint8_t ch, uint16_t period);
static void InitHardware(void);
static void ResetState(void);
static void StopTest(void);

static bool CheckHardwarePresent(void) {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    return (status == FT_OK && numDevs > 0);
}

static bool CheckHardwareReady(void) {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    if (status != FT_OK || numDevs == 0) return false;

    FT_DEVICE_LIST_INFO_NODE devInfo;
    status = FT_GetDeviceInfoDetail(0, &devInfo.Flags, &devInfo.Type,
        &devInfo.ID, &devInfo.LocId, devInfo.SerialNumber,
        devInfo.Description, &devInfo.ftHandle);
    if (status != FT_OK) return false;

    return !(devInfo.Flags & 0x01);
}

static void ConnectHardware(void) {
    if (s_connected) return;
    if (YM2163::g_hardwareConnected)
        YM2163::DisconnectHardware();
    YM2163::g_manualDisconnect = true;
    if (spfm_init(0) == 0) {
        s_connected = true;
        s_manualDisconnect = false;
        ResetState();
        InitHardware();
        DcLog("[SN] Hardware connected\n");
    } else {
        YM2163::g_manualDisconnect = false;
        DcLog("[SN] Hardware connection failed\n");
    }
}

static void ResetState(void) {
    s_testRunning = false;
    s_testType = 0;
    s_testStep = 0;
    s_testStepMs = 0.0;
    s_vol[0] = s_vol[1] = s_vol[2] = s_vol[3] = 15;
    s_noiseType = 0;
    s_noiseFreq = 0;
    s_noiseUseCh2 = false;
    s_tonePeriodLo[0] = s_tonePeriodLo[1] = s_tonePeriodLo[2] = 0;
    s_tonePeriodHi[0] = s_tonePeriodHi[1] = s_tonePeriodHi[2] = 0;
    s_fullPeriod[0] = s_fullPeriod[1] = s_fullPeriod[2] = 0;
}

static void InitHardware(void) {
    sn76489_mute_all();
    sn76489_set_noise(0, 0);
    sn76489_set_tone(0, 0);
    sn76489_set_tone(1, 0);
    sn76489_set_tone(2, 0);
    spfm_flush();
}

static void DisconnectHardware(void) {
    if (!s_connected) return;
    if (s_testRunning) s_testRunning = false;
    InitHardware();
    spfm_cleanup();
    s_connected = false;
    s_manualDisconnect = true;
    YM2163::g_manualDisconnect = false;
    DcLog("[SN] Hardware disconnected\n");
}

static void CheckAutoConnect(void) {
    if (s_manualDisconnect) return;
    if (s_connected) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - s_lastCheckTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
    if (elapsed < CHECK_INTERVAL_MS) return;
    s_lastCheckTime = now;

    if (!CheckHardwarePresent()) { s_connectRetries = 0; return; }
    if (!CheckHardwareReady()) {
        s_connectRetries++;
        if (s_connectRetries >= MAX_RETRIES) s_connectRetries = 0;
        return;
    }

    ConnectHardware();
    if (s_connected) {
        s_connectRetries = 0;
    } else {
        s_connectRetries++;
        if (s_connectRetries >= MAX_RETRIES) s_connectRetries = 0;
    }
}

// ============ Helpers ============
static void sn76489_write(uint8_t data) {
    ::spfm_write_data(0, data);
    ::spfm_hw_wait(3);
}

static void sn76489_set_tone(uint8_t ch, uint16_t period) {
    sn76489_write(sn76489_tone_latch(ch, (uint8_t)(period & 0x0F)));
    sn76489_write(sn76489_tone_data((uint8_t)(period >> 4)));
}

static void sn76489_set_vol(uint8_t ch, uint8_t vol) {
    if (ch < 3)
        sn76489_write(sn76489_vol_latch(ch, vol));
    else
        sn76489_write(sn76489_noise_vol_latch(vol));
}

static void sn76489_mute_all(void) {
    sn76489_write(0x9F);
    sn76489_write(0xBF);
    sn76489_write(0xDF);
    sn76489_write(0xFF);
}

static void sn76489_set_noise(uint8_t ntype, uint8_t shift_freq) {
    sn76489_write(sn76489_noise_latch(ntype, shift_freq));
}

static double GetTestElapsedMs(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_testStartTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
}

// ============ Test Functions ============
static double GetStepDurationMs(int type) {
    switch (type) {
        case 0: return 300.0; // Scale
        case 1: return 150.0; // Arpeggio
        case 2: return 800.0; // Chord
        case 3: return 200.0; // Volume sweep
        case 4: return 500.0; // Noise
        default: return 300.0;
    }
}

static void TestStep(void) {
    switch (s_testType) {
        case 0: { // Scale: C4-C5, 8 notes, 300ms each
            static const uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
            if (s_testStep >= 8) { s_testRunning = false; return; }
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, notes[s_testStep]);
            sn76489_set_tone(0, period);
            s_fullPeriod[0] = period;
            s_tonePeriodLo[0] = period & 0x0F;
            s_tonePeriodHi[0] = (period >> 4) & 0x3F;
            break;
        }
        case 1: { // Arpeggio: 12 notes, 150ms each
            static const uint8_t notes[] = {60, 64, 67, 72, 76, 79, 84, 79, 76, 72, 67, 64};
            if (s_testStep >= 12) { s_testRunning = false; return; }
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, notes[s_testStep]);
            sn76489_set_tone(1, period);
            s_fullPeriod[1] = period;
            s_tonePeriodLo[1] = period & 0x0F;
            s_tonePeriodHi[1] = (period >> 4) & 0x3F;
            break;
        }
        case 2: { // Chord: C/F/G major
            if (s_testStep >= 3) { s_testRunning = false; return; }
            sn76489_mute_all();
            static const struct { uint8_t n0, n1, n2; } chords[] = {
                {60, 64, 67}, // C major
                {65, 69, 72}, // F major
                {67, 71, 74}, // G major
            };
            sn76489_set_vol(0, 0);
            sn76489_set_vol(1, 0);
            sn76489_set_vol(2, 0);
            uint16_t p0 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n0);
            uint16_t p1 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n1);
            uint16_t p2 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n2);
            sn76489_set_tone(0, p0); s_fullPeriod[0] = p0; s_tonePeriodLo[0] = p0 & 0x0F; s_tonePeriodHi[0] = (p0 >> 4) & 0x3F;
            sn76489_set_tone(1, p1); s_fullPeriod[1] = p1; s_tonePeriodLo[1] = p1 & 0x0F; s_tonePeriodHi[1] = (p1 >> 4) & 0x3F;
            sn76489_set_tone(2, p2); s_fullPeriod[2] = p2; s_tonePeriodLo[2] = p2 & 0x0F; s_tonePeriodHi[2] = (p2 >> 4) & 0x3F;
            s_vol[0] = s_vol[1] = s_vol[2] = 0;
            break;
        }
        case 3: { // Volume sweep: A4, vol 0->15->0
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, 69.0);
            sn76489_set_tone(0, period);
            s_fullPeriod[0] = period; s_tonePeriodLo[0] = period & 0x0F; s_tonePeriodHi[0] = (period >> 4) & 0x3F;
            if (s_testStep < 16) {
                s_vol[0] = (uint8_t)s_testStep;
            } else {
                s_vol[0] = (uint8_t)(30 - s_testStep);
            }
            sn76489_set_vol(0, s_vol[0]);
            if (s_testStep >= 30) { s_testRunning = false; sn76489_set_vol(0, 15); return; }
            break;
        }
        case 4: { // Noise
            if (s_testStep >= 4) { s_testRunning = false; sn76489_set_vol(3, 15); return; }
            switch (s_testStep) {
                case 0: sn76489_set_noise(0, 0); s_vol[3] = 0; sn76489_set_vol(3, 0); s_noiseType = 0; s_noiseFreq = 0; break;
                case 1: sn76489_set_noise(1, 0); s_noiseType = 1; break;
                case 2: sn76489_set_noise(1, 3); s_noiseFreq = 3; break;
                case 3:
                    for (int v = 0; v <= 15; v++) { s_vol[3] = (uint8_t)v; sn76489_set_vol(3, s_vol[3]); }
                    return;
            }
            break;
        }
        default:
            s_testRunning = false;
            return;
    }

    spfm_flush();
    s_testStep++;
}

static void StopTest(void) {
    if (!s_testRunning) return;
    s_testRunning = false;
    if (s_connected) InitHardware();
    ResetState();
}

static void StartTest(int type) {
    if (!s_connected) return;
    StopTest();
    s_testType = type;
    s_testStep = 0;
    s_testStepMs = 0.0;
    QueryPerformanceCounter(&s_testStartTime);
    s_testRunning = true;

    switch (type) {
        case 0: sn76489_set_vol(0, 0); spfm_flush(); break;
        case 1: sn76489_set_vol(1, 0); spfm_flush(); break;
        case 4: sn76489_set_noise(0, 0); sn76489_set_vol(3, 0); spfm_flush(); break;
    }
}

// ============ Public API ============
void Init() {
    QueryPerformanceFrequency(&s_perfFreq);
    QueryPerformanceCounter(&s_lastCheckTime);
}

void Shutdown() {
    DisconnectHardware();
}

void Update() {
    CheckAutoConnect();
    if (!s_testRunning) return;
    double elapsed = GetTestElapsedMs();
    if (elapsed >= s_testStepMs) {
        TestStep();
        s_testStepMs += GetStepDurationMs(s_testType);
    }
}

void Render() {
    ImGui::Begin("SN76489(DCSG)");

    // ===== Connection =====
    ImGui::TextDisabled("-- SPFM Connection --");
    ImGui::PushStyleColor(ImGuiCol_Button,
        s_connected ? ImVec4(0.2f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    if (s_connected) {
        if (ImGui::SmallButton("Disconnect##sn76489")) {
            DisconnectHardware();
        }
    } else {
        if (ImGui::SmallButton("Connect##sn76489")) {
            s_manualDisconnect = false;
            ConnectHardware();
        }
    }
    ImGui::PopStyleColor();

    if (s_connected) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Test Buttons =====
    ImGui::TextDisabled("-- Test Functions --");
    float btnW = 130.0f;
    if (ImGui::Button("Scale##sn76489scale", ImVec2(btnW, 0))) StartTest(0);
    ImGui::SameLine();
    if (ImGui::Button("Arpeggio##sn76489arpeg", ImVec2(btnW, 0))) StartTest(1);
    ImGui::SameLine();
    if (ImGui::Button("Chord##sn76489chord", ImVec2(btnW, 0))) StartTest(2);
    ImGui::SameLine();
    if (ImGui::Button("Stop##sn76489stop", ImVec2(btnW, 0))) StopTest();

    if (ImGui::Button("Volume Sweep##sn76489vol", ImVec2(btnW, 0))) StartTest(3);
    ImGui::SameLine();
    if (ImGui::Button("Noise##sn76489noise", ImVec2(btnW, 0))) StartTest(4);

    if (s_testRunning) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Channel Controls =====
    ImGui::TextDisabled("-- Channel Controls --");

    for (int ch = 0; ch < 3; ch++) {
        ImGui::PushID(ch);
        ImGui::TextColored(ImVec4(
            ((kChColors[ch] >> 0) & 0xFF) / 255.0f,
            ((kChColors[ch] >> 8) & 0xFF) / 255.0f,
            ((kChColors[ch] >> 16) & 0xFF) / 255.0f, 1.0f),
            "%s", kChNames[ch]);
        ImGui::SameLine();
        ImGui::TextDisabled("Vol:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        int volVal = s_vol[ch];
        if (ImGui::SliderInt("##vol", &volVal, 0, 15)) {
            s_vol[ch] = (uint8_t)volVal;
            if (s_connected) { sn76489_set_vol((uint8_t)ch, s_vol[ch]); spfm_flush(); }
        }

        // Frequency slider (period 0-1023)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        int periodVal = (int)s_fullPeriod[ch];
        if (ImGui::SliderInt("##period", &periodVal, 0, 1023)) {
            s_fullPeriod[ch] = (uint16_t)periodVal;
            if (s_connected) {
                sn76489_set_tone((uint8_t)ch, s_fullPeriod[ch]);
                spfm_flush();
            }
        }
        ImGui::PopID();
    }

    // Noise channel
    ImGui::TextColored(ImVec4(
        ((kChColors[3] >> 0) & 0xFF) / 255.0f,
        ((kChColors[3] >> 8) & 0xFF) / 255.0f,
        ((kChColors[3] >> 16) & 0xFF) / 255.0f, 1.0f),
        "Noise");
    ImGui::SameLine();
    ImGui::TextDisabled("Vol:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    int nVolVal = s_vol[3];
    if (ImGui::SliderInt("##nvol", &nVolVal, 0, 15)) {
        s_vol[3] = (uint8_t)nVolVal;
        if (s_connected) { sn76489_set_vol(3, s_vol[3]); spfm_flush(); }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Type:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Periodic##sn76489p", s_noiseType == 0)) {
        s_noiseType = 0; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("White##sn76489w", s_noiseType == 1)) {
        s_noiseType = 1; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); }
    }

    // Noise frequency mode
    ImGui::SameLine();
    ImGui::TextDisabled("Freq:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Ch2##sn76489fch2", s_noiseUseCh2)) {
        s_noiseUseCh2 = true;
        if (s_connected) { sn76489_set_noise(s_noiseType, 3); spfm_flush(); }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Shift##sn76489fshift", !s_noiseUseCh2)) {
        s_noiseUseCh2 = false;
        if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); }
    }
    if (!s_noiseUseCh2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        int nFreqVal = s_noiseFreq;
        if (ImGui::SliderInt("##nfreq", &nFreqVal, 0, 3)) {
            s_noiseFreq = (uint8_t)nFreqVal;
            if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Register Display =====
    ImGui::TextDisabled("-- Registers --");
    if (ImGui::BeginTable("##sn76489regs", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < 3; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", ch);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("Tone%d", ch);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%03X (%u)", s_fullPeriod[ch], s_fullPeriod[ch]);
            ImGui::TableSetColumnIndex(3);
            double freq = (s_fullPeriod[ch] > 0) ? SN76489_CLOCK_NTSC / (32.0 * s_fullPeriod[ch]) : 0.0;
            ImGui::Text("%.1f Hz", freq);
        }
        for (int ch = 0; ch < 4; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", ch);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s Vol", kChNames[ch]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u (0x%X)", s_vol[ch], s_vol[ch]);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", s_vol[ch] == 15 ? "[MUTE]" : "");
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("3");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Noise");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("0x%02X", sn76489_noise_latch(s_noiseType, s_noiseFreq));
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%s", s_noiseType == 0 ? "White" : "Periodic");

        ImGui::EndTable();
    }

    // ===== Log =====
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("-- Log --");
    ImGui::BeginChild("SN_Log##sn76489log", ImVec2(0, 100), true);
    ImGui::TextUnformatted(s_log.c_str(), s_log.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

bool WantsKeyboardCapture() { return false; }

} // namespace SN76489Window
