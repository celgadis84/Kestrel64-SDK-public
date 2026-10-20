#!/usr/bin/env python3
"""kestrel64-sdk ROM builder: flat binary (ELF LMA 0x1000..) -> bootable .z64.

Layout: [0x0000 header][0x0040 IPL3 (libdragon ipl3_compat, CIC-6102)][0x1000 image...]
The header entry point is 0x80000400; IPL3 copies ROM 0x1000..0x101000 there.
With --save the header follows the Advanced Homebrew ROM Header (n64brew): "ED" at 0x3C
and the save type in the high nibble of 0x3F, which flashcarts and emulators read; libk64's
k64_save_init() reads it too.
The CIC-6102 checksum is computed over that 1 MiB so the ROM is also valid for any
loader that verifies it.
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IPL3 = os.path.join(HERE, '..', 'third_party', 'ipl3', 'ipl3_compat.z64')
M = 0xFFFFFFFF
SAVE_TYPES = {'none': 0, 'eeprom4k': 1, 'eeprom16k': 2, 'sram256k': 3, 'sram768k': 4, 'flash1m': 5, 'sram1m': 6}


def rol(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M if n else v


def cic6102_crc(rom):
    t1 = t2 = t3 = t4 = t5 = t6 = 0xF8CA4DDC
    words = struct.unpack_from('>262144I', rom, 0x1000)
    for d in words:
        k = (t6 + d) & M
        if k < t6:
            t4 = (t4 + 1) & M
        t6 = k
        t3 ^= d
        r = rol(d, d & 0x1F)
        t5 = (t5 + r) & M
        if t2 > d:
            t2 ^= r
        else:
            t2 ^= t6 ^ d
        t1 = (t1 + (t5 ^ d)) & M
    return (t6 ^ t4 ^ t3) & M, (t5 ^ t2 ^ t1) & M


def elf_functions(path):
    """(addr, size, name) of every function symbol of an ELF32/64 file."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        sys.exit('k64rom: %s is not an ELF file' % path)
    e = '>' if d[5] == 2 else '<'
    if d[4] == 1:
        shoff, = struct.unpack_from(e + 'I', d, 0x20)
        shentsize, shnum = struct.unpack_from(e + 'HH', d, 0x2E)
        sec = lambda i: struct.unpack_from(e + '10I', d, shoff + i * shentsize)
    else:
        shoff, = struct.unpack_from(e + 'Q', d, 0x28)
        shentsize, shnum = struct.unpack_from(e + 'HH', d, 0x3A)
        sec = lambda i: struct.unpack_from(e + 'IIQQQQIIQQ', d, shoff + i * shentsize)
    out = []
    for i in range(shnum):
        s = sec(i)
        if s[1] != 2:                                        # SHT_SYMTAB
            continue
        strtab = sec(s[6])
        off, size, ent = s[4], s[5], s[9]
        for j in range(off, off + size, ent):
            if d[4] == 1:
                name, value, sz, info, _, shndx = struct.unpack_from(e + 'IIIBBH', d, j)
            else:
                name, info, _, shndx, value, sz = struct.unpack_from(e + 'IBBHQQ', d, j)
            if info & 15 != 2 or not value:                  # STT_FUNC
                continue
            n = d[strtab[4] + name:d.index(b'\0', strtab[4] + name)]
            out.append((value & 0xFFFFFFFF, sz & 0xFFFFFFFF, n[:47]))
    return out


def symtab_blob(path):
    """'K64S', count, then (addr, size, name offset) sorted by address, then the names."""
    funcs = sorted(set(elf_functions(path)))
    seen, uniq = set(), []
    for f in funcs:
        if f[0] not in seen:
            seen.add(f[0])
            uniq.append(f)
    head = 8 + 12 * len(uniq)
    names, ents = bytearray(), bytearray()
    for addr, size, name in uniq:
        ents += struct.pack('>III', addr, size, head + len(names))
        names += name + b'\0'
    blob = b'K64S' + struct.pack('>I', len(uniq)) + ents + names
    return blob + bytes(-len(blob) & 15)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', help='flat binary from llvm-objcopy -O binary (starts at ROM 0x1000)')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--title', default='KESTREL64 SDK')
    ap.add_argument('--code', default='NK6E', help='4-char game code (category, id, region)')
    ap.add_argument('--save', choices=sorted(SAVE_TYPES), help='save device the game expects')
    ap.add_argument('--ipl3', default=DEFAULT_IPL3)
    ap.add_argument('--min-size', type=int, default=2 << 20)
    ap.add_argument('--elf', help='ELF to take function names from, for the crash screen')
    a = ap.parse_args()

    boot = bytearray(open(a.ipl3, 'rb').read())
    if len(boot) != 0x1000 or boot[:4] != b'\x80\x37\x12\x40':
        sys.exit('k64rom: %s is not a 4 KiB big-endian header+IPL3 block' % a.ipl3)
    image = open(a.binary, 'rb').read()

    struct.pack_into('>I', boot, 0x08, 0x80000400)          # entry point
    title = a.title.encode('ascii', 'replace')[:20].ljust(20, b' ')
    boot[0x20:0x34] = title
    code = a.code.encode('ascii')[:4].ljust(4, b' ')
    boot[0x3B:0x3F] = code
    boot[0x3F] = 0                                           # version
    if a.save:
        boot[0x3C:0x3E] = b'ED'                              # Advanced Homebrew ROM Header
        boot[0x3F] = SAVE_TYPES[a.save] << 4

    rom = boot + image
    if a.elf:                                                # symbol table, offset at 0x34
        rom += bytes(-len(rom) & 15)
        struct.pack_into('>I', rom, 0x34, len(rom))
        rom += symtab_blob(a.elf)
    size = max(a.min_size, len(rom))
    size = (size + 0xFFFFF) & ~0xFFFFF                       # whole MiB: flashcart friendly
    rom += bytes(size - len(rom))

    crc1, crc2 = cic6102_crc(rom)
    struct.pack_into('>II', rom, 0x10, crc1, crc2)
    with open(a.output, 'wb') as f:
        f.write(rom)
    print('k64rom: %s  image %d KB  rom %d KB  crc %08X %08X' %
          (os.path.basename(a.output), len(image) >> 10, size >> 10, crc1, crc2))


if __name__ == '__main__':
    main()
