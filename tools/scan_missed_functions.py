# scan_missed_functions.py <default.xbe> <gen dir>: list code the generated C does not have as a
# function. Two checks: (1) the code right after each switch's inline jump/index tables -- MSVC
# puts them at a function's end, and a disassembler that reads through them misses the next
# function (sub_0010DF10, the plasma weapon that could not be switched away from); (2) code in
# gaps in .text no generated function covers. Each line is a candidate to seed in
# seeds/recomp_extra_seeds.json and check by eye: data that decodes as code, and addresses inside a
# function the line above already names, are not functions. Needs capstone.
import bisect, glob, os, re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

CODE_SECTIONS = ('.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'XMV', 'XPP')
PROLOGUE = re.compile(r'(push (ebp|ebx|esi|edi|ecx|0x)|push -1$|sub esp|mov e[a-z]{2}, e[a-z]{2}$'
                      r'|mov e[a-z]{2}, dword ptr \[esp|mov eax, dword ptr \[)')
JUNK = {'outsb', 'outsd', 'insb', 'insd', 'popal', 'pushal', 'ljmp', 'lcall', 'arpl', 'les', 'lds', 'aaa', 'aas',
        'daa', 'das', 'aad', 'aam', 'into', 'salc', 'hlt', 'in', 'out', 'bound', 'sahf', 'lahf', 'xlatb', 'cwde'}


def load(xbe):
    d = open(xbe, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    count, headers = struct.unpack_from('<I', d, 0x11C)[0], struct.unpack_from('<I', d, 0x120)[0] - base
    sections = []
    for i in range(count):
        _, va, _, raw, size, name = struct.unpack_from('<6I', d, headers + i * 56)
        sections.append((d[name - base:name - base + 12].split(b'\0')[0].decode(), va, raw, size))
    return d, [s for s in sections if s[0] in CODE_SECTIONS]


def main(xbe, gen):
    d, code = load(xbe)

    def section(va):
        for s in code:
            if s[1] <= va < s[1] + s[3]:
                return s

    def byte(va):
        s = section(va)
        return d[s[2] + va - s[1]] if s else None

    def dword(va):
        s = section(va)
        return struct.unpack_from('<I', d, s[2] + va - s[1])[0] if s and va + 4 <= s[1] + s[3] else None

    known = set(int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(gen, 'recomp_dispatch.c')).read()))
    ranges = sorted((int(a, 16), int(b, 16)) for f in glob.glob(os.path.join(gen, 'recomp_0*.c'))
                    for a, b in re.findall(r'Original: 0x([0-9A-F]{8}) - 0x([0-9A-F]{8})', open(f).read()))
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    def looks_like_function(va, limit=96):
        s = section(va)
        ins = list(md.disasm(d[s[2] + va - s[1]:s[2] + va - s[1] + limit], va))
        if len(ins) < 4 or not PROLOGUE.match(ins[0].mnemonic + ' ' + ins[0].op_str):
            return None
        # data decoded as code: instructions a compiler does not emit, or no way out of the block
        body = ins[:12]
        if any(i.mnemonic in JUNK or re.search(r'(es|cs|ss|ds|fs|gs)', i.op_str) and 'fs:[0]' not in i.op_str
               for i in body) or not any(i.mnemonic in ('ret', 'jmp', 'call') or i.mnemonic.startswith('j') for i in ins):
            return None
        return '; '.join(i.mnemonic + ' ' + i.op_str for i in ins[:3])

    found = {}
    # (1) after switch tables: jmp dword ptr [reg*4 + table]
    for name, va, raw, size in code:
        blob = d[raw:raw + size]
        for m in re.finditer(rb'\xFF\x24[\x85\x8D\x95\x9D\xAD\xB5\xBD]', blob):
            at = va + m.start()
            table = struct.unpack_from('<I', blob, m.start() + 3)[0]
            if not section(table) or abs(table - at) > 0x4000:
                continue
            end = table
            while (t := dword(end)) is not None and va <= t < va + size and abs(t - at) <= 0x4000:
                end += 4
            entries = (end - table) // 4
            if not entries:
                continue
            nxt = end
            while (b := byte(nxt)) is not None and b < entries and nxt - end < 1024:   # index table
                nxt += 1
            while byte(nxt) in (0xCC, 0x90):
                nxt += 1
            nxt = (nxt + 15) & ~15
            if nxt not in known and section(nxt) and (text := looks_like_function(nxt)):
                found.setdefault(nxt, 'after the switch table at %08X (jmp at %08X): %s' % (table, at, text))
    # (2) gaps between generated functions, in the game's own code (the library sections carry
    # embedded data -- DSP and codec images -- that decodes as junk)
    for name, va, raw, size in code:
        if name != '.text':
            continue
        pos = va
        for a, b in [r for r in ranges if va <= r[0] < va + size] + [(va + size, va + size)]:
            x = pos
            while x < a:
                while x < a and byte(x) in (0xCC, 0x90, 0x00):
                    x += 1
                y = (x + 15) & ~15
                if y >= a:
                    break
                if y not in known and (text := looks_like_function(y, min(96, a - y + 16))):
                    found.setdefault(y, 'in a %d-byte %s gap: %s' % (a - y, name, text))
                x = y + 16
            pos = max(pos, b)
    for va in sorted(found):
        print('%08X %s' % (va, found[va]))
    print('%d candidates' % len(found))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__ if __doc__ else 'usage: scan_missed_functions.py <default.xbe> <gen dir>')
    main(sys.argv[1], sys.argv[2])
