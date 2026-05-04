#include "winmm.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

#define NUM_BUFFERS 4
#define BUFFER_SIZE 8192

#include <process.h>

static HWAVEOUT hWaveOut = NULL; // Initialize to NULL
static WAVEHDR waveHdrs[NUM_BUFFERS];
static char buffers[NUM_BUFFERS][BUFFER_SIZE];
static int currentBuffer = 0;
static HANDLE hThread = NULL; // Initialize to NULL
static HANDLE hSignal = NULL; // Initialize to NULL
static CRITICAL_SECTION critSect; // CRITICAL_SECTION does not have a NULL equivalent, relies on InitializeCriticalSection
static int critSectInitialized = 0; // Flag to track if critSect has been initialized (0 for false, 1 for true)
static volatile int running = 0; // Initialize to 0

#define RING_BUFFER_SIZE (BUFFER_SIZE * 8)
static char ring_buffer[RING_BUFFER_SIZE];
static volatile int ring_buffer_wpos = 0;
static volatile int ring_buffer_rpos = 0;

static void audio_thread(void *arg);
static void winmm_close(void); // Forward declaration

static int winmm_init(int sample_rate) {
    // Ensure previous resources are cleaned up if init is called again without proper close
    if (hWaveOut != NULL) {
        winmm_close(); // Close existing resources before re-initializing
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = sample_rate;
    wfx.nAvgBytesPerSec = sample_rate * 2 * 2;
    wfx.nBlockAlign = 4;
    wfx.wBitsPerSample = 16;
    wfx.cbSize = 0;

    hSignal = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hSignal == NULL) {
        fprintf(stderr, "Failed to create event.\n");
        return 0;
    }

    MMRESULT res = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)hSignal, 0, CALLBACK_EVENT);
    if (res != MMSYSERR_NOERROR) {
        fprintf(stderr, "Failed to open waveout device. Error code: %d\n", res);
        CloseHandle(hSignal);
        hSignal = NULL;
        return 0;
    }

    if (!critSectInitialized) {
        InitializeCriticalSection(&critSect);
        critSectInitialized = 1;
    }
    running = 1;
    hThread = (HANDLE)_beginthread(audio_thread, 0, NULL);
    if (hThread == NULL) {
        fprintf(stderr, "Failed to create audio thread.\n");
        DeleteCriticalSection(&critSect);
        waveOutClose(hWaveOut);
        CloseHandle(hSignal);
        hWaveOut = NULL;
        hSignal = NULL;
        return 0;
    }

    return 1;
}

static void winmm_play(char *buffer, int len) {
    EnterCriticalSection(&critSect);
    while (len > 0) {
        int free_space = (ring_buffer_rpos - ring_buffer_wpos - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
        if (len > free_space) {
            // If ring buffer is full, return immediately to avoid blocking the GUI thread.
            // This might cause audio dropouts but prevents GUI from freezing.
            // A proper solution would involve a separate audio thread or a more sophisticated buffer management.
            LeaveCriticalSection(&critSect);
            return;
        }

        int to_copy = len;
        int space_to_end = RING_BUFFER_SIZE - ring_buffer_wpos;
        if (to_copy > space_to_end) {
            to_copy = space_to_end;
        }

        memcpy(ring_buffer + ring_buffer_wpos, buffer, to_copy);
        ring_buffer_wpos = (ring_buffer_wpos + to_copy) % RING_BUFFER_SIZE;

        buffer += to_copy;
        len -= to_copy;
    }
    LeaveCriticalSection(&critSect);
}

static void winmm_close(void) {
    running = 0; // Signal the audio thread to stop
    if (hThread != NULL) {
        // Set the event to unblock the audio thread if it's waiting
        SetEvent(hSignal);
        WaitForSingleObject(hThread, INFINITE); // Wait for the audio thread to terminate
        CloseHandle(hThread);
        hThread = NULL;
    }
    if (hSignal != NULL) {
        CloseHandle(hSignal);
        hSignal = NULL;
    }
    // Only delete critical section if it was initialized.
    // This is a bit tricky as there's no direct way to check if it's initialized.
    if (critSectInitialized) {
        DeleteCriticalSection(&critSect);
        critSectInitialized = 0;
    }
    
    if (hWaveOut != NULL) {
        // Unprepare headers before closing the waveOut device
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (waveHdrs[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
    }
}

const audio_output_t audio_output_winmm = {
    .name = "winmm",
    .init = winmm_init,
    .play = winmm_play,
    .close = winmm_close,
};
static void audio_thread(void *arg) {
    (void)arg;

    // Set thread priority for real-time audio
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveHdrs[i].lpData = buffers[i];
        waveHdrs[i].dwBufferLength = BUFFER_SIZE;
        waveHdrs[i].dwFlags = 0;
        waveOutPrepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
        waveHdrs[i].dwFlags |= WHDR_DONE;
    }

    while (running) {
        // Wait for a buffer to be ready for writing
        if (!(waveHdrs[currentBuffer].dwFlags & WHDR_DONE)) {
            WaitForSingleObject(hSignal, INFINITE);
            continue;
        }

        EnterCriticalSection(&critSect);
        int data_len = (ring_buffer_wpos - ring_buffer_rpos + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
        LeaveCriticalSection(&critSect);

        if (data_len < BUFFER_SIZE) {
            // Not enough data in ring buffer, wait for more
            Sleep(1); // Or use a condition variable to signal data availability
            continue;
        }

        EnterCriticalSection(&critSect);

        int rpos = ring_buffer_rpos;
        int contiguous_bytes = RING_BUFFER_SIZE - rpos;

        if (contiguous_bytes >= BUFFER_SIZE) {
            // No wrap-around
            memcpy(buffers[currentBuffer], ring_buffer + rpos, BUFFER_SIZE);
        } else {
            // Wrap-around
            char* dest = buffers[currentBuffer];
            memcpy(dest, ring_buffer + rpos, contiguous_bytes);
            int remaining_bytes = BUFFER_SIZE - contiguous_bytes;
            memcpy(dest + contiguous_bytes, ring_buffer, remaining_bytes);
        }
        
        ring_buffer_rpos = (rpos + BUFFER_SIZE) % RING_BUFFER_SIZE;

        LeaveCriticalSection(&critSect);

        waveOutWrite(hWaveOut, &waveHdrs[currentBuffer], sizeof(WAVEHDR));
        currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveOutUnprepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
    }
}