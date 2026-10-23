#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.
"""Checks the hardware T&L vertex program (make-shaders.py, tl_program) without a PS3: runs its
assembly with a small interpreter on vertices dumped by a test build (PPSSPP_TL_DUMP, see
GPU/Software/TransformUnit.cpp) and compares with the batched CPU path's results.

    PPSSPP_TL_DUMP=/tmp/tl.txt PPSSPPHeadless ... ; python3 PS3/rsx/check-tl.py /tmp/tl.txt
"""
import importlib.util, math, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ms', os.path.join(here, 'make-shaders.py'))
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

def parse(src):
    prog = []
    for line in src.split('\n'):
        line = line.strip()
        if not line or line.startswith('!!') or line.startswith('#') or line == 'END':
            continue
        line = line.rstrip(';')
        op, rest = line.split(None, 1)
        args = [a.strip() for a in re.split(r',(?![^\[]*\])', rest)]
        prog.append((op, args))
    return prog

COMP = 'xyzw'

def read(arg, regs, v, c):
    neg = arg.startswith('-')
    if neg:
        arg = arg[1:]
    ab = arg.startswith('|')
    if ab:
        arg = arg.strip('|')
    m = re.match(r'(R\d+|c\[\d+\]|v\[\w+\])(?:\.(\w+))?$', arg)
    name, sw = m.group(1), m.group(2) or 'xyzw'
    if name[0] == 'R':
        val = regs.setdefault(name, [0.0] * 4)
    elif name[0] == 'c':
        val = c[int(name[2:-1])]
    else:
        val = v[name[2:-1]]
    if len(sw) == 1:
        sw = sw * 4
    out = [val[COMP.index(ch)] for ch in sw]
    if ab:
        out = [abs(x) for x in out]
    if neg:
        out = [-x for x in out]
    return out

def write(dst, val, regs, outs):
    m = re.match(r'(R\d+|o\[\w+\])(?:\.(\w+))?$', dst)
    name, mask = m.group(1), m.group(2) or 'xyzw'
    target = regs.setdefault(name, [0.0] * 4) if name[0] == 'R' else outs.setdefault(name[2:-1], [0.0, 0.0, 0.0, 1.0])
    for ch in mask:
        target[COMP.index(ch)] = val[COMP.index(ch)]

def safe(f):
    try:
        return f()
    except (OverflowError, ZeroDivisionError, ValueError):
        return float('inf')

def run(prog, v, c):
    regs, outs = {}, {}
    for op, a in prog:
        s = [read(x, regs, v, c) for x in a[1:]]
        if op == 'MOV': r = s[0]
        elif op == 'ADD': r = [p + q for p, q in zip(s[0], s[1])]
        elif op == 'MUL': r = [p * q for p, q in zip(s[0], s[1])]
        elif op == 'MAD': r = [p * q + t for p, q, t in zip(s[0], s[1], s[2])]
        elif op == 'MAX': r = [max(p, q) for p, q in zip(s[0], s[1])]
        elif op == 'MIN': r = [min(p, q) for p, q in zip(s[0], s[1])]
        elif op == 'SGE': r = [1.0 if p >= q else 0.0 for p, q in zip(s[0], s[1])]
        elif op == 'DP3': r = [sum(s[0][i] * s[1][i] for i in range(3))] * 4
        elif op == 'DP4': r = [sum(s[0][i] * s[1][i] for i in range(4))] * 4
        elif op == 'RSQ': r = [safe(lambda: 1.0 / math.sqrt(abs(s[0][0])))] * 4
        elif op == 'RCP': r = [safe(lambda: 1.0 / s[0][0])] * 4
        elif op == 'LG2': r = [safe(lambda: math.log2(abs(s[0][0])))] * 4
        elif op == 'EX2': r = [safe(lambda: 2.0 ** s[0][0])] * 4
        elif op == 'LIT':
            # As the hardware computes it: y^w = 2^(w * log2(y)), so 0^0 is NaN (not 1).
            x, y, w = max(s[0][0], 0.0), max(s[0][1], 0.0), max(-128.0, min(128.0, s[0][3]))
            lg = math.log2(y) if y > 0 else -math.inf
            prod = w * lg if not (w == 0 and math.isinf(lg)) else math.nan
            z = (safe(lambda: 2.0 ** prod) if not math.isnan(prod) else math.nan) if s[0][0] > 0 else 0.0
            r = [1.0, x, z, 1.0]
        else:
            raise SystemExit('unknown op ' + op)
        write(a[0], r, regs, outs)
    return outs

def main():
    prog = parse(ms.tl_program())
    c = None
    n = bad = 0
    worst = {}
    for line in open(sys.argv[1]):
        f = line.split()
        if not f or (f[0] == 'D' and len(f) != 1 + 52 * 4) or (f[0] != 'D' and len(f) < 22):
            continue  # The last line can be cut off (the dump isn't flushed at exit).
        if f[0] == 'D':
            vals = [float(x) for x in f[1:]]
            c = [[0.0] * 4 for _ in range(3)] + [vals[i * 4:i * 4 + 4] for i in range(52)]
            continue
        x, y, z, nx, ny, nz = map(float, f[1:7])
        col = [int(t) for t in f[7:11]]
        u, vv = float(f[11]), float(f[12])
        ex = list(map(float, f[13:17]))
        c0, c1 = int(f[17], 16), int(f[18], 16)
        es, et, efog = map(float, f[19:22])
        eq = float(f[22]) if len(f) > 22 else 1.0
        v = {'OPOS': [x, y, z, 1.0], 'NRML': [nx, ny, nz, 1.0], 'COL0': [t / 255.0 for t in col], 'TEX0': [u, vv, 0.0, 1.0]}
        o = run(prog, v, c)
        n += 1
        problems = []
        hp = o['HPOS']
        w = ex[3]
        if c[20][3] != 0.0:
            ex[2] = max(-ex[3], min(ex[3], ex[2]))  # Depth clamped (GE depth clip off).

        for i in range(4):
            if abs(hp[i] - ex[i]) > 1e-3 * max(1.0, abs(w), abs(ex[i])):
                problems.append('pos%d %g vs %g' % (i, hp[i], ex[i]))
        for name, got, want in (('col0', o['COL0'], c0), ('col1', o['COL1'], c1)):
            for i in range(4 if name == 'col0' else 3):
                if math.isnan(got[i]):
                    problems.append('%s[%d] NaN' % (name, i))
                    continue
                g = round(min(1.0, max(0.0, got[i])) * 255)
                e = (want >> (8 * i)) & 0xFF
                worst[name] = max(worst.get(name, 0), abs(g - e))
                if abs(g - e) > 4:
                    problems.append('%s[%d] %d vs %d' % (name, i, g, e))
        # Compared after the divide by q (TXP), as the fragment program samples.
        qw = o['TEX0'][3] if o['TEX0'][3] != 0 else 1e-30
        gs, gt = o['TEX0'][0] / qw, o['TEX0'][1] / qw
        es, et = es / (eq if eq != 0 else 1e-30), et / (eq if eq != 0 else 1e-30)
        if abs(gs - es) > 1e-3 * max(1.0, abs(es)) or abs(gt - et) > 1e-3 * max(1.0, abs(et)):
            problems.append('uv %g,%g vs %g,%g' % (gs, gt, es, et))
        if abs(o['TEX1'][0] - efog) > 0.01:
            problems.append('fog %g vs %g' % (o['TEX1'][0], efog))
        if problems:
            bad += 1
            if bad <= 15 or os.environ.get("ALL"):
                print('vertex %d: %s' % (n, '; '.join(problems)))
    print('%d vertices, %d with differences; largest color difference %s' % (n, bad, worst))

if __name__ == '__main__':
    main()
