#ifndef AUDIO_AC97_REG_H
#define AUDIO_AC97_REG_H

// [1] https://web.archive.org/web/20260416043004/https://www.alsa-project.org/files/pub/datasheets/intel/ac97r21.pdf
// [2] https://web.archive.org/web/20060322081039/http://www.amd.com/us-en/assets/content_type/white_papers_and_tech_docs/24467.pdf

#include <stdint.h>

// buffer descriptor from AC97 specification
typedef struct
{
    unsigned int buffer_start_address;
    unsigned short buffer_sample_count; // 0=no smaples
    unsigned short buffer_control;      // b15=1=issue IRQ on completion, b14=1=last in stream
} ac97_descriptor_t __attribute__((aligned(8)));

// Global Control
#define AC97_GLOBAL_CR_RESET 0x00000002 // 0x2C Bit 1: Warm Reset

// Global Status
#define AC97_GLOBAL_SR_CODEC_READY 0x00000100 // 0x30 Bit 8: Primary Codec Ready

// Mixer Control Values
#define AC97_POWER_NORMAL 0x0000 // Everything powered on

// Busmaster PCM or SPDIF control 0x1B or 0x7B
#define AC97_BM_CR_START 0x01 // Bit 0: Run/Pause Bus Master
#define AC97_BM_CR_RESET 0x02 // Bit 1: Reset Registers (Auto-clears)
#define AC97_BM_CR_LVBIE 0x04 // Bit 2: Last Valid Buffer Interrupt Enable
#define AC97_BM_CR_FEIE  0x08 // Bit 3: FIFO Error Interrupt Enable
#define AC97_BM_CR_IOCE  0x10 // Bit 4: Interrupt On Completion Enable

#define AC97_BM_CR_START_WITH_INTS (AC97_BM_CR_START | AC97_BM_CR_LVBIE | AC97_BM_CR_FEIE | AC97_BM_CR_IOCE)

// Xbox AC97 specific register value
#define AC97_7C_UNK 0x02000000

// AC97 Mixer Registers (Minimal)
// Ref [1] Section 6.3 and [2] 5.8.1.2
typedef struct
{
    uint8_t _pad0[0x02];                 // 0x00 - 0x01
    volatile uint16_t master_volume;     // 0x02 (MSB=Left, LSB=Right). Each step = 1.5dB
    uint8_t _pad1[0x14];                 // 0x04 - 0x17
    volatile uint16_t pcm_out_volume;    // 0x18 (MSB=Left, LSB=Right). Each step = 1.5dB
    uint8_t _pad2[0x0C];                 // 0x1A - 0x25
    volatile uint16_t powerdown_control; // 0x26
} __attribute__((packed)) ac97_mixer_regs_t;
static_assert(sizeof(ac97_mixer_regs_t) == 0x28, "Mixer Registers must be exactly 0x28 bytes");

// AC97 Busmaster Registers (Minimal)
// Ref [2] Section 5.8.1.3
typedef struct
{
    uint8_t _pad0[0x10]; // 0x00 - 0x0F

    // PCM Out (OFfset 0x10)
    volatile uint32_t pcm_out_buffer_base;     // 0x10
    volatile uint8_t pcm_out_current_index;    // 0x14
    volatile uint8_t pcm_out_last_valid_index; // 0x15
    uint8_t _pad1[0x05];                       // 0x16 - 0x1A
    volatile uint8_t pcm_out_control;          // 0x1B

    // Global Control/Status (Offset 0x2C/0x30)
    uint8_t _pad2[0x10];              // 0x1C - 0x2B
    volatile uint32_t global_control; // 0x2C
    volatile uint32_t global_status;  // 0x30
    uint8_t _pad3[0x3C];              // 0x34 - 0x6F

    // SPDIF Out (Offset 0x70)
    volatile uint32_t spdif_buffer_base;     // 0x70
    volatile uint8_t spdif_current_index;    // 0x74
    volatile uint8_t spdif_last_valid_index; // 0x75
    uint8_t _pad4[0x05];                     // 0x76 - 0x7A
    volatile uint8_t spdif_control;          // 0x7B

    // MCPX specific?
    volatile uint32_t x7c_unk_control;       // 0x7C
} __attribute__((packed)) ac97_busmaster_regs_t;
static_assert(sizeof(ac97_busmaster_regs_t) == 0x80, "Busmaster Registers must be exactly 0x80 bytes");

#endif // AUDIO_AC97_REG_H
