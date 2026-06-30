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
    uint32_t unused;
} nxAudioInitParams;

struct nxAudioBuffer;
struct nxAudioVoice;
typedef void (*nxAudioVoiceBufferCompleteCallback)(struct nxAudioVoice *voice, struct nxAudioBuffer *buffer,
                                                   void *user_data);

typedef struct nxAudioBuffer
{
    void *buffer;
    uint32_t size_bytes;
} nxAudioBuffer;

/**
 * Hardware ADSR Envelope State Machine
 *
 * Volume
 *   ^
 *   |               (3) HOLD
 * P |             +----------+
 * E |            /            \      (4) DECAY
 * A |           /              \
 * K |          /                +-----------------------+ (5) SUSTAIN (INDEFINITLY UNTIL VOICE OFF)
 *   |         /                 |                       |\
 *   | (2) ATTACK                |                       | \ (7) RELEASE (AFTER VOICE OFF COMMAND)
 *   |       /                   |                       |  \
 *   |      /                    |                       |   \
 *   |     /                     |                       |    \
 * 0 +----+----------------------+-----------------------+-----+----> Time
 *   (1) DELAY                                             (0) OFF
 */
typedef struct
{
    uint32_t delayTime;    // Count of 512-sample blocks to delay before attack (0-8191)
    uint32_t attackTime;   // Attack segment length, in 512-sample blocks (0-8191)
    uint32_t holdTime;     // Count of 512-sample blocks to hold after attack (0-8191)
    uint32_t decayTime;    // Decay segment length, in 512-sample blocks (0-8191)
    uint32_t sustainLevel; // Sustain level (0-255)
    uint32_t releaseTime;  // Release segment length, in 512-sample blocks (0-8191)
} nxAudioAdsr;

typedef struct nxAudioVoice
{
    nxAudioBuffer *currentBuffer[2];
    nxAudioFormat format;
    nxAudioVoiceBufferCompleteCallback callback;
    void *user_data;

    bool paused;
    float volume;
    float pitch_multiplier;
    nxAudioAdsr adsr;
    uint32_t hw_cfg_fmt;
    uint8_t currentBufferIndex;
    uint8_t voice_index;
} nxAudioVoice;

/**
 * @brief Initializes the audio subsystem.
 * @param parameters Pointer to the configuration parameters for initialization.
 * @return true if the audio subsystem was successfully initialized, false otherwise.
 */
bool nxAudioInit (const nxAudioInitParams *parameters);

/**
 * @brief Shuts down the audio subsystem and releases associated resources.
 */
void nxAudioShutdown (void);

/**
 * @brief Initializes an audio buffer wrapping a user-provided memory region.
 * @param buffer Pointer to the audio buffer structure to initialize.
 * @param user_buffer Pointer to the user-allocated memory containing the raw audio data.
 * @param size_bytes The size of the user buffer in bytes.
 * @return true if the buffer was successfully initialized, false otherwise.
 */
bool nxAudioBufferInitialise (nxAudioBuffer *buffer, void *user_buffer, uint32_t size_bytes);

/**
 * @brief Submits a prepared audio buffer to a voice's playback queue.
 * @param voice Pointer to the audio voice.
 * @param buffer Pointer to the initialized audio buffer to enqueue.
 * @return true if the buffer was successfully submitted, false otherwise.
 */
bool nxAudioVoiceSubmitBuffer (nxAudioVoice *voice, nxAudioBuffer *buffer);

/**
 * @brief Creates and configures a new audio voice for playback.
 * @note The provided callback is executed from a Deferred Procedure Call (DPC) context.
 * @param voice Pointer to the voice structure to be created.
 * @param audio_format Pointer to the format specification for this voice.
 * @param callback Function to be invoked when a submitted buffer completes playback.
 * @param user_data User-defined context pointer passed directly to the callback function.
 * @return true if the voice was successfully created, false otherwise.
 */
bool nxAudioVoiceCreate (nxAudioVoice *voice, const nxAudioFormat *audio_format,
                         nxAudioVoiceBufferCompleteCallback callback, void *user_data);

/**
 * @brief Destroys an audio voice and cleans up its allocated resources.
 * @param voice Pointer to the audio voice to destroy.
 */
void nxAudioVoiceDestroy (nxAudioVoice *voice);

/**
 * @brief Starts or resumes playback of submitted buffers on the specified voice.
 * @param voice Pointer to the audio voice to start.
 * @return true if playback was successfully started, false otherwise.
 */
bool nxAudioVoiceStart (nxAudioVoice *voice);

/**
 * @brief Stopping a voice will start a voice off decay set by the ADSR. If the ADSR is not set then the voice will be
 * stopped immediately. If you want to stop a voice immediately, call nxAudioVoicePause() before calling
 * nxAudioVoiceStop()
 * @param voice Pointer to the audio voice to stop.
 * @return true if playback was successfully stopped, false otherwise.
 */
bool nxAudioVoiceStop (nxAudioVoice *voice);

/**
 * @brief Pausing a voice will stop the hardware from accessing the buffer but will not change any hardware voice
 * settings. If you call nxAudioVoiceStart() after nxAudioVoicePause() then the voice will resume from where it left
 * off.
 * @param voice Pointer to the audio voice to pause.
 * @return true if playback was successfully paused, false otherwise.
 */
bool nxAudioVoicePause (nxAudioVoice *voice);

/**
 * @brief Sets the output volume level for a specific voice.
 * @param voice Pointer to the audio voice to adjust.
 * @param volume Floating-point volume level clamped between 0.0f (muted) and 1.0f (maximum).
 * @return true if the volume was successfully updated, false otherwise.
 */
bool nxAudioVoiceSetVolume (nxAudioVoice *voice, float volume);

/**
 * @brief Sets the playback pitch for a specific voice.
 * @param voice Pointer to the audio voice to adjust.
 * @param pitch_multiplier Pitch multiplier (1.0f = original pitch, 2.0f = double pitch, etc.).
 * @return true if the pitch was successfully updated, false otherwise.
 */
bool nxAudioVoiceSetPitch (nxAudioVoice *voice, float pitch_multiplier);

/**
 * @brief Sets the ADSR Amplitude Envelope for a specific voice.
 * @param voice Pointer to the audio voice to adjust.
 * @param adsr Pointer to the ADSR configuration structure.
 * @return true if the envelope was successfully updated, false otherwise.
 */
bool nxAudioVoiceSetAdsr (nxAudioVoice *voice, const nxAudioAdsr *adsr);

nxAudioResult nxAudioGetLastError (void);

#endif // NXAUDIO_H
