// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Ryzee119
#pragma once

#include "audio_ac97_reg.h"
#include "audio_apu_regs.h"
#include <nxaudio.h>

#include <assert.h>
#include <hal/debug.h>
#include <stdbool.h>
#include <stdint.h>
#include <xboxkrnl/xboxkrnl.h>

#define AC97_MMIO_BASE      0xFEC00000
#define AC97_MIXER_MMIO     (AC97_MMIO_BASE + 0x000)
#define AC97_BUSMASTER_MMIO (AC97_MMIO_BASE + 0x100)

#define APU_MMIO_BASE 0xFE800000
#define APU_VP_OFFSET 0x00020000
#define APU_GP_OFFSET 0x00030000
#define APU_EP_OFFSET 0x00050000
#define XBOX_APU_IRQ  5

// Each voice has notifiers which is what is provided to the IRQ
#define MCPX_HW_MAX_NOTIFIERS (MCPX_HW_NOTIFIER_BASE_OFFSET + MCPX_HW_MAX_VOICES * MCPX_HW_NOTIFIER_COUNT)

// 0x2000 Matches retail titles
#define GP_FIFO_OUTPUT_SIZE 0x2000
#define AC97_BUFFER_SIZE    0x2000

// When GP is first powered up some bootloader code reads 2 pages of data to boot strap its program memory
// We make this atleast 2 pages to cover this requirement.
#define MAX_DSP_PAYLOAD 0x2000

#define APU_SHIFT(reg_mask)             (__builtin_ffs((int)(reg_mask)) - 1)
#define APU_MAKE_VALUE(reg_mask, value) ((((uint32_t)(value)) << APU_SHIFT(reg_mask)) & (uint32_t)(reg_mask))

// EF and EA constants are the same, so just map a define for readability in our code.
#define NV_PAVS_VOICE_CFG_ENV1_EF_ATTACKRATE   NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE
#define NV_PAVS_VOICE_CFG_ENVF_EF_DECAYRATE    NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE
#define NV_PAVS_VOICE_CFG_ENVF_EF_HOLDTIME     NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME
#define NV_PAVS_VOICE_CFG_ENVF_EF_SUSTAINLEVEL NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL
#define NV_PAVS_VOICE_CFG_ENV1_EF_DELAYTIME    NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME
#define NV_PAVS_VOICE_TAR_FCB_FC2              NV_PAVS_VOICE_TAR_FCA_FC0
#define NV_PAVS_VOICE_TAR_FCB_FC3              NV_PAVS_VOICE_TAR_FCA_FC1

#define HW_VOICE_ARRAY_SIZE           (MCPX_HW_MAX_VOICES * NV_PAVS_SIZE)
#define HW_VOICE_SSL_ARRAY_SIZE       (MCPX_HW_MAX_VOICES * MCPX_HW_MAX_PRD_ENTRIES_PER_VOICE * NV_PSGE_SIZE)
#define HW_VOICE_SGE_ARRAY_SIZE       (MCPX_HW_MAX_BUFFER_SGES * NV_PSGE_SIZE)
#define HW_VOICE_NOTIFIERS_ARRAY_SIZE (MCPX_HW_MAX_NOTIFIERS * sizeof(mcpx_notifier_t))

// This is abritrary, but we only use this to load the bootloader into the GP so its basically minimum 1 page which can
// hold more than enough SGEs for this purpose.
#define MCPX_HW_GP_SGE_ARRAY_SIZE 0x1000

// Not sure on this, but retail caps out at 2048.
#define MCPX_HW_MAX_BUFFER_SGES 2048

// The APU allows 2 HRTF targets per 3D voice. If you set one HRTF target while another is applied, it will transition
// over to avoid pops
#define MCPX_HW_HRTF_TARGETS_PER_VOICE 2

// Each HRTF target is 64 bytes (16 32-bit registers containing interleaved Left/Right HRIR coefficients + ITD)
#define NV_HRTF_TARGET_SIZE 0x40

// The active current state is 128 bytes per voice (64 bytes for coefficients + 64 bytes for filter history/delay line)
#define NV_HRTF_CURRENT_SIZE 0x80

#define HW_HRTF_TARGET_ARRAY_SIZE  (MCPX_HW_MAX_3D_VOICES * MCPX_HW_HRTF_TARGETS_PER_VOICE * NV_HRTF_TARGET_SIZE)
#define HW_HRTF_CURRENT_ARRAY_SIZE (MCPX_HW_MAX_3D_VOICES * NV_HRTF_CURRENT_SIZE)

extern volatile mcpx_voice_t *g_hw_voices;
extern volatile mcpx_notifier_t *g_hw_notifiers;
extern nxAudioVoice *g_tracked_voices[];
extern uint32_t g_voice_allocation_mask[];

static inline void apu_write_reg (uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(APU_MMIO_BASE + offset) = value;
}

static inline uint32_t apu_read_reg (uint32_t offset)
{
    return *(volatile uint32_t *)(APU_MMIO_BASE + offset);
}

void apu_wait_for_pio (uint32_t count);
void apu_set_last_error (nxAudioResult result);
void apu_flush_voice (nxAudioVoice *voice);
uint32_t apu_format_get_container_size (const nxAudioFormat *format);
uint32_t apu_format_get_sample_size (const nxAudioFormat *format);
uint32_t apu_format_bytes_to_blocks (const nxAudioFormat *format, uint32_t bytes);
uint32_t apu_format_blocks_to_bytes (const nxAudioFormat *format, uint32_t blocks);
void apu_sge_free (uint32_t sge_index, uint32_t sge_count);
