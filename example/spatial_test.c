#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>

#include <SDL.h>
#include <nxaudio.h>

#define CHUNK_SAMPLES 24000
#define SAMPLE_RATE   48000

static int hrtf_index = 0;

static void fill_noise_chunk (int16_t *out, int num_samples)
{
    for (int i = 0; i < num_samples; i++) {
        out[i] = (int16_t)((rand() * 2) - 32768);
    }
}

int main (void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    pb_init();
    pb_show_front_screen();

    // SDL2 controller init
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        debugPrint("SDL init failed: %s\n", SDL_GetError());
    }

    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        debugPrint("nxAudioInit failed\n");
        return -1;
    }

    // 3D static voice format — mono, 16-bit
    nxAudioFormat format = {
        .sample_rate = SAMPLE_RATE,
        .channels = 1,
        .bytes_per_sample = 2,
        .codec = NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_3D_STATIC,
    };

    nxAudioVoice voice;
    nxAudioVoiceCreate(&voice, &format);
    nxAudioVoiceSetMasterGain(&voice, 1.5f);
    nxAudioVoiceSetLooping(&voice, true);

    int16_t *noise_data = (int16_t *)MmAllocateContiguousMemory(CHUNK_SAMPLES * sizeof(int16_t));
    if (!noise_data) {
        debugPrint("Failed to allocate noise data\n");
        nxAudioShutdown();
        return -1;
    }
    fill_noise_chunk(noise_data, CHUNK_SAMPLES);

    nxAudioBuffer buffer;
    nxAudioBufferInitialize(&buffer, noise_data, CHUNK_SAMPLES * sizeof(int16_t));
    nxAudioBufferSubmit(&voice, &buffer);

    float azimuth = -90.0f;
    float elevation = 0.0f;
    bool running = true;

    // Load initial HRTF (straight ahead) and start playback
    nxAudioHRTFSetParamsFromAngles(hrtf_index, azimuth, elevation);
    nxAudioVoiceSetHRTFTarget(&voice, hrtf_index);

    // HW supports two HRTF filters for better transitions. Toggle this bit so the next update
    // goes to an alternating index.
    hrtf_index ^= 1;

    nxAudioVoiceStart(&voice);

    SDL_GameController *controller = NULL;

    while (running) {
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

            int16_t rx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
            int16_t ry = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);

            bool dpad_left = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            bool dpad_right = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            bool dpad_up = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
            bool dpad_down = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            static bool prev_left = false, prev_right = false, prev_up = false, prev_down = false;

            const int16_t deadzone = 6000;
            bool updated = false;

            // Right stick left/right controls azimuth
            if (abs(rx) > deadzone) {
                azimuth += (float)rx / 32768.0f * 3.0f;
                if (azimuth > 180.0f) {
                    azimuth -= 360.0f;
                }
                if (azimuth < -180.0f) {
                    azimuth += 360.0f;
                }
                updated = true;
            }

            // Right stick up/down controls elevation (SDL Y is inverted)
            if (abs(ry) > deadzone) {
                elevation -= (float)ry / 32768.0f * 2.0f;
                updated = true;
            }

            // D-PAD incremental control
            if (dpad_left && !prev_left) {
                azimuth -= 10.0f;
                updated = true;
            }
            if (dpad_right && !prev_right) {
                azimuth += 10.0f;
                updated = true;
            }
            if (dpad_up && !prev_up) {
                elevation += 10.0f;
                updated = true;
            }
            if (dpad_down && !prev_down) {
                elevation -= 10.0f;
                updated = true;
            }

            prev_left = dpad_left;
            prev_right = dpad_right;
            prev_up = dpad_up;
            prev_down = dpad_down;

            if (updated) {
                if (azimuth > 180.0f) {
                    azimuth -= 360.0f;
                }
                if (azimuth < -180.0f) {
                    azimuth += 360.0f;
                }
                if (elevation > 90.0f) {
                    elevation = 90.0f;
                }
                if (elevation < -90.0f) {
                    elevation = -90.0f;
                }
                nxAudioHRTFSetParamsFromAngles(hrtf_index, azimuth, elevation);
                nxAudioVoiceSetHRTFTarget(&voice, hrtf_index);
                hrtf_index ^= 1;
            }
        }

        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();
        pb_erase_depth_stencil_buffer(0, 0, 640, 480);
        pb_fill(0, 0, 640, 480, 0);
        pb_erase_text_screen();

        pb_print("3D Audio HRTF Test (noise)\n\n");
        pb_print("Right Stick: Pan sound smoothly\n");
        pb_print("D-PAD      : Incremental steps (10 deg)\n");
        pb_print("  Left/Right = Azimuth\n");
        pb_print("  Up/Down    = Elevation\n");
        pb_print("START        = Exit\n\n");
        pb_print("Azimuth:   %d deg\n", (int)azimuth);
        pb_print("Elevation: %d deg\n", (int)elevation);

        pb_draw_text_screen();

        while (pb_busy()) {
        }
        while (pb_finished()) {
        }
    }

    nxAudioVoiceStop(&voice);
    nxAudioVoiceDestroy(&voice);

    if (noise_data) {
        MmFreeContiguousMemory(noise_data);
    }

    nxAudioShutdown();

    if (controller) {
        SDL_GameControllerClose(controller);
    }
    SDL_Quit();

    return 0;
}
