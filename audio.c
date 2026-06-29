// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Ryan Wendland

// https://web.archive.org/web/20191214202407/https://www.nvidia.com/attach/10203
// https://web.archive.org/web/20151012113834/http://www.nvidia.com/attach/9004
// https://www.nvidia.com/object/LO_20020712_6735.html

#include "audio.h"
#include "audio_ac97_reg.h"
#include "audio_apu_regs.h"

#include <assert.h>
#include <hal/debug.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

// 0x2000 Matches XDK
#define GP_FIFO_OUTPUT_SIZE 0x2000
#define AC97_BUFFER_SIZE    0x2000

// When GP is first powered up some bootloader code reads 2 pages of data to boot strap its program memory
// We make this atleast 2 pages to cover this requirement.
#define MAX_DSP_PAYLOAD 0x2000

// The GP SGEs are only used during the bootloader bootstrap. We only need 2 (~16 bytes) but minimum is 1 page
#define MCPX_GP_SGE_SIZE 0x1000

#define APU_SHIFT(reg_mask)             (__builtin_ffs((int)(reg_mask)) - 1)
#define APU_MAKE_VALUE(reg_mask, value) ((((uint32_t)(value)) << APU_SHIFT(reg_mask)) & (uint32_t)(reg_mask))

static nxAudioResult g_last_error = 0;
static mcpx_voice_t *g_voice_array = NULL;
static mcpx_ssl_t *g_ssl_array = NULL;
static mcpx_notifier_t *g_notifier_array = NULL;
static mcpx_sge_t *g_gp_sge_array = NULL;
static mcpx_sge_t *g_gp_fifo_array = NULL;
static void *g_ac97_buffer = NULL;
static void *g_gp_pmem = NULL;
static nxAudioVoice *g_running_voices[MCPX_HW_MAX_VOICES] = {0};
static uint32_t g_voice_allocation_mask[(MCPX_HW_MAX_VOICES + 31) / 32];
static KINTERRUPT g_apu_interrupt;
static KDPC g_apu_dpc;
static ac97_descriptor_t spdif_output_descriptor[1];
static ac97_descriptor_t pcm_output_descriptor[1];

static const char passthrough_text[] = {
#embed "passthrough.out"
    , '\0'};

static inline void apu_write_reg (uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(APU_MMIO_BASE + offset) = value;
}

static inline uint32_t apu_read_reg (uint32_t offset)
{
    return *(volatile uint32_t *)(APU_MMIO_BASE + offset);
}

static uint8_t voice_allocate (bool is_3d)
{
    // Voices 0-63 are reserved for 3D voices.
    for (int i = (is_3d ? 0 : 2); i < (MCPX_HW_MAX_VOICES + 31) / 32; i++) {
        if (g_voice_allocation_mask[i] != 0x00000000) {
            const int bit = __builtin_ctz(g_voice_allocation_mask[i]);
            g_voice_allocation_mask[i] &= ~(1U << bit); // Mark as allocated
            return (uint8_t)((i * 32) + bit);
        }
    }
    return 0xFF;
}

static void voice_free (uint8_t voice_index)
{
    const uint32_t dword = voice_index / 32;
    const uint32_t bit = voice_index % 32;
    g_voice_allocation_mask[dword] |= (1U << bit);
}

static void set_last_error (nxAudioResult result)
{
    g_last_error = result;
}

static uint32_t get_voice_container_size (const nxAudioFormat *format)
{
    if (format->codec == NX_AUDIO_CODEC_ADPCM) {
        return NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    }

    switch (format->bytesPerSample) {
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

static uint32_t get_voice_sample_size (const nxAudioFormat *format)
{
    if (format->codec == NX_AUDIO_CODEC_ADPCM) {
        return NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
    }

    switch (format->bytesPerSample) {
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

static void wait_for_pio_available (uint32_t count)
{
    assert(count <= 0x80); // The PIO FIFO is 128 entries deep, so we cannot wait for more than that.
    while (apu_read_reg(APU_VP_OFFSET + NV1BA0_PIO_FREE) < count)
        ;
}

static BOOLEAN NTAPI apu_isr (PKINTERRUPT Interrupt, PVOID ServiceContext)
{
    (void)Interrupt;
    (void)ServiceContext;
    const uint32_t ists = apu_read_reg(NV_PAPU_ISTS);
    apu_write_reg(NV_PAPU_ISTS, ists);

    // debugPrint("APU ISR: ISTS=0x%08X\n", ists);

    if (!ists) {
        return FALSE;
    }

    // Check for Front-End Trap interrupt
    if (ists & NV_PAPU_ISTS_FETINTSTS) {
        const uint32_t fectl = apu_read_reg(NV_PAPU_FECTL);
        const uint32_t fecv = apu_read_reg(NV_PAPU_FECV);
        const uint32_t meth = apu_read_reg(NV_PAPU_FEDECMETH) & 0xFFFF;
        const uint32_t param = apu_read_reg(NV_PAPU_FEDECPARAM);
        const uint32_t reason = (fectl >> 8) & 0xF;

        debugPrint("\n=== APU FRONT-END TRAP ===\n");
        debugPrint("Reason Code : 0x%X\n", reason);
        debugPrint("FECTL       : 0x%08X\n", fectl);
        debugPrint("FECV        : 0x%08X\n", fecv);
        debugPrint("Failed METH : 0x%08X\n", meth);
        debugPrint("Failed PARAM: 0x%08X\n", param);
        debugPrint("==========================\n");
        while (1)
            ; // Halt on trap for debugging

        // Restore FECTL to recover the engine
        apu_write_reg(NV_PAPU_FECTL, 0x0000100F);
    }

    // Check for voice completion interrupt
    if (ists & NV_PAPU_ISTS_FEVINTSTS) {
        KeInsertQueueDpc(&g_apu_dpc, NULL, NULL);
    }

    return TRUE;
}

static void NTAPI apu_dpc (PKDPC Dpc, PVOID DeferredContext, PVOID arg1, PVOID arg2)
{
    (void)Dpc;
    (void)DeferredContext;
    (void)arg1;
    (void)arg2;

    for (uint32_t i = 0; i < (MCPX_HW_MAX_VOICES + 31) / 32; i++) {
        // Small optimisation to only check allocated voices,
        uint32_t allocated_voices = ~g_voice_allocation_mask[i];
        while (allocated_voices) {
            uint32_t allocated_voice = (uint32_t)__builtin_ctz(allocated_voices);
            allocated_voices &= ~(1U << allocated_voice);

            const uint32_t voice_index = (i * 32) + allocated_voice;
            nxAudioVoice *voice = g_running_voices[voice_index];

            // If this voice isnt running we can bail now (User might have stopped the voice)
            if (!voice) {
                continue;
            }

            const uint32_t notifier_index = MCPX_HW_NOTIFIER_BASE_OFFSET + (voice_index * MCPX_HW_NOTIFIER_COUNT);

            // Check SSLA and SSLB notifiers for this voice. If either is done, call the callback and remark the
            // notifier as in progress.
            for (uint32_t j = 0; j < 2; j++) {
                if (voice->currentBuffer[j]) {
                    if (g_notifier_array[notifier_index + j].status == NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS) {
                        g_notifier_array[notifier_index + j].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;
                        if (voice->callback) {
                            MmLockUnlockBufferPages(voice->currentBuffer[j]->buffer,
                                                    voice->currentBuffer[j]->size_bytes, TRUE);
                            voice->callback(voice, voice->currentBuffer[j], voice->user_data);
                        }
                    }
                }
            }
        }
    }
}

nxAudioResult nxAudioGetLastError (void)
{
    return g_last_error;
}

static size_t parse_a56_output (const char *a56_text, uint32_t *output_buffer, size_t max_out)
{
    size_t count = 0;
    const char *cursor = a56_text;
    char line[128];

    while (*cursor != '\0') {
        // Find the end of the current line
        const char *eol = strchr(cursor, '\n');
        size_t line_len = eol ? (size_t)(eol - cursor) : strlen(cursor);

        // Copy the line to a temporary buffer and null-terminate it
        if (line_len < sizeof(line)) {
            memcpy(line, cursor, line_len);
            line[line_len] = '\0';

            // Look for lines that start with 'P ' (Program Memory)
            if (line[0] == 'P' && line[1] == ' ') {
                uint32_t address;
                uint32_t opcode;

                // Parse the 4-digit address and 6-digit opcode as hex (%x)
                if (sscanf(line, "P %x %x", &address, &opcode) == 2) {
                    if (count < max_out) {
                        // The %x naturally parses '44F400' into 0x0044F400 (32-bit padded)
                        output_buffer[count++] = opcode;
                    }
                }
            }
        }

        // Advance the cursor to the next line
        if (eol) {
            cursor = eol + 1;
        } else {
            break;
        }
    }

    return count;
}

bool nxAudioInit (const nxAudioInitParams *parameters)
{
    (void)parameters;
    volatile ac97_mixer_regs_t *mixer = (ac97_mixer_regs_t *)AC97_MIXER_MMIO;
    volatile ac97_busmaster_regs_t *bm = (ac97_busmaster_regs_t *)AC97_BUSMASTER_MMIO;

    g_voice_array =
        MmAllocateContiguousMemoryEx(MCPX_HW_MAX_VOICES * sizeof(mcpx_voice_t), 0, 0xFFFFFFFF, 0x8000, PAGE_READWRITE);

    g_ssl_array =
        MmAllocateContiguousMemoryEx(MCPX_HW_MAX_SSL_PRDS * sizeof(mcpx_ssl_t), 0, 0xFFFFFFFF, 0x4000, PAGE_READWRITE);

    g_notifier_array = MmAllocateContiguousMemoryEx(MCPX_HW_MAX_NOTIFIERS * sizeof(mcpx_notifier_t), 0, 0xFFFFFFFF,
                                                    0x4000, PAGE_READWRITE);

    g_gp_fifo_array = MmAllocateContiguousMemoryEx(GP_FIFO_OUTPUT_SIZE, 0, 0xFFFFFFFF, 0x4000, PAGE_READWRITE);

    g_ac97_buffer = MmAllocateContiguousMemoryEx(AC97_BUFFER_SIZE, 0, 0xFFFFFFFF, 0x4000, PAGE_READWRITE);

    g_gp_sge_array = MmAllocateContiguousMemoryEx(MCPX_GP_SGE_SIZE, 0, 0xFFFFFFFF, 0x4000, PAGE_READWRITE);

    g_gp_pmem =
        MmAllocateContiguousMemoryEx(MAX_DSP_PAYLOAD, 0, 0xFFFFFFFF, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);

    if (!g_voice_array || !g_ssl_array || !g_gp_sge_array || !g_notifier_array || !g_gp_fifo_array || !g_ac97_buffer ||
        !g_gp_pmem) {
        if (g_voice_array) {
            MmFreeContiguousMemory(g_voice_array);
            g_voice_array = NULL;
        }
        if (g_ssl_array) {
            MmFreeContiguousMemory(g_ssl_array);
            g_ssl_array = NULL;
        }
        if (g_gp_sge_array) {
            MmFreeContiguousMemory(g_gp_sge_array);
            g_gp_sge_array = NULL;
        }
        if (g_notifier_array) {
            MmFreeContiguousMemory(g_notifier_array);
            g_notifier_array = NULL;
        }
        if (g_gp_fifo_array) {
            MmFreeContiguousMemory(g_gp_fifo_array);
            g_gp_fifo_array = NULL;
        }
        if (g_ac97_buffer) {
            MmFreeContiguousMemory(g_ac97_buffer);
            g_ac97_buffer = NULL;
        }
        if (g_gp_pmem) {
            MmFreeContiguousMemory(g_gp_pmem);
            g_gp_pmem = NULL;
        }

        set_last_error(NX_AUDIO_ERR_OUT_OF_MEMORY);
        return false;
    }

    memset(g_voice_array, 0, MCPX_HW_MAX_VOICES * sizeof(mcpx_voice_t));
    memset(g_ssl_array, 0, MCPX_HW_MAX_SSL_PRDS * sizeof(mcpx_ssl_t));
    memset(g_notifier_array, 0, MCPX_HW_MAX_NOTIFIERS * sizeof(mcpx_notifier_t));
    memset(g_gp_sge_array, 0, MCPX_GP_SGE_SIZE);
    memset(g_gp_fifo_array, 0, GP_FIFO_OUTPUT_SIZE);
    memset(g_ac97_buffer, 0, AC97_BUFFER_SIZE);
    memset(g_gp_pmem, 0, MAX_DSP_PAYLOAD);
    memset(g_voice_allocation_mask, 0xFF, sizeof(g_voice_allocation_mask));
    memset(g_running_voices, 0, sizeof(g_running_voices));

    // Reset all notifiers to "In Progress".
    for (int i = 0; i < MCPX_HW_MAX_NOTIFIERS; i++) {
        g_notifier_array[i].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;
    }

    // All voices should link to themselves
    for (int i = 0; i < MCPX_HW_MAX_VOICES; i++) {
        g_voice_array[i].tar_pitch_link = (i & 0xFFFF);
    }

    // Disable interrupts and front-end traps
    apu_write_reg(NV_PAPU_IEN, 0);
    apu_write_reg(NV_PAPU_FECTL, 0);

    // Disable the sound engine
    apu_write_reg(NV_PAPU_SECTL, 0x00000000);

    // This is required or we trap. Enable writing for the below registers?
    apu_write_reg(0x1510, 0x00000001);

    // Disable GP and EP
    apu_write_reg(APU_GP_OFFSET + NV_PAPU_GPRST, 0x00000000);
    apu_write_reg(APU_EP_OFFSET + NV_PAPU_GPRST, 0x00000000);

    // Clear voice lists
    apu_write_reg(NV_PAPU_TVL2D, 0xFFFF);
    apu_write_reg(NV_PAPU_TVL3D, 0xFFFF);
    apu_write_reg(NV_PAPU_TVLMP, 0xFFFF);
    apu_write_reg(NV_PAPU_CVL2D, 0xFFFF);
    apu_write_reg(NV_PAPU_CVL3D, 0xFFFF);
    apu_write_reg(NV_PAPU_CVLMP, 0xFFFF);
    apu_write_reg(NV_PAPU_NVL2D, 0xFFFF);
    apu_write_reg(NV_PAPU_NVL3D, 0xFFFF);
    apu_write_reg(NV_PAPU_NVLMP, 0xFFFF);

    // Set up something for the front end traps
    apu_write_reg(NV_PAPU_FETFORCE0, 0x00000000);
    apu_write_reg(NV_PAPU_FETFORCE1, 0x00008000);
    apu_write_reg(0x1508, 0x00800040);
    apu_write_reg(0x150c, 0x00000000);

    // Set Counters - FIXME: Confirm values
    apu_write_reg(0x2008, 0x00000800);
    apu_write_reg(NV_PAPU_XGSCNT, 0x00000000);
    apu_write_reg(0x2010, 0x00000800);
    apu_write_reg(0x2014, 0x00000400);
    apu_write_reg(0x2018, 0x000007ff);
    apu_write_reg(0x201c, 0x00000000);
    apu_write_reg(0x2020, 0x00000600);
    apu_write_reg(0x2024, 0x00000100);
    apu_write_reg(0x2028, 0x00000100);

    // FIXME: What are these
    apu_write_reg(0x1104, 0x000000FF);
    apu_write_reg(0x1108, 0x0000003F);
    apu_write_reg(0x111c, 0x0000007F);
    apu_write_reg(0x1124, 0x00001FFF);
    apu_write_reg(0x1138, 0x000007FF);
    apu_write_reg(0x1158, 0x00000020);
    apu_write_reg(0x112c, 0x00000000);
    apu_write_reg(0x1130, 0x08000000);
    apu_write_reg(0x1140, 0x00000000);
    apu_write_reg(0x1144, 0x08000000);
    apu_write_reg(0x1150, 0x00000000);
    apu_write_reg(0x1154, 0x08000000);

    apu_write_reg(0x1510, 0x00000000); // Undo what we did earlier

    // Soft power up? Maybe turn on VP but no processing
    apu_write_reg(NV_PAPU_SECTL, 0x00000007);

    // Setup voice Processor Voice Address to voice structures
    apu_write_reg(NV_PAPU_VPVADDR, (uint32_t)MmGetPhysicalAddress(g_voice_array));

    // Voice Process Scatter-Gather Stream List Address
    apu_write_reg(NV_PAPU_VPSSLADDR, (uint32_t)MmGetPhysicalAddress(g_ssl_array));

    // Voice Process Notifier Address
    apu_write_reg(NV_PAPU_FENADDR, (uint32_t)MmGetPhysicalAddress(g_notifier_array));

    // GP FIFO Scatter-Gather Element Address
    apu_write_reg(NV_PAPU_GPFADDR, (uint32_t)MmGetPhysicalAddress(g_gp_fifo_array));

    // GP Scatter-Gather Element Address - In this driver its only purpose is to load in DSP assembler code into the GP
    // memory space.
    apu_write_reg(NV_PAPU_GPSADDR, (uint32_t)MmGetPhysicalAddress(g_gp_sge_array));

    // FIXME - What are these
    apu_write_reg(APU_VP_OFFSET + 0x2A0, 0x00000FFF); // ?
    apu_write_reg(APU_VP_OFFSET + 0x2A4, 0x00000FFF); // ?
    apu_write_reg(APU_VP_OFFSET + 0x2A8, 0x00000FFF); // ?
    apu_write_reg(APU_VP_OFFSET + 0x2AC, 0x00000FFF); // ?
    apu_write_reg(APU_VP_OFFSET + 0x2B0, 0x00000FFF); // ?

    // Initalise AC97 hardware and point them to our DMA descriptors.
    {
        pcm_output_descriptor[0].buffer_start_address = MmGetPhysicalAddress(g_ac97_buffer);
        pcm_output_descriptor[0].buffer_sample_count = (AC97_BUFFER_SIZE / 2);
        pcm_output_descriptor[0].buffer_control = 0x0000;

        spdif_output_descriptor[0].buffer_start_address = MmGetPhysicalAddress(g_ac97_buffer);
        spdif_output_descriptor[0].buffer_sample_count = (AC97_BUFFER_SIZE / 2);
        spdif_output_descriptor[0].buffer_control = 0x0000;

        // Trigger Reset and wait for the Codec to report Ready
        bm->global_control |= AC97_GLOBAL_CR_RESET;
        while (!(bm->global_status & AC97_GLOBAL_SR_CODEC_READY))
            ;

        // Turn everything on - Reset mixer powerdown and PCM/SPDIF control
        mixer->powerdown_control = AC97_POWER_NORMAL;
        bm->pcm_out_control = AC97_BM_CR_RESET;
        bm->spdif_control = AC97_BM_CR_RESET;

        // Set volumes
        mixer->master_volume = 0x0000;
        mixer->pcm_out_volume = 0x0808; // -12dB on left and right channel

        // Reset PCM and SPDIF Out
        bm->pcm_out_control = AC97_BM_CR_RESET;
        while (bm->pcm_out_control & AC97_BM_CR_RESET)
            ;

        bm->spdif_control = AC97_BM_CR_RESET;
        while (bm->spdif_control & AC97_BM_CR_RESET)
            ;

        bm->pcm_out_buffer_base = MmGetPhysicalAddress(pcm_output_descriptor);
        bm->pcm_out_current_index = 0;
        bm->pcm_out_last_valid_index = 0;

        bm->spdif_buffer_base = MmGetPhysicalAddress(spdif_output_descriptor);
        bm->spdif_current_index = 0;
        bm->spdif_last_valid_index = 0;

        // ?
        bm->x7c_unk_control = AC97_7C_UNK;
    }

    // Set the GP FIFO size characteristics
    {
        apu_write_reg(NV_PAPU_GPFMAXSGE, (GP_FIFO_OUTPUT_SIZE / PAGE_SIZE) - 1); // How many pages
        apu_write_reg(0x1148, (GP_FIFO_OUTPUT_SIZE / PAGE_SIZE) - 1);            // ? Always seem to be same as above
        apu_write_reg(NV_PAPU_GPOFBASE0, 0);
        apu_write_reg(NV_PAPU_GPOFEND0, GP_FIFO_OUTPUT_SIZE);

        // Point the GP FIFO SGE table to our AC97 buffer in RAM
        for (uint32_t i = 0; i < (AC97_BUFFER_SIZE / PAGE_SIZE); i++) {
            g_gp_fifo_array[i].phys_addr = MmGetPhysicalAddress(g_ac97_buffer) + (i * PAGE_SIZE);
            g_gp_fifo_array[i].length_and_format = PAGE_SIZE;

            wait_for_pio_available(2);
            apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE, i);
            apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET, g_gp_fifo_array[i].phys_addr);
        }

        // We are only using GP FIFO0, so set the base address in our SGE array (0 position) and length for the GP FIFO0
        const uint32_t gp_fifo_index = 0;
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_OUTBUF_BA + gp_fifo_index * 8, gp_fifo_index * PAGE_SIZE);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_OUTBUF_LEN + gp_fifo_index * 8, AC97_BUFFER_SIZE);

        // Reset GP FIFO0 positions
        apu_write_reg(0x2000 + gp_fifo_index * 0x10, apu_read_reg(0x2000));
        apu_write_reg(0x2004 + gp_fifo_index * 0x10, apu_read_reg(0x2004));
        apu_write_reg(0x2008 + gp_fifo_index * 0x10, apu_read_reg(0x2008));
        apu_write_reg(0x200C + gp_fifo_index * 0x10, apu_read_reg(0x200C));
    }

    // ? Probably 3D Only
    wait_for_pio_available(34);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_HRTF_SUBMIXES, 0x09070806); // This value from XDK RE
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_HRTF_HEADROOM, 0);
    for (uint32_t i = 0; i < 31; i++) {
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_SUBMIX_HEADROOM + (i * 4), 1);
    }
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_SUBMIX_HEADROOM + (31 * 4), 0);

    // Soft power on GP?
    apu_write_reg(APU_GP_OFFSET + NV_PAPU_GPRST, NV_PAPU_GPRST_GPRST);
    apu_write_reg(APU_GP_OFFSET + 0xFF10, 0x01); // ?
    apu_write_reg(APU_GP_OFFSET + 0xFF14, 0xFF); // ?

    // Prep DSP code for bootstrapping the GP. Seems like its hardcoded to read two pages of 4k each
    {
        apu_write_reg(NV_PAPU_GPSMAXSGE, MCPX_GP_SGE_SIZE / sizeof(mcpx_sge_t));
        parse_a56_output(passthrough_text, g_gp_pmem, MAX_DSP_PAYLOAD / sizeof(uint32_t));
        g_gp_sge_array[0].phys_addr = MmGetPhysicalAddress(g_gp_pmem);
        g_gp_sge_array[0].length_and_format = PAGE_SIZE;
        g_gp_sge_array[1].phys_addr = MmGetPhysicalAddress(g_gp_pmem) + PAGE_SIZE;
        g_gp_sge_array[1].length_and_format = PAGE_SIZE;
    }

    // Enable GP which automatically reads in the DSP code (Now setup in NV_PAPU_GPSADDR)
    apu_write_reg(APU_GP_OFFSET + NV_PAPU_GPRST, NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST);

    // Clear any latent interrupts enable IRQs for the APU and reset the Front End (running isochronous with the GP)
    apu_write_reg(NV_PAPU_ISTS, 0xFFFFFFFF);
    apu_write_reg(NV_PAPU_IEN,
                  NV_PAPU_ISTS_GINTSTS | NV_PAPU_ISTS_FETINTSTS | NV_PAPU_ISTS_FENINTSTS | NV_PAPU_ISTS_FEVINTSTS);
    apu_write_reg(NV_PAPU_FECTL, 0x0000100F);

    // Connect to the APU's IRQ line and setup our ISR and DPC
    KIRQL irql;
    ULONG vector = HalGetInterruptVector(XBOX_APU_IRQ, &irql);
    KeInitializeDpc(&g_apu_dpc, apu_dpc, NULL);
    KeInitializeInterrupt(&g_apu_interrupt, &apu_isr, NULL, vector, irql, LevelSensitive, TRUE);
    KeConnectInterrupt(&g_apu_interrupt);

    // Start the Sound Engine
    apu_write_reg(NV_PAPU_SECTL, 0x0000000F);

    // Start AC97
    bm->spdif_control = AC97_BM_CR_START;
    bm->pcm_out_control = AC97_BM_CR_START;
    return true;
}

void nxAudioShutdown (void)
{
    volatile ac97_busmaster_regs_t *bm = (ac97_busmaster_regs_t *)AC97_BUSMASTER_MMIO;
    bm->spdif_control = AC97_BM_CR_RESET;
    bm->pcm_out_control = AC97_BM_CR_RESET;

    apu_write_reg(NV_PAPU_IEN, 0x00000000);

    apu_write_reg(0x1510, 0x00000001);
    apu_write_reg(NV_PAPU_FECTL, 0);
    apu_write_reg(NV_PAPU_SECTL, 0);

    KeDisconnectInterrupt(&g_apu_interrupt);
    KeRemoveQueueDpc(&g_apu_dpc);

    apu_write_reg(APU_GP_OFFSET + NV_PAPU_GPRST, 0x00000000);
    apu_write_reg(APU_EP_OFFSET + NV_PAPU_GPRST, 0x00000000);

    if (g_ac97_buffer) {
        MmFreeContiguousMemory(g_ac97_buffer);
        g_ac97_buffer = NULL;
    }

    if (g_voice_array) {
        MmFreeContiguousMemory(g_voice_array);
        g_voice_array = NULL;
    }

    if (g_ssl_array) {
        MmFreeContiguousMemory(g_ssl_array);
        g_ssl_array = NULL;
    }

    if (g_notifier_array) {
        MmFreeContiguousMemory(g_notifier_array);
        g_notifier_array = NULL;
    }

    if (g_gp_sge_array) {
        MmFreeContiguousMemory(g_gp_sge_array);
        g_gp_sge_array = NULL;
    }

    if (g_gp_fifo_array) {
        MmFreeContiguousMemory(g_gp_fifo_array);
        g_gp_fifo_array = NULL;
    }

    if (g_gp_pmem) {
        MmFreeContiguousMemory(g_gp_pmem);
        g_gp_pmem = NULL;
    }
}

bool nxAudioBufferInitialise (nxAudioBuffer *buffer, void *user_buffer, uint32_t size_bytes)
{
    if (!buffer || !user_buffer || size_bytes == 0) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    buffer->buffer = user_buffer;
    buffer->size_bytes = size_bytes;
    return true;
}

bool nxAudioVoiceCreate (nxAudioVoice *voice, const nxAudioFormat *audio_format,
                         nxAudioVoiceBufferCompleteCallback callback, void *user_data)
{
    if (!voice || !audio_format) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (audio_format->bytesPerSample > 4) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    const uint8_t voice_index = voice_allocate(false);
    if (voice_index == 0xFF) {
        set_last_error(NX_AUDIO_ERR_OUT_OF_VOICES);
        return false;
    }
    mcpx_voice_t hw_voice;
    memset(&hw_voice, 0, sizeof(mcpx_voice_t));

    voice->voice_index = voice_index;

    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE, get_voice_container_size(audio_format));
    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE, get_voice_sample_size(audio_format));

    // VP support two types of sound data.
    // Streaming (via SSLs) and "Buffer Mode" (via SGEs)
    // Streaming supports dual buffer ping pong for really long audio stream.
    // Single buffer it suqed up fully and played one (once off sound effects etc)
    // Currently we only support streaming mode. This can cover all use cases in anycase.
    if (audio_format->type == NX_VOICE_TYPE_2D_STREAM) {
        hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_DATA_TYPE, 1);
    }

    if (audio_format->channels == 2) {
        hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_STEREO, 1);
    }

    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK, audio_format->channels - 1);

    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_PERSIST, 1);
    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_HEADROOM, 7);

    // Maybe needed?
    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V6BIN, 6);
    hw_voice.cfg_fmt |= APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_FMT_V7BIN, 7);

    // Disable 3d
    hw_voice.hrtf_target = HRTF_NULL_HANDLE;

    // Set attenuation. VOL0 is front left, VOL1 is front right.
    hw_voice.tar_vola = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME0, 0) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME1, 0) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0, 0xF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0, 0xF);

    hw_voice.tar_volb = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4, 0xF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME2, 0xFFF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4, 0x3F) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME3, 0xFFF);

    hw_voice.tar_volc = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8, 0xF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME4, 0xFFF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8, 0xF) |
                        APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME5, 0xFFF);

    // Route the voice to the Front Left and Front Right mixbins. These are used for 2D voices
    hw_voice.cfg_vbin =
        APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V0BIN, 0) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V1BIN, 1) |
        APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V2BIN, 0) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V3BIN, 0) |
        APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V4BIN, 0) | APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_VBIN_V5BIN, 0);

    // Enable voice completion notification interrupts?
    // No IRQ otherwise
    hw_voice.cfg_misc = (1 << 23);

    // Set pitch interpolation based on sample rate
    // Fast Pitch Math: 4096.0 * (log2(sample_rate) - log2(48000))
    // 4096 / ln(2) =  5909.27888748f
    // 4096 * log(48000) / log(2) =  63695.8588329f
    const int32_t pitch_val = (int32_t)(roundf((5909.585727f * logf((float)audio_format->sampleRate)) - 63695.858833f));
    hw_voice.tar_pitch_link = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH, (uint32_t)(int16_t)pitch_val) |
                              APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE, 0x0000);
    hw_voice.cfg_env0 = APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE, 0);
    hw_voice.cfg_enva = APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL, 0);

    voice->currentBuffer[0] = NULL;
    voice->currentBuffer[1] = NULL;
    voice->currentBufferIndex = 0;
    voice->callback = callback;
    voice->user_data = user_data;
    voice->paused = false;
    voice->volume = 1.0f;
    memcpy(&voice->format, audio_format, sizeof(nxAudioFormat));

    // Now push it all to hardware.
    wait_for_pio_available(18);
    KIRQL irql = KeRaiseIrqlToDpcLevel(); // Avoid context switches after NV1BA0_PIO_SET_CURRENT_VOICE
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_FMT, hw_voice.cfg_fmt);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_MISC, hw_voice.cfg_misc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV0, hw_voice.cfg_env0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVA, hw_voice.cfg_enva);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENV1, hw_voice.cfg_env1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_ENVF, hw_voice.cfg_envf);
    apu_write_reg(APU_VP_OFFSET + 0x350, 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_LFO_ENV, hw_voice.tar_lfo_env);
    apu_write_reg(APU_VP_OFFSET + 0x370, 0);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCA, hw_voice.tar_fca);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCB, hw_voice.tar_fcb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_VBIN, hw_voice.cfg_vbin);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, hw_voice.tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, hw_voice.tar_volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, hw_voice.tar_volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_PITCH, hw_voice.tar_pitch_link);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_HRTF, hw_voice.hrtf_target);
    KfLowerIrql(irql);

    return true;
}

void nxAudioVoiceDestroy (nxAudioVoice *voice)
{
    if (!voice) {
        return;
    }

    nxAudioVoiceStop(voice);
    voice_free(voice->voice_index);
}

static void get_memory_properties (void *virtual_pointer, uint32_t max_length, uint32_t *out_physical_address,
                                   uint32_t *out_contiguous_length)
{
    uint8_t *virtual_address = (uint8_t *)virtual_pointer;
    uint32_t physical_address = (uint32_t)MmGetPhysicalAddress(virtual_address);
    uint32_t contiguous_bytes;

    // --- XBOX KSEG0 FAST-PATH ---
    // If the pointer is in the direct-mapped KSEG0 region, it is guaranteed
    // to be physically contiguous. Skip the page table walking entirely!
    if ((uintptr_t)virtual_address >= 0x80000000 && (uintptr_t)virtual_address < 0x90000000) {
        contiguous_bytes = max_length;
    } else {
        // Standard Page-Table Probing (For paged memory like malloc)
        contiguous_bytes = PAGE_SIZE - (physical_address & (PAGE_SIZE - 1));
        if (contiguous_bytes > max_length) {
            contiguous_bytes = max_length;
        }

        uint8_t *cursor = virtual_address + contiguous_bytes;
        uint32_t remaining = max_length - contiguous_bytes;
        uint32_t current_address = physical_address + contiguous_bytes;

        while (remaining > 0) {
            uint32_t next_address = (uint32_t)MmGetPhysicalAddress(cursor);
            if (next_address != current_address) {
                break;
            }

            uint32_t next_contiguous = PAGE_SIZE - (next_address & (PAGE_SIZE - 1));
            if (next_contiguous > remaining) {
                next_contiguous = remaining;
            }

            contiguous_bytes += next_contiguous;
            cursor += next_contiguous;
            remaining -= next_contiguous;
            current_address += next_contiguous;
        }
    }
    *out_physical_address = physical_address;
    *out_contiguous_length = contiguous_bytes;
}

bool nxAudioVoiceSubmitBuffer (nxAudioVoice *voice, nxAudioBuffer *buffer)
{
    mcpx_ssl_t hw_ssl;
    if (!voice || !buffer) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    const uint32_t ssl_base_index = (voice->voice_index * MCPX_HW_MAX_PRD_ENTRIES_PER_VOICE) +
                                    (voice->currentBufferIndex * MCPX_HW_MAX_PRD_ENTRIES_PER_SSL);

    const uint8_t container_bytes_lookup[4] = {1, 2, 4, 4};
    const uint32_t container_index = get_voice_container_size(&voice->format);
    const uint32_t container_bytes = container_bytes_lookup[container_index & 3];

    uint32_t bytes_per_block;
    uint32_t max_bytes_per_ssl;
    if (voice->format.codec == NX_AUDIO_CODEC_ADPCM) {
        bytes_per_block = 36 * voice->format.channels;
        max_bytes_per_ssl = (65535 / 64) * bytes_per_block;
    } else {
        bytes_per_block = container_bytes * voice->format.channels;
        max_bytes_per_ssl = 65535 * bytes_per_block;
    }

    MmLockUnlockBufferPages(buffer->buffer, buffer->size_bytes, FALSE);

    uint8_t *virtual_address = (uint8_t *)buffer->buffer;
    uint32_t bytes_left = buffer->size_bytes;
    uint32_t index = 0;

    while (bytes_left > 0) {
        if (index >= MCPX_HW_MAX_PRD_ENTRIES_PER_SSL) {
            DbgPrint("nxAudio Error: SSL limit exceeded!, %d/%d\n", index, MCPX_HW_MAX_PRD_ENTRIES_PER_SSL);
            break;
        }

        // Each SSL entry can be up to 65535 samples and it must be divided into block of contigous memory (if the user
        // did not provide a contiguous buffer). We can have 16 SSL entries in the SSL list. If your memory is
        // contiguous this is 16*65535 samples. If your memory is fragmented, it will be less than that.
        uint32_t physical_address;
        uint32_t contiguous_bytes;
        get_memory_properties(virtual_address, bytes_left, &physical_address, &contiguous_bytes);

        // Clamp it to allowable size for a SSL entry
        if (contiguous_bytes > max_bytes_per_ssl) {
            contiguous_bytes = max_bytes_per_ssl;
        }

        // Also ensure we dont split a block of samples across SSL entries.
        contiguous_bytes -= (contiguous_bytes % bytes_per_block);

        virtual_address += contiguous_bytes;
        bytes_left -= contiguous_bytes;

        uint32_t block_samples = 0;
        if (voice->format.codec == NX_AUDIO_CODEC_ADPCM) {
            block_samples = (contiguous_bytes / bytes_per_block) * 64;
        } else {
            block_samples = contiguous_bytes / bytes_per_block;
        }
        assert(block_samples <= 65535);

        hw_ssl.phys_addr = physical_address;
        hw_ssl.length_and_format = 0;
        hw_ssl.length_and_format |= (block_samples & 0xFFFF);
        hw_ssl.length_and_format |= (container_index << 16U);
        hw_ssl.length_and_format |= ((uint32_t)(voice->format.channels - 1) << 18);
        hw_ssl.length_and_format |= ((voice->format.channels > 1 ? 1U : 0U) << 23);

        // The hardware needs to know what 64 SLL segment our target SSL is in then the offset within that segment.
        // i.e SSL at index 70 would have a segment base of 1, and an offset of 6 (70 / 64 = 1, 70 % 64 = 6)
        // Within that base we push the physical address of the audio buffer.
        const uint32_t absolute_ssl_index = ssl_base_index + index;
        const uint32_t ssl_segment_base = absolute_ssl_index / 64;
        const uint32_t ssl_index_in_segment = absolute_ssl_index % 64;

        // Now push it to hardware
        wait_for_pio_available(3);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_SSL,
                      APU_MAKE_VALUE(NV1BA0_PIO_SET_CURRENT_SSL_BASE_PAGE, ssl_segment_base));

        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET + (ssl_index_in_segment * 8), hw_ssl.phys_addr);

        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + (ssl_index_in_segment * 8),
                      hw_ssl.length_and_format);

        index++;
    }

    wait_for_pio_available(2);
    KIRQL irql = KeRaiseIrqlToDpcLevel();

    const int notifier_idx =
        MCPX_HW_NOTIFIER_BASE_OFFSET + (voice->voice_index * MCPX_HW_NOTIFIER_COUNT) + voice->currentBufferIndex;
    g_notifier_array[notifier_idx].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;

    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);

    const uint32_t ssl_cmd_arg = APU_MAKE_VALUE(NV1BA0_PIO_SET_VOICE_SSL_A_BASE, ssl_base_index) |
                                 APU_MAKE_VALUE(NV1BA0_PIO_SET_VOICE_SSL_A_COUNT, index);
    if (voice->currentBufferIndex == 0) {
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_SSL_A, ssl_cmd_arg);
    } else {
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_SSL_B, ssl_cmd_arg);
    }

    voice->currentBuffer[voice->currentBufferIndex] = buffer;
    voice->currentBufferIndex = (voice->currentBufferIndex + 1) % 2;

    KfLowerIrql(irql);

    return true;
}

bool nxAudioVoiceStart (nxAudioVoice *voice)
{
    if (!voice) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (!voice->currentBuffer[0] && !voice->currentBuffer[1]) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    // If we are just paused, we can skip the whole setup and just unpause the voice.
    if (voice->paused) {
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice->voice_index);
        voice->paused = false;
        return true;
    }

    wait_for_pio_available(6);
    KIRQL irql = KeRaiseIrqlToDpcLevel();

    g_running_voices[voice->voice_index] = voice;

    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);

    // Insert the voice into the voice list.
    const uint32_t antecedent_cmd =
        APU_MAKE_VALUE(NV1BA0_PIO_SET_ANTECEDENT_VOICE_HANDLE, 0xFFFF) |
        APU_MAKE_VALUE(NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST, (voice->format.type == NX_VOICE_TYPE_2D_STREAM)
                                                                 ? NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP
                                                                 : NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_ANTECEDENT_VOICE, antecedent_cmd);

    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_ON, voice->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice->voice_index);

    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    KfLowerIrql(irql);

    while (apu_read_reg(APU_VP_OFFSET + NV1BA0_PIO_FREE) < 64)
        ;

    return true;
}

bool nxAudioVoiceStop (nxAudioVoice *voice)
{
    if (!voice) {
        return false;
    }

    const uint16_t voice_index = voice->voice_index;

    KIRQL irql = KeRaiseIrqlToDpcLevel();

    g_running_voices[voice_index] = NULL;

    const uint32_t notifier_index = MCPX_HW_NOTIFIER_BASE_OFFSET + (voice_index * MCPX_HW_NOTIFIER_COUNT);
    g_notifier_array[notifier_index + 0].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;
    g_notifier_array[notifier_index + 1].status = NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS;

    KfLowerIrql(irql);

    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_OFF, voice_index);

    MmLockUnlockBufferPages(voice->currentBuffer[0]->buffer, voice->currentBuffer[0]->size_bytes, TRUE);
    MmLockUnlockBufferPages(voice->currentBuffer[1]->buffer, voice->currentBuffer[1]->size_bytes, TRUE);
    voice->currentBuffer[0] = NULL;
    voice->currentBuffer[1] = NULL;
    voice->currentBufferIndex = 0;
    voice->paused = false;

    return true;
}

bool nxAudioVoicePause (nxAudioVoice *voice)
{
    if (!voice) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (!voice->paused) {
        const uint16_t voice_index = voice->voice_index;
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_PAUSE, voice_index | NV1BA0_PIO_VOICE_PAUSE_ACTION);
        voice->paused = true;
    }

    return true;
}

bool nxAudioVoiceSetVolume (nxAudioVoice *voice, float volume)
{
    if (!voice) {
        set_last_error(NX_AUDIO_ERR_INVALID_PARAM);
        return false;
    }

    if (voice->volume == volume) {
        return true;
    }

    uint32_t attenuation_hw;
    if (volume <= 0.0f) {
        attenuation_hw = 0xFFF; // mute
    } else if (volume > 1.0f) {
        attenuation_hw = 0x000; // 0 dB, no attenuation
    } else {
        const float attenuation = -20.0f * log10f(volume);
        attenuation_hw = (uint32_t)roundf(attenuation * 64.0f);
        if (attenuation_hw > 0xFFF) {
            attenuation_hw = 0xFFF;
        }
    }

    const uint16_t voice_index = voice->voice_index;
    const uint32_t tar_vola = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME0, attenuation_hw) |
                              APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME1, attenuation_hw) |
                              APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0, 0xF) |
                              APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0, 0xF);

    wait_for_pio_available(4);
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, tar_vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    voice->volume = volume;
    KfLowerIrql(irql);

    return true;
}
