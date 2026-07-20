#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <hal/video.h>
#include <pbkit.h>
#include <pbkit_draw.h>
#include <pbkit_print.h>

#include <nxaudio.h>

#define TML_IMPLEMENTATION
#include "tml.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define MAX_VOICES  64
#define SAMPLE_RATE 9600

// Instrument one-shots: 1 second samples pre-rendered from SoundFont
#define WAVETABLE_SAMPLES 4800

// Drum one-shots: longer buffers for percussive transients
#define DRUM_SAMPLES   4800
#define DRUM_BASE_FREQ ((float)SAMPLE_RATE / DRUM_SAMPLES) // 11.72 Hz

// GM drum key range
#define DRUM_KEY_MIN   35
#define DRUM_KEY_MAX   81
#define DRUM_KEY_COUNT (DRUM_KEY_MAX - DRUM_KEY_MIN + 1)

// ---------------------------------------------------------------------------
// Pre-rendered instrument data
// ---------------------------------------------------------------------------
static int16_t *instrument_data[128];
static nxAudioBuffer instrument_buffers[128];

static int16_t *drum_data[DRUM_KEY_COUNT];
static nxAudioBuffer drum_buffers[DRUM_KEY_COUNT];

static void generate_instrument_samples (tsf *soundfont)
{
    int16_t *temp = (int16_t *)malloc(2048 * sizeof(int16_t));

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

        // Render warmup to get past attack transient
        tsf_render_short(soundfont, temp, 2048, 0);

        // Render the sustain portion as our wavetable
        tsf_render_short(soundfont, instrument_data[prog], WAVETABLE_SAMPLES, 0);

        tsf_note_off_all(soundfont);

        nxAudioBufferInitialize(&instrument_buffers[prog], instrument_data[prog], WAVETABLE_SAMPLES * sizeof(int16_t));

        DbgPrint("Instrument %d rendered\n", prog);
    }

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

        // Render the full transient (no warmup skip for drums)
        tsf_render_short(soundfont, drum_data[idx], DRUM_SAMPLES, 0);

        tsf_note_off_all(soundfont);

        nxAudioBufferInitialize(&drum_buffers[idx], drum_data[idx], DRUM_SAMPLES * sizeof(int16_t));
    }

    DbgPrint("Drums rendered (%d keys)\n", DRUM_KEY_COUNT);
}

// ---------------------------------------------------------------------------
// Per-voice tracking
// ---------------------------------------------------------------------------
typedef struct
{
    int note;
    int channel;
    float base_pitch;
} VoiceInfo;

// ---------------------------------------------------------------------------
// Per-channel state
// ---------------------------------------------------------------------------
typedef struct
{
    int program;
    int volume;
    int expression;
    int pan;
    int pitch_bend;
    bool sustain;
} ChannelState;

// ---------------------------------------------------------------------------
// Visualizer
// ---------------------------------------------------------------------------
static void draw_visualizer (nxAudioVoice voices[], VoiceInfo voice_info[], ChannelState channels[],
                             const char *song_name, uint32_t current_time_ms)
{
    pb_wait_for_vbl();
    pb_target_back_buffer();
    pb_reset();
    pb_fill(0, 0, 640, 480, 0);
    pb_erase_text_screen();

    uint32_t secs = current_time_ms / 1000;
    uint32_t mins = secs / 60;
    secs %= 60;

    int active = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voice_info[i].note >= 0 && nxAudioVoiceGetState(&voices[i]) != NX_STOPPED) {
            active++;
        }
    }

    pb_print("  %s  %d:%02d  Voices: %d/%d\n\n", song_name, mins, secs, active, MAX_VOICES);

    // Voice grid 8x8
    for (int row = 0; row < 8; row++) {
        pb_print("  ");
        for (int col = 0; col < 8; col++) {
            int idx = row * 8 + col;
            if (voice_info[idx].note >= 0 && nxAudioVoiceGetState(&voices[idx]) != NX_STOPPED) {
                pb_print(" %3d ", voice_info[idx].note);
            } else {
                pb_print("  .  ");
            }
        }
        pb_print("\n");
    }

    pb_draw_text_screen();
    while (pb_busy())
        ;
    while (pb_finished())
        ;
}

#include <hal/debug.h>

int main (void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    pb_init();
    pb_show_front_screen();

    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        pb_print("nxAudioInit failed\n");
        pb_draw_text_screen();
        while (pb_busy())
            ;
        while (pb_finished())
            ;
        Sleep(5000);
        pb_kill();
        return -1;
    }

    // -----------------------------------------------------------------------
    // Load SoundFont and pre-render instrument samples
    // -----------------------------------------------------------------------
    const char *sf_path = "D:\\assets\\soundfont.sf2";
    FILE *sf_file = fopen(sf_path, "rb");
    if (!sf_file) {
        pb_print("Could not open %s\n", sf_path);
        pb_draw_text_screen();
        while (pb_busy())
            ;
        while (pb_finished())
            ;
        Sleep(5000);
        pb_kill();
        nxAudioShutdown();
        return -1;
    }

    fseek(sf_file, 0, SEEK_END);
    long sf_size = ftell(sf_file);
    fseek(sf_file, 0, SEEK_SET);

    void *sf_data = malloc(sf_size);
    fread(sf_data, 1, sf_size, sf_file);
    fclose(sf_file);

    DbgPrint("SoundFont loaded: %ld bytes\n", sf_size);

    tsf *soundfont = tsf_load_memory(sf_data, sf_size);
    if (!soundfont) {
        pb_print("Failed to parse SoundFont\n");
        pb_draw_text_screen();
        while (pb_busy())
            ;
        while (pb_finished())
            ;
        Sleep(5000);
        free(sf_data);
        pb_kill();
        nxAudioShutdown();
        return -1;
    }

    DbgPrint("SoundFont parsed: %d presets\n", tsf_get_presetcount(soundfont));

    // Pre-render all instrument wavetables from SoundFont
    generate_instrument_samples(soundfont);
    generate_drum_samples(soundfont);

    // Done with TSF — free it and the SoundFont data
    tsf_close(soundfont);
    free(sf_data);
    DbgPrint("TSF closed, using hardware voices now\n");

    // -----------------------------------------------------------------------
    // Set up hardware voices
    // -----------------------------------------------------------------------
    nxAudioFormat format = {
        .sample_rate = SAMPLE_RATE,
        .channels = 1,
        .bytes_per_sample = 2,
        .codec = NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_2D_STATIC,
    };

    nxAudioVoice voices[MAX_VOICES];
    VoiceInfo voice_info[MAX_VOICES];

    for (int i = 0; i < MAX_VOICES; i++) {
        nxAudioVoiceCreate(&voices[i], &format);
        nxAudioVoiceSetLooping(&voices[i], true);
        nxAudioVoiceSetAmplitudeEnvelope(&voices[i], &(nxAudioADSR){0, 1, 0, 8, 200, 200});
        voice_info[i].note = -1;
        voice_info[i].channel = -1;
        voice_info[i].base_pitch = 1.0f;
    }

    ChannelState channels[16];
    for (int i = 0; i < 16; i++) {
        channels[i].program = 0;
        channels[i].volume = 100;
        channels[i].expression = 127;
        channels[i].pan = 64;
        channels[i].pitch_bend = 8192;
        channels[i].sustain = false;
    }

    // -----------------------------------------------------------------------
    // Load MIDI file
    // -----------------------------------------------------------------------
    const char *midi_path = "D:\\assets\\madworld.mid";
    FILE *f = fopen(midi_path, "rb");
    if (!f) {
        pb_print("Could not open %s\n", midi_path);
        pb_draw_text_screen();
        while (pb_busy())
            ;
        while (pb_finished())
            ;
        Sleep(5000);
        pb_kill();
        nxAudioShutdown();
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *midi_buffer = malloc(file_size);
    fread(midi_buffer, 1, file_size, f);
    fclose(f);

    tml_message *midi = tml_load_memory(midi_buffer, file_size);
    tml_message *msg = midi;
    uint32_t current_time_ms = 0;
    uint32_t last_vis_time = 0;

    const char *song_name = strrchr(midi_path, '\\');
    song_name = song_name ? song_name + 1 : midi_path;

    DbgPrint("MIDI loaded, starting playback...\n");

    // -----------------------------------------------------------------------
    // Playback loop — hardware voices driven by MIDI events
    // -----------------------------------------------------------------------
    while (msg) {
        if (msg->time > current_time_ms) {
            uint32_t wait_time = msg->time - current_time_ms;

            while (wait_time > 0) {
                uint32_t sleep_amt = (wait_time > 50) ? 50 : wait_time;

                if (current_time_ms - last_vis_time >= 100) {
                    draw_visualizer(voices, voice_info, channels, song_name, current_time_ms);
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

            case TML_CONTROL_CHANGE:
                switch (msg->control) {
                    case TML_VOLUME_MSB:
                        channels[ch].volume = msg->control_value;
                        for (int i = 0; i < MAX_VOICES; i++) {
                            if (voice_info[i].channel == ch && voice_info[i].note >= 0) {
                                float vol = (channels[ch].volume / 127.0f) * (channels[ch].expression / 127.0f);
                                nxAudioVoiceSetMasterGain(&voices[i], vol);
                            }
                        }
                        break;
                    case TML_EXPRESSION_MSB:
                        channels[ch].expression = msg->control_value;
                        for (int i = 0; i < MAX_VOICES; i++) {
                            if (voice_info[i].channel == ch && voice_info[i].note >= 0) {
                                float vol = (channels[ch].volume / 127.0f) * (channels[ch].expression / 127.0f);
                                nxAudioVoiceSetMasterGain(&voices[i], vol);
                            }
                        }
                        break;
                    case TML_PAN_MSB:
                        channels[ch].pan = msg->control_value;
                        for (int i = 0; i < MAX_VOICES; i++) {
                            if (voice_info[i].channel == ch && voice_info[i].note >= 0) {
                                float pan = ((float)channels[ch].pan - 64.0f) / 64.0f;
                                float left = pan <= 0.0f ? 1.0f : 1.0f - pan;
                                float right = pan >= 0.0f ? 1.0f : 1.0f + pan;
                                nxAudioVoiceSetChannelGain(&voices[i], left, right, 0.0f, 0.0f, 0.0f, 0.0f);
                            }
                        }
                        break;
                    case TML_SUSTAIN_SWITCH:
                        channels[ch].sustain = (msg->control_value >= 64);
                        if (!channels[ch].sustain) {
                            for (int i = 0; i < MAX_VOICES; i++) {
                                if (voice_info[i].channel == ch && voice_info[i].note < 0 &&
                                    nxAudioVoiceGetState(&voices[i]) != NX_STOPPED) {
                                    nxAudioVoiceRelease(&voices[i]);
                                }
                            }
                        }
                        break;
                    case TML_ALL_NOTES_OFF:
                    case TML_ALL_SOUND_OFF:
                        for (int i = 0; i < MAX_VOICES; i++) {
                            if (voice_info[i].channel == ch) {
                                nxAudioVoiceRelease(&voices[i]);
                                voice_info[i].note = -1;
                                voice_info[i].channel = -1;
                            }
                        }
                        break;
                    default:
                        break;
                }
                break;

            case TML_PITCH_BEND: {
                channels[ch].pitch_bend = msg->pitch_bend;
                float bend_semitones = ((float)msg->pitch_bend - 8192.0f) / 8192.0f * 2.0f;
                float bend_ratio = powf(2.0f, bend_semitones / 12.0f);
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (voice_info[i].channel == ch && voice_info[i].note >= 0) {
                        nxAudioVoiceSetPitch(&voices[i], voice_info[i].base_pitch * bend_ratio);
                    }
                }
                break;
            }

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
                    float ch_gain = (channels[ch].volume / 127.0f) * (channels[ch].expression / 127.0f);

                    float bend_semitones = ((float)channels[ch].pitch_bend - 8192.0f) / 8192.0f * 2.0f;
                    float bend_ratio = powf(2.0f, bend_semitones / 12.0f);

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

                    nxAudioVoiceSetPitch(&voices[free_idx], base_pitch * bend_ratio);
                    nxAudioVoiceSetMasterGain(&voices[free_idx], vel_gain * ch_gain);
                    float pan = ((float)channels[ch].pan - 64.0f) / 64.0f;
                    float left = pan <= 0.0f ? 1.0f : 1.0f - pan;
                    float right = pan >= 0.0f ? 1.0f : 1.0f + pan;
                    nxAudioVoiceSetChannelGain(&voices[free_idx], left, right, 0.0f, 0.0f, 0.0f, 0.0f);
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
                        if (channels[ch].sustain) {
                            voice_info[i].note = -1;
                        } else {
                            nxAudioVoiceRelease(&voices[i]);
                            voice_info[i].note = -1;
                            voice_info[i].channel = -1;
                        }
                        break;
                    }
                }
                break;

            default:
                break;
        }

        msg = msg->next;
    }

    draw_visualizer(voices, voice_info, channels, song_name, current_time_ms);
    Sleep(2000);

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------
    for (int i = 0; i < MAX_VOICES; i++) {
        nxAudioVoiceStop(&voices[i]);
        nxAudioVoiceDestroy(&voices[i]);
    }

    tml_free(midi);
    free(midi_buffer);

    for (int i = 0; i < 128; i++) {
        MmFreeContiguousMemory(instrument_data[i]);
    }
    for (int i = 0; i < DRUM_KEY_COUNT; i++) {
        MmFreeContiguousMemory(drum_data[i]);
    }

    nxAudioShutdown();
    pb_kill();

    return 0;
}
