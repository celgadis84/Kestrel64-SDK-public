#!/usr/bin/env python3
"""Writes the four synthetic Game Boy cartridges demo 27_tpak expects (one per port):
   mkgbrom.py outdir  ->  mbc1.gb mbc3.gb mbc5.gb mbc2.gb
ROM byte i = rom_byte(i) (same formula as main.c) with a valid header at 0x100-0x14F."""
import os, sys

CARTS = [  # file, title, cgb flag, type, rom code (32 KiB << n), ram code
    ("mbc1.gb", b"K64 MBC1", 0x00, 0x03, 5, 2),
    ("mbc3.gb", b"K64 MBC3", 0x00, 0x10, 2, 3),
    ("mbc5.gb", b"K64 MBC5", 0x80, 0x1B, 4, 4),
    ("mbc2.gb", b"K64 MBC2", 0x00, 0x06, 3, 0),
]

def rom_byte(i):
    return (i * 31 ^ (i >> 14) * 0x5B ^ (i >> 8)) & 0xFF

def main(out):
    os.makedirs(out, exist_ok=True)
    for name, title, cgb, typ, romc, ramc in CARTS:
        rom = bytearray(rom_byte(i) for i in range(0x8000 << romc))
        rom[0x134:0x144] = title.ljust(16, b"\0")
        rom[0x143] = cgb
        rom[0x147], rom[0x148], rom[0x149] = typ, romc, ramc
        x = 0
        for b in rom[0x134:0x14D]:
            x = (x - b - 1) & 0xFF
        rom[0x14D] = x
        with open(os.path.join(out, name), "wb") as f:
            f.write(rom)
        sav = os.path.join(out, name[:-3] + ".sav")
        if os.path.exists(sav):
            os.remove(sav)

if __name__ == "__main__":
    main(sys.argv[1])
