#!/usr/bin/env python3
"""k64rsp — RSP (N64 Reality Signal Processor) assembler for kestrel64-sdk.

Pure Python, no dependencies. Syntax follows armips' RSP dialect so code (and encodings) can
be cross-checked against it:

  vmudh $v1, $v2, $v3[0]      compute ops: element [0q] [1q] [0h]..[3h] [0]..[7] (none = vector)
  lqv   $v1[0], 16($s0)       vector load/store: [byte element]; offset in bytes (scaled)
  mfc2  $t0, $v1[4]           byte element
  vrcp  $v1[2], $v3[2]        vrcp*/vrsq*/vmov: destination element, source element
  cfc2  $t0, vcc              control regs vco/vcc/vce

Sections:  .text [base]  -> IMEM image (labels are IMEM offsets, 0x000-0xFFF)
           .data [base]  -> DMEM image (labels are DMEM offsets)
           .overlay NAME, base -> IMEM code assembled at `base` (an overlay slot) but kept out
                            of the IMEM image; `.text` resumes the image. Overlays are written
                            to out_prefix.ovl.bin, each padded to 8 bytes, and the header gets
                            NAME_OVL_<NAME>_OFF / _SIZE (offset in .ovl.bin, padded size).
Directives: .org .align(2^n) .balign .byte .half .word .dword .space .ascii .asciiz
            .equ/.set/= , .def NAME text (textual alias, e.g. register names), .undef, .global, .include, .macro/.endm (\\arg, \\@), .if/.else/.endif,
            .error, .assert expr[, msg]
Pseudo-ops: nop, move, li, la, b, beqz, bnez, bal, neg, not, subi/addi aliases kept literal.

Usage: k64rsp.py src.S -o out_prefix [--header out.h --prefix NAME] [-I dir] [-D name=val]
       writes out_prefix.text.bin, out_prefix.data.bin and out_prefix.ovl.bin
"""
import argparse, os, re, sys

GPR = {'zero': 0, 'at': 1, 'v0': 2, 'v1': 3, 'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7,
       't0': 8, 't1': 9, 't2': 10, 't3': 11, 't4': 12, 't5': 13, 't6': 14, 't7': 15,
       's0': 16, 's1': 17, 's2': 18, 's3': 19, 's4': 20, 's5': 21, 's6': 22, 's7': 23,
       't8': 24, 't9': 25, 'k0': 26, 'k1': 27, 'gp': 28, 'sp': 29, 'fp': 30, 's8': 30, 'ra': 31}
COP0 = {'sp_mem_addr': 0, 'sp_dram_addr': 1, 'sp_rd_len': 2, 'sp_wr_len': 3, 'sp_status': 4,
        'sp_dma_full': 5, 'sp_dma_busy': 6, 'sp_semaphore': 7, 'dpc_start': 8, 'dpc_end': 9,
        'dpc_current': 10, 'dpc_status': 11, 'dpc_clock': 12, 'dpc_bufbusy': 13,
        'dpc_pipebusy': 14, 'dpc_tmem': 15}
CTRL = {'vco': 0, 'vcc': 1, 'vce': 2}

R_OPS = {'sll': 0, 'srl': 2, 'sra': 3, 'sllv': 4, 'srlv': 6, 'srav': 7, 'jr': 8, 'jalr': 9,
         'break': 13, 'add': 0x20, 'addu': 0x21, 'sub': 0x22, 'subu': 0x23, 'and': 0x24,
         'or': 0x25, 'xor': 0x26, 'nor': 0x27, 'slt': 0x2A, 'sltu': 0x2B}
I_ARITH = {'addi': 8, 'addiu': 9, 'slti': 10, 'sltiu': 11, 'andi': 12, 'ori': 13, 'xori': 14}
LOADSTORE = {'lb': 0x20, 'lh': 0x21, 'lw': 0x23, 'lbu': 0x24, 'lhu': 0x25, 'lwu': 0x27,
             'sb': 0x28, 'sh': 0x29, 'sw': 0x2B}
BR2 = {'beq': 4, 'bne': 5}
BR1 = {'blez': (6, 0), 'bgtz': (7, 0), 'bltz': (1, 0), 'bgez': (1, 1), 'bltzal': (1, 0x10), 'bgezal': (1, 0x11)}
VOPS = {'vmulf': 0x00, 'vmulu': 0x01, 'vrndp': 0x02, 'vmulq': 0x03, 'vmudl': 0x04, 'vmudm': 0x05,
        'vmudn': 0x06, 'vmudh': 0x07, 'vmacf': 0x08, 'vmacu': 0x09, 'vrndn': 0x0A, 'vmacq': 0x0B,
        'vmadl': 0x0C, 'vmadm': 0x0D, 'vmadn': 0x0E, 'vmadh': 0x0F, 'vadd': 0x10, 'vsub': 0x11,
        'vabs': 0x13, 'vaddc': 0x14, 'vsubc': 0x15, 'vsar': 0x1D, 'vlt': 0x20, 'veq': 0x21,
        'vne': 0x22, 'vge': 0x23, 'vcl': 0x24, 'vch': 0x25, 'vcr': 0x26, 'vmrg': 0x27,
        'vand': 0x28, 'vnand': 0x29, 'vor': 0x2A, 'vnor': 0x2B, 'vxor': 0x2C, 'vnxor': 0x2D}
VSINGLE = {'vrcp': 0x30, 'vrcpl': 0x31, 'vrcph': 0x32, 'vmov': 0x33, 'vrsq': 0x34, 'vrsql': 0x35, 'vrsqh': 0x36}
# vector load/store: name -> (opcode field, scale)
VLS = {'bv': (0, 1), 'sv': (1, 2), 'lv': (2, 4), 'dv': (3, 8), 'qv': (4, 16), 'rv': (5, 16),
       'pv': (6, 8), 'uv': (7, 8), 'hv': (8, 16), 'fv': (9, 16), 'wv': (10, 16), 'tv': (11, 16)}


class AsmError(Exception):
    pass


class Section:
    def __init__(self, name, base, limit):
        self.name, self.base, self.limit = name, base, limit
        self.data = bytearray()
        self.pc = base

    def emit(self, b):
        off = self.pc - self.base
        if len(self.data) < off:
            self.data += bytes(off - len(self.data))
        self.data[off:off + len(b)] = b
        self.pc += len(b)


class Assembler:
    def __init__(self, incdirs, defines):
        self.incdirs = incdirs
        self.symbols = dict(defines)
        self.globals = []
        self.macros = {}
        self.macro_count = 0
        self.defs = {}
        self.pass_no = 1
        self.loc = ('?', 0)

    # ---- lexing helpers ----------------------------------------------------------------
    def err(self, msg):
        raise AsmError('%s:%d: %s' % (self.loc[0], self.loc[1], msg))

    def eval(self, expr, allow_undef=False):
        expr = expr.strip()
        if not expr:
            self.err('missing expression')
        def repl(m):
            name = m.group(0)
            if name in self.symbols:
                return '(%d)' % self.symbols[name]
            if self.pass_no == 1 or allow_undef:
                raise KeyError(name)
            self.err('undefined symbol %s' % name)
        py = re.sub(r'(?<![0-9xXa-fA-F.])\b[A-Za-z_.][A-Za-z0-9_.]*\b', repl, expr.replace('/', '//'))
        py = py.replace('!', ' not ').replace(' not =', '!=').replace('&&', ' and ').replace('||', ' or ')
        try:
            v = eval(py, {'__builtins__': {}}, {})
        except Exception as e:
            self.err('bad expression "%s": %s' % (expr, e))
        return int(v)

    def try_eval(self, expr):
        try:
            return self.eval(expr)
        except KeyError:
            return None

    def gpr(self, tok):
        t = tok.strip().lower()
        if not t.startswith('$'):
            if t in self.symbols and False:
                pass
            self.err('expected register, got "%s"' % tok)
        t = t[1:]
        if t.isdigit() and int(t) < 32:
            return int(t)
        if t in GPR:
            return GPR[t]
        self.err('bad register "%s"' % tok)

    def c0reg(self, tok):
        t = tok.strip().lower().lstrip('$')
        if t.startswith('c') and t[1:].isdigit():
            return int(t[1:])
        if t in COP0:
            return COP0[t]
        if t.isdigit():
            return int(t)
        self.err('bad COP0 register "%s"' % tok)

    VREG = re.compile(r'^\$v(\d+)\s*(?:\[\s*([^\]]*)\s*\])?$', re.I)

    def vreg(self, tok):
        m = self.VREG.match(tok.strip())
        if not m or int(m.group(1)) > 31:
            self.err('bad vector register "%s"' % tok)
        return int(m.group(1)), m.group(2)

    def velem_compute(self, e):
        if e is None or e == '':
            return 0
        e = e.strip().lower()
        m = re.match(r'^([0-1])q$', e)
        if m:
            return 2 + int(m.group(1))
        m = re.match(r'^([0-3])h$', e)
        if m:
            return 4 + int(m.group(1))
        v = self.eval(e)
        if not 0 <= v <= 7:
            self.err('element out of range: %s' % e)
        return 8 + v

    def velem_num(self, e, hi):
        if e is None or e == '':
            return 0
        v = self.eval(e)
        if not 0 <= v <= hi:
            self.err('element out of range: %s' % e)
        return v

    def mem_operand(self, tok):
        m = re.match(r'^(.*)\(\s*(\$[A-Za-z0-9]+)\s*\)$', tok.strip())
        if not m:
            self.err('expected offset($reg), got "%s"' % tok)
        off = m.group(1).strip()
        return (off if off else '0'), self.gpr(m.group(2))

    # ---- source handling ----------------------------------------------------------------
    def read_file(self, path):
        for d in [''] + self.incdirs:
            p = os.path.join(d, path) if d else path
            if os.path.exists(p):
                with open(p, encoding='utf-8') as f:
                    return p, f.read().splitlines()
        self.err('cannot find include "%s"' % path)

    def lines(self, path, text_lines, depth=0):
        """Yield (file, lineno, line) with includes and macros expanded."""
        if depth > 32:
            self.err('include/macro nesting too deep')
        i = 0
        while i < len(text_lines):
            raw = text_lines[i]
            self.loc = (path, i + 1)
            line = re.sub(r'(//|#|;).*$', '', raw).strip()
            i += 1
            if not line:
                continue
            m = re.match(r'^\.include\s+"([^"]+)"$', line)
            if m:
                p, sub = self.read_file(m.group(1))
                yield from self.lines(p, sub, depth + 1)
                continue
            m = re.match(r'^\.macro\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$', line)
            if m:
                name = m.group(1)
                params = [p.strip() for p in m.group(2).replace(',', ' ').split()] if m.group(2).strip() else []
                body = []
                while i < len(text_lines):
                    l2 = text_lines[i]
                    i += 1
                    if re.match(r'^\s*\.endm\b', l2):
                        break
                    body.append(l2)
                else:
                    self.err('.macro %s without .endm' % name)
                self.macros[name.lower()] = (params, body)
                continue
            yield path, i, line

    # ---- assembly -------------------------------------------------------------------------
    def assemble(self, path):
        p, src = self.read_file(path)
        for self.pass_no in (1, 2):
            self.text = Section('text', 0, 0x1000)
            self.data = Section('data', 0, 0x1000)
            self.overlays = {}
            self.sec = self.text
            self.cond = []
            self.defs = {}
            self.in_delay = False
            self.macro_count = 0
            self.listing = {}
            self.text_labels = {}       # label -> code section name
            self.run(p, src)
            if self.cond:
                self.err('unterminated .if')
        return self.text, self.data

    def run(self, path, src, depth=0):
        for fpath, lineno, line in self.lines(path, src, depth):
            self.loc = (fpath, lineno)
            self.cur_line = line
            self.statement(line, depth)

    def statement(self, line, depth):
        # conditionals
        m = re.match(r'^\.(if|ifdef|ifndef|else|endif)\b\s*(.*)$', line)
        if m:
            d, arg = m.group(1), m.group(2)
            active = all(self.cond)
            if d == 'if':
                self.cond.append(bool(self.eval(arg)) if active else False)
            elif d == 'ifdef':
                self.cond.append(arg.strip() in self.symbols)
            elif d == 'ifndef':
                self.cond.append(arg.strip() not in self.symbols)
            elif d == 'else':
                if not self.cond:
                    self.err('.else without .if')
                self.cond[-1] = not self.cond[-1] and all(self.cond[:-1])
            else:
                if not self.cond:
                    self.err('.endif without .if')
                self.cond.pop()
            return
        if not all(self.cond):
            return
        m = re.match(r'^\.(def|undef)\s+([A-Za-z_][A-Za-z0-9_]*)\s*,?\s*(.*)$', line)
        if m:
            if m.group(1) == 'def':
                self.defs[m.group(2)] = m.group(3).strip()
            else:
                self.defs.pop(m.group(2), None)
            return
        if self.defs:
            line = re.sub(r'(?<![$\.\w])[A-Za-z_]\w*',
                          lambda mm: self.defs.get(mm.group(0), mm.group(0)), line)
        # labels (possibly several on a line)
        while True:
            m = re.match(r'^([A-Za-z_.][A-Za-z0-9_.]*)\s*:\s*(.*)$', line)
            if not m:
                break
            self.define(m.group(1), self.sec.pc, label=True)
            if self.sec is not self.data: self.text_labels[m.group(1)] = self.sec.name
            line = m.group(2)
        if not line:
            return
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_.]*)\s*=\s*(.+)$', line)
        if m:
            v = self.try_eval(m.group(2))
            if v is not None:
                self.define(m.group(1), v)
            elif self.pass_no == 2:
                self.eval(m.group(2))
            return
        parts = line.split(None, 1)
        op = parts[0].lower()
        args = self.split_args(parts[1]) if len(parts) > 1 else []
        if op.startswith('.'):
            self.directive(op, args, parts[1] if len(parts) > 1 else '')
        elif op in self.macros:
            self.expand_macro(op, args, depth)
        else:
            self.instruction(op, args)

    def split_args(self, s):
        out, depth, cur, q = [], 0, '', False
        for c in s:
            if c == '"':
                q = not q
            if not q and c in '([':
                depth += 1
            elif not q and c in ')]':
                depth -= 1
            if c == ',' and depth == 0 and not q:
                out.append(cur.strip())
                cur = ''
            else:
                cur += c
        if cur.strip():
            out.append(cur.strip())
        return out

    def expand_macro(self, name, args, depth):
        params, body = self.macros[name]
        if len(args) > len(params):
            self.err('macro %s: too many arguments' % name)
        self.macro_count += 1
        uniq = str(self.macro_count)
        subst = []
        for l in body:
            for pi, pn in enumerate(params):
                pname, _, default = pn.partition('=')
                val = args[pi] if pi < len(args) else default
                l = re.sub(r'\\' + re.escape(pname) + r'\b', lambda _m: val, l)
            l = l.replace('\\@', uniq)
            subst.append(l)
        saved = self.loc
        self.run('%s(macro %s)' % (saved[0], name), subst, depth + 1)
        self.loc = saved

    def define(self, name, value, label=False):
        if self.pass_no == 1 and label and name in self.symbols:
            self.err('duplicate label %s' % name)
        if self.pass_no == 2 and label and self.symbols.get(name) != value:
            self.err('label %s moved between passes (%s -> %s)' % (name, self.symbols.get(name), value))
        self.symbols[name] = value

    def directive(self, op, args, rest):
        self.in_delay = False
        s = self.sec
        if op in ('.text', '.data'):
            self.sec = self.text if op == '.text' else self.data
            if args:
                self.sec.pc = self.eval(args[0])
        elif op == '.overlay':
            if len(args) != 2:
                self.err('.overlay NAME, base')
            name = args[0]
            if name not in self.overlays:
                base = self.eval(args[1])
                self.overlays[name] = Section(name, base, 0x1000 - base)
            self.sec = self.overlays[name]
        elif op == '.org':
            v = self.eval(args[0])
            if v < s.pc - 0 and v < s.base:
                self.err('.org backwards')
            s.pc = v
        elif op in ('.align', '.balign'):
            a = self.eval(args[0])
            n = (1 << a) if op == '.align' else a
            pad = (-s.pc) % n
            s.emit(bytes(pad))
        elif op in ('.byte', '.half', '.word', '.dword'):
            size = {'.byte': 1, '.half': 2, '.word': 4, '.dword': 8}[op]
            for a in args:
                v = self.try_eval(a) if self.pass_no == 1 else self.eval(a)
                v = 0 if v is None else v
                s.emit((v & ((1 << (8 * size)) - 1)).to_bytes(size, 'big'))
        elif op == '.space':
            n = self.eval(args[0])
            fill = self.eval(args[1]) if len(args) > 1 else 0
            s.emit(bytes([fill & 0xFF]) * n)
        elif op in ('.ascii', '.asciiz'):
            m = re.match(r'^"(.*)"$', rest.strip())
            if not m:
                self.err('bad string')
            b = m.group(1).encode().decode('unicode_escape').encode('latin-1')
            s.emit(b + (b'\0' if op == '.asciiz' else b''))
        elif op in ('.equ', '.set'):
            v = self.try_eval(args[1])
            if v is not None:
                self.define(args[0], v)
            elif self.pass_no == 2:
                self.eval(args[1])
        elif op in ('.global', '.globl'):
            if self.pass_no == 2:
                self.globals += [a for a in args if a not in self.globals]
        elif op == '.error':
            self.err(rest)
        elif op == '.assert':
            if self.pass_no == 2 and not self.eval(args[0]):
                self.err('assertion failed: ' + (args[1] if len(args) > 1 else args[0]))
        elif op in ('.rsp', '.set_noreorder', '.section'):
            pass
        else:
            self.err('unknown directive ' + op)

    # ---- instructions ---------------------------------------------------------------------
    def word(self, w):
        if self.sec is self.data:
            self.err('instruction outside .text')
        self.listing[(self.sec.name, self.sec.pc)] = (self.loc[0], self.loc[1], self.cur_line)
        if self.sec.pc & 3:
            self.err('misaligned instruction')
        w &= 0xFFFFFFFF
        if self.in_delay and self.is_branch(w):
            self.err('branch/jump in a delay slot')
        self.in_delay = self.is_branch(w)
        self.sec.emit(w.to_bytes(4, 'big'))

    @staticmethod
    def is_branch(w):
        op = w >> 26
        if op == 0:
            return (w & 0x3F) in (8, 9)
        return op in (1, 2, 3, 4, 5, 6, 7)

    def imm(self, expr, signed=True, bits=16):
        if self.pass_no == 1:
            v = self.try_eval(expr)
            return 0 if v is None else v & ((1 << bits) - 1)
        v = self.eval(expr)
        lo, hi = (-(1 << (bits - 1)), (1 << bits) - 1) if signed else (0, (1 << bits) - 1)
        if not lo <= v <= hi:
            self.err('immediate out of range: %s = %d' % (expr, v))
        return v & ((1 << bits) - 1)

    def branch_off(self, expr):
        if self.pass_no == 1:
            return 0
        t = self.eval(expr)
        if t & 3:
            self.err('branch target not aligned')
        d = (t - (self.sec.pc + 4)) >> 2
        if not -0x8000 <= d <= 0x7FFF:
            self.err('branch out of range')
        return d & 0xFFFF

    def instruction(self, op, a):
        n = len(a)

        def need(k):
            if n != k:
                self.err('%s expects %d operands, got %d' % (op, k, n))

        # pseudo ops
        if op == 'nop':
            return self.word(0)
        if op == 'move':
            need(2); return self.word((self.gpr(a[1]) << 21) | (self.gpr(a[0]) << 11) | 0x25)
        if op == 'not':
            need(2); return self.word((self.gpr(a[1]) << 21) | (self.gpr(a[0]) << 11) | 0x27)
        if op == 'neg':
            need(2); return self.word((self.gpr(a[1]) << 16) | (self.gpr(a[0]) << 11) | 0x23)
        if op in ('li', 'la'):
            need(2)
            rt = self.gpr(a[0])
            v = self.try_eval(a[1])
            if v is None:
                if self.pass_no == 2:
                    self.eval(a[1])
                v = 0
            v &= 0xFFFFFFFF
            sv = v - (1 << 32) if v & 0x80000000 else v
            if -0x8000 <= sv <= 0x7FFF:
                return self.word((9 << 26) | (rt << 16) | (sv & 0xFFFF))
            if v <= 0xFFFF:
                return self.word((13 << 26) | (rt << 16) | v)
            self.word((15 << 26) | (rt << 16) | (v >> 16))
            if v & 0xFFFF:
                self.word((13 << 26) | (rt << 21) | (rt << 16) | (v & 0xFFFF))
            return
        if op == 'b':
            need(1); return self.word((4 << 26) | self.branch_off(a[0]))
        if op == 'bal':
            need(1); return self.word((1 << 26) | (0x11 << 16) | self.branch_off(a[0]))
        if op in ('beqz', 'bnez'):
            need(2); return self.word(((4 if op == 'beqz' else 5) << 26) | (self.gpr(a[0]) << 21) | self.branch_off(a[1]))

        if op in R_OPS:
            f = R_OPS[op]
            if op in ('sll', 'srl', 'sra'):
                need(3); return self.word((self.gpr(a[1]) << 16) | (self.gpr(a[0]) << 11) | (self.imm(a[2], False, 5) << 6) | f)
            if op in ('sllv', 'srlv', 'srav'):
                need(3); return self.word((self.gpr(a[2]) << 21) | (self.gpr(a[1]) << 16) | (self.gpr(a[0]) << 11) | f)
            if op == 'jr':
                need(1); return self.word((self.gpr(a[0]) << 21) | f)
            if op == 'jalr':
                if n == 1:
                    return self.word((self.gpr(a[0]) << 21) | (31 << 11) | f)
                need(2); return self.word((self.gpr(a[1]) << 21) | (self.gpr(a[0]) << 11) | f)
            if op == 'break':
                code = self.imm(a[0], False, 20) if n else 0
                return self.word((code << 6) | f)
            need(3); return self.word((self.gpr(a[1]) << 21) | (self.gpr(a[2]) << 16) | (self.gpr(a[0]) << 11) | f)
        if op in I_ARITH:
            need(3)
            signed = op in ('addi', 'addiu', 'slti', 'sltiu')
            return self.word((I_ARITH[op] << 26) | (self.gpr(a[1]) << 21) | (self.gpr(a[0]) << 16) | self.imm(a[2], signed))
        if op == 'lui':
            need(2); return self.word((15 << 26) | (self.gpr(a[0]) << 16) | self.imm(a[1], False))
        if op in LOADSTORE:
            need(2)
            off, base = self.mem_operand(a[1])
            return self.word((LOADSTORE[op] << 26) | (base << 21) | (self.gpr(a[0]) << 16) | self.imm(off, True))
        if op in ('j', 'jal'):
            need(1)
            t = 0 if self.pass_no == 1 else self.eval(a[0])
            if t & 3 or not 0 <= t < 0x1000 and self.pass_no == 2:
                self.err('bad jump target %#x (IMEM offset expected)' % t)
            return self.word(((2 if op == 'j' else 3) << 26) | ((t >> 2) & 0x3FFFFFF))
        if op in BR2:
            need(3); return self.word((BR2[op] << 26) | (self.gpr(a[0]) << 21) | (self.gpr(a[1]) << 16) | self.branch_off(a[2]))
        if op in BR1:
            need(2)
            opc, rt = BR1[op]
            return self.word((opc << 26) | (self.gpr(a[0]) << 21) | (rt << 16) | self.branch_off(a[1]))
        if op in ('mfc0', 'mtc0'):
            need(2); return self.word((0x10 << 26) | ((0 if op == 'mfc0' else 4) << 21) | (self.gpr(a[0]) << 16) | (self.c0reg(a[1]) << 11))
        if op in ('mfc2', 'mtc2'):
            need(2)
            vs, e = self.vreg(a[1])
            return self.word((0x12 << 26) | ((0 if op == 'mfc2' else 4) << 21) | (self.gpr(a[0]) << 16) | (vs << 11) | (self.velem_num(e, 15) << 7))
        if op in ('cfc2', 'ctc2'):
            need(2)
            r = a[1].strip().lower().lstrip('$')
            if r not in CTRL:
                self.err('bad control register %s' % a[1])
            return self.word((0x12 << 26) | ((2 if op == 'cfc2' else 6) << 21) | (self.gpr(a[0]) << 16) | (CTRL[r] << 11))
        if op in VOPS:
            if n == 2:   # vd, vt[e] form (vs = vd), as accepted by armips for some ops
                a = [a[0], a[0].split('[')[0], a[1]]
            if len(a) != 3:
                self.err('%s expects 3 operands' % op)
            vd, ed = self.vreg(a[0])
            vs, es = self.vreg(a[1])
            vt, e = self.vreg(a[2])
            if ed or es:
                self.err('element only allowed on last operand')
            ev = self.velem_compute(e)   # vsar: [0]=acc high, [1]=mid, [2]=low (armips convention)
            return self.word((0x12 << 26) | (1 << 25) | (ev << 21) | (vt << 16) | (vs << 11) | (vd << 6) | VOPS[op])
        if op in VSINGLE:
            need(2)
            vd, de = self.vreg(a[0])
            vt, e = self.vreg(a[1])
            return self.word((0x12 << 26) | (1 << 25) | (self.velem_compute(e) << 21) | (vt << 16) |
                             ((8 | self.velem_num(de, 7)) << 11) | (vd << 6) | VSINGLE[op])
        if op == 'vnop':
            return self.word((0x12 << 26) | (1 << 25) | 0x37)
        if len(op) == 3 and op[0] in 'ls' and op[1:] in VLS:
            need(2)
            code, scale = VLS[op[1:]]
            vt, e = self.vreg(a[0])
            off, base = self.mem_operand(a[1])
            ov = 0 if self.pass_no == 1 and self.try_eval(off) is None else self.eval(off)
            if ov % scale:
                self.err('%s offset %d not a multiple of %d' % (op, ov, scale))
            ov //= scale
            if not -64 <= ov <= 63:
                self.err('%s offset out of range' % op)
            opc = 0x32 if op[0] == 'l' else 0x3A
            return self.word((opc << 26) | (base << 21) | (vt << 16) | (code << 11) | (self.velem_num(e, 15) << 7) | (ov & 0x7F))
        self.err('unknown instruction "%s"' % op)


def main():
    ap = argparse.ArgumentParser(description='kestrel64-sdk RSP assembler')
    ap.add_argument('src')
    ap.add_argument('-o', '--out', required=True, help='output prefix (writes .text.bin/.data.bin)')
    ap.add_argument('--header', help='C header with .global symbols')
    ap.add_argument('--prefix', default='UCODE')
    ap.add_argument('--listing', help='write an IMEM listing (address, word, label, source line)')
    ap.add_argument('-I', dest='incdirs', action='append', default=[])
    ap.add_argument('-D', dest='defines', action='append', default=[])
    a = ap.parse_args()
    defines = {}
    for d in a.defines:
        k, _, v = d.partition('=')
        defines[k] = int(v, 0) if v else 1
    asm = Assembler([os.path.dirname(os.path.abspath(a.src))] + a.incdirs, defines)
    try:
        text, data = asm.assemble(a.src)
    except AsmError as e:
        print('k64rsp: error: %s' % e, file=sys.stderr)
        sys.exit(1)
    for sec in [text, data] + list(asm.overlays.values()):
        if len(sec.data) > sec.limit:
            print('k64rsp: error: %s is %d bytes (limit %d)' % (sec.name, len(sec.data), sec.limit), file=sys.stderr)
            sys.exit(1)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tb = bytes(text.data) + bytes((-len(text.data)) & 7)
    db = bytes(data.data) + bytes((-len(data.data)) & 7)
    with open(a.out + '.text.bin', 'wb') as f:
        f.write(tb)
    with open(a.out + '.data.bin', 'wb') as f:
        f.write(db)
    ob, ovl = bytearray(), []
    for o in asm.overlays.values():
        b = bytes(o.data) + bytes((-len(o.data)) & 7)
        ovl.append((o.name, len(ob), len(b)))
        ob += b
    with open(a.out + '.ovl.bin', 'wb') as f:
        f.write(bytes(ob) or bytes(8))           # never empty (it is .incbin'ed)
    if a.header:
        with open(a.header, 'w', encoding='utf-8') as f:
            f.write('// generated by k64rsp.py from %s — do not edit\n#pragma once\n' % os.path.basename(a.src))
            f.write('#define %s_TEXT_SIZE %d\n#define %s_DATA_SIZE %d\n' % (a.prefix, len(tb), a.prefix, len(db)))
            for name, off, size in ovl:
                f.write('#define %s_OVL_%s_OFF %d\n#define %s_OVL_%s_SIZE %d\n'
                        % (a.prefix, name.upper(), off, a.prefix, name.upper(), size))
            for g in asm.globals:
                f.write('#define %s_%s 0x%03X\n' % (a.prefix, g.upper(), asm.symbols[g]))
    if a.listing:
        labels = {}
        for k, v in asm.symbols.items():
            if k in asm.text_labels:
                labels.setdefault((asm.text_labels[k], v), []).append(k)
        with open(a.listing, 'w', encoding='utf-8') as f:
            for sec in [text] + list(asm.overlays.values()):
                if sec is not text:
                    f.write('; overlay %s\n' % sec.name)
                for name, pc in sorted(k for k in asm.listing if k[0] == sec.name):
                    fpath, ln, src = asm.listing[(name, pc)]
                    for lab in labels.get((name, pc), []):
                        f.write('%s:\n' % lab)
                    w = int.from_bytes(sec.data[pc - sec.base:pc - sec.base + 4], 'big')
                    f.write('%03X %08X  %-48s %s:%d\n' % (pc, w, src, os.path.basename(fpath), ln))
    print('k64rsp: %s  imem %d/4096  dmem %d/4096%s' % (os.path.basename(a.src), len(tb), len(db),
          ''.join('  ovl %s %d' % (n, sz) for n, _, sz in ovl)))


if __name__ == '__main__':
    main()
