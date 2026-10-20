#!/usr/bin/env python3
"""kestrel64-sdk ROM file system image: a directory tree packed for k64/romfs.h.

    k64fs.py out.k64fs dir [--pack GLOB ...]

Files whose path (relative, '/' separators) matches a --pack glob are stored as .k64z
(tools/k64pack.py) unless that does not make them smaller; k64_romfs_load decodes them.

Image layout (big-endian, everything 16-byte aligned):
  0   'K64S'
  4   u32 count        files
  8   u32 dir_size     bytes from 0 to the end of the name table (the part the runtime loads)
  12  u32 total_size
  16  count x entry, sorted by (hash, name):
        u32 hash       FNV-1a 32 of the path bytes
        u32 name       offset of the NUL-terminated path
        u32 data       offset of the file data (16-aligned)
        u32 size       stored bytes; bit 31 = packed (.k64z)
      name table, then the file data.
"""
import fnmatch, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k64pack

PACKED = 0x80000000


def fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def align(n, a=16):
    return (n + a - 1) & ~(a - 1)


def build(root, pack_globs):
    files = []
    for d, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(d, n)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            data = open(full, 'rb').read()
            packed = False
            if any(fnmatch.fnmatch(rel, g) for g in pack_globs):
                z = k64pack.pack(data)
                if len(z) < len(data):
                    data, packed = z, True
            files.append((fnv1a(rel.encode()), rel, data, packed))
    files.sort(key=lambda f: (f[0], f[1]))
    for a, b in zip(files, files[1:]):
        if a[1] == b[1]:
            sys.exit('k64fs: duplicate path %s' % a[1])
    names = b''
    name_off = []
    base = 16 + 16 * len(files)
    for f in files:
        name_off.append(base + len(names))
        names += f[1].encode() + b'\0'
    dir_size = align(base + len(names))
    pos = dir_size
    entries, blobs = b'', b''
    for f, no in zip(files, name_off):
        size = len(f[2])
        entries += struct.pack('>IIII', f[0], no, pos, size | (PACKED if f[3] else 0))
        blob = f[2] + bytes(align(size) - size)
        blobs += blob
        pos += len(blob)
    head = b'K64S' + struct.pack('>III', len(files), dir_size, pos)
    img = head + entries + names
    img += bytes(dir_size - len(img)) + blobs
    assert len(img) == pos
    return img, files


def main():
    args = sys.argv[1:]
    globs = []
    while '--pack' in args:
        i = args.index('--pack')
        globs.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    img, files = build(args[1], globs)
    open(args[0], 'wb').write(img)
    print('k64fs: %d files, %d bytes (%d packed)' % (len(files), len(img), sum(f[3] for f in files)))


if __name__ == '__main__':
    main()
