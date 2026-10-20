// kestrel64-sdk — raw hardware map (HAL level 0).
// Every RCP/peripheral register lives in KSEG1 (uncached). Nothing here hides the
// hardware: names follow n64brew. Higher layers build on these.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

#define K64_INLINE    static inline __attribute__((always_inline))
#define K64_NOINLINE  __attribute__((noinline))
#define K64_ALIGN(n)  __attribute__((aligned(n)))
#define K64_LIKELY(x)   __builtin_expect(!!(x), 1)
#define K64_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define K64_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// ---- address spaces -------------------------------------------------------
#define K64_KSEG0(p)   ((void*)(((u32)(p) & 0x1FFFFFFF) | 0x80000000))  // cached
#define K64_KSEG1(p)   ((void*)(((u32)(p) & 0x1FFFFFFF) | 0xA0000000))  // uncached
#define K64_PHYS(p)    ((u32)(p) & 0x1FFFFFFF)

#define K64_REG(a)     (*(volatile u32*)(a))

// ---- RSP (SP) --------------------------------------------------------------
#define SP_DMEM        ((volatile u32*)0xA4000000)
#define SP_IMEM        ((volatile u32*)0xA4001000)
#define SP_MEM_ADDR    K64_REG(0xA4040000)
#define SP_DRAM_ADDR   K64_REG(0xA4040004)
#define SP_RD_LEN      K64_REG(0xA4040008)   // RDRAM -> SP mem
#define SP_WR_LEN      K64_REG(0xA404000C)   // SP mem -> RDRAM
#define SP_STATUS      K64_REG(0xA4040010)
#define SP_DMA_FULL    K64_REG(0xA4040014)
#define SP_DMA_BUSY    K64_REG(0xA4040018)
#define SP_SEMAPHORE   K64_REG(0xA404001C)
#define SP_PC          K64_REG(0xA4080000)

// SP_STATUS read bits
#define SP_ST_HALT        (1u<<0)
#define SP_ST_BROKE       (1u<<1)
#define SP_ST_DMA_BUSY    (1u<<2)
#define SP_ST_DMA_FULL    (1u<<3)
#define SP_ST_IO_FULL     (1u<<4)
#define SP_ST_SSTEP       (1u<<5)
#define SP_ST_INTR_BREAK  (1u<<6)
#define SP_ST_SIG(n)      (1u<<(7+(n)))
// SP_STATUS write bits
#define SP_WR_CLR_HALT       (1u<<0)
#define SP_WR_SET_HALT       (1u<<1)
#define SP_WR_CLR_BROKE      (1u<<2)
#define SP_WR_CLR_INTR       (1u<<3)
#define SP_WR_SET_INTR       (1u<<4)
#define SP_WR_CLR_SSTEP      (1u<<5)
#define SP_WR_SET_SSTEP      (1u<<6)
#define SP_WR_CLR_INTR_BREAK (1u<<7)
#define SP_WR_SET_INTR_BREAK (1u<<8)
#define SP_WR_CLR_SIG(n)     (1u<<(9+2*(n)))
#define SP_WR_SET_SIG(n)     (1u<<(10+2*(n)))

// ---- RDP command interface (DPC) ------------------------------------------
#define DPC_START      K64_REG(0xA4100000)
#define DPC_END        K64_REG(0xA4100004)
#define DPC_CURRENT    K64_REG(0xA4100008)
#define DPC_STATUS     K64_REG(0xA410000C)
#define DPC_CLOCK      K64_REG(0xA4100010)
#define DPC_BUFBUSY    K64_REG(0xA4100014)
#define DPC_PIPEBUSY   K64_REG(0xA4100018)
#define DPC_TMEM       K64_REG(0xA410001C)

#define DPC_ST_XBUS        (1u<<0)
#define DPC_ST_FREEZE      (1u<<1)
#define DPC_ST_FLUSH       (1u<<2)
#define DPC_ST_GCLK        (1u<<3)
#define DPC_ST_TMEM_BUSY   (1u<<4)
#define DPC_ST_PIPE_BUSY   (1u<<5)
#define DPC_ST_CMD_BUSY    (1u<<6)
#define DPC_ST_CBUF_READY  (1u<<7)
#define DPC_ST_DMA_BUSY    (1u<<8)
#define DPC_ST_END_VALID   (1u<<9)
#define DPC_ST_START_VALID (1u<<10)

#define DPC_WR_CLR_XBUS    (1u<<0)
#define DPC_WR_SET_XBUS    (1u<<1)
#define DPC_WR_CLR_FREEZE  (1u<<2)
#define DPC_WR_SET_FREEZE  (1u<<3)
#define DPC_WR_CLR_FLUSH   (1u<<4)
#define DPC_WR_SET_FLUSH   (1u<<5)
#define DPC_WR_CLR_TMEM    (1u<<6)
#define DPC_WR_CLR_PIPE    (1u<<7)
#define DPC_WR_CLR_CMD     (1u<<8)
#define DPC_WR_CLR_CLOCK   (1u<<9)

// ---- MIPS interface --------------------------------------------------------
#define MI_MODE        K64_REG(0xA4300000)
#define MI_VERSION     K64_REG(0xA4300004)
#define MI_INTR        K64_REG(0xA4300008)
#define MI_MASK        K64_REG(0xA430000C)

#define MI_INTR_SP  (1u<<0)
#define MI_INTR_SI  (1u<<1)
#define MI_INTR_AI  (1u<<2)
#define MI_INTR_VI  (1u<<3)
#define MI_INTR_PI  (1u<<4)
#define MI_INTR_DP  (1u<<5)
#define MI_MODE_CLR_REPEAT (1u<<7)
#define MI_MODE_SET_REPEAT (1u<<8)   // | (bytes - 1): the next RDRAM write repeats
#define MI_MODE_CLR_DP (1u<<11)
// MI_MASK write: clr = bit 2n, set = bit 2n+1 (n = SP,SI,AI,VI,PI,DP)
#define MI_MASK_CLR(n) (1u<<(2*(n)))
#define MI_MASK_SET(n) (1u<<(2*(n)+1))

// ---- Video interface -------------------------------------------------------
#define VI_CTRL        K64_REG(0xA4400000)
#define VI_ORIGIN      K64_REG(0xA4400004)
#define VI_WIDTH       K64_REG(0xA4400008)
#define VI_V_INTR      K64_REG(0xA440000C)
#define VI_V_CURRENT   K64_REG(0xA4400010)
#define VI_BURST       K64_REG(0xA4400014)
#define VI_V_SYNC      K64_REG(0xA4400018)
#define VI_H_SYNC      K64_REG(0xA440001C)
#define VI_H_SYNC_LEAP K64_REG(0xA4400020)
#define VI_H_VIDEO     K64_REG(0xA4400024)
#define VI_V_VIDEO     K64_REG(0xA4400028)
#define VI_V_BURST     K64_REG(0xA440002C)
#define VI_X_SCALE     K64_REG(0xA4400030)
#define VI_Y_SCALE     K64_REG(0xA4400034)
#define VI_REGS        ((volatile u32*)0xA4400000)

// ---- Audio interface -------------------------------------------------------
#define AI_DRAM_ADDR   K64_REG(0xA4500000)
#define AI_LEN         K64_REG(0xA4500004)
#define AI_CONTROL     K64_REG(0xA4500008)
#define AI_STATUS      K64_REG(0xA450000C)
#define AI_DACRATE     K64_REG(0xA4500010)
#define AI_BITRATE     K64_REG(0xA4500014)
#define AI_ST_FULL     (1u<<31)
#define AI_ST_BUSY     (1u<<30)

// ---- Peripheral interface (cartridge) --------------------------------------
#define PI_DRAM_ADDR   K64_REG(0xA4600000)
#define PI_CART_ADDR   K64_REG(0xA4600004)
#define PI_RD_LEN      K64_REG(0xA4600008)   // RDRAM -> cart
#define PI_WR_LEN      K64_REG(0xA460000C)   // cart -> RDRAM
#define PI_STATUS      K64_REG(0xA4600010)
#define PI_ST_DMA_BUSY (1u<<0)
#define PI_ST_IO_BUSY  (1u<<1)
#define PI_ST_ERROR    (1u<<2)
#define PI_WR_RESET    (1u<<0)
#define PI_WR_CLR_INTR (1u<<1)
#define PI_BSD_DOM2_LAT K64_REG(0xA4600024)
#define PI_BSD_DOM2_PWD K64_REG(0xA4600028)
#define PI_BSD_DOM2_PGS K64_REG(0xA460002C)
#define PI_BSD_DOM2_RLS K64_REG(0xA4600030)
#define PI_DOM2_ADDR2   0x08000000u          // SRAM / FlashRAM

// ---- Serial interface (PIF / joybus) ---------------------------------------
#define SI_DRAM_ADDR      K64_REG(0xA4800000)
#define SI_PIF_AD_RD64B   K64_REG(0xA4800004)
#define SI_PIF_AD_WR64B   K64_REG(0xA4800010)
#define SI_STATUS         K64_REG(0xA4800018)
#define SI_ST_DMA_BUSY    (1u<<0)
#define SI_ST_IO_BUSY     (1u<<1)
#define PIF_RAM_PHYS      0x1FC007C0u

// ---- boot info left by IPL3 (compat layout) --------------------------------
#define K64_BOOT_TVTYPE   (*(volatile u32*)0x80000300)   // 0 PAL, 1 NTSC, 2 MPAL
#define K64_BOOT_RESET    (*(volatile u32*)0x8000030C)   // 0 cold, 1 warm
#define K64_BOOT_MEMSIZE  (*(volatile u32*)0x80000318)   // bytes of RDRAM

// ---- clocks ----------------------------------------------------------------
#define K64_CPU_HZ     93750000u
#define K64_COUNT_HZ   (K64_CPU_HZ / 2)       // COP0 Count ticks at half CPU clock
#define K64_RCP_HZ     62500000u
