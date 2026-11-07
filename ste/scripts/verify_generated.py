# SPDX-License-Identifier: GPL-2.0-or-later
# Regression checks on freshly generated code (known lifter pitfalls). Prints PASS/FAIL lines, exit code 1 on any failure.
#!/usr/bin/env python
from __future__ import annotations
from pathlib import Path
import argparse, sys

ap = argparse.ArgumentParser()
ap.add_argument('--root', required=True, help='directory that contains generated/ (the --out of generate.py)')
args = ap.parse_args()
ROOT = Path(args.root)
errors: list[str] = []

memcpy = ROOT / 'generated' / 'recomp_0010.c'
if not memcpy.exists():
    errors.append(f'missing memcpy TU: {memcpy}')
else:
    txt = memcpy.read_text(encoding='utf-8', errors='replace')
    if 'loc_001A4054' not in txt:
        errors.append('memcpy TU lost loc_001A4054 internal block')
    if 'MEM32(ecx * 4 + 0x1A4048)' not in txt:
        errors.append('memcpy TU missing jump-table root')
    if 'sub_001A4054' in txt:
        errors.append('memcpy TU incorrectly exposed sub_001A4054 as a standalone function')

# Shared-epilogue functions must be DEFINED in the tree under test (--root), not in some other
# tree's lift_*.c files. sub_0024D593 is `ret 0x10`: its body must pop the 16 argument bytes
# (esp += 20); a `g_esp += 4` stub leaves them on the stack (see STUB_RECOVERY_REPORT.md section 6).
import re
gen_dir = ROOT / 'generated'
gen_files = sorted(gen_dir.glob('recomp_*.c')) + sorted(gen_dir.glob('lift_*.c'))
if not gen_files:
    errors.append(f'no generated C files under {gen_dir}')
for name in ['sub_0024CFDC', 'sub_0024D321', 'sub_0024D593']:
    body = None
    for f in gen_files:
        t = f.read_text(encoding='utf-8', errors='replace')
        m = re.search(r'(?m)^void %s\(void\)\s*(\{.*?\n\})' % name, t, re.S)
        if m:
            body = m.group(1)
            break
        if re.search(r'(?m)^void %s\(void\) \{ g_esp \+= \d+;' % name, t):
            body = 'STUB'
            break
    if body is None:
        errors.append(f'shared epilogue function {name} not defined in {gen_dir}')
    elif body == 'STUB':
        errors.append(f'shared epilogue function {name} is a ret-only stub in {gen_dir}')
    elif name == 'sub_0024D593' and 'esp += 20' not in body:
        errors.append('sub_0024D593 does not pop its 16 argument bytes (expected `esp += 20`)')

# cinit functions: each must be defined EXACTLY ONCE in the tree under test (generated or hand-written)
# and be present in recomp_dispatch.c. (The old check demanded a specific hand TU and its trace strings.)
cinit_funcs = ['sub_00252C8D', 'sub_00252C98', 'sub_00255B5A', 'sub_00255B65', 'sub_002E0917', 'sub_002E0947']
all_c = {f: f.read_text(encoding='utf-8', errors='replace') for f in sorted(gen_dir.glob('*.c'))}
for name in cinit_funcs:
    n = sum(len(re.findall(r'(?m)^void %s\(void\)\s*(?:\{|$)' % name, t)) for t in all_c.values())
    if n != 1:
        errors.append(f'{name} defined {n} times in {gen_dir} (expected exactly 1)')
    va = name[4:]
    disp = gen_dir / 'recomp_dispatch.c'
    if not disp.exists() or not re.search(r'\{ 0x%su, \(recomp_func_t\)%s \}' % (va, name), disp.read_text(encoding='utf-8', errors='replace')):
        errors.append(f'{name} missing from recomp_dispatch.c')
if not any('Original: 0x002E0947 - 0x002E0977 (48 bytes, 17 insns)' in t for t in all_c.values()):
    errors.append('cinit function sub_002E0947 has wrong boundary (expected 0x002E0947 - 0x002E0977)')
# STE: a fall-through successor that begins by testing flags must not be reached by a plain tail call
# (`sub_X(); return; /* fallthrough 0xADDR */`): the flags the predecessor computed are local variables and are lost.
# The generator (fallthrough_inline) keeps such successors inline in the predecessor.
_flag_first = set()
for _t in all_c.values():
    for _m in re.finditer(r'(?m)^void (sub_[0-9A-F]{8})\(void\)\r?\n\{.*?\nloc_[0-9A-F]{8}: ;\r?\n\s+(.*?)\r?\n', _t, re.S):
        if _m.group(2).startswith('if (_flags'):
            _flag_first.add(_m.group(1))
_bad = []
for _t in all_c.values():
    for _m in re.finditer(r'(sub_[0-9A-F]{8})\(\); return; /\* fallthrough 0x[0-9A-F]{8} \*/', _t):
        if _m.group(1) in _flag_first:
            _bad.append(_m.group(1))
if _bad:
    errors.append('FALLTHROUGH_FLAGS_CHECK=FAIL: %d tail call(s) into flag-testing fall-through successors: %s' % (len(_bad), ' '.join(sorted(set(_bad))[:10])))
# STE: the _icall_esp save point must never sit in front of a prologue register-save push (see generator patch
# prologue_saves): a skipped indirect call would rewind g_esp over the function's own saved register.
_bad_pro = []
for _t in all_c.values():
    for _m in re.finditer(r'(?m)^void (sub_[0-9A-F]{8})\(void\)\r?\n\{(.*?)\n\}', _t, re.S):
        _b = _m.group(2)
        _e = re.search(r'(?m)^\s*loc_[0-9A-Fa-f]{8}: ;\r?\n', _b)
        if not _e: continue
        _mm = re.match(r'(?:\s*(?:PUSH32\(esp, (?:ebx|esi|edi|ebp)\);|ebp = esp;))*\s*\{ uint32_t _icall_esp = g_esp;\s*\r?\n\s*PUSH32\(esp, (ebx|esi|edi|ebp)\);', _b[_e.end():])
        if _mm and ('POP32(esp, %s)' % _mm.group(1)) in _b:
            _bad_pro.append(_m.group(1))
if _bad_pro:
    errors.append('PROLOGUE_SAVES_CHECK=FAIL: %d function(s) with an _icall_esp save point before a prologue register save: %s' % (len(_bad_pro), ' '.join(_bad_pro[:10])))
if errors:
    print('\n'.join(errors))
    raise SystemExit(1)
print('VERIFY_PHASE3F_MEMCPY_FIX=PASS')
print('SHARED_EPILOGUE_CHECK=PASS')
print('CINIT_DISPATCH_CHECK=PASS')
print('CINIT_2E0947_CHECK=PASS')
print('FALLTHROUGH_FLAGS_CHECK=PASS')
print('PROLOGUE_SAVES_CHECK=PASS')
