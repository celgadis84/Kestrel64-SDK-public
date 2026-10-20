/* kestrel64-sdk — linker script.
 *
 * LMA = ROM offset (the ROM builder prepends the 0x1000-byte header+IPL3 block).
 * VMA of resident code/data = KSEG0 from 0x80000400 (bank 0 of RDRAM).
 * ROM-resident data (.rom.*) keeps VMA = PI address 0x10000000+offset: an `extern` symbol
 * there IS the cartridge address to DMA from. It costs zero RDRAM until requested.
 *
 * RDRAM bank plan (1 MiB banks, see docs/MEMORY.md):
 *   bank 0  0x80000000  vectors, code, data, bss, stack (top)
 *   bank 1+ managed at runtime by k64_mem (framebuffers / z / textures / heap)
 */
OUTPUT_ARCH(mips)
ENTRY(_start)

K64_STACK_SIZE = DEFINED(K64_STACK_SIZE) ? K64_STACK_SIZE : 0x10000;
K64_BANK0_END  = 0x80100000;
/* an app may align the start of .bss (--defsym=K64_BSS_ALIGN=0x2000) and of the heap
   (K64_HEAP_ALIGN) so a change of code size moves neither its hot data's D-cache lines nor
   the I-cache lines of code it generates on the heap */

SECTIONS
{
  . = 0x80000400;
  __image_start = .;

  .text : AT(0x1000) {
    __text_start = .;
    KEEP(*(.text.boot))
    *(.text.hot .text.hot.*)
    *(.text .text.*)
    *(.coldtext)                         /* run-once code: its size moves no hot code */
    KEEP(*(.text.vec)) KEEP(*(.text.exc))
    __text_end = .;
  }
  .rodata : ALIGN(16) {
    *(.rodata .rodata.* .srodata .srodata.*)
  }
  .data : ALIGN(16) {
    *(.data .data.* .sdata .sdata.*)
    *(.got .got.*)
    . = ALIGN(16);
  }
  __load_end = .;
  __boot_copy_end = MIN(__load_end, 0x80000400 + 0x100000);
  __load_lma_end = LOADADDR(.data) + SIZEOF(.data);

  /* NOLOAD: LMA = VMA so lld does not see it overlap the ROM payload LMAs. */
  .bss (NOLOAD) : AT(ADDR(.bss)) ALIGN(16) {
    . = ALIGN(DEFINED(K64_BSS_ALIGN) ? K64_BSS_ALIGN : 16);
    __bss_start = .;
    *(.sbss .sbss.* .bss .bss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  __heap0_start = ALIGN(__bss_end, DEFINED(K64_HEAP_ALIGN) ? K64_HEAP_ALIGN : 64);

  /* an app may lower the top (--defsym=K64_STACK_TOP=...) to put its hot frames on D-cache
     lines its hot data does not use: the cache is direct-mapped, 8 KiB */
  __stack_top = DEFINED(K64_STACK_TOP) ? K64_STACK_TOP : K64_BANK0_END;
  __stack_bottom = __stack_top - K64_STACK_SIZE;
  ASSERT(__heap0_start <= __stack_bottom, "bank 0 overflow: code+data+bss collide with the stack")

  /* ROM-only payload: VMA = PI address, LMA follows the resident image. */
  __rom_data_lma = ALIGN(__load_lma_end, 16);
  .rom (0x10000000 + __rom_data_lma) : AT(__rom_data_lma) ALIGN(16) {
    KEEP(*(.rom .rom.*))
    . = ALIGN(16);
  }
  __rom_end = .;

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.mdebug.*) *(.gnu.attributes) *(.note*) }
}
