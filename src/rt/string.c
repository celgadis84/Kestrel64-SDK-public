// kestrel64-sdk — freestanding mem* (the compiler emits calls to these).
// 64-bit moves when both pointers share 8-byte alignment: VR4300 does ld/sd in one cycle
// on a D$ hit, so wide copies halve the instruction count versus word loops.
#include <k64/hw.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
  u8 *d = dst; const u8 *s = src;
  if ((((u32)d ^ (u32)s) & 7) == 0) {
    while (n && ((u32)d & 7)) { *d++ = *s++; n--; }
    u64 *d8 = (u64*)d; const u64 *s8 = (const u64*)s;
    while (n >= 32) { u64 a = s8[0], b = s8[1], c = s8[2], e = s8[3];
                      d8[0] = a; d8[1] = b; d8[2] = c; d8[3] = e; d8 += 4; s8 += 4; n -= 32; }
    while (n >= 8)  { *d8++ = *s8++; n -= 8; }
    d = (u8*)d8; s = (const u8*)s8;
  } else if ((((u32)d ^ (u32)s) & 3) == 0) {
    while (n && ((u32)d & 3)) { *d++ = *s++; n--; }
    u32 *d4 = (u32*)d; const u32 *s4 = (const u32*)s;
    while (n >= 4) { *d4++ = *s4++; n -= 4; }
    d = (u8*)d4; s = (const u8*)s4;
  }
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  u8 *d = dst; const u8 *s = src;
  if (d <= s || d >= s + n) return memcpy(dst, src, n);
  while (n--) d[n] = s[n];
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  u8 *d = dst;
  u64 v = (u8)c; v |= v << 8; v |= v << 16; v |= v << 32;
  while (n && ((u32)d & 7)) { *d++ = (u8)c; n--; }
  u64 *d8 = (u64*)d;
  while (n >= 32) { d8[0] = v; d8[1] = v; d8[2] = v; d8[3] = v; d8 += 4; n -= 32; }
  while (n >= 8)  { *d8++ = v; n -= 8; }
  d = (u8*)d8;
  while (n--) *d++ = (u8)c;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
  return 0;
}

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) if (*a != *b || !*a) return (u8)*a - (u8)*b;
  return 0;
}
