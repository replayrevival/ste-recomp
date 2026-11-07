# EDF particle/effect files

Credit: Digitzaki / GZBuildr. The editable codec is `edf_dump_codec.py`; EDF is also mapped to type 20 by GZBuildr.

- GZBuildr treats entry type 20 as particle/EDF and appends `.edf` when a type-20 name lacks that suffix (from GZBuildr source).
- The codec represents short two-letter EDF fields/opcodes, preserving the original code in brackets while providing friendly names (from GZBuildr source).
- Known field labels include emitter count/priority, flags, generation rate, start area, velocity, lifetime, size, color, blend, collision responses, sound loop, particle mode, tile stepping, and constant/keyframed/curve values (from GZBuildr source).
- Editable text can preserve unknown structural lines as escaped `@raw` records and can round-trip known lines to binary (from GZBuildr source).
- The default field-name map is a convenience layer, not a claim that every two-letter field has a globally fixed meaning (inferred).
- EDF payloads were observed as type-20 entries in character and effect bundles (observed).
- Exact emitter graph evaluation, resource binding, and runtime endian rules require more samples (inferred).
