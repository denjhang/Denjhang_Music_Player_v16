// Chip Parameter Structures for Visualization
// Defines common data structures for extracting chip state information

#pragma once

#include <stdint.h>
#include <cmath>

// Common channel parameters across all chips
struct ChannelParams {
    int note;           // MIDI note number (-1 = silent, 0-127 = MIDI notes)
    float frequency;    // Frequency in Hz
    int volume;         // Volume (0-127, similar to MIDI velocity)
    int pan;            // Panning (0=L, 1=C, 2=R, 3=LR)
    bool keyOn;         // Key on/off state

    ChannelParams() : note(-1), frequency(0.0f), volume(0), pan(1), keyOn(false) {}
};

// YM2612 (OPN2) chip parameters
struct YM2612Params {
    ChannelParams channels[9];  // 6 FM channels + 3 CH3 special mode operators

    // Per-channel FM parameters
    struct FMChannel {
        uint8_t algorithm;      // Algorithm (0-7)
        uint8_t feedback;       // Feedback (0-7)
        uint8_t inst[47];       // Voice/instrument parameters
        uint16_t fnum;          // Frequency number (F-Number)
        uint8_t block;          // Block (octave)
        uint8_t ams;            // AMS (Amplitude Modulation Sensitivity)
        uint8_t fms;            // FMS (Frequency Modulation Sensitivity)

        // Per-operator parameters (4 operators per channel)
        struct Operator {
            uint8_t tl;         // Total Level (0-127)
            uint8_t ar;         // Attack Rate
            uint8_t dr;         // Decay Rate
            uint8_t sr;         // Sustain Rate
            uint8_t rr;         // Release Rate
            uint8_t sl;         // Sustain Level
            uint8_t mul;        // Multiplier
            uint8_t dt;         // Detune
            uint8_t ks;         // Key Scale
        } operators[4];

        FMChannel() : algorithm(0), feedback(0), fnum(0), block(0), ams(0), fms(0) {
            for (int i = 0; i < 47; i++) inst[i] = 0;
        }
    } fmChannels[6];

    // Global parameters
    uint8_t lfo;                // LFO frequency
    bool ch3SpecialMode;        // CH3 special mode (operator frequency independence)

    // XGM PCM channels (for XGM/XGM2 files)
    struct PCMChannel {
        bool active;            // PCM channel active
        uint8_t sample;         // Sample number
        uint8_t volume;         // Volume
        uint16_t pitch;         // Pitch (XGM2)

        PCMChannel() : active(false), sample(0), volume(0), pitch(0) {}
    } pcm[4];

    // DAC state (channel 6 can be used as DAC)
    bool dacEnabled;
    uint8_t dacSample;

    YM2612Params() : lfo(0), ch3SpecialMode(false), dacEnabled(false), dacSample(0) {}
};

// SN76489 (PSG/DCSG) chip parameters
struct SN76489Params {
    ChannelParams channels[4];  // 3 tone channels + 1 noise channel

    uint16_t registers[8];      // Raw register values (tone/volume for each channel)

    // Noise channel specific parameters
    struct NoiseChannel {
        uint8_t type;           // 0 = periodic (tonal), 1 = white noise
        uint8_t freqMode;       // 0-2 = use tone gen, 3 = fixed frequency
        uint16_t shiftRate;     // Shift register rate

        NoiseChannel() : type(1), freqMode(3), shiftRate(0) {}
    } noise;

    bool isNGPMode;             // Neo Geo Pocket dual PSG mode

    // Game Gear stereo support
    uint8_t ggStereo;           // GG stereo register (bit 0-3 = ch0-3 right, bit 4-7 = ch0-3 left)

    SN76489Params() : isNGPMode(false), ggStereo(0xFF) {
        for (int i = 0; i < 8; i++) registers[i] = 0;
    }
};

// AY-3-8910 (SSG) chip parameters
struct AY8910Params {
    ChannelParams channels[3];  // 3 tone channels

    uint8_t registers[16];      // 16 registers

    // Envelope parameters
    struct Envelope {
        bool active;
        uint8_t shape;          // 0-15
        uint16_t period;

        Envelope() : active(false), shape(0), period(0) {}
    } envelope;

    // Noise parameters
    struct Noise {
        bool enabled[3];        // Per-channel noise enable
        uint8_t period;

        Noise() : period(0) {
            for (int i = 0; i < 3; i++) enabled[i] = false;
        }
    } noise;

    AY8910Params() {
        for (int i = 0; i < 16; i++) registers[i] = 0;
    }
};

// Helper functions
inline const char* GetNoteName(int midiNote) {
    static const char* noteNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (midiNote < 0 || midiNote > 127) return "---";
    return noteNames[midiNote % 12];
}

inline int GetNoteOctave(int midiNote) {
    if (midiNote < 0 || midiNote > 127) return -1;
    return (midiNote / 12) - 1;  // MIDI note 60 = C4
}

inline bool IsBlackKey(int midiNote) {
    static const bool blackKeys[] = {false, true, false, true, false, false, true, false, true, false, true, false};
    if (midiNote < 0 || midiNote > 127) return false;
    return blackKeys[midiNote % 12];
}

// MIDI note to frequency conversion (A4 = 440Hz)
inline float MIDINoteToFrequency(int midiNote) {
    if (midiNote < 0 || midiNote > 127) return 0.0f;
    return 440.0f * powf(2.0f, (midiNote - 69) / 12.0f);
}

// Frequency to MIDI note conversion
inline int FrequencyToMIDINote(float frequency) {
    if (frequency <= 0.0f) return -1;
    float note = 12.0f * log2f(frequency / 440.0f) + 69.0f;
    int midiNote = (int)(note + 0.5f);  // Round to nearest
    if (midiNote < 0) return 0;
    if (midiNote > 127) return 127;
    return midiNote;
}
