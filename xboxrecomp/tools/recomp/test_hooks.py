# SPDX-License-Identifier: GPL-2.0-or-later
"""hooks.toml: parsing, and how lift_block_with_hooks weaves the calls into a block."""
import pytest
from types import SimpleNamespace as NS

from tools.recomp import hooks


def _insn(addr, mn="dec", branch=False):
    return NS(address=addr, mnemonic=mn, is_branch=branch, is_ret=False)


def _fake_lift(lifter, bb, flag_state=None):
    return [f"/* {i.mnemonic} {i.address:X} */" for i in bb.instructions], flag_state


def _toml(tmp_path, text):
    p = tmp_path / "hooks.toml"
    p.write_text(text, encoding="utf-8")
    return str(p)


def test_parse(tmp_path):
    h = hooks.load_hooks(_toml(tmp_path, '[[hook]]\naddr = 0x10\nfn = "a"\nmode = "replace"\n[[hook]]\naddr = 0x10\nfn = "b"\n'))
    assert h == {0x10: [("replace", "a"), ("before", "b")]}


def test_bad_mode_and_double_replace(tmp_path):
    with pytest.raises(ValueError):
        hooks.load_hooks(_toml(tmp_path, '[[hook]]\naddr = 1\nfn = "a"\nmode = "around"\n'))
    with pytest.raises(ValueError):
        hooks.load_hooks(_toml(tmp_path, '[[hook]]\naddr=1\nfn="a"\nmode="replace"\n[[hook]]\naddr=1\nfn="b"\nmode="replace"\n'))


def test_before_replace_after_order_and_wrapping():
    bb = NS(instructions=[_insn(0x10, "mov"), _insn(0x12), _insn(0x14, "inc")])
    used = set()
    out, _ = hooks.lift_block_with_hooks(
        _fake_lift, None, bb, None,
        {0x12: [("after", "ha"), ("replace", "hr"), ("before", "hb")]}, used)
    text = "\n".join(out)
    assert used == {0x12}
    assert text.index("hb()") < text.index("hr()") < text.index("/* dec 12 */") < text.index("ha()")
    assert "if (!hr())" in text
    assert text.index("/* mov 10 */") < text.index("hb()") and text.index("ha()") < text.index("/* inc 14 */")


def test_replace_on_branch_is_refused():
    bb = NS(instructions=[_insn(0x10, "jne", branch=True)])
    with pytest.raises(ValueError):
        hooks.lift_block_with_hooks(_fake_lift, None, bb, None, {0x10: [("replace", "h")]}, set())


def test_unused_reported():
    assert hooks.unused({1: [], 2: []}, {1}) == [2]
