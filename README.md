# nxdk-audio

A powerful, open-source hardware audio library for the original Xbox using the [nxdk](https://github.com/XboxDev/nxdk) toolchain. 

`nxdk-audio` interfaces directly with the MCPX Audio Processing Unit (APU) to deliver high-performance, low-latency audio mixing straight on the hardware.

## Features

* Hardware accelerated mixing (up to 256 concurrent voices)
* Amplitude & Filter Envelopes (Delay, Attack, Hold, Decay, Sustain, Release)
* 3D Spatial Audio Head-Related Transfer Functions (with headphones).
* Playback Modes: 
  * Static Buffers: One-shot sounds or infinitely looping buffers stored in memory
  * Streaming Buffers: Double-buffered audio queue API for streaming large files
* Format Support:
  * Uncompressed PCM (8-bit, 16-bit, 24-bit 4 byte container, 32-bit)
  * Stereo, Mono
  * Hardware accelerated ADPCM decoding (Xbox ADPCM)
  * Any sample rate with hardware-accelerated pitch conversion

## Minimal Example

```c
#include <nxaudio.h>

extern const uint8_t pcm_data[];
extern const uint32_t pcm_data_size;

int main(void) {
    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        return -1;
    }

    nxAudioFormat format = {
        .sample_rate = 44100,
        .channels = 2,
        .bytes_per_sample = 2,
        .codec = NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_2D_STATIC,
    };

    nxAudioVoice voice;
    nxAudioVoiceCreate(&voice, &format);

    nxAudioBuffer buffer;
    nxAudioBufferInitialize(&buffer, pcm_data, pcm_data_size);
    nxAudioBufferSubmit(&voice, &buffer);
    nxAudioVoiceStart(&voice);

    // ... game loop ...

    nxAudioVoiceStop(&voice);
    nxAudioVoiceDestroy(&voice);
    nxAudioShutdown();

    return 0;
}
```

## CMake Usage
```cmake
add_subdirectory(nxdk-audio)
target_link_libraries(my_game_target PRIVATE nxaudio)
```

## TODO
* [ ] Reverb hardware effects (Or just more DSP effects)
* [ ] Direct 5.1 surround
* [ ] More HRTF validation
* [ ] More user configuration (Configurable voice count etc to reduce RAM usage)

## Attribution
* https://xboxdevwiki.net/APU
* https://xemu.app/
* https://www.sonicom.eu/tools-and-resources/hrtf-dataset/ (HRTF dataset CC BY 4.0) 
* https://github.com/Ryzee119/xbox-apu-hrtf
