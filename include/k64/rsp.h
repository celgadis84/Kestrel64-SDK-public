// kestrel64-sdk — RSP (signal processor) control: microcode load, DMA, run/wait.
//
// Microcode is written in RSP assembly and built by tools/k64rsp.py (k64_add_ucode / the
// UCODE argument of k64_add_rom). Each ucode NAME yields:
//   extern const u8 NAME_text[], NAME_text_end[], NAME_data[], NAME_data_end[];
//   #include "NAME.h"   -> NAME_TEXT_SIZE, NAME_DATA_SIZE and every `.global` DMEM/IMEM label
//                          as NAME_<LABEL> (offset).
// K64_UCODE(NAME) builds the descriptor.
//
// DMA rules (hardware): 8-byte aligned RDRAM + SP addresses, length multiple of 8, max 4 KB.
// The CPU side handles the cache: RDRAM->SP writes back the source lines first, SP->RDRAM
// invalidates the destination.
#pragma once
#include "hw.h"

typedef struct {
  const u8 *text, *text_end;   // IMEM image
  const u8 *data, *data_end;   // DMEM image (initial)
} k64_ucode_t;

#define K64_UCODE_DECL(n) extern const u8 n##_text[], n##_text_end[], n##_data[], n##_data_end[]
#define K64_UCODE(n) ((k64_ucode_t){ n##_text, n##_text_end, n##_data, n##_data_end })

#define K64_DMEM_SIZE 4096
#define K64_IMEM_SIZE 4096

// Halts the RSP (if running), loads IMEM + DMEM. Synchronous.
void k64_rsp_load(const k64_ucode_t *u);
// IMEM image currently loaded (text pointer of the last k64_rsp_load), to skip reloads.
extern const u8 *k64_rsp_loaded;
// Synchronous DMA RDRAM <-> DMEM (dmem offset 0..0xFFF; set bit 12 for IMEM).
void k64_rsp_dma_to(u32 sp_addr, const void *src, u32 len);
void k64_rsp_dma_from(void *dst, u32 sp_addr, u32 len);
// Starts at IMEM offset `pc` (clears halt/broke/signals).
void k64_rsp_run(u32 pc);
// Same, with the SP interrupt raised by the final `break` (for an OS that waits on it).
void k64_rsp_run_intr(u32 pc);
K64_INLINE bool k64_rsp_halted(void) { return (SP_STATUS & SP_ST_HALT) != 0; }
// Waits for halt (`break` sets halt+broke). Returns false on timeout (`max_spins` polls, 0=forever).
bool k64_rsp_wait(u32 max_spins);
// Uncached word access to DMEM (debug / small mailboxes only: ~each access is an RCP bus trip).
K64_INLINE u32 k64_dmem_read(u32 off) { return SP_DMEM[off >> 2]; }
K64_INLINE void k64_dmem_write(u32 off, u32 v) { SP_DMEM[off >> 2] = v; }
