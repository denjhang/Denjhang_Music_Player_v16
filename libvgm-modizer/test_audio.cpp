// Quick DirectSound audio test
#include <windows.h>
#include <stdio.h>
#include <math.h>

extern "C" {
#include "stdtype.h"
#include "audio/AudioStream.h"
}

static void* audDrv = NULL;
static UINT32 callCount = 0;

UINT32 TestFillBuffer(void* drvObj, void* userParam, UINT32 bufSize, void* data)
{
    callCount++;

    // Generate simple tone
    INT16* samples = (INT16*)data;
    UINT32 sampleCount = bufSize / 4;  // 16-bit stereo

    for (UINT32 i = 0; i < sampleCount; i++)
    {
        INT16 value = (INT16)(sin(i * 0.1) * 10000);
        samples[i * 2] = value;      // Left
        samples[i * 2 + 1] = value;  // Right
    }

    return bufSize;
}

int main()
{
    printf("Testing DirectSound audio...\n");

    Audio_Init();
    UINT32 drvCount = Audio_GetDriverCount();
    printf("Found %u audio drivers\n", drvCount);

    UINT32 drvID = (UINT32)-1;
    for (UINT32 i = 0; i < drvCount; i++)
    {
        AUDDRV_INFO* drvInfo;
        Audio_GetDriverInfo(i, &drvInfo);
        printf("  Driver %u: %s\n", i, drvInfo->drvName);
        if (strcmp(drvInfo->drvName, "DirectSound") == 0)
        {
            drvID = i;
        }
    }

    if (drvID == (UINT32)-1)
    {
        printf("ERROR: DirectSound not found!\n");
        return 1;
    }

    printf("Using DirectSound (ID %u)\n", drvID);

    AudioDrv_Init(drvID, &audDrv);
    AUDIO_OPTS* opts = AudioDrv_GetOptions(audDrv);
    opts->sampleRate = 44100;
    opts->numChannels = 2;
    opts->numBitsPerSmpl = 16;
    opts->usecPerBuf = 10000;
    opts->numBuffers = 10;

    printf("Setting callback...\n");
    AudioDrv_SetCallback(audDrv, TestFillBuffer, NULL);

    printf("Starting audio...\n");
    UINT8 ret = AudioDrv_Start(audDrv, 0);
    printf("Start returned: %u\n", ret);

    printf("Playing tone for 3 seconds...\n");
    Sleep(3000);

    printf("FillBuffer was called %u times\n", callCount);

    AudioDrv_Stop(audDrv);
    AudioDrv_Deinit(&audDrv);
    Audio_Deinit();

    printf("Test complete.\n");
    return 0;
}
