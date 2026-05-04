#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

typedef struct audio_output_t {
    const char *name;
    int (*init)(int sample_rate);
    void (*play)(char *buffer, int len);
    void (*close)(void);
} audio_output_t;

#endif // AUDIO_OUTPUT_H