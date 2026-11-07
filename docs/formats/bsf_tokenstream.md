# BSF PPTokenStream

Credit: Digitzaki / GZBuildr. The implementation described here is `gste_script_tool_v3.py`.

- BSF script header is `Compiled PPTokenStream version 1.03.\x1A\x00\x00\x00` (from GZBuildr source).
- At `0x10` is a total-size `u32`, at `0x14` a zero/reserved `u32`, and at `0x18` an absolute source-name string pointer (from GZBuildr source).
- Token code begins at `0x34` (from GZBuildr source).
- `0x26` is an absolute pointer to a string literal, `0x27` is a little-endian float32, and `0x28` is an absolute pointer to a symbol (from GZBuildr source).
- `0xFD` terminates the token stream; `0xFE`/`0xFF` consume compiler/source metadata markers (from GZBuildr source).
- Operators, punctuation, and keywords have one-byte token IDs; unknown IDs are retained as diagnostics (from GZBuildr source).
- String pointers are absolute file offsets into the trailing NUL-terminated string pool (from GZBuildr source).
- The tool can decompile to readable source and compile tokenized source back; exact rebuild requires preserved raw sections where available (from GZBuildr source).
- Little-endian interpretation and the 1.03 header were observed in the staged STE BSF samples (observed).
- Full compiler semantics and every metadata marker are not yet proven (inferred).
