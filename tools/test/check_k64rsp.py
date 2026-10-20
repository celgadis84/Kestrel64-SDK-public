#!/usr/bin/env python3
"""Regression test for tools/k64rsp.py: assembles rsp_enc_body.inc and compares it byte for byte
with rsp_enc_armips.bin (golden output produced once by armips from rsp_enc_armips.s).
Also checks .def/.undef register aliases. No armips needed. Exit code 0 = pass."""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASM = os.path.join(HERE, '..', 'k64rsp.py')

def assemble(src_text, name):
    d = tempfile.mkdtemp(prefix='k64rsp_')
    src = os.path.join(d, name + '.S')
    with open(src, 'w') as f:
        f.write(src_text)
    out = os.path.join(d, name)
    subprocess.run([sys.executable, ASM, src, '-o', out, '-I', HERE], check=True)
    with open(out + '.text.bin', 'rb') as f:
        return f.read()

def main():
    ok = True
    golden = open(os.path.join(HERE, 'rsp_enc_armips.bin'), 'rb').read()
    got = assemble('.text\n.include "rsp_enc_body.inc"\n', 'enc')
    if got[:len(golden)] != golden or any(got[len(golden):]):
        for i in range(0, len(golden), 4):
            if got[i:i+4] != golden[i:i+4]:
                print(f'encoding mismatch at +0x{i:03x}: k64rsp {got[i:i+4].hex()} armips {golden[i:i+4].hex()}')
                break
        ok = False
    a = assemble('.text\n.def vtmp $v12\n.def rptr $s3\nvadd vtmp, vtmp, $v1[0]\nlw $t0, 4(rptr)\n'
                 '.undef vtmp\n', 'alias')
    b = assemble('.text\nvadd $v12, $v12, $v1[0]\nlw $t0, 4($s3)\n', 'plain')
    if a != b:
        print('alias mismatch'); ok = False
    print('k64rsp: PASS' if ok else 'k64rsp: FAIL')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
