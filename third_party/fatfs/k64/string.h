/* kestrel64-sdk: the <string.h> FatFs needs, over the SDK runtime (src/rt/string.c). */
#pragma once
#include <stddef.h>
void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
static inline char *strchr(const char *s, int c) {
  for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
