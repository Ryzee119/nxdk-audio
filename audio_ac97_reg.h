#ifndef AUDIO_AC97_REG_H
#define AUDIO_AC97_REG_H


#ifdef __cplusplus
extern "C" {
#endif

// MIXER https://www.alsa-project.org/files/pub/datasheets/intel/ac97r21.pdf
// BUS MASTER https://www.intel.com/Assets/PDF/manual/252751.pdf

#include <stdint.h>

// buffer descriptor from AC97 specification
typedef struct 
{  
	unsigned int   bufferStartAddress;
	unsigned short bufferLengthInSamples;  // 0=no smaples
	unsigned short bufferControl;          // b15=1=issue IRQ on completion, b14=1=last in stream
} ac97_descriptor_t __attribute__ ((aligned (8)));

// --- AC'97 Global Registers ---
#define AC97_GLOB_CNT_WARM_RESET   0x00000002 // 0x2C Bit 1: Warm Reset
#define AC97_GLOB_STA_CODEC_READY  0x00000100 // 0x30 Bit 8: Primary Codec Ready

// --- AC'97 Mixer Values ---
#define AC97_VOL_MAX                0x0000  // 0dB attenuation (Max Volume)
#define AC97_VOL_MIN                0x1F1F  // Maximum attenuation / Mute
#define AC97_VOL_12DB_ATTEN         0x0808  // 0x08 left / 0x08 right (12dB attenuation)
#define AC97_POWER_NORMAL           0x0000  // Everything powered on

// --- AC'97 Bus Master Control Register (CR) Bits ---
#define AC97_BM_CR_RUN              0x01    // Bit 0: Run/Pause Bus Master
#define AC97_BM_CR_RESET            0x02    // Bit 1: Reset Registers (Auto-clears)
#define AC97_BM_CR_LVBIE            0x04    // Bit 2: Last Valid Buffer Interrupt Enable
#define AC97_BM_CR_FEIE             0x08    // Bit 3: FIFO Error Interrupt Enable
#define AC97_BM_CR_IOCE             0x10    // Bit 4: Interrupt On Completion Enable

// 0x1D combines Run + all three Interrupt flags
#define AC97_BM_CR_RUN_WITH_INTS    (AC97_BM_CR_RUN | AC97_BM_CR_LVBIE | AC97_BM_CR_FEIE | AC97_BM_CR_IOCE)

// --- AC'97 Bus Master Status Register (SR) Bits ---
#define AC97_BM_SR_DCH              0x01    // Bit 0: DMA Controller Halted
#define AC97_BM_SR_CELV             0x02    // Bit 1: Current Equals Last Valid (Write 1 to clear)
#define AC97_BM_SR_LVBCI            0x04    // Bit 2: Last Valid Buffer Completion Interrupt
#define AC97_BM_SR_BCIS             0x08    // Bit 3: Buffer Completion Interrupt Status
#define AC97_BM_SR_FIFOE            0x10    // Bit 4: FIFO Error

// --- nForce / APU Specific ---
// 0x02000000 is a hardware-specific flag, likely enabling the SPDIF DMA engine
#define NFORCE_SPDIF_DMA_ENABLE     0x02000000

#pragma pack(push, 1)

// --- AC'97 Mixer Registers (Minimal) ---
typedef struct {
    uint8_t           _pad0[0x02];              // 0x00 - 0x01
    volatile uint16_t master_volume;            // 0x02
    uint8_t           _pad1[0x14];              // 0x04 - 0x17
    volatile uint16_t pcm_out_volume;           // 0x18
    uint8_t           _pad2[0x0C];              // 0x1A - 0x25
    volatile uint16_t powerdown_control;        // 0x26
} ac97_mixer_regs_t;

// --- AC'97 Bus Master Registers (Minimal) ---
typedef struct {
    uint8_t           _pad0[0x10];              // 0x00 - 0x0F
    
    // --- Analog PCM Out Channel ---
    volatile uint32_t pcm_out_buffer_base;      // 0x10
    volatile uint8_t  pcm_out_current_index;    // 0x14
    volatile uint8_t  pcm_out_last_valid_index; // 0x15
    uint8_t           _pad1[0x05];              // 0x16 - 0x1A
    volatile uint8_t  pcm_out_control;          // 0x1B
    
    // --- Global Registers & Codec Sync ---
    uint8_t           _pad2[0x10];              // 0x1C - 0x2B
    volatile uint32_t global_control;           // 0x2C
    volatile uint32_t global_status;            // 0x30
    volatile uint8_t  codec_access_semaphore;   // 0x34
    
    uint8_t           _pad3[0x3B];              // 0x35 - 0x6F
    
    // --- Digital SPDIF Channel ---
    volatile uint32_t spdif_buffer_base;        // 0x70
    volatile uint8_t  spdif_current_index;      // 0x74
    volatile uint8_t  spdif_last_valid_index;   // 0x75
    volatile uint16_t spdif_status;             // 0x76
    uint8_t           _pad4[0x03];              // 0x78 - 0x7A
    volatile uint8_t  spdif_control;            // 0x7B
    volatile uint32_t spdif_dma_config;         // 0x7C (Handles the 0x02000000 flag)
} ac97_busmaster_regs_t;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // AUDIO_AC97_REG_H
