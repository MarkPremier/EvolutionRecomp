"""Find where a lifted function (tools/x86_lift.py) parts from the generated
code (tools/lift_trace.py).

Writes two traced copies for RECOMP_LIFT_TRACE builds: the generated function
with a lift_trace() call at each of its labels, and the lifted one with a call
at the same addresses. Both record the registers and the top of the x87 stack;
the verify run compares the two records and names the first place they differ.

  py -3 tools/lift_trace.py <gen .c> <guest hex> <xbe> <end hex> <out dir> [callee=fpdelta ...]
"""
import os, re, subprocess, sys

gen_c, start, xbe, end, outdir = sys.argv[1:6]
deltas = sys.argv[6:]
name = 'sub_%08X' % int(start, 16)
text = open(gen_c, encoding='utf-8').read()
m = re.search(r'\nvoid %s(_gen)?\(void\)\r?\n\{' % name, text)
body_start = m.start() + 1
depth, k = 0, text.index('{', body_start)
for k in range(k, len(text)):
    if text[k] == '{': depth += 1
    elif text[k] == '}':
        depth -= 1
        if depth == 0: break
func = text[body_start:k + 1]
func = re.sub(r'void %s(_gen)?\(void\)' % name, 'void %s_tracegen(void)' % name, func, count=1)
labels = [int(a, 16) for a in re.findall(r'loc_([0-9A-F]{8}): ;', func)]
func = re.sub(r'(loc_([0-9A-F]{8}): ;)',
              lambda mm: mm.group(1) + ' lift_trace(0x%su, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, '
                         'g_fp_stack[g_fp_top & 7], g_fp_top);' % mm.group(2), func)
def hook(line):
    m = re.match(r'^(\s*)MEM(32|F|16|8)\((.*)\) = (.*?);(.*)$', line)
    if m and 'for (' not in line:
        indent, kind, address, value, rest = m.groups()
        read = {'32': 'MEM32(a_)', 'F': 'MEM32(a_)', '16': 'MEM16(a_)', '8': 'MEM8(a_)'}[kind]
        return '%s{ uint32_t a_ = (uint32_t)(%s); MEM%s(a_) = %s; lift_store(a_, %s); }%s' % (
            indent, address, kind, value, read, rest)
    return re.sub(r'PUSH32\(esp, ([^;]*)\);', r'PUSH32(esp, \1); lift_store(esp, MEM32(esp));', line)
func = '\n'.join(hook(l) for l in func.split('\n'))
os.makedirs(outdir, exist_ok=True)
with open(os.path.join(outdir, '%s_tracegen.c' % name), 'w', newline='\n') as f:
    f.write('/* %s with a lift_trace() at each label: tools/lift_trace.py, regenerate. */\n' % name)
    f.write('#define RECOMP_GENERATED_CODE\n#include "recomp/gen/recomp_funcs.h"\n#include <math.h>\n')
    f.write('void lift_trace(uint32_t at, uint32_t a, uint32_t c, uint32_t d, uint32_t b, uint32_t s, uint32_t i, '
            'uint32_t sp, double top, int fp_top);\n')
    f.write('void lift_store(uint32_t address, uint32_t value);\n')
    f.write(func + '\n')
points = os.path.join(outdir, '%s_trace_points.txt' % name)
open(points, 'w').write('\n'.join('%X' % a for a in labels))
here = os.path.dirname(os.path.abspath(__file__))
subprocess.check_call([sys.executable, os.path.join(here, 'x86_lift.py'), xbe, start, end, name + '_tracelift',
                       os.path.join(outdir, '%s_tracelift.c' % name)] + deltas + ['--trace=' + points])
print('%d trace points' % len(labels))
