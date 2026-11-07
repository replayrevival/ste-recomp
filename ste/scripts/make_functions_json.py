#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn ste/gen/functions.tsv (function start/end addresses found by analysing the user's own default.xbe)
into the functions.json the generator reads.  usage: make_functions_json.py functions.tsv functions.json

The list holds addresses only (no code, no data).  The call graph fields the generator also accepts are
not needed and are written empty."""
import json, sys

ENTRY = 0x00015586   # the XBE entry point keeps its own name
rows = [ln.rstrip('\n').split('\t') for ln in open(sys.argv[1], encoding='utf-8') if ln.strip()]
hdr, rows = rows[0], rows[1:]
out = []
for start, end, section, conf, method, ninsn, prol in rows:
    s, e = int(start, 16), int(end, 16)
    out.append({'start': start, 'end': end, 'size': e - s,
                'name': 'xbe_entry_point' if s == ENTRY else 'sub_%08X' % s,
                'section': section, 'confidence': float(conf), 'detection_method': method,
                'num_instructions': int(ninsn), 'has_prologue': prol == '1',
                'calls_to': [], 'called_by': []})
json.dump(out, open(sys.argv[2], 'w'))
print('wrote %d functions' % len(out))
