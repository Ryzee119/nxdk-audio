
#include "audio.h"
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>

#define PI                 3.14159265f
#define STREAM_BUFFER_SIZE (0x4000 * 2) // 64KB Ping-Pong Buffers
#define NUM_VOICES         1

// A context struct to cleanly track the state of each concurrent stream
typedef struct
{
    nxAudioVoice voice;
    nxAudioFormat format;
    nxAudioBuffer buffers[2];
    void *ramA;
    void *ramB;
    float phase;
    float frequency;
} StreamContext;

// A universal generator that automatically adapts to 8-bit, 16-bit, 24-bit, Mono, and Stereo
void GenerateMultiFormatSine (StreamContext *ctx, nxAudioBuffer *buffer)
{
    // FIX 1: If 24-bit (3 bytes), force the physical memory stride to 4 bytes for the math
    uint32_t containerBytes = (ctx->format.bytesPerSample == 3) ? 4 : ctx->format.bytesPerSample;
    uint32_t numFrames = buffer->size_bytes / (containerBytes * ctx->format.channels);

    float phaseStep = (2.0f * PI * ctx->frequency) / (float)ctx->format.sampleRate;

    uint8_t *dst8 = (uint8_t *)buffer->buffer;
    int16_t *dst16 = (int16_t *)buffer->buffer;
    int32_t *dst32 = (int32_t *)buffer->buffer;

    for (uint32_t i = 0; i < numFrames; i++) {
        // Generate the raw wave and scale volume to 25%
        float rawSine = sinf(ctx->phase);
        float volSine = rawSine * 0.25f;

        ctx->phase += phaseStep;
        if (ctx->phase >= (2.0f * PI)) {
            ctx->phase -= (2.0f * PI);
        }

        for (uint8_t c = 0; c < ctx->format.channels; c++) {
            if (ctx->format.bytesPerSample == 1) {
                // 8-bit PCM is Unsigned (U8). Center is 128.
                *dst8++ = (uint8_t)(128.0f + (volSine * 127.0f));
            } else if (ctx->format.bytesPerSample == 2) {
                // 16-bit PCM is Signed (S16)
                *dst16++ = (int16_t)(volSine * 32767.0f);
            } else if (ctx->format.bytesPerSample == 3) {
                // FIX 2: 24-bit PCM (S24). Scale to 24-bit max, then Left-Justify by shifting up 8 bits!
                int32_t sample24 = (int32_t)(volSine * 8388607.0f);
                *dst32++ = sample24;
            } else if (ctx->format.bytesPerSample == 4) {
                // 32-bit PCM (S32). Fill the whole container.
                *dst32++ = (int32_t)(volSine * 2147483647.0f);
            }
        }
    }
}


// voice callback
static void audio_callback (nxAudioVoice *voice, nxAudioBuffer *buffer, void *user_data)
{
    // Generate the next buffer of audio data
    StreamContext *ctx = (StreamContext *)user_data;
    GenerateMultiFormatSine(ctx, buffer);

    // Submit the new buffer to the voice
    nxAudioVoiceSubmitBuffer(voice, buffer);
}

void TestStreamingAudio (void)
{
    DbgPrint("--- Starting Multi-Voice Audio Streaming Test ---\n");

    nxAudioInitParams initParams = {0};
    if (!nxAudioInit(&initParams)) {
        DbgPrint("FATAL: nxAudioInit failed.\n");
        return;
    }

    StreamContext streams[NUM_VOICES];

    // Define the 4 voices to create an A Major Chord (A4, C#5, E5, A5)
    // We mix sample rates, channels, and bit-depths to stress test the hardware resampler.

    streams[0].format = (nxAudioFormat){80000, 2, 1, NX_AUDIO_CODEC_PCM, NX_VOICE_TYPE_2D_STREAM}; // 16-bit Stereo
    streams[0].frequency = 2000.00f;                                                                // Root: A4
#if NUM_VOICES > 1
    streams[1].format = (nxAudioFormat){48000, 2, 1, NX_AUDIO_CODEC_PCM, NX_VOICE_TYPE_2D_STREAM}; // 8-bit Mono
    streams[1].frequency = 800.0f;                                                                // Major Third: C#5
#endif

#if NUM_VOICES > 2
    streams[2].format = (nxAudioFormat){48000, 1, 1, NX_AUDIO_CODEC_PCM, NX_VOICE_TYPE_2D_STREAM}; // 24-bit Stereo
    streams[2].frequency = 600.0f;                                                               // Perfect Fifth: E5
#endif

#if NUM_VOICES > 3
    streams[3].format = (nxAudioFormat){48000, 1, 1, NX_AUDIO_CODEC_PCM, NX_VOICE_TYPE_2D_STREAM}; // 16-bit Mono
    streams[3].frequency = 400.00f;                                                                // Octave: A5
#endif

    // Initialize all voices and buffers
    for (int i = 0; i < NUM_VOICES; i++) {
        streams[i].phase = 0.0f;

        if (!nxAudioVoiceCreate(&streams[i].voice, &streams[i].format, audio_callback, &streams[i])) {
            DbgPrint("FATAL: Failed to create voice %d\n", i);
            return;
        }

        streams[i].ramA = malloc(STREAM_BUFFER_SIZE);
        streams[i].ramB = malloc(STREAM_BUFFER_SIZE);

        nxAudioBufferInitialise(&streams[i].buffers[0], streams[i].ramA, STREAM_BUFFER_SIZE);
        nxAudioBufferInitialise(&streams[i].buffers[1], streams[i].ramB, STREAM_BUFFER_SIZE);

        GenerateMultiFormatSine(&streams[i], &streams[i].buffers[0]);
        GenerateMultiFormatSine(&streams[i], &streams[i].buffers[1]);

        nxAudioVoiceSubmitBuffer(&streams[i].voice, &streams[i].buffers[0]);
        nxAudioVoiceSubmitBuffer(&streams[i].voice, &streams[i].buffers[1]);
    }

    if (!nxAudioVoiceStart(&streams[0].voice)
#if NUM_VOICES > 1
        || !nxAudioVoiceStart(&streams[1].voice)
#endif
#if NUM_VOICES > 2
        || !nxAudioVoiceStart(&streams[2].voice)
#endif
#if NUM_VOICES > 3
        || !nxAudioVoiceStart(&streams[3].voice)
#endif
    ) {
        DbgPrint("FATAL: Failed to start one or more voices.\n");
        return;
    }

    DbgPrint("All voices started. Entering main polling loop...\n");

    // The Main Game/Streaming Loop
    // Because we have multiple voices, we use a timeout of '0' to poll them non-blockingly.
    uint32_t buffersProcessed = 0;
    while (buffersProcessed < 2000) // Run longer since polling happens much faster
    {
        Sleep(1000);
        debugPrint("Changing volume of voice 0 to 50%%\n");
        nxAudioVoiceSetVolume(&streams[0].voice, 0.51f);

        Sleep(1000);
        debugPrint("Pausing voice 0\n");
        nxAudioVoicePause(&streams[0].voice); 

        Sleep(1000);
        debugPrint("Unpausing voice 0\n");
        nxAudioVoiceStart(&streams[0].voice);

        //Sleep(1000);
        //debugPrint("Shutting down voice 0\n");
        //nxAudioVoiceStop(&streams[0].voice);

        Sleep(1000);
        debugPrint("Shutting down audio system\n");
        nxAudioShutdown();
        while(1) {
            Sleep(1000);
        }
    }
    debugPrint("Test complete. Shutting down...\n");

    for (int i = 0; i < NUM_VOICES; i++) {
        nxAudioVoiceStop(&streams[i].voice);
        nxAudioVoiceDestroy(&streams[i].voice);
        free(streams[i].ramA);
        free(streams[i].ramB);
    }

    nxAudioShutdown();
}

int main (void)
{
    XVideoSetMode(1280, 720, 32, REFRESH_DEFAULT);
    TestStreamingAudio();
    while (1) {
        XVideoWaitForVBlank();
    }
    return 0;
}
