/*
 * WAV Attribution:
 * https://mauvecloud.net/sounds/
 * Public Domain
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <hal/debug.h>
#include <hal/video.h>

#include <SDL.h>
#include <nxaudio.h>

#define NUM_BUFFERS 2
#define CHUNK_BYTES 4608 // 128 blocks * 36 bytes

#define WAV_FORMAT_UNCOMPRESSED 0x0001
#define WAV_FORMAT_XBOX_ADPCM   0x0069

static const uint8_t steam_wav[] = {
#embed "assets/adpcm_stereo.wav"
};

static const uint8_t *find_chunk (const uint8_t *riff_data, uint32_t riff_size, const char *magic, uint32_t *out_size)
{
    const uint8_t *chunk = riff_data + 12;
    while (chunk + 8 <= riff_data + riff_size) {
        uint32_t chunk_size = *(uint32_t *)(chunk + 4);
        if (memcmp(chunk, magic, 4) == 0) {
            if (out_size) {
                *out_size = chunk_size;
            }
            return chunk + 8;
        }
        chunk += 8 + chunk_size;
        if (chunk_size % 2 != 0) {
            chunk++;
        }
    }
    return NULL;
}

typedef struct
{
    nxAudioBuffer buffers[NUM_BUFFERS];
    uint8_t *data[NUM_BUFFERS];
    int next_buffer_to_fill;
    volatile int buffers_completed;

    const uint8_t *pcm_data;
    uint32_t pcm_total_bytes;
    uint32_t pcm_pos;
} StreamContext;

static void voice_callback (struct nxAudioVoice *voice, void *user_context)
{
    (void)voice;
    StreamContext *ctx = (StreamContext *)user_context;
    ctx->buffers_completed++;
    // Ping pong debug print
    static int i = 0;
    DbgPrint("Callback: Buffer completed! %d\n", i ^= 1);
}

static void fill_wav_chunk (StreamContext *ctx, uint8_t *out, int num_bytes)
{
    for (int i = 0; i < num_bytes; i++) {
        if (ctx->pcm_pos >= ctx->pcm_total_bytes && ctx->pcm_total_bytes > 0) {
            ctx->pcm_pos = 0; // Loop back
        }
        if (ctx->pcm_total_bytes > 0) {
            out[i] = ctx->pcm_data[ctx->pcm_pos];
            ctx->pcm_pos++;
        } else {
            out[i] = 0;
        }
    }
}

int main (void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        DbgPrint("SDL init failed: %s\n", SDL_GetError());
    }

    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        DbgPrint("nxAudioInit failed\n");
        return -1;
    }

    uint32_t fmt_size = 0;
    const uint8_t *fmt_chunk = find_chunk(steam_wav, sizeof(steam_wav), "fmt ", &fmt_size);
    uint32_t data_size = 0;
    const uint8_t *data_chunk = find_chunk(steam_wav, sizeof(steam_wav), "data", &data_size);

    if (!fmt_chunk || !data_chunk) {
        DbgPrint("Invalid WAV file\n");
        nxAudioShutdown();
        return -1;
    }

    uint16_t format_tag = *(uint16_t *)(fmt_chunk);
    uint16_t channels = *(uint16_t *)(fmt_chunk + 2);
    uint32_t sample_rate = *(uint32_t *)(fmt_chunk + 4);
    uint16_t bits_per_sample = *(uint16_t *)(fmt_chunk + 14);

    StreamContext ctx = {0};
    ctx.pcm_data = data_chunk;
    ctx.pcm_total_bytes = data_size;
    ctx.pcm_pos = 0;

    debugClearScreen();
    DbgPrint("Streaming Audio Test (steam.wav)\n\n");
    DbgPrint("START = Exit\n\n");
    DbgPrint("WAV: %d Hz, %d-bit, %d ch, %d bytes\n", sample_rate, bits_per_sample, channels, data_size);

    nxAudioFormat format = {
        .sample_rate = sample_rate,
        .channels = channels,
        .bytes_per_sample = format_tag == WAV_FORMAT_XBOX_ADPCM ? 0 : (bits_per_sample / 8),
        .codec = format_tag == WAV_FORMAT_XBOX_ADPCM ? NX_AUDIO_CODEC_ADPCM : NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_2D_STREAM,
    };

    nxAudioVoice voice;
    nxAudioVoiceCreate(&voice, &format);
    nxAudioVoiceSetMasterGain(&voice, 1.0f);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        ctx.data[i] = (uint8_t *)MmAllocateContiguousMemory(CHUNK_BYTES);
        if (!ctx.data[i]) {
            DbgPrint("Failed to allocate buffer memory\n");
            nxAudioShutdown();
            return -1;
        }
        fill_wav_chunk(&ctx, ctx.data[i], CHUNK_BYTES);
        nxAudioBufferInitialize(&ctx.buffers[i], ctx.data[i], CHUNK_BYTES);
        nxAudioBufferQueue(&voice, &ctx.buffers[i]);
    }

    nxAudioBufferSetCallback(&voice, voice_callback, &ctx);
    nxAudioVoiceStart(&voice);

    bool running = true;
    SDL_GameController *controller = NULL;

    while (running) {
        while (ctx.buffers_completed > 0) {
            ctx.buffers_completed--;
            int buf_idx = ctx.next_buffer_to_fill;
            fill_wav_chunk(&ctx, ctx.data[buf_idx], CHUNK_BYTES);
            nxAudioBufferInitialize(&ctx.buffers[buf_idx], ctx.data[buf_idx], CHUNK_BYTES);
            nxAudioBufferQueue(&voice, &ctx.buffers[buf_idx]);
            ctx.next_buffer_to_fill = (buf_idx + 1) % NUM_BUFFERS;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                if (!controller) {
                    controller = SDL_GameControllerOpen(e.cdevice.which);
                }
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (controller &&
                    e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
                    SDL_GameControllerClose(controller);
                    controller = NULL;
                }
            }
        }

        if (controller) {
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) {
                running = false;
            }
        }

        Sleep(16);
    }

    nxAudioVoiceStop(&voice);
    nxAudioVoiceDestroy(&voice);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (ctx.data[i]) {
            MmFreeContiguousMemory(ctx.data[i]);
        }
    }

    nxAudioShutdown();

    if (controller) {
        SDL_GameControllerClose(controller);
    }
    SDL_Quit();

    return 0;
}
