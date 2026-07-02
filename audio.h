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
 * @brief ADSR Envelope parameters.
 * Time values are specified in units of 512 samples.
 * At a 48,000 Hz sample rate, 1 unit (512 samples) is approximately 10.66 milliseconds.
 *
 *                            HOLD
 *                   +---------------------+
 *                  /|                     |\
 *          ATTACK / |                     | \ DECAY
 *                /  |                     |  \    SUSTAIN LEVEL
 *               /   |                     |   +-------------------+
 *              /    |                     |   |                   |\
 *             /     |                     |   |                   | \
 *            /      |                     |   |                   |  \  RELEASE
 *           /       |                     |   |                   |   \
 *          /        |                     |   |                   |    \
 * 0 ------+---------+---------------------+---+-------------------+-----+----> Time
 *   DELAY
 */
typedef struct
{
    uint32_t delayTime;    // Delay before the attack phase, in 512-sample units (0-8191)
    uint32_t attackTime;   // Duration of the attack phase, in 512-sample units (0-8191)
    uint32_t holdTime;     // Duration of the hold phase, in 512-sample units (0-8191)
    uint32_t decayTime;    // Duration of the decay phase, in 512-sample units (0-8191)
    uint32_t sustainLevel; // Sustain level (u.8 format: 0 to 255). 255 = 100% level
    uint32_t releaseTime;  // Duration of the release phase, in 512-sample units (0-8191)
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
    uint32_t hw_voice_on_flags;
    uint32_t hw_cfg_env0;
    uint32_t hw_cfg_enva;
    uint32_t hw_cfg_misc;
    uint32_t hw_tar_lfo_env;
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
 * @brief Releasing a voice will start a voice off decay set by the ADSR. If the ADSR is not set then the voice will be
 * stopped immediately.
 * @param voice Pointer to the audio voice to stop.
 * @return true if playback was successfully stopped, false otherwise.
 */
bool nxAudioVoiceRelease (nxAudioVoice *voice);

/**
 * @brief Immediately stops the voice from playing and removes it from the active hardware list.
 * @param voice Pointer to the audio voice to stop.
 * @return true if the command was successfully sent.
 */
bool nxAudioVoiceStop (nxAudioVoice *voice);

/**
 * @brief Checks if the audio voice is currently actively playing.
 * @param voice Pointer to the audio voice.
 * @return true if playing, false otherwise.
 */
bool nxAudioVoiceIsPlaying (nxAudioVoice *voice);

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
bool nxAudioVoiceSetAmplitudeEnvelope (nxAudioVoice *voice, const nxAudioAdsr *adsr);

nxAudioResult nxAudioGetLastError (void);

#endif // NXAUDIO_H
