#ifndef MODIZER_CONSTANTS_H
#define MODIZER_CONSTANTS_H

// Minimal constants needed for oscilloscope voice buffer system
// Extracted from full ModizerConstants.h

#define SOUND_BUFFER_SIZE_SAMPLE 512
#define SOUND_MAXVOICES_BUFFER_FX 64
#define SOUND_VOICES_MAX_ACTIVE_CHIPS 8
#define SOUND_MAXMOD_CHANNELS 256
#define MODIZER_OSCILLO_OFFSET_FIXEDPOINT 16
#define SCOPE_MAX_CHIPS 16

#define LIMIT8(a) (a>127?127:(a<-128?-128:a))

#endif
