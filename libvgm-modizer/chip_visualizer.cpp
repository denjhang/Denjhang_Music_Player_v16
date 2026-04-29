// Chip Visualizer Implementation
// Extracts real-time chip state data from libvgm emulators

#include "chip_visualizer.h"
#include <cmath>
#include <algorithm>

// Include libvgm type definitions
#include "emu/snddef.h"

// Include internal chip structures
#include "emu/cores/ym2612_int.h"
#include "emu/cores/sn76489_private.h"

// =============================================================================
// YM2612 Visualizer Implementation
// =============================================================================

YM2612Visualizer::YM2612Visualizer() : hasChanged(false) {
    // Initialize to silent state
    for (int i = 0; i < 9; i++) {
        currentParams.channels[i] = ChannelParams();
    }
}

void YM2612Visualizer::Update(void* chipDataPtr) {
    if (!chipDataPtr) return;

    // Save previous state
    previousParams = currentParams;

    ym2612_* chip = reinterpret_cast<ym2612_*>(chipDataPtr);

    // Extract chip data
    ExtractChannelData(chip);
    ExtractFMParameters(chip);
    ExtractRegisters(chip);

    // Check if anything changed
    hasChanged = (memcmp(&currentParams, &previousParams, sizeof(YM2612Params)) != 0);
}

int YM2612Visualizer::CalculateNote(uint16_t fnum, uint8_t block, uint32_t chipClock) {
    if (fnum == 0) return -1;  // Silent

    // YM2612 frequency formula:
    // freq = (fnum * clock) / (144 * 2^(20 - block))
    // Default YM2612 clock = 7670454 Hz (NTSC Genesis)
    if (chipClock == 0) chipClock = 7670454;

    float frequency = (float)(fnum * chipClock) / (144.0f * (float)(1 << (20 - block)));

    return FrequencyToMIDINote(frequency);
}

void YM2612Visualizer::ExtractChannelData(ym2612_* chip) {
    // Extract data for 6 FM channels
    for (int ch = 0; ch < 6; ch++) {
        channel_* channel = &chip->CHANNEL[ch];
        ChannelParams* params = &currentParams.channels[ch];

        // Calculate note from F-Number and Block
        params->note = CalculateNote(channel->FNUM[0], channel->FOCT[0], chip->Clock);
        params->frequency = MIDINoteToFrequency(params->note);

        // Calculate volume from slot Total Levels (TL)
        // YM2612 uses algorithm to determine which slots contribute to output
        // For simplicity, use minimum TL of all slots as approximation
        int minTL = 127;
        for (int slot = 0; slot < 4; slot++) {
            int tl = channel->SLOT[slot].TL;
            if (tl < minTL) minTL = tl;
        }
        params->volume = 127 - minTL;  // Invert (TL is attenuation)

        // Pan (LEFT/RIGHT flags)
        if (channel->LEFT && channel->RIGHT)
            params->pan = 3;  // LR
        else if (channel->LEFT)
            params->pan = 0;  // L
        else if (channel->RIGHT)
            params->pan = 2;  // R
        else
            params->pan = 1;  // C (muted)

        // Key On state - check if any slot is in attack/decay/sustain phase
        params->keyOn = false;
        for (int slot = 0; slot < 4; slot++) {
            if (channel->SLOT[slot].Ecurp != 4) {  // 4 = Release phase
                params->keyOn = true;
                break;
            }
        }
    }

    // CH3 special mode - extract individual operator frequencies
    if (chip->Mode & 0x40) {  // CH3 special mode enabled
        currentParams.ch3SpecialMode = true;

        for (int op = 0; op < 3; op++) {
            ChannelParams* params = &currentParams.channels[6 + op];
            channel_* channel = &chip->CHANNEL[2];  // CH3 is channel index 2

            // Each operator has its own F-Number in special mode
            params->note = CalculateNote(channel->FNUM[op + 1], channel->FOCT[op + 1], chip->Clock);
            params->frequency = MIDINoteToFrequency(params->note);

            // Use individual slot TL
            int tl = channel->SLOT[op + 1].TL;
            params->volume = 127 - tl;

            params->pan = currentParams.channels[2].pan;  // Same pan as CH3
            params->keyOn = (channel->SLOT[op + 1].Ecurp != 4);
        }
    } else {
        currentParams.ch3SpecialMode = false;
        // Clear CH3 special mode channels
        for (int op = 0; op < 3; op++) {
            currentParams.channels[6 + op] = ChannelParams();
        }
    }

    // DAC state (channel 6 can be DAC)
    currentParams.dacEnabled = (chip->DAC != 0);
    if (currentParams.dacEnabled) {
        currentParams.dacSample = (chip->DACdata >> 1) & 0xFF;
    }
}

void YM2612Visualizer::ExtractFMParameters(ym2612_* chip) {
    for (int ch = 0; ch < 6; ch++) {
        channel_* channel = &chip->CHANNEL[ch];
        YM2612Params::FMChannel* fmCh = &currentParams.fmChannels[ch];

        fmCh->algorithm = channel->ALGO;
        fmCh->feedback = channel->FB;
        fmCh->fnum = channel->FNUM[0];
        fmCh->block = channel->FOCT[0];
        fmCh->ams = channel->AMS;
        fmCh->fms = channel->FMS;

        // Extract per-operator parameters
        for (int slot = 0; slot < 4; slot++) {
            slot_* s = &channel->SLOT[slot];
            YM2612Params::FMChannel::Operator* op = &fmCh->operators[slot];

            op->tl = s->TL;
            op->mul = s->MUL;
            // AR, DR, SR, RR are pointers to tables, need to extract actual values
            // For now, use simplified approach
            op->ar = 0;  // Would need to reverse-lookup in rate table
            op->dr = 0;
            op->sr = 0;
            op->rr = 0;
        }
    }

    // LFO
    currentParams.lfo = chip->LFOinc >> 16;  // Simplified
}

void YM2612Visualizer::ExtractRegisters(ym2612_* chip) {
    // Registers are already stored in chip->REG[port][addr]
    // We don't need to copy them, just reference them if needed
}

// =============================================================================
// SN76489 Visualizer Implementation
// =============================================================================

SN76489Visualizer::SN76489Visualizer() : hasChanged(false), chipClock(3579545) {
    // Initialize to silent state
    for (int i = 0; i < 4; i++) {
        currentParams.channels[i] = ChannelParams();
    }
}

void SN76489Visualizer::Update(void* chipDataPtr) {
    if (!chipDataPtr) return;

    // Save previous state
    previousParams = currentParams;

    SN76489_Context* chip = reinterpret_cast<SN76489_Context*>(chipDataPtr);

    // Update chip clock if available
    if (chip->Clock > 0) {
        chipClock = (uint32_t)chip->Clock;
    }

    // Extract chip data
    ExtractChannelData(chip);
    ExtractRegisters(chip);

    // Check if anything changed
    hasChanged = (memcmp(&currentParams, &previousParams, sizeof(SN76489Params)) != 0);
}

float SN76489Visualizer::CalculateFrequency(uint16_t toneReg) {
    if (toneReg == 0) return 0.0f;

    // SN76489 frequency formula:
    // freq = clock / (32 * toneReg)
    // Default clock = 3579545 Hz (NTSC)
    return chipClock / (32.0f * toneReg);
}

int SN76489Visualizer::CalculateNote(uint16_t toneReg) {
    float freq = CalculateFrequency(toneReg);
    if (freq <= 0.0f) return -1;

    return FrequencyToMIDINote(freq);
}

void SN76489Visualizer::ExtractChannelData(SN76489_Context* chip) {
    // Check for NGP mode (dual PSG)
    currentParams.isNGPMode = (chip->NgpFlags & 0x80) != 0;

    // Extract 3 tone channels
    for (int ch = 0; ch < 3; ch++) {
        ChannelParams* params = &currentParams.channels[ch];

        // Tone register (10-bit value, stored in Registers[ch*2])
        uint16_t toneReg = chip->Registers[ch * 2];

        params->note = CalculateNote(toneReg);
        params->frequency = CalculateFrequency(toneReg);

        // Volume (4-bit, inverted - 0 = max, 15 = silent)
        uint8_t volReg = chip->Registers[ch * 2 + 1] & 0x0F;
        params->volume = (15 - volReg) * 8;  // Scale to 0-120

        // Check key on state using channel output state
        // If channel is outputting non-zero signal, it's active
        params->keyOn = (chip->ToneFreqVals[ch] != 0) && (volReg < 15);

        // Pan (from Game Gear stereo register or fake stereo)
        if (chip->PSGStereo != 0xFF) {
            // Game Gear stereo
            bool left = (chip->PSGStereo & (0x10 << ch)) != 0;
            bool right = (chip->PSGStereo & (0x01 << ch)) != 0;

            if (left && right)
                params->pan = 3;  // LR
            else if (left)
                params->pan = 0;  // L
            else if (right)
                params->pan = 2;  // R
            else
                params->pan = 1;  // C (muted)
        } else {
            params->pan = 3;  // Default to LR
        }
    }

    // Noise channel (channel 3)
    ChannelParams* noiseParams = &currentParams.channels[3];

    uint8_t volReg = chip->Registers[7] & 0x0F;
    noiseParams->volume = (15 - volReg) * 8;
    noiseParams->keyOn = (volReg < 15);

    // Noise has no specific note, set to special value
    noiseParams->note = -1;
    noiseParams->frequency = 0.0f;

    // Noise parameters
    uint8_t noiseReg = chip->Registers[6];
    currentParams.noise.type = (noiseReg & 0x04) ? 1 : 0;  // Bit 2: white/periodic
    currentParams.noise.freqMode = noiseReg & 0x03;         // Bits 0-1: frequency mode
    currentParams.noise.shiftRate = chip->NoiseFreq;

    // Pan for noise channel
    if (chip->PSGStereo != 0xFF) {
        bool left = (chip->PSGStereo & 0x80) != 0;
        bool right = (chip->PSGStereo & 0x08) != 0;

        if (left && right)
            noiseParams->pan = 3;
        else if (left)
            noiseParams->pan = 0;
        else if (right)
            noiseParams->pan = 2;
        else
            noiseParams->pan = 1;
    } else {
        noiseParams->pan = 3;
    }

    // Store GG stereo register
    currentParams.ggStereo = chip->PSGStereo;
}

void SN76489Visualizer::ExtractRegisters(SN76489_Context* chip) {
    // Copy register values
    for (int i = 0; i < 8; i++) {
        currentParams.registers[i] = chip->Registers[i];
    }
}

// =============================================================================
// AY-3-8910 Visualizer Implementation (stub for now)
// =============================================================================

AY8910Visualizer::AY8910Visualizer() : hasChanged(false), chipClock(1789773) {
}

void AY8910Visualizer::Update(void* chipDataPtr) {
    // TODO: Implement AY8910 data extraction
    hasChanged = false;
}

float AY8910Visualizer::CalculateFrequency(uint16_t tonePeriod) {
    if (tonePeriod == 0) return 0.0f;
    return chipClock / (16.0f * tonePeriod);
}

int AY8910Visualizer::CalculateNote(uint16_t tonePeriod) {
    float freq = CalculateFrequency(tonePeriod);
    if (freq <= 0.0f) return -1;
    return FrequencyToMIDINote(freq);
}

void AY8910Visualizer::ExtractChannelData(void* chip) {
    // TODO
}

void AY8910Visualizer::ExtractRegisters(void* chip) {
    // TODO
}
