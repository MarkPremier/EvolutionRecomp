"""Lift one guest x86 function to native C with registers and x87 slots in
locals (tools/x86_lift.py).

The recompiler's generated code keeps every guest register in a thread-local
global and the x87 stack in a thread-local array, so each instruction is a
round trip through memory, and a guest memory store (to the compiler, possibly
anywhere) forces them to be reloaded. For a few hot functions this lifts the
same instructions again with:

  * the integer registers in C locals, written back to the globals around
    every call into another guest function and at the return;
  * the x87 stack resolved at translation time -- its depth is the same at
    each instruction on every path (checked) -- so st(i) is a local double;
  * the generated code's arithmetic: every x87 result rounded by the control
    word's precision (the RECOMP_FP_PC rule), compares setting C0/C2/C3 as
    RECOMP_FCMP_CC, FNSTSW giving TOP and those bits;
  * guest memory read and written through memcpy (no volatile: the function's
    own stack slots can stay in registers between a store and a reload).

  py -3 tools/x86_lift.py <xbe> <start hex> <end hex> <name> <out.c> [callee=fpdelta ...]

A callee that returns a float leaves one more value on the x87 stack; name it
with its delta (cf0c0=1). The lifter stops at any instruction it does not
know, so what it emits is the whole function or nothing.
"""
import struct, sys
import capstone
from capstone import x86

def load_xbe(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = [struct.unpack_from('<5I', d, sh + i * 56)[1:] for i in range(n)]
    def read(va, size):
        for sva, svs, sra, srs in secs:
            if sva <= va < sva + srs:
                return d[sra + va - sva: sra + va - sva + size]
        raise ValueError(hex(va))
    return read

REG32 = {'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'}
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
JCC = {'je': 'zf', 'jne': '!zf', 'jb': 'cf', 'jae': '!cf', 'jbe': '(cf || zf)', 'ja': '(!cf && !zf)',
       'js': 'sf', 'jns': '!sf', 'jl': '(sf != of)', 'jge': '(sf == of)', 'jle': '(zf || sf != of)',
       'jg': '(!zf && sf == of)', 'jp': 'pf', 'jnp': '!pf'}
FP_POP1 = {'fstp', 'fcomp', 'fucomp', 'faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp', 'fistp'}

class Lift:
    def __init__(self, read, start, end, name, deltas, trace=()):
        self.start, self.end, self.name, self.deltas = start, end, name, deltas
        self.trace = set(trace)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        self.ins = [i for i in md.disasm(read(start, end - start), start)]
        assert self.ins and self.ins[-1].address + self.ins[-1].size <= end
        self.at = {i.address: k for k, i in enumerate(self.ins)}
        self.calls = set()

    # ---- control flow and x87 depth -------------------------------------
    def successors(self, k):
        i = self.ins[k]
        m = i.mnemonic
        nxt = [k + 1] if k + 1 < len(self.ins) else []
        if m == 'ret': return []
        if m == 'jmp':
            return [self.target(i)]
        if m in JCC:
            return [self.target(i)] + nxt
        return nxt

    def target(self, i):
        op = i.operands[0]
        if op.type != x86.X86_OP_IMM: raise SystemExit('indirect jump at %x' % i.address)
        t = op.imm & 0xFFFFFFFF
        if t not in self.at: raise SystemExit('jump out of the function at %x -> %x' % (i.address, t))
        return self.at[t]

    def fp_effect(self, i):
        m = i.mnemonic
        if m == 'call':
            t = i.operands[0].imm & 0xFFFFFFFF
            return self.deltas.get(t, 0)
        if m in ('fld', 'fild', 'fld1', 'fldz'): return 1
        if m in ('fcompp', 'fucompp'): return -2
        if m in FP_POP1: return -1
        return 0

    def depths(self):
        depth = {0: 0}
        work = [0]
        while work:
            k = work.pop()
            d = depth[k] + self.fp_effect(self.ins[k])
            if not 0 <= d <= 8: raise SystemExit('x87 depth %d at %x' % (d, self.ins[k].address))
            for s in self.successors(k):
                if s in depth:
                    if depth[s] != d:
                        raise SystemExit('x87 depth differs at %x: %d vs %d' % (self.ins[s].address, depth[s], d))
                else:
                    depth[s] = d
                    work.append(s)
        return depth

    # ---- operands --------------------------------------------------------
    def addr(self, op):
        mem = op.mem
        parts = []
        if mem.base: parts.append(self.i.reg_name(mem.base))
        if mem.index: parts.append('%s*%d' % (self.i.reg_name(mem.index), mem.scale))
        if mem.disp or not parts: parts.append('0x%Xu' % (mem.disp & 0xFFFFFFFF))
        if mem.segment: raise SystemExit('segment override at %x' % self.i.address)
        return '(uint32_t)(' + ' + '.join(parts) + ')'

    def read(self, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_IMM: return '0x%Xu' % (op.imm & ((1 << (size * 8)) - 1))
        if op.type == x86.X86_OP_REG:
            r = self.i.reg_name(op.reg)
            if r in REG32: return r
            if r in REG16: return '(%s & 0xFFFFu)' % REG16[r]
            if r in REG8L: return '(%s & 0xFFu)' % REG8L[r]
            if r in REG8H: return '((%s >> 8) & 0xFFu)' % REG8H[r]
            raise SystemExit('register %s at %x' % (r, self.i.address))
        a = self.addr(op)
        return {1: 'L8(%s)', 2: 'L16(%s)', 4: 'L32(%s)'}[size] % a

    def write(self, op, value, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            r = self.i.reg_name(op.reg)
            if r in REG32: return '%s = %s;' % (r, value)
            if r in REG16: return '%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % (REG16[r], REG16[r], value)
            if r in REG8L: return '%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFFu);' % (REG8L[r], REG8L[r], value)
            if r in REG8H: return '%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);' % (REG8H[r], REG8H[r], value)
            raise SystemExit('register %s at %x' % (r, self.i.address))
        a = self.addr(op)
        return {1: 'S8(%s, %s);', 2: 'S16(%s, %s);', 4: 'S32(%s, %s);'}[size] % (a, value)

    def flags(self, res, size, kind, a=None, b=None):
        bits = size * 8
        mask = '0x%Xu' % ((1 << bits) - 1)
        sign = '0x%Xu' % (1 << (bits - 1))
        out = ['{ uint32_t r_ = (uint32_t)(%s) & %s;' % (res, mask),
               'zf = r_ == 0; sf = (r_ & %s) != 0; pf = PARITY8(r_);' % sign]
        if kind == 'sub':
            out.append('cf = ((%s) & %s) < ((%s) & %s); of = ((((%s) ^ (%s)) & ((%s) ^ r_)) & %s) != 0;'
                       % (a, mask, b, mask, a, b, a, sign))
        elif kind == 'add':
            out.append('cf = r_ < ((%s) & %s); of = (((~((%s) ^ (%s))) & ((%s) ^ r_)) & %s) != 0;'
                       % (a, mask, a, b, a, sign))
        elif kind == 'logic':
            out.append('cf = 0; of = 0;')
        elif kind == 'incdec':
            pass   # CF unchanged; OF rarely read after inc/dec here
        out.append('}')
        return ' '.join(out)

    # ---- x87 ------------------------------------------------------------
    def st(self, d, i): return 'f%d' % (d - 1 - i)

    def fsrc(self, op, d):
        if op.type == x86.X86_OP_REG:
            r = self.i.reg_name(op.reg)
            return self.st(d, int(r[3]))
        a = self.addr(op)
        if op.size == 4: return '(double)LF(%s)' % a
        if op.size == 8: return 'LD(%s)' % a
        raise SystemExit('x87 operand size %d at %x' % (op.size, self.i.address))

    def fp(self, i, d):
        m, ops = i.mnemonic, i.operands
        st = lambda k: self.st(d, k)
        arith = {'fadd': '%s + %s', 'fsub': '%s - %s', 'fmul': '%s * %s', 'fdiv': '%s / %s',
                 'fsubr': '%s - %s', 'fdivr': '%s / %s'}
        if m in ('fld',):
            if ops[0].type == x86.X86_OP_REG:
                return 'f%d = %s;' % (d, st(int(i.reg_name(ops[0].reg)[3])))
            return 'f%d = %s;' % (d, self.fsrc(ops[0], d))
        if m in ('fst', 'fstp'):
            op = ops[0]
            if op.type == x86.X86_OP_REG:
                k = int(i.reg_name(op.reg)[3])
                return '%s = %s;' % (st(k), st(0)) if k else ';'
            a = self.addr(op)
            if op.size == 4: return 'SF(%s, (float)%s);' % (a, st(0))
            if op.size == 8: return 'SD(%s, %s);' % (a, st(0))
        if m == 'fxch':
            # capstone lists fxch's operands as st(0), st(i): the last is the other one
            k = int(i.reg_name(ops[-1].reg)[3]) if ops else 1
            return '{ double t_ = %s; %s = %s; %s = t_; }' % (st(0), st(0), st(k), st(k))
        if m in arith:
            rev = m in ('fsubr', 'fdivr')
            op = arith[m]
            if len(ops) == 2:   # st(i), st(0): into st(i)
                k = int(i.reg_name(ops[0].reg)[3])
                x, y = st(k), st(0)
                expr = op % ((y, x) if rev else (x, y))
                return '%s = PC(%s);' % (x, expr)
            if ops[0].type == x86.X86_OP_REG and i.bytes[0] != 0xD8:
                # DC /r register forms write st(i) and swap sub/subr, div/divr.
                raise SystemExit('x87 register form %02X at %x' % (i.bytes[0], i.address))
            src = self.fsrc(ops[0], d)
            expr = op % ((src, st(0)) if rev else (st(0), src))
            return '%s = PC(%s);' % (st(0), expr)
        if m in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp'):
            k = int(i.reg_name(ops[0].reg)[3]) if ops else 1
            x, y = st(k), st(0)
            expr = {'faddp': (x, '+', y), 'fsubp': (x, '-', y), 'fmulp': (x, '*', y), 'fdivp': (x, '/', y),
                    'fsubrp': (y, '-', x), 'fdivrp': (y, '/', x)}[m]
            return '%s = PC(%s %s %s);' % (x, expr[0], expr[1], expr[2])
        if m in ('fcom', 'fcomp', 'fucom', 'fucomp'):
            src = self.fsrc(ops[0], d) if ops else st(1)
            return 'fcc = FCMP_CC(%s, %s);' % (st(0), src)
        if m in ('fcompp', 'fucompp'):
            return 'fcc = FCMP_CC(%s, %s);' % (st(0), st(1))
        if m == 'fnstsw':
            return 'eax = (eax & 0xFFFF0000u) | (uint32_t)(uint16_t)((((uint32_t)(top0 - %d) & 7u) << 11) | fcc);' % d
        raise SystemExit('x87 %s at %x' % (i.op_str and m + ' ' + i.op_str or m, i.address))

    # ---- calls and exits -------------------------------------------------
    def sync_out(self, d):
        s = 'g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi; g_esp = esp; '
        for k in range(d):
            s += 'g_fp_stack[(top0 - 1 - %d) & 7] = f%d; ' % (k, k)
        s += 'g_fp_top = (top0 - %d) & 7; g_fp_cc = fcc;' % d
        return s

    def sync_in(self, d):
        s = 'eax = g_eax; ecx = g_ecx; edx = g_edx; ebx = g_ebx; esi = g_esi; edi = g_edi; esp = g_esp; '
        s += 'top0 = (g_fp_top + %d) & 7; ' % d
        for k in range(d):
            s += 'f%d = g_fp_stack[(top0 - 1 - %d) & 7]; ' % (k, k)
        s += 'pc24 = !(g_fp_control_word & 0x300u); fcc = g_fp_cc;'
        return s

    # ---- one instruction ------------------------------------------------
    def emit(self, i, d):
        self.i = i
        m, ops = i.mnemonic, i.operands
        if m.startswith('f'): return self.fp(i, d)
        if m == 'nop': return ';'
        if m == 'mov': return self.write(ops[0], self.read(ops[1], ops[0].size))
        if m == 'lea': return self.write(ops[0], self.addr(ops[1]))
        if m == 'push':
            return '{ uint32_t v_ = %s; esp -= 4; S32(esp, v_); }' % self.read(ops[0], 4)
        if m == 'pop':
            return '{ uint32_t v_ = L32(esp); esp += 4; %s }' % self.write(ops[0], 'v_')
        if m in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test'):
            size = ops[0].size
            a, b = self.read(ops[0]), self.read(ops[1], size)
            op = {'add': '+', 'sub': '-', 'cmp': '-', 'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[m]
            kind = 'add' if m == 'add' else 'sub' if m in ('sub', 'cmp') else 'logic'
            res = '(uint32_t)(%s %s %s)' % (a, op, b)
            if m in ('xor',) and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_REG \
                    and ops[0].reg == ops[1].reg:
                return self.write(ops[0], '0') + ' zf = 1; sf = 0; pf = 1; cf = 0; of = 0;'
            out = '{ uint32_t a_ = %s, b_ = %s; %s ' % (a, b, self.flags('a_ %s b_' % op, size, kind, 'a_', 'b_'))
            if m not in ('cmp', 'test'): out += self.write(ops[0], 'a_ %s b_' % op)
            return out + ' }'
        if m in ('inc', 'dec'):
            size = ops[0].size
            op = '+' if m == 'inc' else '-'
            return '{ uint32_t a_ = %s; %s %s }' % (self.read(ops[0]), self.flags('a_ %s 1u' % op, size, 'incdec'),
                                                     self.write(ops[0], 'a_ %s 1u' % op))
        if m in ('shl', 'sar', 'shr'):
            size = ops[0].size
            assert size == 4, i
            cnt = ('(%s & 31u)' % self.read(ops[1], 1)) if ops[1].type != x86.X86_OP_IMM else '%du' % (ops[1].imm & 31)
            a = self.read(ops[0])
            if m == 'shl': v, c = 'a_ << n_', '(a_ >> (32u - n_)) & 1u'
            elif m == 'shr': v, c = 'a_ >> n_', '(a_ >> (n_ - 1u)) & 1u'
            else: v, c = '(uint32_t)((int32_t)a_ >> n_)', '((uint32_t)((int32_t)a_ >> (n_ - 1u))) & 1u'
            return ('{ uint32_t a_ = %s, n_ = %s; if (n_) { uint32_t v_ = %s; cf = (int)(%s); %s %s } }'
                    % (a, cnt, v, c, self.flags('v_', 4, 'shift'), self.write(ops[0], 'v_')))
        if m == 'imul':
            if len(ops) == 2:
                return self.write(ops[0], '(uint32_t)((int32_t)%s * (int32_t)%s)' % (self.read(ops[0]), self.read(ops[1])))
            return self.write(ops[0], '(uint32_t)((int32_t)%s * (int32_t)%s)' % (self.read(ops[1]), self.read(ops[2], 4)))
        if m == 'sete': return self.write(ops[0], 'zf ? 1u : 0u')
        if m == 'setne': return self.write(ops[0], 'zf ? 0u : 1u')
        if m in JCC: return 'if (%s) goto L_%X;' % (JCC[m], self.ins[self.target(i)].address)
        if m == 'jmp': return 'goto L_%X;' % self.ins[self.target(i)].address
        if m == 'call':
            op = ops[0]
            if op.type != x86.X86_OP_IMM: raise SystemExit('indirect call at %x' % i.address)
            t = op.imm & 0xFFFFFFFF
            self.calls.add(t)
            d2 = d + self.deltas.get(t, 0)
            return ('esp -= 4; S32(esp, 0x%Xu); %s sub_%08X(); %s'
                    % (i.address + i.size, self.sync_out(d), t, self.sync_in(d2)))
        if m == 'ret':
            n = ops[0].imm if ops else 0
            if d: raise SystemExit('ret with %d on the x87 stack at %x' % (d, i.address))
            return 'esp += %du; %s return;' % (4 + n, self.sync_out(0))
        if m == 'rep stosd' or (m == 'rep' and 'stosd' in i.op_str) or m == 'stosd':
            return '{ int32_t s_ = g_df ? -4 : 4; for (; ecx; ecx--) { S32N(edi, eax); edi += (uint32_t)s_; } }'
        if m == 'rep stosb' or (m == 'rep' and 'stosb' in i.op_str) or m == 'stosb':
            return '{ int32_t s_ = g_df ? -1 : 1; for (; ecx; ecx--) { S8N(edi, eax & 0xFFu); edi += (uint32_t)s_; } }'
        raise SystemExit('unknown %s %s at %x' % (m, i.op_str, i.address))

    def run(self):
        depth = self.depths()
        targets = set()
        for k, i in enumerate(self.ins):
            if i.mnemonic in JCC or i.mnemonic == 'jmp':
                targets.add(self.ins[self.target(i)].address)
        maxd = max(depth.values()) + max([0] + list(self.deltas.values()))
        body = []
        for k, i in enumerate(self.ins):
            if k not in depth: continue   # unreachable (padding)
            if i.address in targets: body.append('L_%X: ;' % i.address)
            if i.address in self.trace:
                d = depth[k]
                body.append('    lift_trace(0x%Xu, eax, ecx, edx, ebx, esi, edi, esp, %s, (int)((top0 - %d) & 7));'
                            % (i.address, 'f%d' % (d - 1) if d else 'g_fp_stack[top0 & 7]', d))
            body.append('    %s /* %08x %s %s [x87 %d] */' % (self.emit(i, depth[k]), i.address, i.mnemonic, i.op_str, depth[k]))
        head = ['/* %s: lifted from guest 0x%08X-0x%08X by tools/x86_lift.py -- regenerate, do not edit. */'
                % (self.name, self.start, self.end)]
        head.append('#include "native/native_lift.h"')
        if self.trace:
            head.append('void lift_trace(uint32_t at, uint32_t a, uint32_t c, uint32_t d, uint32_t b, uint32_t s, '
                        'uint32_t i, uint32_t sp, double top, int fp_top);')
            head.append('void lift_store(uint32_t address, uint32_t value);')
            head.append('static void S32T(uint32_t a, uint32_t v) { S32(a, v); lift_store(a, v); }')
            head.append('static void S16T(uint32_t a, uint32_t v) { S16(a, v); lift_store(a, v & 0xFFFFu); }')
            head.append('static void S8T(uint32_t a, uint32_t v) { S8(a, v); lift_store(a, v & 0xFFu); }')
            head.append('static void SFT(uint32_t a, float v) { SF(a, v); lift_store(a, L32(a)); }')
            head.append('#define S32 S32T')
            head.append('#define S16 S16T')
            head.append('#define S8 S8T')
            head.append('#define SF SFT')
        for t in sorted(self.calls): head.append('extern void sub_%08X(void);' % t)
        head.append('void %s(void)\n{' % self.name)
        head.append('    uint32_t eax, ecx, edx, ebx, esi, edi, esp, ebp = g_seh_ebp, top0;')
        head.append('    int zf = 0, sf = 0, cf = 0, of = 0, pf = 0, pc24;')
        head.append('    uint16_t fcc;')
        head.append('    double ' + ', '.join('f%d = 0' % k for k in range(max(1, min(8, maxd + 1)))) + ';')
        head.append('    ' + self.sync_in(0))
        head.append('    (void)ebp; (void)of; (void)sf; (void)pf; (void)cf; (void)zf;')
        return '\n'.join(head + body + ['}', ''])

def main():
    xbe, start, end, name, out = sys.argv[1:6]
    deltas, trace = {}, []
    for a in sys.argv[6:]:
        if a.startswith('--trace='):
            trace = [int(x, 16) for x in open(a[8:]).read().split()]
            continue
        k, v = a.split('=')
        deltas[int(k, 16)] = int(v)
    lift = Lift(load_xbe(xbe), int(start, 16), int(end, 16), name, deltas, trace)
    text = lift.run()
    open(out, 'w', newline='\n').write(text)
    print('%s: %d instructions, calls %s' % (name, len(lift.ins), ' '.join('%X' % c for c in sorted(lift.calls))))

if __name__ == '__main__':
    main()
