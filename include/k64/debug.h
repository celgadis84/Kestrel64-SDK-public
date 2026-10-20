// kestrel64-sdk — debug output + crash handling.
// Output goes to the IS-Viewer 64 port (cart 0x13FF0000): supported by kestrel64, ares,
// cen64 and most flashcarts' USB debug bridges. Harmless on retail hardware.
#pragma once
#include "hw.h"
#include <stdarg.h>

int  k64_vsnprintf(char *buf, u32 size, const char *fmt, va_list ap);
int  k64_snprintf(char *buf, u32 size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void k64_debug_write(const char *s, u32 len);
void k64_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

__attribute__((noreturn)) void k64_panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void k64_exit(int code);

// Crash screen (k64_panic, unhandled CPU exceptions): message, PC and backtrace with function
// names (from the symbol table k64rom.py puts in the ROM), RSP/RDP and CPU registers. The
// hook adds the app's own lines (a port's guest state); it runs with interrupts off.
void k64_crash_set_hook(void (*fn)(char *buf, u32 size));

#define K64_ASSERT(c) do { if (K64_UNLIKELY(!(c))) k64_panic("assert %s @ %s:%d", #c, __FILE__, __LINE__); } while (0)

// Saved CPU context (layout shared with crt0.S).
typedef struct {
  u64 gpr[32];     // 0..255 ($0,$26,$27 unsaved)
  u64 hi, lo;      // 256, 264
  u32 epc, status, cause, badvaddr, fcsr; // 272..288
} k64_exc_frame_t;
