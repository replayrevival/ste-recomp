#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the recompiled C code from the user's own default.xbe.

usage: python ste/scripts/generate.py --xbe <path to default.xbe> --out <empty dir>

Steps (all inputs are in ste/gen/, the XBE is read only):
  1. ste/gen/functions.tsv -> functions.json (function boundaries found by analysis; addresses only)
  2. python -m tools.recomp (xboxrecomp/tools/recomp, the generator with this project's patches)
       --exclude-manual ste/gen/exclude_manual.c   functions that recomp_manual.c replaces by hand
       --hooks          ste/gen/ste_hooks.toml     instruction hooks (docs/formats/RECOMP_HOOKS.md)
  3. unresolved-call stubs are made loud (they log "[STUB HIT]" and exit with code 4)
  4. a missing "_cf" declaration the lifter leaves in two functions is added
Output: <out>/generated/recomp_*.c, recomp_dispatch.c, recomp_funcs.h, recomp_types.h, recomp_stubs_unresolved.c
The generated code is derived from the game: never commit or publish it."""
import argparse, glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN_IN = os.path.join(ROOT, 'ste', 'gen')

def make_loud_stubs(gen):
    p = os.path.join(gen, 'recomp_stubs_unresolved.c')
    s = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
    stub = re.compile(r'(?m)^void (sub_([0-9A-F]{8}))\(void\) \{ g_esp \+= (\d+); /\* (0x[0-9A-F]+: [^*]*)\*/ \}')
    found = stub.findall(s)
    if not found:
        sys.exit('no unresolved-call stubs found in ' + p)
    nl = '\r\n' if '\r\n' in s else '\n'
    s = stub.sub(lambda m: 'void %s(void) { recomp_stub_hit(0x%sU, "%s"); }' % (m.group(1), m.group(2), m.group(4).strip()), s)
    helper = ('#include <stdio.h>%s#include <stdlib.h>%s'
              'static void recomp_stub_hit(uint32_t va, const char *what) {%s'
              '    fprintf(stderr, "[STUB HIT] 0x%%08X (%%s) ret=0x%%08X esp=0x%%08X\\n", va, what, MEM32(g_esp), g_esp);%s'
              '    fflush(stderr); _exit(4);%s}%s') % ((nl,) * 6)
    s = s.replace('#include "recomp_funcs.h"', '#include "recomp_funcs.h"' + nl + helper, 1)
    open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
    print('%d unresolved-call stubs made loud' % len(found))

def fix_cf(gen):
    fixed = []
    for p in sorted(glob.glob(os.path.join(gen, 'recomp_00*.c'))):
        s = open(p, encoding='utf-8', errors='surrogateescape', newline='').read(); changed = False
        for m in list(re.finditer(r'(?m)^void (sub_[0-9A-F]{8})\(void\)\r?\n\{', s))[::-1]:
            end = s.find('\nvoid sub_', m.end()); end = len(s) if end < 0 else end
            body = s[m.end():end]
            if re.search(r'\b_cf\b', body) and not re.search(r'\bint _cf\b|\buint32_t _cf\b', body):
                k = body.find('    int _flags = 0; /* fallback flag var */')
                if 0 <= k < 600:
                    nl = '\r\n' if '\r\n' in body[:200] else '\n'
                    pos = m.end() + k
                    s = s[:pos] + '    int _cf = 0; /* the lifter uses _cf (lahf after inc) without declaring it */' + nl + s[pos:]
                    fixed.append(m.group(1)); changed = True
        if changed:
            open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
    print('_cf declaration added to', len(fixed), 'functions:', ' '.join(fixed))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xbe', required=True, help="path to the user's own default.xbe")
    ap.add_argument('--out', required=True, help='output directory (must not exist or be empty)')
    ap.add_argument('--python', default=sys.executable)
    a = ap.parse_args()
    xbe, out = os.path.abspath(a.xbe), os.path.abspath(a.out)
    if not os.path.isfile(xbe):
        sys.exit('default.xbe not found: ' + xbe)
    if os.path.isdir(out) and os.listdir(out):
        sys.exit('refusing to write into a non-empty directory: ' + out)
    work, gen = os.path.join(out, 'work'), os.path.join(out, 'generated')
    os.makedirs(work, exist_ok=True); os.makedirs(gen, exist_ok=True)
    fj = os.path.join(work, 'functions.json')
    subprocess.check_call([a.python, os.path.join(HERE, 'make_functions_json.py'), os.path.join(GEN_IN, 'functions.tsv'), fj])
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    cmd = [a.python, '-m', 'tools.recomp', xbe, '--functions', fj,
           '--exclude-manual', os.path.join(GEN_IN, 'exclude_manual.c'), '--hooks', os.path.join(GEN_IN, 'ste_hooks.toml'),
           '--all', '--split', '1000', '--gen-dir', gen, '-o', os.path.join(work, 'out'), '--game-name', 'Godzilla STE']
    print('generator:', ' '.join(cmd))
    r = subprocess.run(cmd, cwd=os.path.join(ROOT, 'xboxrecomp'), env=env)
    if r.returncode:
        sys.exit('generator failed with exit code %d' % r.returncode)
    make_loud_stubs(gen)
    fix_cf(gen)
    print('generated code in', gen)

if __name__ == '__main__':
    main()