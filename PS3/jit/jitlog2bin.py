#!/usr/bin/env python3
"""Turns the "JIT block" listings of a PS3 launch.log into MIPS code for PPC_JITTEST.

    jitlog2bin.py launch.log outdir   -> outdir/<addr>.bin + outdir/jittest.txt
"""
import os, re, subprocess, sys

GPR = set('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split())
UNSUPPORTED = re.compile(r'^(syscall|lv\.|sv\.|lvl|lvr|svl|svr|v[a-z]|mfv|mtv|mfvc|mtvc|max|min|break|halt|bvf|bvt|bvfl|bvtl|ulv|usv|ll |sc )')


def convert(line, syms):
    line = line.strip()
    if UNSUPPORTED.match(line):
        return None
    line = re.sub(r'->([a-z][a-z0-9]*)\b', r'\1', line)
    def target(m):
        a = m.group(1).lower()
        syms.add(a)
        return 'T' + a
    line = re.sub(r'->\$([0-9a-fA-F]{8})', target, line)
    parts = line.split(None, 1)
    if len(parts) == 1:
        return parts[0]
    op, args = parts
    def reg(m):
        t = m.group(0)
        if t in GPR or re.fullmatch(r'f\d+', t):
            return '$' + t
        return t
    args = re.sub(r'\b[a-z][a-z0-9]*\b', reg, args)
    return op + ' ' + args


def main():
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    blocks = []
    cur = None
    for line in open(log, errors='replace'):
        m = re.match(r'JIT block ([0-9a-f]{8}):', line)
        if m:
            cur = (m.group(1), [])
            blocks.append(cur)
            continue
        m = re.match(r'\s+mips (.*)', line)
        if m and cur:
            cur[1].append(m.group(1))
    seen = set()
    test = []
    for addr, lines in blocks:
        if addr in seen or not lines:
            continue
        seen.add(addr)
        syms = set()
        asm = ['.set noreorder', '.set noat', '.set nomacro', '.text']
        ok = True
        for l in lines:
            c = convert(l, syms)
            if c is None:
                ok = False
                break
            asm.append(c)
        if not ok:
            print('skip %s (unsupported: %s)' % (addr, l))
            continue
        s = os.path.join(out, addr + '.s')
        o = os.path.join(out, addr + '.o')
        e = os.path.join(out, addr + '.elf')
        b = os.path.join(out, addr + '.bin')
        open(s, 'w').write('\n'.join(asm) + '\n')
        r = subprocess.run(['llvm-mc', '-triple=mipsel', '-mcpu=mips32r2', '-filetype=obj', s, '-o', o], capture_output=True, text=True)
        if r.returncode:
            print('asm %s failed: %s' % (addr, r.stderr.strip().splitlines()[:3]))
            continue
        defs = []
        for t in syms:
            defs += ['--defsym', 'T%s=0x%s' % (t, t)]
        r = subprocess.run(['ld.lld', '-Ttext=0x' + addr, '-e', '0x' + addr, '--no-relax'] + defs + [o, '-o', e], capture_output=True, text=True)
        if r.returncode:
            print('link %s failed: %s' % (addr, r.stderr.strip().splitlines()[:3]))
            continue
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', e, b], check=True)
        test.append('load %s %s' % (addr, os.path.abspath(b)))
        test.append('block %s' % addr)
    open(os.path.join(out, 'jittest.txt'), 'w').write('\n'.join(test) + '\n')
    print('%d blocks' % (len(test) // 2))


main()
