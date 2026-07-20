// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Ryzee119
#include "audio_apu_regs.h"
#include "audio_internal.h"
#include <nxaudio.h>

#include <stdbool.h>
#include <stdint.h>

static inline float fast_mul_log2f (float multiplier, float x)
{
    float result;

    __asm__ __volatile__("flds %[mult] \n\t" // ST(1) = multiplier
                         "flds %[val] \n\t"  // ST(0) = val
                         "fyl2x \n\t"        // result = ST(1) * log2(ST(0))
                         "fstps %[res] \n\t"
                         : [res] "=m"(result)
                         : [val] "m"(x), [mult] "m"(multiplier)
                         : "st", "st(1)");

    return result;
}

static int16_t voice_allocate (bool is_3d)
{
    int16_t voice_index = -1;

    // Voices 0-63 are reserved for 3D voices.
    const int end_i = is_3d ? 2 : (MCPX_HW_MAX_VOICES + 31) / 32;
    const int start_i = is_3d ? 0 : 2;

    for (int i = start_i; i < end_i; i++) {
        if (g_voice_allocation_mask[i]) {
            const int bit = __builtin_ctz(g_voice_allocation_mask[i]);
            g_voice_allocation_mask[i] &= ~(1U << bit);
            voice_index = (int16_t)((i * 32) + bit);
            break;
        }
    }
    return voice_index;
}

static void voice_free (uint8_t voice_index)
{
    const uint32_t dword = voice_index / 32;
    const uint32_t bit = voice_index % 32;
    g_voice_allocation_mask[dword] |= (1U << bit);
}

uint32_t apu_format_get_container_size (const nxAudioFormat *format)
{
    if (format->codec == NX_AUDIO_CODEC_ADPCM) {
        return NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    }

    switch (format->bytes_per_sample) {
        case 1:
            return NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B8;
        case 2:
            return NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16;
        case 3:
        case 4:
            return NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B32;
        default:
            return 0;
    }
}

uint32_t apu_format_get_sample_size (const nxAudioFormat *format)
{
    if (format->codec == NX_AUDIO_CODEC_ADPCM) {
        return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
    }

    switch (format->bytes_per_sample) {
        case 1:
            return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8;
        case 2:
            return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16;
        case 3:
            return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        case 4:
            return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32;
        default:
            return 0;
    }
}

static inline int32_t convert_sample_rate_to_pitch_value (float sample_rate)
{
    // p = 4096 * log2(f / 48000)
    return (int32_t)fast_mul_log2f(4096.0f, sample_rate / 48000.0f);
}

void apu_flush_voice (nxAudioVoice *voice)
{
    apu_wait_for_pio(18);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + 0x350, 0);
    apu_write_reg(APU_VP_OFFSET + 0x370, 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_MISC, voice->cfg_misc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV0, voice->cfg_env0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVA, voice->cfg_enva);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV1, voice->cfg_env1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVF, voice->cfg_envf);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_LFO_ENV, voice->tar_lfo_env);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCA, voice->tar_fca);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCB, voice->tar_fcb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_VBIN, voice->cfg_vbin);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, voice->tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, voice->tar_volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, voice->tar_volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_PITCH, voice->tar_pitch_link);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_HRTF, voice->hrtf_target);
}

bool nxAudioVoiceCreate (nxAudioVoice *voice, const nxAudioFormat *format)
{
    if (!voice || !format) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    const int16_t voice_index =
        voice_allocate(format->type == NX_VOICE_TYPE_3D_STATIC || format->type == NX_VOICE_TYPE_3D_STREAM);
    if (voice_index == -1) {
        apu_set_last_error(NX_AUDIO_ERR_OUT_OF_VOICES);
        return false;
    }
    memset(voice, 0, sizeof(nxAudioVoice));

    voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_HEADROOM, 7);

    // Enable notification IRQ?
    voice->cfg_misc |= (1 << 23);

    // Setup the voice default state
    voice->state = NX_STOPPED;
    voice->voice_index = (uint8_t)voice_index;

    apu_flush_voice(voice);

    if (!nxAudioVoiceSetFormat(voice, format)) {
        voice_free((uint8_t)voice_index);
        return false;
    }

    nxAudioVoiceSetHRTFTarget(voice, HRTF_NULL_HANDLE);
    nxAudioVoiceSetMasterGain(voice, 1.0f);
    nxAudioVoiceSetChannelGain(voice, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    nxAudioVoiceSetHRTFGain(voice, 1.0f, 1.0f);
    nxAudioVoiceSetPitch(voice, 1.0f);

    return true;
}

bool nxAudioVoiceStart (nxAudioVoice *voice)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (!voice->buffers_hardware[0] && !voice->buffers_hardware[1]) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_STATE);
        return false;
    }

    // If we are just paused, we can skip the whole setup and just unpause the voice.
    if (voice->state == NX_PAUSED) {
        voice->state = NX_PLAYING;

        const KIRQL irql = KeRaiseIrqlToDpcLevel();
        apu_wait_for_pio(1);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice->voice_index | 0);
        KfLowerIrql(irql);
        return true;
    }

    if (nxAudioVoiceGetState(voice) != NX_STOPPED) {
        return true;
    }

    const bool is_2d = voice->format.type == NX_VOICE_TYPE_2D_STREAM || voice->format.type == NX_VOICE_TYPE_2D_STATIC;
    const uint32_t antecedent_cmd =
        APU_MAKE_VALUE(NV1BA0_PIO_SET_ANTECEDENT_VOICE_HANDLE, 0xFFFF) |
        APU_MAKE_VALUE(NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST, is_2d ? NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP
                                                                   : NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP);

    const uint32_t notifier_index = MCPX_HW_NOTIFIER_BASE_OFFSET + (voice->voice_index * MCPX_HW_NOTIFIER_COUNT);
    const KIRQL irql = KeRaiseIrqlToDpcLevel();

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_PERSIST);
    if (voice->streaming) {
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_PERSIST, 1);
    }

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_LOOP);
    if (voice->looping) {
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_LOOP, 1);
    }

    voice->state = NX_PLAYING;

    g_tracked_voices[voice->voice_index] = voice;
    g_hw_notifiers[notifier_index + 0].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;
    g_hw_notifiers[notifier_index + 1].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;

    apu_wait_for_pio(7);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_ANTECEDENT_VOICE, antecedent_cmd);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_ON, voice->voice_index | voice->voice_on_flags);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice->voice_index | 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    KfLowerIrql(irql);
    return true;
}

bool nxAudioVoiceRelease (nxAudioVoice *voice)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->state != NX_PLAYING) {
        return true;
    }

    const KIRQL irql = KeRaiseIrqlToDpcLevel();
    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_PERSIST);
    voice->state = NX_STOPPING;

    apu_wait_for_pio(5);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_RELEASE, voice->voice_index);
    KfLowerIrql(irql);

    return true;
}

bool nxAudioVoiceStop (nxAudioVoice *voice)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->state == NX_STOPPED) {
        return true;
    }

    const KIRQL irql = KeRaiseIrqlToDpcLevel();
    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_PERSIST);
    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_LOOP);
    voice->state = NX_STOPPING;

    apu_wait_for_pio(5);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_OFF, voice->voice_index);
    KfLowerIrql(irql);

    return true;
}

nxAudioVoiceState nxAudioVoiceGetState (const nxAudioVoice *voice)
{
    if (!voice) {
        return NX_STOPPED;
    }
    return voice->state;
}

bool nxAudioVoicePause (nxAudioVoice *voice)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->state == NX_STOPPED) {
        return true;
    }

    const KIRQL irql = KeRaiseIrqlToDpcLevel();
    voice->state = NX_PAUSED;
    apu_wait_for_pio(1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice->voice_index | NV1BA0_PIO_VOICE_PAUSE_ACTION);
    KfLowerIrql(irql);

    return true;
}

void nxAudioVoiceDestroy (nxAudioVoice *voice)
{
    if (!voice) {
        return;
    }

    nxAudioVoiceStop(voice);

    // Ensure the voice has truly stopped before freeing resources
    int32_t timeout = 100000;
    while (nxAudioVoiceGetState(voice) != NX_STOPPED && (timeout > 0)) {
        KeStallExecutionProcessor(100);
        timeout -= 100;
    }

    for (uint32_t j = 0; j < 2; j++) {
        if (voice->buffers_hardware[j]) {
            MmLockUnlockBufferPages((PVOID)voice->buffers_hardware[j]->buffer, voice->buffers_hardware[j]->size_bytes,
                                    TRUE);
            voice->buffers_hardware[j] = NULL;
        }
    }

    if (voice->sge_count > 0) {
        apu_sge_free(voice->sge_base, voice->sge_count);
        voice->sge_count = 0;
    }

    voice_free(voice->voice_index);
}

static uint32_t gain_to_attenuation (float gain)
{
    if (gain <= 0.0f) {
        return 0xFFF; // Mute
    }

    // Convert linear gain to decibels of attenuation:
    //   dB = -20 * log10(gain)
    //      = -20 * log10(2) * log2(gain)
    //      = -6.0206 * log2(gain)
    const float attenuation_db = fast_mul_log2f(-6.0206f, gain);

    // Add a 6 dB floor preserving headroom in the mixbin accumulator when many voices sum together.
    const float total_db = attenuation_db + 6.0f;

    // Hardware register is 12-bit, in units of 1/64 dB (0x000 = 0 dB, 0xFFF = mute)
    const int32_t units = (int32_t)(total_db * 64.0f);

    if (units <= 0) {
        return 0x000;
    } else if (units >= 0xFFF) {
        return 0xFFF;
    }
    return (uint32_t)units;
}

static void voice_update_volumes (nxAudioVoice *voice)
{
    const uint32_t master = gain_to_attenuation(voice->master_gain);
    uint32_t vol[8];

    if (voice->hrtf_target != HRTF_NULL_HANDLE) {
        // HRTF mapping overrides bins 0-3
        // V0BIN (6) = Front Left
        // V1BIN (7) = Front Right
        // V2BIN (8) = Rear Left
        // V3BIN (9) = Rear Right
        uint32_t front = gain_to_attenuation(voice->hrtf_front_gain);
        uint32_t rear = gain_to_attenuation(voice->hrtf_rear_gain);

        vol[0] = master + front;
        vol[1] = master + front;
        vol[2] = master + rear;
        vol[3] = master + rear;
        vol[4] = 0xFFF;
        vol[5] = 0xFFF;
        vol[6] = 0xFFF;
        vol[7] = 0xFFF;
    } else {
        vol[0] = master + gain_to_attenuation(voice->gain_front_left);
        vol[1] = master + gain_to_attenuation(voice->gain_front_right);
        vol[2] = master + gain_to_attenuation(voice->gain_center);
        vol[3] = master + gain_to_attenuation(voice->gain_lfe);
        vol[4] = master + gain_to_attenuation(voice->gain_rear_left);
        vol[5] = master + gain_to_attenuation(voice->gain_rear_right);
        vol[6] = 0xFFF;
        vol[7] = 0xFFF;
    }

    for (int i = 0; i < 8; i++) {
        if (vol[i] > 0xFFF) {
            vol[i] = 0xFFF;
        }
    }

    voice->tar_vola = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME0, vol[0]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME1, vol[1]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0, vol[6]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0, vol[7]);

    voice->tar_volb = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME2, vol[2]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME3, vol[3]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4, vol[6] >> 4) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4, vol[7] >> 4);

    voice->tar_volc = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME4, vol[4]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME5, vol[5]) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8, vol[6] >> 8) |
                      APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8, vol[7] >> 8);
}

bool nxAudioVoiceSetMasterGain (nxAudioVoice *voice, float gain)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->master_gain == gain) {
        return true;
    }

    voice->master_gain = gain;
    voice_update_volumes(voice);

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, voice->tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, voice->tar_volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, voice->tar_volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    return true;
}

bool nxAudioVoiceSetChannelGain (nxAudioVoice *voice, float front_left, float front_right, float center, float lfe,
                                 float rear_left, float rear_right)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->gain_front_left == front_left && voice->gain_front_right == front_right &&
        voice->gain_center == center && voice->gain_lfe == lfe && voice->gain_rear_left == rear_left &&
        voice->gain_rear_right == rear_right) {
        return true;
    }

    voice->gain_front_left = front_left;
    voice->gain_front_right = front_right;
    voice->gain_center = center;
    voice->gain_lfe = lfe;
    voice->gain_rear_left = rear_left;
    voice->gain_rear_right = rear_right;

    voice_update_volumes(voice);

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, voice->tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, voice->tar_volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, voice->tar_volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}

bool nxAudioVoiceSetHRTFGain (nxAudioVoice *voice, float front_gain, float rear_gain)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->hrtf_front_gain == front_gain && voice->hrtf_rear_gain == rear_gain) {
        return true;
    }

    voice->hrtf_front_gain = front_gain;
    voice->hrtf_rear_gain = rear_gain;

    voice_update_volumes(voice);

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, voice->tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, voice->tar_volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, voice->tar_volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}

bool nxAudioVoiceSetLooping (nxAudioVoice *voice, bool looping)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    // Looping is only supported on static buffers
    if (voice->streaming && looping) {
        apu_set_last_error(NX_AUDIO_ERR_UNSUPPORTED);
        return false;
    }

    voice->looping = looping;

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_LOOP);
    if (looping) {
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_LOOP, 1);
    }

    apu_wait_for_pio(4);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    return true;
}

bool nxAudioVoiceSetFormat (nxAudioVoice *voice, const nxAudioFormat *format)
{
    if (!voice || !format || format->channels == 0) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->state != NX_STOPPED) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_STATE);
        return false;
    }

    const bool request_3d = format->type == NX_VOICE_TYPE_3D_STATIC || format->type == NX_VOICE_TYPE_3D_STREAM;
    const bool is_3d = voice->voice_index < MCPX_HW_MAX_3D_VOICES;

    // If we need to swap to a 3D or 2D voice do that now
    if (request_3d != is_3d) {
        const int16_t new_voice_index = voice_allocate(request_3d);
        if (new_voice_index == -1) {
            apu_set_last_error(NX_AUDIO_ERR_OUT_OF_VOICES);
            return false;
        }
        voice_free(voice->voice_index);
        voice->voice_index = (uint8_t)new_voice_index;
        apu_flush_voice(voice);
    }

    voice->format = *format;

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE, apu_format_get_container_size(format));

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE, apu_format_get_sample_size(format));

    // VP support two types of sound data.
    // Streaming mode (via SSLs) and Static mode (via SGEs)
    // Streaming mode supports dual buffer ping pong for really long audio stream.
    // Static mode queues a (usually short) sound in one go. It supports looping and replay with minimal CPU
    // intervention.
    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    if (voice->streaming) {
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_DATA_TYPE, 1);
    }

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_STEREO);
    if (format->channels == 2) {
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_STEREO, 1);
    }

    voice->cfg_fmt &= ~((uint32_t)NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK, format->channels - 1);

    const int32_t pitch_val = convert_sample_rate_to_pitch_value((float)format->sample_rate * voice->pitch);
    voice->tar_pitch_link &= ~((uint32_t)NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    voice->tar_pitch_link |= APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH, (uint32_t)(int16_t)pitch_val);

    apu_wait_for_pio(5);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_PITCH, voice->tar_pitch_link);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}

bool nxAudioVoiceSetPitch (nxAudioVoice *voice, float pitch)
{
    if (!voice || pitch <= 0.0f) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    const float effective_sample_rate = (float)voice->format.sample_rate * pitch;
    const int32_t pitch_val = convert_sample_rate_to_pitch_value(effective_sample_rate);
    const uint32_t tar_pitch = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH, (uint32_t)(int16_t)pitch_val);
    voice->pitch = pitch;

    apu_wait_for_pio(4);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_PITCH, tar_pitch);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    return true;
}

bool nxAudioVoiceSetAmplitudeEnvelope (nxAudioVoice *voice, const nxAudioADSR *adsr)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    // You can only change ADSR envelope when voice is stopped
    if (nxAudioVoiceGetState(voice) != NX_STOPPED) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_STATE);
        return false;
    }

    // Passing NULL turns off ADSR
    if (!adsr) {
        voice->voice_on_flags &= ~NV1BA0_PIO_VOICE_ON_ENVA;
        voice->amplitude_adsr_enabled = false;
        return true;
    }

    voice->amplitude_adsr = *adsr;

    voice->cfg_env0 &= ~((uint32_t)(NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE | NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME));
    voice->cfg_env0 |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE, adsr->attack_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME, adsr->delay_time);

    voice->cfg_enva &= ~((uint32_t)(NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE | NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME |
                                    NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL));
    voice->cfg_enva |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE, adsr->decay_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME, adsr->hold_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL, adsr->sustain_level);

    voice->tar_lfo_env &= ~((uint32_t)NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
    voice->tar_lfo_env |= APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE, adsr->release_time);

    voice->voice_on_flags |= APU_MAKE_VALUE(NV1BA0_PIO_VOICE_ON_ENVA, NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY);

    voice->amplitude_adsr_enabled = true;

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV0, voice->cfg_env0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVA, voice->cfg_enva);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_LFO_ENV, voice->tar_lfo_env);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    return true;
}

bool nxAudioVoiceSetFilterEnvelope (nxAudioVoice *voice, const nxAudioADSR *adsr)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    // You can only change ADSR envelope when voice is stopped
    if (nxAudioVoiceGetState(voice) != NX_STOPPED) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_STATE);
        return false;
    }

    // Passing NULL turns off ADSR
    if (!adsr) {
        voice->voice_on_flags &= ~((uint32_t)NV1BA0_PIO_VOICE_ON_ENVF);
        voice->filter_adsr_enabled = false;
        return true;
    }

    voice->filter_adsr = *adsr;

    voice->cfg_env1 &= ~((uint32_t)(NV_PAVS_VOICE_CFG_ENV1_EF_ATTACKRATE | NV_PAVS_VOICE_CFG_ENV1_EF_DELAYTIME));
    voice->cfg_env1 |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENV1_EF_ATTACKRATE, adsr->attack_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENV1_EF_DELAYTIME, adsr->delay_time);

    voice->cfg_envf &= ~((uint32_t)(NV_PAVS_VOICE_CFG_ENVF_EF_DECAYRATE | NV_PAVS_VOICE_CFG_ENVF_EF_HOLDTIME |
                                    NV_PAVS_VOICE_CFG_ENVF_EF_SUSTAINLEVEL));
    voice->cfg_envf |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVF_EF_DECAYRATE, adsr->decay_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVF_EF_HOLDTIME, adsr->hold_time) |
                       APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVF_EF_SUSTAINLEVEL, adsr->sustain_level);

    voice->cfg_misc &= ~((uint32_t)NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
    voice->cfg_misc |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE, adsr->release_time);

    voice->voice_on_flags |= APU_MAKE_VALUE(NV1BA0_PIO_VOICE_ON_ENVF, NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY);

    voice->filter_adsr_enabled = true;

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV1, voice->cfg_env1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVF, voice->cfg_envf);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_MISC, voice->cfg_misc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    return true;
}

bool nxAudioVoiceSetFilterCoefficients (nxAudioVoice *voice, nxAudioFilterMode mode, uint8_t q_coefficient, int16_t fc0,
                                        int16_t fc1, int16_t fc2, int16_t fc3)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->state != NX_STOPPED) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_STATE);
        return false;
    }

    const uint32_t tar_fca = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_FCA_FC0, (uint16_t)fc0) |
                             APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_FCA_FC1, (uint16_t)fc1);

    const uint32_t tar_fcb = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_FCB_FC2, (uint16_t)fc2) |
                             APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_FCB_FC3, (uint16_t)fc3);

    voice->tar_fca = tar_fca;
    voice->tar_fcb = tar_fcb;

    voice->cfg_misc &= ~((uint32_t)(NV_PAVS_VOICE_CFG_MISC_FMODE | (0x7 << 18)));
    voice->cfg_misc |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_MISC_FMODE, mode) | APU_MAKE_VALUE((0x7 << 18), q_coefficient);

    apu_wait_for_pio(6);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCA, tar_fca);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCB, tar_fcb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_MISC, voice->cfg_misc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}

uint32_t nxAudioVoiceGetPlaybackOffset (const nxAudioVoice *voice)
{
    if (!voice) {
        return 0;
    }

    return g_hw_voices[voice->voice_index].par_offset & NV_PAVS_VOICE_PAR_OFFSET_CBO;
}

bool nxAudioVoiceSetPlaybackOffset (nxAudioVoice *voice, uint32_t byte_offset)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->streaming) {
        apu_set_last_error(NX_AUDIO_ERR_UNSUPPORTED);
        return false;
    }

    const uint32_t block_offset = apu_format_bytes_to_blocks(&voice->format, byte_offset);

    if (nxAudioVoiceGetState(voice) != NX_STOPPED) {
        uint32_t par_state;
        do {
            par_state = g_hw_voices[voice->voice_index].par_state;
        } while (par_state & NV_PAVS_VOICE_PAR_STATE_NEW_VOICE);
    }

    const uint32_t ebo = g_hw_voices[voice->voice_index].par_next & NV_PAVS_VOICE_PAR_NEXT_EBO;
    if (block_offset > ebo) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    apu_wait_for_pio(4);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_BUF_CBO, block_offset);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}

bool nxAudioVoiceSetHRTFTarget (nxAudioVoice *voice, uint16_t index)
{
    if (!voice) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (index >= HRTF_ENTRY_COUNT && index != NX_AUDIO_HRTF_NONE) {
        apu_set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    // When transitioning between 2D and 3D (HRTF) modes, we must reconfigure which
    // hardware mixbins each voice volume slot routes to.
    //
    // The VP has 8 volume slots (vol0-vol7) per voice. Each slot is paired with a
    // mixbin assignment via CFG_VBIN (slots 0-5) and CFG_FMT (slots 6-7).
    //
    // For 3D voices (voice index < 64), the VP *ignores* V0BIN-V3BIN and forcibly
    // overrides bins 0-3 with the global HRTF submix values set in audio_core.c
    // (SET_HRTF_SUBMIXES = 0x09070806: bin0->6, bin1->8, bin2->7, bin3->9).
    // The HRTF filter produces stereo (L/R) output, and the VP routes it as:
    //   vol0 -> hrtf_submix[0] = mixbin 6 (front-left,  left ear,  front attenuation)
    //   vol1 -> hrtf_submix[1] = mixbin 8 (back-left,   left ear,  rear attenuation)
    //   vol2 -> hrtf_submix[2] = mixbin 7 (front-right, right ear, front attenuation)
    //   vol3 -> hrtf_submix[3] = mixbin 9 (back-right,  right ear, rear attenuation)
    bool hrtf_state_dirty = false;

    if (voice->hrtf_target == HRTF_NULL_HANDLE && index != NX_AUDIO_HRTF_NONE) {
        // Transitioning 2D to HRTF: set bins to HRTF-compatible values.
        // V0-V3: Written for consistency but overridden by VP for 3D voices.
        // V4-V5: Routed to bins 10 and 0 (muted, just need unique indices).
        // V6-V7: In CFG_FMT, routed to bins 1 and 2 (muted, unique padding).
        voice->cfg_vbin =
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V0BIN, 6) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V1BIN, 7) |
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V2BIN, 8) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V3BIN, 9) |
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V4BIN, 10) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V5BIN, 0);

        voice->cfg_fmt &= ~((uint32_t)(NV_PAVS_VOICE_CFG_FMT_V6BIN | NV_PAVS_VOICE_CFG_FMT_V7BIN));
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V6BIN, 1);
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V7BIN, 2);
        hrtf_state_dirty = true;

    } else if (voice->hrtf_target != HRTF_NULL_HANDLE && index == NX_AUDIO_HRTF_NONE) {
        // Transitioning HRTF to 2D: restore standard 1:1 bin mapping.
        // V0-V7 map directly to mixbins 0-7 for straightforward multi-channel output.
        voice->cfg_vbin =
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V0BIN, 0) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V1BIN, 1) |
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V2BIN, 2) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V3BIN, 3) |
            APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V4BIN, 4) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V5BIN, 5);

        voice->cfg_fmt &= ~((uint32_t)(NV_PAVS_VOICE_CFG_FMT_V6BIN | NV_PAVS_VOICE_CFG_FMT_V7BIN));
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V6BIN, 6);
        voice->cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V7BIN, 7);
        hrtf_state_dirty = true;
    }

    voice->hrtf_target = index;

    // Recalculate volumes when HRTF state changes (volume layout differs between 2D and 3D modes)
    if (hrtf_state_dirty) {
        voice_update_volumes(voice);
    }

    apu_wait_for_pio(3);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_HRTF, voice->hrtf_target);
    if (hrtf_state_dirty) {
        apu_wait_for_pio(5);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_VBIN, voice->cfg_vbin);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, voice->cfg_fmt);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, voice->tar_vola);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, voice->tar_volb);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, voice->tar_volc);
    }
    apu_wait_for_pio(1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);

    return true;
}
