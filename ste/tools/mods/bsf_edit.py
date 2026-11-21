# SPDX-License-Identifier: GPL-2.0-or-later
"""bsf_edit.py - minimal token-level editor for the game's compiled script files (BSF, "Compiled PPTokenStream version 1.03").

Our own implementation, written from the observed file layout (docs\\formats\\bsf_script.md); no third-party code or tool is used.
Python 3.9+, standard library only.

  python -I bsf_edit.py dump  <file.bsf>                  readable text (tokens re-joined, one statement per line)
  python -I bsf_edit.py check <file.bsf>                  parse + re-emit round trip, prints token count and OK

As a library (see build_solo_destruction.py):
  toks = bsf_edit.parse(data)            list of (kind, value, opcode)   kind: sym str num op
  toks = bsf_edit.replace(toks, old_src, new_src)   old_src/new_src are source snippets; old must match exactly once
  data = bsf_edit.emit(toks)

Layout: 52-byte header (magic, total size, 0, pointer to the source-name string), then tokens until 0xFD, then the string table.
Token: 0x26 string literal / 0x28 symbol = u32 absolute file offset of a NUL-terminated string; 0x27 = float32; 0xFE/0xFF = two-byte
source-position marker (dropped on parse: the game does not need them); any other byte = operator or keyword (table below).
"""
import re, struct, sys

MAGIC = b'Compiled PPTokenStream version 1.03.\x1a\x00\x00\x00'
CODE_START = 52
SOURCE_NAME = 'game.scr'

OPS = {0x00: '==', 0x01: '!=', 0x02: '*', 0x03: '+', 0x04: '-', 0x05: '/', 0x06: '%', 0x07: '&', 0x08: '~', 0x09: '|', 0x0A: '^',
       0x0B: '&&', 0x0C: '||', 0x0D: '!', 0x0E: '<<', 0x0F: '>>', 0x10: '<', 0x11: '>', 0x12: '>=', 0x13: '<=', 0x14: '=',
       0x15: '(', 0x16: ')', 0x17: '{', 0x18: '}', 0x19: '[', 0x1A: ']', 0x1B: ',', 0x1C: ';', 0x1D: '.', 0x1E: '::',
       0x1F: '++', 0x20: '--', 0x21: '++', 0x22: '++', 0x23: 'do', 0x24: 'switch', 0x25: 'case', 0x2C: 'for', 0x2D: 'if',
       0x2E: 'else', 0x2F: 'return', 0x30: 'break', 0x31: 'enum', 0x32: 'continue', 0x33: 'struct', 0x34: 'class'}
CODE = {}
for _o, _t in OPS.items(): CODE.setdefault(_t, _o)
KEYWORDS = {'do', 'switch', 'case', 'for', 'if', 'else', 'return', 'break', 'enum', 'continue', 'struct', 'class'}
LONG_OPS = sorted([t for t in CODE if len(t) > 1 and not t.isalpha()], key=len, reverse=True)


class BsfError(Exception):
    pass


def _cstr(d, off):
    e = d.find(b'\0', off)
    return d[off:len(d) if e < 0 else e].decode('latin-1')


def parse(d):
    if not d.startswith(MAGIC): raise BsfError('not a compiled game script (PPTokenStream 1.03)')
    i, toks = CODE_START, []
    while i < len(d):
        op = d[i]; i += 1
        if op in (0x26, 0x28):
            toks.append(('str' if op == 0x26 else 'sym', _cstr(d, struct.unpack_from('<I', d, i)[0]), op)); i += 4
        elif op == 0x27:
            toks.append(('num', struct.unpack_from('<f', d, i)[0], op)); i += 4
        elif op in (0xFE, 0xFF): i += 1
        elif op == 0xFD: return toks
        elif op in OPS: toks.append(('op', OPS[op], op))
        else: raise BsfError('unknown script byte 0x%02X at offset %d' % (op, i - 1))
    raise BsfError('script has no end marker')


def emit(toks):
    strings, index, fix, code = [], {}, [], bytearray()
    def sid(s):
        if s not in index: index[s] = len(strings); strings.append(s)
        return index[s]
    for k, v, op in toks:
        if k in ('sym', 'str'):
            code.append(0x28 if k == 'sym' else 0x26); fix.append((len(code), sid(v))); code += b'\0\0\0\0'
        elif k == 'num':
            code.append(0x27); code += struct.pack('<f', v)
        else: code.append(op)
    code.append(0xFD)
    src = sid(SOURCE_NAME)
    pos = CODE_START + len(code); offs, table = [], bytearray()
    for s in strings:
        offs.append(pos); raw = s.encode('latin-1', 'replace') + b'\0'; table += raw; pos += len(raw)
    table += b'\0\0'
    for at, idx in fix: struct.pack_into('<I', code, at, offs[idx])
    return MAGIC + struct.pack('<III', CODE_START + len(code) + len(table), 0, offs[src]) + bytes(code) + bytes(table)


def lex(text):
    """source snippet -> tokens. Numbers are float32; unary minus stays an operator (as in the game's files)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace(): i += 1; continue
        if text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j + 1; continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2); i = n if j < 0 else j + 2; continue
        if c == '"':
            i += 1; s = []
            while i < n and text[i] != '"':
                if text[i] == '\\' and i + 1 < n:
                    s.append({'n': '\n', 'r': '\r', 't': '\t', '0': '\0'}.get(text[i + 1], text[i + 1])); i += 2
                else: s.append(text[i]); i += 1
            i += 1; out.append(('str', ''.join(s), 0x26)); continue
        m = re.compile(r'(?:\d+\.\d*|\d*\.\d+|\d+)(?:[eE][+-]?\d+)?').match(text, i) if (c.isdigit() or c == '.') else None
        if m:
            out.append(('num', struct.unpack('<f', struct.pack('<f', float(m.group(0))))[0], 0x27)); i = m.end(); continue
        m = re.compile(r'[A-Za-z_][A-Za-z0-9_?]*').match(text, i)
        if m:
            w = m.group(0); i = m.end()
            out.append(('op', w, CODE[w]) if w in KEYWORDS else ('sym', w, 0x28)); continue
        for t in LONG_OPS:
            if text.startswith(t, i): out.append(('op', t, CODE[t])); i += len(t); break
        else:
            if c in '(){}[],;.+-*/%&~|^!<>=': out.append(('op', c, CODE[c])); i += 1
            else: raise BsfError('unexpected character %r in edit text' % c)
    return out


def _key(t): return (t[0], t[1])


def find(toks, pat):
    k = [_key(t) for t in pat]; n = len(k); ks = [_key(t) for t in toks]
    return [i for i in range(len(ks) - n + 1) if ks[i:i + n] == k]


def replace(toks, old_src, new_src):
    old, new = lex(old_src), lex(new_src); hits = find(toks, old)
    if len(hits) != 1:
        raise BsfError('script does not match what this tool expects (%d matches for: %s ...). Is this the original Xbox game?' % (len(hits), old_src.strip()[:50]))
    i = hits[0]; return toks[:i] + new + toks[i + len(old):]


def apply_edits(data, edits):
    """bytes of a script + [{"old": src, "new": src}, ...] -> bytes of the edited script (raises BsfError)"""
    toks = parse(data)
    for e in edits: toks = replace(toks, e['old'], e['new'])
    return emit(toks)


def dump(toks):
    lines, cur, depth = [], [], 0
    def flush():
        if cur: lines.append('\t' * depth + ' '.join(cur)); cur.clear()
    for k, v, _ in toks:
        t = '"%s"' % v if k == 'str' else ('%g' % v if k == 'num' else v)
        if t == '}': flush(); depth = max(0, depth - 1); cur.append(t); flush(); continue
        cur.append(t)
        if t in ('{', ';'):
            flush()
            if t == '{': depth += 1
    flush(); return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == 'dump': sys.stdout.write(dump(parse(open(a[1], 'rb').read())))
    elif len(a) == 2 and a[0] == 'check':
        t = parse(open(a[1], 'rb').read()); e = parse(emit(t)); print('%d tokens, round trip %s' % (len(t), 'OK' if [_key(x) for x in t] == [_key(x) for x in e] else 'MISMATCH'))
    else: print(__doc__)
