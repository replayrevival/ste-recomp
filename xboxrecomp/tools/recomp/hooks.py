# SPDX-License-Identifier: GPL-2.0-or-later
"""Instruction-address hooks (hooks.toml): make the translator emit a call to a
hand-written function at chosen guest instruction addresses, so a project can
change one instruction's behaviour without keeping an edited copy of a generated
file.

    [[hook]]
    addr = 0x0010D36A          # guest address of ONE instruction (must start an instruction)
    fn   = "ste_hook_knock_dec"  # `int fn(void)` defined in the project's manual file
    mode = "replace"           # before | after | replace

Modes
  before   fn() runs just before the instruction; its return value is ignored.
  after    fn() runs just after the instruction (not allowed on branches/calls/returns:
           control never gets there).
  replace  the instruction runs only when fn() returns 0; a non-zero return skips it
           (the hook has done the work itself). Not allowed on branches/calls/returns.

The hook sees the machine state through the same globals the generated code uses
(g_eax .. g_edi, g_esp, MEM32 ...). Registers are globals in the generated
functions, with ONE exception: a function the lifter marks `frameless` keeps ebp in
a local, so a hook must not rely on g_ebp there. Several hooks may share an
address: all `before` hooks run first, then the (single) `replace`, then the
`after` hooks. A hook address that no translated function contains is an error
(see unused()), so a stale address cannot silently do nothing.
"""
import sys

MODES = ("before", "after", "replace")


def load_hooks(path):
    """Parse hooks.toml -> {addr: [(mode, fn), ...]}."""
    import tomllib
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    hooks = {}
    for i, h in enumerate(doc.get("hook", [])):
        try:
            addr = int(h["addr"])
            fn = str(h["fn"])
            mode = str(h.get("mode", "before"))
        except KeyError as e:
            raise ValueError(f"{path}: hook #{i + 1} lacks {e}")
        if mode not in MODES:
            raise ValueError(f"{path}: hook #{i + 1}: mode must be one of {MODES}, not {mode!r}")
        if not fn.isidentifier():
            raise ValueError(f"{path}: hook #{i + 1}: fn {fn!r} is not a C identifier")
        hooks.setdefault(addr, []).append((mode, fn))
    for addr, lst in hooks.items():
        if sum(1 for m, _ in lst if m == "replace") > 1:
            raise ValueError(f"{path}: address 0x{addr:08X} has more than one replace hook")
    return hooks


class _Seg:
    """The part of a basic block lift_basic_block needs: its instruction list."""
    def __init__(self, instructions):
        self.instructions = instructions


def _transfers(insn):
    return bool(getattr(insn, "is_branch", False) or getattr(insn, "is_ret", False)
                or insn.mnemonic in ("call", "ret", "retn", "jmp"))


def lift_block_with_hooks(lift_basic_block, lifter, bb, flag_state, hooks, used):
    """lift_basic_block(), with the hooks of `hooks` woven in. `used` collects the
    addresses that were found. Returns (stmts, flag_state) like lift_basic_block."""
    insns = bb.instructions
    hit = [k for k, i in enumerate(insns) if i.address in hooks]
    if not hit:
        return lift_basic_block(lifter, bb, flag_state=flag_state)
    stmts, state, pos = [], flag_state, 0
    for k in hit:
        if k > pos:
            s, state = lift_basic_block(lifter, _Seg(insns[pos:k]), flag_state=state)
            stmts.extend(s)
        insn = insns[k]
        used.add(insn.address)
        lst = hooks[insn.address]
        s, state = lift_basic_block(lifter, _Seg([insn]), flag_state=state)
        tag = f"hook 0x{insn.address:08X}"
        for mode, fn in lst:
            if mode == "before":
                stmts.append(f"{{ extern int {fn}(void); (void){fn}(); }} /* {tag} before */")
        repl = [fn for m, fn in lst if m == "replace"]
        if repl:
            if _transfers(insn):
                raise ValueError(f"{tag}: replace is not allowed on {insn.mnemonic} (control transfer)")
            stmts.append(f"{{ extern int {repl[0]}(void); if (!{repl[0]}()) {{ /* {tag} replace */")
            stmts.extend("    " + x for x in s)
            stmts.append("} }")
        else:
            stmts.extend(s)
        for mode, fn in lst:
            if mode == "after":
                if _transfers(insn):
                    raise ValueError(f"{tag}: after is not allowed on {insn.mnemonic} (control transfer)")
                stmts.append(f"{{ extern int {fn}(void); (void){fn}(); }} /* {tag} after */")
        pos = k + 1
    if pos < len(insns):
        s, state = lift_basic_block(lifter, _Seg(insns[pos:]), flag_state=state)
        stmts.extend(s)
    return stmts, state


def unused(hooks, used):
    return sorted(set(hooks) - set(used))
