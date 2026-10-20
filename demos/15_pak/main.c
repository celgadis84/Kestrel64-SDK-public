// Demo 15 — controller accessories: identify the pak in each port. A Controller Pak gets the
// notes test (mount, formatting a blank pak; list the notes; a boot counter kept in a note;
// create / delete / repair checks); a Rumble Pak runs its motor while A is held.
#include <k64/k64.h>

static const char *pak_name[] = { "none", "Controller Pak", "Rumble Pak", "Transfer Pak", "unknown" };

#define GAME    0x4E4B3645u     // "NK6E"
#define COMPANY 0x3031u         // "01"

static k64_cpak_fs_t fs;

typedef struct { bool pass; u32 boots, notes, free, us; } notes_result_t;

static bool check(k64_cpak_err_t e, const char *what) {
  if (e != K64_CPAK_OK) k64_printf("[15_pak] %s: error %d\n", what, e);
  return e == K64_CPAK_OK;
}

// The counter sits at offset 254 of a 600-byte (3-page) note: the 4 bytes cross a page.
// Then a scratch note whose entry is wiped by a raw write (a reset halfway through a create:
// pages taken, no entry) — the next mount must give its pages back.
static notes_result_t notes_test(int port) {
  notes_result_t r = { false, 0, 0, 0, 0 };
  u32 t0 = k64_count();
  k64_cpak_err_t e = k64_cpak_mount(&fs, port);
  if (e == K64_CPAK_ERR_NOFS) {
    k64_printf("[15_pak] blank pak: formatting\n");
    if (!check(k64_cpak_format(port), "format")) return r;
    e = k64_cpak_mount(&fs, port);
  }
  if (!check(e, "mount")) return r;
  for (int i = 0; i < K64_CPAK_NOTES; i++) {
    k64_cpak_note_t n;
    if (!k64_cpak_note(&fs, i, &n)) continue;
    r.notes++;
    k64_printf("[15_pak]   note %2d  %08X %04X  %-16s .%-4s %u pages\n", i, n.game_code, n.company_code,
               n.name, n.ext, n.pages);
  }
  int idx = k64_cpak_find(&fs, GAME, COMPANY, "K64 Demo 15", "SAV");
  if (idx < 0 && !check(k64_cpak_create(&fs, GAME, COMPANY, "K64 Demo 15", "SAV", 600, &idx), "create")) return r;
  if (!check(k64_cpak_note_read(&fs, idx, 254, &r.boots, 4), "read")) return r;
  r.boots++;
  if (!check(k64_cpak_note_write(&fs, idx, 254, &r.boots, 4), "write")) return r;
  r.us = K64_US(k64_count() - t0);

  u32 before = k64_cpak_free_pages(&fs);
  int tmp;
  bool pass = check(k64_cpak_create(&fs, GAME, COMPANY, "SCRATCH", "", 5 * 256, &tmp), "create scratch") &&
              k64_cpak_free_pages(&fs) == before - 5;
  u8 zero[32] = { 0 };
  pass = pass && k64_cpak_write(port, (1 + 2u * fs.banks) * K64_CPAK_PAGE + (u32)tmp * 32, zero, 32);
  pass = pass && check(k64_cpak_mount(&fs, port), "remount") && fs.fixed && k64_cpak_free_pages(&fs) == before;
  if (!pass) k64_printf("[15_pak] repair check failed\n");

  // a normal create / delete, duplicate and bad names refused
  bool cd = check(k64_cpak_create(&fs, GAME, COMPANY, "SCRATCH", "", 2000, &tmp), "create 2") &&
            k64_cpak_create(&fs, GAME, COMPANY, "scratch", "", 10, NULL) == K64_CPAK_ERR_EXISTS &&
            k64_cpak_create(&fs, GAME, COMPANY, "bad\\name", "", 10, NULL) == K64_CPAK_ERR_NAME &&
            k64_cpak_free_pages(&fs) == before - 8 && check(k64_cpak_delete(&fs, tmp), "delete") &&
            k64_cpak_free_pages(&fs) == before;
  if (!cd) k64_printf("[15_pak] create/delete check failed\n");

  // all of it again after a clean mount
  u32 back = 0;
  bool again = check(k64_cpak_mount(&fs, port), "mount 3") && !fs.fixed &&
               k64_cpak_find(&fs, GAME, COMPANY, "k64 demo 15", "sav") == idx &&
               k64_cpak_note_read(&fs, idx, 254, &back, 4) == K64_CPAK_OK && back == r.boots &&
               k64_cpak_note_read(&fs, idx, 3 * 256 - 2, &back, 4) == K64_CPAK_ERR_RANGE;
  if (!again) k64_printf("[15_pak] remount check failed\n");
  r.free = k64_cpak_free_pages(&fs);
  r.pass = pass && cd && again;
  k64_printf("[15_pak] notes: %u on pak, boots %u, free pages %u, mount+counter %u us: %s\n", r.notes,
             r.boots, r.free, r.us, r.pass ? "PASS" : "FAIL");
  return r;
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  k64_pak_type_t type[4];
  notes_result_t res[4] = { 0 };
  for (int p = 0; p < 4; p++) {
    u32 t0 = k64_count();
    type[p] = k64_pak_probe(p);
    u32 us = K64_US(k64_count() - t0);
    k64_printf("[15_pak] port %d: %s (probe %u us)\n", p + 1, pak_name[type[p]], us);
    if (type[p] == K64_PAK_CONTROLLER) res[p] = notes_test(p);
  }

  bool rumbling[4] = { false };
  for (;;) {
    k64_pad_poll();
    for (int p = 0; p < 4; p++) {
      bool want = type[p] == K64_PAK_RUMBLE && (k64_pads[p].held & K64_PAD_A);
      if (want != rumbling[p]) { k64_rumble(p, want); rumbling[p] = want; }
    }
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  15 controller paks");
    for (int p = 0; p < 4; p++) {
      k64_text_printf(8, 32 + p * 24, 0xC0C0C0FF, "port %d  %s%s", p + 1, pak_name[type[p]],
                      rumbling[p] ? "  (rumble)" : "");
      if (type[p] == K64_PAK_CONTROLLER)
        k64_text_printf(24, 44 + p * 24, res[p].pass ? 0x40FF40FF : 0xFF4040FF, "notes %u  free %u pages  boots %u  %s",
                        res[p].notes, res[p].free, res[p].boots, res[p].pass ? "PASS" : "FAIL");
    }
    k64_text_printf(8, 140, 0x808080FF, "A = rumble (Rumble Pak ports)");
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
