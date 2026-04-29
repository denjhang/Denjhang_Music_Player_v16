// ImGui Chip Visualization Windows
// Defines window classes for displaying chip state

#pragma once

#include "imgui/imgui.h"
#include "chip_visualizer.h"
#include "imgui_piano_roll.h"

// Base class for chip visualization windows
class ChipWindow {
public:
    ChipWindow() : isVisible(false), windowPos(100, 100), windowSize(600, 400) {}
    virtual ~ChipWindow() = default;

    // Render the window
    virtual void Render() = 0;

    // Show/hide window
    void SetVisible(bool visible) { isVisible = visible; }
    bool IsVisible() const { return isVisible; }
    void ToggleVisible() { isVisible = !isVisible; }

protected:
    bool isVisible;
    ImVec2 windowPos;
    ImVec2 windowSize;
};

// YM2612 (OPN2) Visualization Window
class YM2612Window : public ChipWindow {
private:
    YM2612Visualizer* visualizer;
    PianoRoll pianoRoll;
    int zoom;
    bool showRegisters;
    bool showFMParams;

public:
    YM2612Window(YM2612Visualizer* vis);

    void Render() override;

private:
    void RenderToolbar();
    void RenderPianoRoll();
    void RenderChannelTable();
    void RenderFMParameters();
    void RenderRegisters();
    void RenderPCMChannels();
};

// SN76489 (PSG) Visualization Window
class SN76489Window : public ChipWindow {
private:
    SN76489Visualizer* visualizer;
    PianoRoll pianoRoll;
    int zoom;
    bool showRegisters;

public:
    SN76489Window(SN76489Visualizer* vis);

    void Render() override;

private:
    void RenderToolbar();
    void RenderPianoRoll();
    void RenderChannelTable();
    void RenderNoiseInfo();
    void RenderRegisters();
};

// Register Viewer Window (for any chip)
class RegisterViewerWindow : public ChipWindow {
private:
    void* chipDataPtr;
    uint8_t* registerData;
    uint8_t* prevRegisterData;
    int registerCount;
    const char* chipName;

public:
    RegisterViewerWindow(const char* name, int regCount);
    ~RegisterViewerWindow();

    void SetChipData(void* dataPtr, uint8_t* regs);
    void Render() override;

private:
    void RenderHexDump();
};
