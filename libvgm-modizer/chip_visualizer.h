// Chip Visualizer - Data extraction layer for chip visualization
// Extracts real-time chip state from libvgm emulators

#pragma once

#include "chip_params.h"
#include <cstring>

// Forward declarations of chip structures
// These are defined in libvgm emu cores
typedef struct ym2612__ ym2612_;
typedef struct _SN76489_Context SN76489_Context;

// Base class for chip visualizers
class ChipVisualizer {
public:
    virtual ~ChipVisualizer() = default;

    // Update chip state from emulator data pointer
    virtual void Update(void* chipDataPtr) = 0;

    // Get chip name for display
    virtual const char* GetChipName() const = 0;

    // Check if parameters have changed since last update
    virtual bool HasChanged() const = 0;
};

// YM2612 (OPN2) Visualizer
class YM2612Visualizer : public ChipVisualizer {
private:
    YM2612Params currentParams;
    YM2612Params previousParams;
    bool hasChanged;

public:
    YM2612Visualizer();

    void Update(void* chipDataPtr) override;
    const char* GetChipName() const override { return "YM2612 (OPN2)"; }
    bool HasChanged() const override { return hasChanged; }

    const YM2612Params& GetParams() const { return currentParams; }

private:
    // Calculate MIDI note from YM2612 F-Number and Block
    int CalculateNote(uint16_t fnum, uint8_t block, uint32_t chipClock);

    // Extract channel data from chip structure
    void ExtractChannelData(ym2612_* chip);

    // Extract FM parameters
    void ExtractFMParameters(ym2612_* chip);

    // Extract register values
    void ExtractRegisters(ym2612_* chip);
};

// SN76489 (PSG/DCSG) Visualizer
class SN76489Visualizer : public ChipVisualizer {
private:
    SN76489Params currentParams;
    SN76489Params previousParams;
    bool hasChanged;
    uint32_t chipClock;

public:
    SN76489Visualizer();

    void Update(void* chipDataPtr) override;
    const char* GetChipName() const override { return "SN76489 (PSG)"; }
    bool HasChanged() const override { return hasChanged; }

    const SN76489Params& GetParams() const { return currentParams; }

    // Set chip clock for frequency calculations
    void SetChipClock(uint32_t clock) { chipClock = clock; }

private:
    // Calculate MIDI note from PSG tone register
    int CalculateNote(uint16_t toneReg);

    // Extract channel data from chip structure
    void ExtractChannelData(SN76489_Context* chip);

    // Extract register values
    void ExtractRegisters(SN76489_Context* chip);

    // Calculate frequency from tone register
    float CalculateFrequency(uint16_t toneReg);
};

// AY-3-8910 (SSG) Visualizer
class AY8910Visualizer : public ChipVisualizer {
private:
    AY8910Params currentParams;
    AY8910Params previousParams;
    bool hasChanged;
    uint32_t chipClock;

public:
    AY8910Visualizer();

    void Update(void* chipDataPtr) override;
    const char* GetChipName() const override { return "AY-3-8910 (SSG)"; }
    bool HasChanged() const override { return hasChanged; }

    const AY8910Params& GetParams() const { return currentParams; }

    void SetChipClock(uint32_t clock) { chipClock = clock; }

private:
    int CalculateNote(uint16_t tonePeriod);
    void ExtractChannelData(void* chip);
    void ExtractRegisters(void* chip);
    float CalculateFrequency(uint16_t tonePeriod);
};
