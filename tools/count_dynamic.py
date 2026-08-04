#!/usr/bin/env python3
"""Dynamic instruction counts for the VIS kernels from GCC -S output.

Walk each kernel function's instruction list; every backward branch is
a loop. The fixed-trip inner loops are fully unrolled (the kernels are
compiled with aggressive complete peeling), leaving only the c-loop
over super-blocks. The largest loop is assumed to be that c-loop, with
trip counts given in LOOP_TRIPS (super-blocks per row at ncols=1024).

Usage: count_dynamic.py backend_vis.s
"""
import re
import sys

LOOP_TRIPS = {
    # kernel: c-loop iterations per row (ncols = 1024)
    'vis_dot_q4_K': 4,    # 1024 / 256
    'vis_dot_q5_K': 4,
    'vis_dot_q6_K': 4,
    'vis_dot_q8_0': 32,   # 1024 / 32
    'vis_dot_q4_0': 32,
}

def parse(path):
    src = open(path).read()
    funcs = {}
    cur = None
    for line in src.splitlines():
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):', line)
        if m:
            if cur:
                funcs[cur['name']] = cur
            cur = {'name': m.group(1), 'lines': [], 'labels': {}}
        elif cur is not None:
            if re.match(r'\s*\.(size|cfi_endproc)', line):
                funcs[cur['name']] = cur
                cur = None
            else:
                lm = re.match(r'\s*\.L?([0-9A-Za-z_]+):', line)
                if lm:
                    cur['labels'][lm.group(1)] = len(cur['lines'])
                    cur['lines'].append(('label', lm.group(1)))
                else:
                    t = line.split('#')[0].strip()
                    if t and not t.startswith('.'):
                        cur['lines'].append(('insn', t))
    if cur:
        funcs[cur['name']] = cur
    return funcs

def insns_between(lines, a, b):
    return sum(1 for j in range(a, b + 1) if lines[j][0] == 'insn')

def analyze(fn):
    lines = fn['lines']
    n = len(lines)
    total = sum(1 for k, t in lines if k == 'insn')
    backs = []
    for i, (k, t) in enumerate(lines):
        if k != 'insn':
            continue
        m = re.search(r'\.L([0-9A-Za-z_]+)', t)
        if m and m.group(1) in fn['labels']:
            tgt = fn['labels'][m.group(1)]
            if tgt < i:
                backs.append((insns_between(lines, tgt, i), tgt, i))
    if not backs:
        return total, total, 0
    backs.sort()
    c_span, c_a, c_b = backs[-1]
    # inner loops inside the c-loop body
    extra = 0
    for span, a, b in backs[:-1]:
        if c_a <= a and b <= c_b:
            extra += span  # each inner loop adds one body per c iteration
    return total, c_span, extra

def main():
    funcs = parse(sys.argv[1])
    print(f"{'kernel':16s} {'static':>6s} {'c-body':>6s} {'dyn/row':>8s} {'ops/MAC':>7s}")
    for name, trips in LOOP_TRIPS.items():
        fn = funcs.get(name)
        if not fn:
            print(f"{name:16s} MISSING")
            continue
        total, cbody, inner = analyze(fn)
        dyn = total + (trips - 1) * cbody
        macs = trips * (256 if name in ('vis_dot_q4_K', 'vis_dot_q5_K', 'vis_dot_q6_K') else 32)
        print(f"{name:16s} {total:6d} {cbody:6d} {dyn:8d} {dyn / macs:7.2f}")

if __name__ == '__main__':
    main()
