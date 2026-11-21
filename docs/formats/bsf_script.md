# BSF compiled script files ("Compiled PPTokenStream version 1.03")

Game scripts (`ActionData\*.bsf`, `ShellData\*.bsf`, `Scripts\...`) are token streams, not machine code. Observed layout
(Xbox STE; our own reading of the files, implemented in `tools\mods\bsf_edit.py`):

| Offset | Content |
|---|---|
| 0x00 | magic `Compiled PPTokenStream version 1.03.` + `1A 00 00 00` (40 bytes) |
| 0x28 | u32 total file size |
| 0x2C | u32 zero |
| 0x30 | u32 absolute offset of the source-file-name string (`game.scr` in the files we write) |
| 0x34 | token stream, ends with `FD`; the string table follows, then two zero bytes |

Tokens: `26` string literal, `28` symbol (identifier): both followed by a u32 absolute file offset of a NUL-terminated string;
`27` + float32 number (all numbers are float32); `FE xx` / `FF xx` two-byte source-position markers (the game does not need
them, our editor drops them, so an edited file is slightly smaller than the original); everything else is a one-byte
operator or keyword: `00 ==` `01 !=` `02 *` `03 +` `04 -` `05 /` `06 %` `07 &` `08 ~` `09 |` `0A ^` `0B &&` `0C ||` `0D !` `0E <<`
`0F >>` `10 <` `11 >` `12 >=` `13 <=` `14 =` `15 (` `16 )` `17 {` `18 }` `19 [` `1A ]` `1B ,` `1C ;` `1D .` `1E ::` `1F ++(prefix)`
`20 --` (the game also uses 0x20 as the class-scope operator, e.g. `Rules -- viewRules(...)`, which other tools print as `--`) `21/22 ++(postfix)` `23 do` (observed as postfix decrement `i--` in loops) `24 switch` `25 case` `2C for` `2D if` `2E else` `2F return` `30 break` `31 enum` `32 continue`
`33 struct` `34 class`. The language is C-like (classes, `number`/`string`/object variables, `loadScript`, engine calls).

## Editing without shipping game data
`tools\mods\bsf_edit.py` parses a script into tokens, replaces a token sequence given as a source snippet (the snippet must match
exactly once, otherwise the tool refuses: wrong game version), and writes the file again (strings in first-use order).
Unedited tokens keep their original opcodes. Edits are applied on the player's own files and the result is delivered as a
patch mod (`docs\formats\mod_patch.md`); the compiled result is never published. `python -I bsf_edit.py dump <file.bsf>` prints
readable text, `check` does a parse/emit round trip.

Cross-check: every script the Solo Destruction builder writes was compared offline with the output of a separate third-party
tool (used for research only, not shipped): same token streams.
