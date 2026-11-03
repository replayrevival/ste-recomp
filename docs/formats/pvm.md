# PWK PVM virtual-machine modules

Credit: Digitzaki / GZBuildr. The implementation described here is `pvm_script_tool.py`.

- A PVM begins with `PWK Virtual machine module ` and has a fixed `0x80`-byte header (from GZBuildr source).
- The section table begins at `0x38` and contains nine sections; each section stores an offset and size (from GZBuildr source).
- The parser accepts big-endian GameCube/Wii modules and little-endian PS2 modules (from GZBuildr source).
- Header fields at `0x30` and `0x34` are a marker and total-size field, interpreted in the detected byte order (from GZBuildr source).
- The tool extracts null-terminated strings, typed globals, symbol pools, debug frames, code entries, and routine offsets (from GZBuildr source).
- Opcode sizing is based on recovered `GetPVOpSize__FPUc` evidence; fixed opcodes `0x64..0x7A` have a table and `0x6C`/`0x6D` are variable-width records (from GZBuildr source).
- `0x6D` subtype sizes are explicitly table-driven; version text changes canonical opcode numbering for later assignment/call records (from GZBuildr source).
- The PVM output is a conservative disassembly/source scaffold; opcode names and complete semantics remain partly recovered (from GZBuildr source / inferred).
- Big-endian Wii and little-endian PS2 handling is consistent with the observed bundles (observed).
