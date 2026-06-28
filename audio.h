#ifndef NXAUDIO_H
#define NXAUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <xboxkrnl/xboxkrnl.h>

typedef enum
{
    NX_AUDIO_SUCCESS = 0,
    NX_AUDIO_ERR_OUT_OF_MEMORY,
    NX_AUDIO_ERR_OUT_OF_VOICES,
    NX_AUDIO_ERR_INVALID_PARAM,
    NX_AUDIO_ERR_TIMEOUT,
    NX_AUDIO_ERR_BUFFER_IN_USE,
} nxAudioResult;

typedef enum
{
    NX_FILTER_BYPASS = 0,
    NX_FILTER_LOWPASS,
    NX_FILTER_HIGHPASS
} nxFilterType;

typedef enum
{
    NX_VOICE_TYPE_2D_STREAM,
    // 3D STREAM
    // 2D AND 3D BUFFER MODE
} nxAudioVoiceType;

typedef enum
{
    NX_AUDIO_CODEC_PCM,
    NX_AUDIO_CODEC_ADPCM
} nxAudioCodec;

typedef struct
{
    uint32_t sampleRate;
    uint8_t channels;
    uint8_t bytesPerSample;
    nxAudioCodec codec;
    nxAudioVoiceType type;
} nxAudioFormat;

typedef struct
{
    uint32_t dummy;
} nxAudioInitParams;

struct nxAudioBuffer;
struct nxAudioVoice;
typedef void (*nxAudioVoiceBufferCompleteCallback)(struct nxAudioVoice *voice, struct nxAudioBuffer *buffer, void *user_data);

typedef struct nxAudioBuffer
{
    void *pUserBuffer;
    uint32_t sizeBytes;
} nxAudioBuffer;

typedef struct nxAudioVoice
{
    nxAudioBuffer *currentBuffer[2];
    nxAudioFormat format;
    nxAudioVoiceBufferCompleteCallback callback;
    void *user_data;

    bool paused;
    float volume;
    uint8_t currentBufferIndex;
    uint8_t voice_index;
} nxAudioVoice;

bool nxAudioInit (const nxAudioInitParams *parameters);
void nxAudioShutdown (void);

bool nxAudioBufferInitialise (nxAudioBuffer *voice, void *pUserBuffer, uint32_t sizeBytes);
bool nxAudioVoiceSubmitBuffer (nxAudioVoice *voice, nxAudioBuffer *buffer);

// Note that the callback is called from DPC context. 
bool nxAudioVoiceCreate (nxAudioVoice *voice, const nxAudioFormat *audio_format, nxAudioVoiceBufferCompleteCallback callback, void *user_data);
void nxAudioVoiceDestroy (nxAudioVoice *voice);
bool nxAudioVoiceStart (nxAudioVoice *voice);
bool nxAudioVoiceStop (nxAudioVoice *voice);
bool nxAudioVoicePause (nxAudioVoice *voice);

bool nxAudioVoiceSetVolume (nxAudioVoice *voice, float volume); //0.0f - 1.0f
bool nxAudioVoiceSetFilter (nxAudioVoice *voice, nxFilterType type, float cutoffHz, float q);

nxAudioResult nxAudioGetLastError (void);

#endif // NXAUDIO_H
