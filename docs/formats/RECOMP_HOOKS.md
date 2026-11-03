<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Instruction-address hooks for the xboxrecomp translator (`--hooks hooks.toml`)

Purpose: change or observe ONE guest instruction (or add a call before/after it) without keeping an edited copy of a
generated `recomp_NNNN.c`. The translator emits a call to a hand-written C function at the chosen address; the project
keeps the function in its manual file (e.g. `recomp_manual.c`). Generic: nothing here is specific to Godzilla STE, so the
same file works for the DAMM Xbox port and any other title built with this tool.

Where: `tools/recomp/hooks.py` (parser + weaving), `translator.py` (`INSN_HOOKS`, one call site in the emit pass),
`__main__.py` (`--hooks FILE`), tests in `tools/recomp/test_hooks.py`. Added 2026-10-29 (STE c198) to both
`tools/xboxrecomp-v014` (the generator that produced candidate86) and `tools/xboxrecomp-v014k`.

## hooks.toml
```toml
[[hook]]
addr = 0x0010D36A            # guest address of one instruction
fn   = "ste_hook_knock_dec"  # `int fn(void)` defined in the manual file, extern-declared by the emitted code
mode = "replace"             # before | after | replace   (default before)
```
| mode | emitted | rule |
|---|---|---|
| before | `{ extern int fn(void); (void)fn(); }` ahead of the instruction | any instruction |
| after | the same call after the instruction | not on branch/call/ret (never reached) |
| replace | `{ extern int fn(void); if (!fn()) { <instruction> } }` | the instruction runs only if `fn()` returns 0; non-zero = skip it. Not on branch/call/ret. |

Several hooks per address: all `before`, then the single `replace`, then all `after`.

## Contract
* The hook reads/writes machine state through the generated code's own globals: `g_eax..g_edi`, `g_esp`, `MEM32(addr)` / `G32`.
  The generated code names registers `eax`/`esi`... via `#define eax g_eax`. Exception: in a function the lifter emits as
  frameless, `ebp` is a local, so a hook must not use `g_ebp` there.
* A skipped (`replace`) instruction does not update the lifter's flag snapshots; do not replace an instruction whose flags
  a following `jcc` consumes.
* The address must be the first byte of an instruction inside a translated function. Unknown addresses make the run fail
  (exit code 3, `hook addresses not found ...`), so a stale hooks.toml cannot silently do nothing. Hooks do not exist in
  the pre-pass that computes flag states; only in the emit pass.
* Without `--hooks` the output is byte-identical to a build without the feature (verified in STE c198 by regeneration diff).

## Usage
`python -m tools.recomp <xbe> --functions f.json --exclude-manual recomp_manual.c --hooks hooks.toml --all --split 1000 --gen-dir gen -o out`
(STE: `mission-l6\tests\regen_hooks.ps1 -Out <dir> -Hooks <hooks.toml>`; STE's hooks live in `mission-l6\hooks\ste_hooks.toml`.)
