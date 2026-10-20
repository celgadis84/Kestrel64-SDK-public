# Controller paks

`k64/pak.h`, demo 15_pak. Every call is blocking joybus traffic (~0.6 ms per 32-byte
transfer): use it at boot, on a save screen, or when a controller reports a pak change.

## Raw access

`k64_pak_probe` identifies the pak (Controller / Rumble / Transfer). `k64_pak_read32` /
`k64_pak_write32` move one 32-byte block and check the CRC the pak returns. `k64_cpak_read` /
`k64_cpak_write` address the 32 KiB of a Controller Pak as bytes (partial blocks are read
first and left alone when unchanged). `k64_rumble` drives the motor.

## Notes

Notes use the layout libultra games use, so a pak keeps its commercial saves and a note
written here shows up in a game's pak manager.

| Pages (256 B) | |
|---|---|
| 0 | ID block x4 (blocks 1, 3, 4, 6: serial, device, bank count, two checksums), label |
| 1 .. B | page table, one per 32 KiB bank: 128 entries `{bank, page}` |
| 1+B .. 2B | backup copy of the page tables |
| 1+2B, 2+2B | note table: 16 x 32 bytes (game code, company code, first page, status, extension, name) |
| 3+2B .. 127 | data, bank 0 (123 pages on a 32 KiB pak); pages 1..127 of the other banks |

A page table entry is the next page of the note, `0x0001` for its last page, `0x0003` for a
free page. Its first entry holds a checksum (low byte: byte sum of the bank's data-page
entries). Names and extensions are in the pak font (`0-9 A-Z ! " # ' * + , - . / : = ? @`
and space; lower case is stored as upper case).

```c
k64_cpak_fs_t fs;                      // ~1.6 KB: ID, page tables, note table
if (k64_cpak_mount(&fs, port) == K64_CPAK_ERR_NOFS) {   // blank or unreadable ID
  k64_cpak_format(port);               // erases every note
  k64_cpak_mount(&fs, port);
}
int i = k64_cpak_find(&fs, GAME, COMPANY, "MY GAME", "SAV");
if (i < 0) k64_cpak_create(&fs, GAME, COMPANY, "MY GAME", "SAV", sizeof save, &i);
k64_cpak_note_read(&fs, i, 0, &save, sizeof save);
k64_cpak_note_write(&fs, i, 0, &save, sizeof save);
```

Mount reads the ID block (the first of its 4 copies with good checksums; damaged copies before
it are rewritten), the page tables (from the backup when the main copy's checksum fails, which
is then rewritten), and the note table, then follows every note's chain: a note whose chain
leaves the data pages, hits a free page or a page another note owns is dropped, and used pages
no note owns are freed. `fs.fixed` says that something was repaired.

Write order keeps a reset harmless: a create writes the page tables (main, then backup) and
the note entry last; a delete clears the entry first. Either way a reset in between only
leaves owned-by-nobody pages, which the next mount frees (demo 15 checks exactly that by
wiping a fresh note's entry with a raw write).

Format detects the bank count by writing a marker to block 0 of each bank (bank select =
a write to 0x8000) and checking that bank 0 still holds its own; up to `K64_CPAK_MAX_BANKS`
(4). Only 1-bank paks are tested: kestrel64 emulates 32 KiB without bank select.

Game and company codes must both be non-zero (an all-zero entry is a free one).

## Measured (kestrel64, demo 15)

| | |
|---|---|
| mount + find + 4-byte read / write (1 note) | 38 ms |
| format + mount + create (blank pak) | 118 ms |
| probe | 5 ms |

## Transfer Pak (`k64/tpak.h`, demo 27)

Register map from n64brew "Transfer Pak", cartridge header and MBC registers from Pan Docs.

| Pak address | Write | Read |
|-------------|-------|------|
| 0x8000 | 0x84 power on, 0xFE off | 0x84 while on |
| 0xA000 | GB bus quarter (0-3) shown at 0xC000 | - |
| 0xB000 | 1 cartridge access on, 0 off | status: 0x80 cart present, 0x40 pulled, 0x04 reset seen, 0x01 access on |
| 0xC000-0xFFFF | GB bus write | GB bus read |

`k64_gbcart_open` powers the pak, checks the header checksum (0x14D) and decodes title, CGB
flag, MBC, ROM/RAM sizes. ROM and RAM are then addressed linearly: the layer writes the MBC
bank registers (a 32-byte block filled with the value, `k64_tpak_poke`) and the pak bank.
MBC1 banks 0x20/0x40/0x60 are read through 0x0000 in mode 1. RAM is enabled (0x0A) only for
the duration of each call, so a stray write cannot reach the save.

Testing: `python demos/27_tpak/mkgbrom.py outdir` writes four carts (MBC1 1 MiB + 8 KiB RAM,
MBC3+RTC 128 KiB + 32 KiB, MBC5 CGB 512 KiB + 128 KiB, MBC2 256 KiB). In kestrel64:
`KESTREL_PADS=1111 KESTREL_PADACC=3333 KESTREL_TPAK_ROM1..4=<cart>`. The demo opens ~0.1-0.9 s
per port (each 32-byte transfer is one joybus round trip).
