// Demo 14 — cartridge save memory: a boot counter that survives power-off, plus a
// read-back check of unaligned writes across block edges. Built four times (EEPROM 16K, SRAM 256K, SRAM 768K, FlashRAM).
#include <k64/k64.h>

typedef struct { u32 magic, boots; } header_t;
#define MAGIC 0x4B363453u   // "K64S"

static const char *type_name(k64_save_type_t t) {
  static const char *n[] = { "none", "EEPROM 4K", "EEPROM 16K", "SRAM 256K", "SRAM 1M", "FlashRAM 1M", "SRAM 768K" };
  return n[t];
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  k64_save_type_t t = k64_save_init();
  u32 size = k64_save_size();
  k64_printf("[14_save] device %s, %u bytes\n", type_name(t), size);

  header_t h = { 0, 0 };
  bool ok = t != K64_SAVE_NONE && k64_save_read(0, &h, sizeof h);
  if (h.magic != MAGIC) h = (header_t){ MAGIC, 0 };
  h.boots++;
  u32 t0 = k64_count();
  ok = ok && k64_save_write(0, &h, sizeof h);
  u32 us_hdr = K64_US(k64_count() - t0);

  // Unaligned pattern across block edges, different every boot; then read it back.
  u8 pat[45], back[45];
  for (u32 i = 0; i < sizeof pat; i++) pat[i] = (u8)(i * 37 + h.boots * 11);
  u32 off = size - 64 + 3;
  t0 = k64_count();
  bool wr = ok && k64_save_write(off, pat, sizeof pat);
  u32 us_wr = K64_US(k64_count() - t0);
  t0 = k64_count();
  bool rd = wr && k64_save_read(off, back, sizeof back);
  u32 us_rd = K64_US(k64_count() - t0);
  bool same = rd;
  for (u32 i = 0; same && i < sizeof pat; i++) same = pat[i] == back[i];
  header_t h2 = { 0, 0 };
  bool hdr_ok = ok && k64_save_read(0, &h2, sizeof h2) && h2.magic == MAGIC && h2.boots == h.boots;
  bool pass = ok && same && hdr_ok;
  bool oob = !k64_save_write(size - 4, pat, 8);                  // past the end: refused
  pass = pass && oob;
  // A write across each 32 KB boundary (768K SRAM: the bank edges) and one marker per 32 KB:
  // banks that alias each other or a split at the wrong place fail here.
  u32 marks = 0;
  for (u32 b = 32768; pass && b + 64 <= size; b += 32768) {
    u8 m[45], r[45];
    for (u32 i = 0; i < sizeof m; i++) m[i] = (u8)(b >> 15) ^ (u8)(i * 13 + h.boots);
    pass = k64_save_write(b - 20, m, sizeof m) && k64_save_read(b - 20, r, sizeof r);
    for (u32 i = 0; pass && i < sizeof m; i++) pass = m[i] == r[i];
    marks++;
  }
  for (u32 b = 0; pass && b + 64 <= size; b += 32768) {
    u8 m[4] = { 0xA5, (u8)(b >> 15), (u8)h.boots, 0x5A };
    pass = k64_save_write(b + 100, m, 4);
  }
  for (u32 b = 0; pass && b + 64 <= size; b += 32768) {
    u8 r[4];
    pass = k64_save_read(b + 100, r, 4) && r[0] == 0xA5 && r[1] == (u8)(b >> 15) && r[2] == (u8)h.boots;
  }
  k64_printf("[14_save] edges %u  boots %u  header %u us  write45 %u us  read45 %u us  %s\n",
             marks, h.boots, us_hdr, us_wr, us_rd, pass ? "PASS" : "FAIL");

  for (;;) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  14 save memory");
    k64_text_printf(8, 32, 0xC0C0C0FF, "device    %s (%u bytes)", type_name(t), size);
    k64_text_printf(8, 48, 0xFFFF80FF, "boots     %u", h.boots);
    k64_text_printf(8, 64, 0xC0C0C0FF, "header write  %u us", us_hdr);
    k64_text_printf(8, 76, 0xC0C0C0FF, "45 B write    %u us", us_wr);
    k64_text_printf(8, 88, 0xC0C0C0FF, "45 B read     %u us", us_rd);
    k64_text_printf(8, 112, pass ? 0x40FF40FF : 0xFF4040FF, "read-back %s", pass ? "PASS" : "FAIL");
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
