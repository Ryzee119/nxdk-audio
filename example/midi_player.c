/*
 * Soundfont Attribution:
 * https://musical-artifacts.com/artifacts/4745
 * 8MBGMGS (UNOFFICIAL) by E-mu/Creative, with the GS presets added by Caed
 * Creative Commons Attribution 3.0 Unported
 *
 * Midi Attribution:
 * https://commons.wikimedia.org/wiki/File:Invention_10_(BWV_781).mid
 * Invention 10 (BWV 781).mid
 * Michael Bednarek, Public domain, via Wikimedia Commons
 */
#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <hal/debug.h>
#include <hal/video.h>

#include <nxaudio.h>

#define TML_IMPLEMENTATION
#include "tml.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define MAX_VOICES  64
#define SAMPLE_RATE 24000

// Instrument one-shots: 0.1 second samples pre-rendered from SoundFont
#define WAVETABLE_SAMPLES (SAMPLE_RATE / 2)

// Drum one-shots: longer buffers for percussive transients
#define DRUM_SAMPLES   (SAMPLE_RATE)
#define DRUM_BASE_FREQ ((float)SAMPLE_RATE / DRUM_SAMPLES)

// GM drum key range
#define DRUM_KEY_MIN   35
#define DRUM_KEY_MAX   81
#define DRUM_KEY_COUNT (DRUM_KEY_MAX - DRUM_KEY_MIN + 1)

static int16_t *instrument_data[128];
static nxAudioBuffer instrument_buffers[128];
static int16_t *drum_data[DRUM_KEY_COUNT];
static nxAudioBuffer drum_buffers[DRUM_KEY_COUNT];
static void generate_instrument_samples (tsf *soundfont)
{
    int16_t *temp = (int16_t *)malloc(2048 * sizeof(int16_t));

    debugPrint("Generating Instruments");
    for (int prog = 0; prog < 128; prog++) {
        instrument_data[prog] = (int16_t *)MmAllocateContiguousMemory(WAVETABLE_SAMPLES * sizeof(int16_t));

        tsf_reset(soundfont);
        tsf_set_output(soundfont, TSF_MONO, SAMPLE_RATE, 0.0f);

        // Play middle C at full velocity
        int preset = tsf_get_presetindex(soundfont, 0, prog);
        if (preset < 0) {
            preset = 0; // Fallback to first preset
        }
        tsf_note_on(soundfont, preset, 60, 1.0f);
        tsf_render_short(soundfont, temp, 2048, 0);
        tsf_render_short(soundfont, instrument_data[prog], WAVETABLE_SAMPLES, 0);
        tsf_note_off_all(soundfont);
        nxAudioBufferInitialize(&instrument_buffers[prog], instrument_data[prog], WAVETABLE_SAMPLES * sizeof(int16_t));
        debugPrint(".");
    }
    debugPrint("\n");

    free(temp);
}

static void generate_drum_samples (tsf *soundfont)
{
    for (int key = DRUM_KEY_MIN; key <= DRUM_KEY_MAX; key++) {
        int idx = key - DRUM_KEY_MIN;
        drum_data[idx] = (int16_t *)MmAllocateContiguousMemory(DRUM_SAMPLES * sizeof(int16_t));

        tsf_reset(soundfont);
        tsf_set_output(soundfont, TSF_MONO, SAMPLE_RATE, 0.0f);

        // Drums use bank 128, preset 0
        int preset = tsf_get_presetindex(soundfont, 128, 0);
        if (preset < 0) {
            preset = 0;
        }
        tsf_note_on(soundfont, preset, key, 1.0f);
        tsf_render_short(soundfont, drum_data[idx], DRUM_SAMPLES, 0);
        tsf_note_off_all(soundfont);
        nxAudioBufferInitialize(&drum_buffers[idx], drum_data[idx], DRUM_SAMPLES * sizeof(int16_t));
    }
}

typedef struct
{
    int note;
    int channel;
    float base_pitch;
} VoiceInfo;

typedef struct
{
    int program;
} ChannelState;

static const unsigned char sf_data[] = {
#embed "assets/soundfont.sf2"
};

static const unsigned char midi_buffer[] = {
#embed "assets/midi.mid"
};

static nxAudioVoice voices[MAX_VOICES];
static VoiceInfo voice_info[MAX_VOICES];
static ChannelState channels[16];

int main (void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        return -1;
    }

    tsf *soundfont = tsf_load_memory(sf_data, sizeof(sf_data));

    // Pre-render all instrument wavetables from SoundFont
    generate_instrument_samples(soundfont);
    generate_drum_samples(soundfont);

    // Done with TSF — free it and the SoundFont data
    tsf_close(soundfont);

    nxAudioFormat format = {
        .sample_rate = SAMPLE_RATE,
        .channels = 1,
        .bytes_per_sample = 2,
        .codec = NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_2D_STATIC,
    };

    nxAudioADSR adsr = {
        .attack_time = 1,
        .hold_time = 10,
        .decay_time = 10,
        .sustain_level = 127,
        .release_time = 10,
    };

    for (int i = 0; i < MAX_VOICES; i++) {
        nxAudioVoiceCreate(&voices[i], &format);
        nxAudioVoiceSetLooping(&voices[i], true);
        nxAudioVoiceSetAmplitudeEnvelope(&voices[i], &adsr);
        voice_info[i].note = -1;
        voice_info[i].channel = -1;
        voice_info[i].base_pitch = 1.0f;
    }

    for (int i = 0; i < 16; i++) {
        channels[i].program = 0;
    }

    tml_message *midi = tml_load_memory(midi_buffer, sizeof(midi_buffer));
    tml_message *msg = midi;
    uint32_t current_time_ms = 0;
    uint32_t last_vis_time = 0;

    debugPrint("Playing MIDI\n");
    while (msg) {
        if (msg->time > current_time_ms) {
            uint32_t wait_time = msg->time - current_time_ms;

            while (wait_time > 0) {
                uint32_t sleep_amt = (wait_time > 50) ? 50 : wait_time;

                if (current_time_ms - last_vis_time >= 100) {
                    last_vis_time = current_time_ms;
                }

                Sleep(sleep_amt > 0 ? sleep_amt - 1 : 0);
                current_time_ms += sleep_amt;
                wait_time -= sleep_amt;
            }
            current_time_ms = msg->time;
        }

        int ch = msg->channel;

        switch (msg->type) {
            case TML_PROGRAM_CHANGE:
                channels[ch].program = msg->program;
                break;

            case TML_NOTE_ON:
                if (msg->velocity > 0) {
                    // Find a free voice
                    int free_idx = -1;
                    for (int i = 0; i < MAX_VOICES; i++) {
                        if (nxAudioVoiceGetState(&voices[i]) == NX_STOPPED && voice_info[i].note < 0) {
                            free_idx = i;
                            break;
                        }
                    }
                    if (free_idx == -1) {
                        for (int i = 0; i < MAX_VOICES; i++) {
                            if (nxAudioVoiceGetState(&voices[i]) == NX_STOPPED) {
                                free_idx = i;
                                break;
                            }
                        }
                    }
                    if (free_idx == -1) {
                        free_idx = rand() % MAX_VOICES;
                        nxAudioVoiceStop(&voices[free_idx]);
                    }

                    float vel_gain = msg->velocity / 127.0f;

                    nxAudioBuffer *buf;
                    float base_pitch;
                    bool loop;

                    if (ch == 9) {
                        // Drum channel — use drum one-shot
                        int drum_idx = msg->key - DRUM_KEY_MIN;
                        if (drum_idx < 0) {
                            drum_idx = 0;
                        }
                        if (drum_idx >= DRUM_KEY_COUNT) {
                            drum_idx = DRUM_KEY_COUNT - 1;
                        }
                        buf = &drum_buffers[drum_idx];
                        base_pitch = 1.0f; // Play at original pitch
                    } else {
                        // Melodic — use instrument one-shot (rendered at Middle C, Note 60)
                        buf = &instrument_buffers[channels[ch].program];
                        base_pitch = powf(2.0f, (msg->key - 60) / 12.0f);
                    }

                    voice_info[free_idx].note = msg->key;
                    voice_info[free_idx].channel = ch;
                    voice_info[free_idx].base_pitch = base_pitch;

                    nxAudioVoiceSetPitch(&voices[free_idx], base_pitch);
                    nxAudioVoiceSetMasterGain(&voices[free_idx], vel_gain);
                    nxAudioBufferSubmit(&voices[free_idx], buf);
                    nxAudioVoiceStart(&voices[free_idx]);
                } else {
                    goto note_off;
                }
                break;

            case TML_NOTE_OFF:
            note_off:
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (voice_info[i].note == msg->key && voice_info[i].channel == ch &&
                        nxAudioVoiceGetState(&voices[i]) != NX_STOPPED) {
                        nxAudioVoiceRelease(&voices[i]);
                        voice_info[i].note = -1;
                        voice_info[i].channel = -1;
                        break;
                    }
                }
                break;

            default:
                break;
        }

        msg = msg->next;
    }

    debugPrint("MIDI Finished\n");
    Sleep(2000);

    for (int i = 0; i < MAX_VOICES; i++) {
        nxAudioVoiceDestroy(&voices[i]);
    }

    tml_free(midi);

    for (int i = 0; i < 128; i++) {
        MmFreeContiguousMemory(instrument_data[i]);
    }
    for (int i = 0; i < DRUM_KEY_COUNT; i++) {
        MmFreeContiguousMemory(drum_data[i]);
    }

    nxAudioShutdown();

    return 0;
}
