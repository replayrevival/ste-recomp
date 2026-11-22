# SPDX-License-Identifier: GPL-2.0-or-later
"""solo_ui: build ShellData\\ControlRoom.bdl with a select-screen title that can hold "SOLO DESTRUCTION" at the normal title size.

build(orig_bytes) -> new bundle bytes. Own code (pw_bundle), stdlib only. The retail MonsterSelect.ifc fits its title text into
<maxwidth>175</maxwidth> by shrinking it, which made the longer SOLO DESTRUCTION title small and thin next to VERSUS, MELEE, ...
This widens that limit (the titles that already fit are not changed). No game bytes are stored in this file.
"""
import pw_bundle

OLD = b'<v>MELEE</v><g o="b" w=" 450">..\\FONT_FOLDER\\SHELLFONT.FDF</g><c>0x5B95FFFF</c><maxwidth>175</maxwidth>'
NEW = b'<v>MELEE</v><g o="b" w=" 450">..\\FONT_FOLDER\\SHELLFONT.FDF</g><c>0x5B95FFFF</c><maxwidth>420</maxwidth>'


def build(orig):
    b = pw_bundle.Bundle.parse(orig)
    e = b.find('MonsterSelect.ifc')
    if e is None: raise SystemExit('ControlRoom.bdl lacks MonsterSelect.ifc: not the retail STE file?')
    m = b.get_main(e)
    if m.count(OLD) != 1: raise SystemExit('MonsterSelect.ifc title layout differs from the retail file')
    b.set_main(e, m.replace(OLD, NEW))
    return b.emit()
