# SPDX-License-Identifier: GPL-2.0-or-later
"""build_biollante.py - build the "Biollante" recipe mod from YOUR OWN game files.

  python -I build_biollante.py <game folder or .iso> <out mod folder> [--id ste.biollante]

Biollante's models, animations, HUD, sounds and bios are on the retail disc but hidden. This mod makes her a normal character:
a 19th select-grid slot (portrait tiles generated on your PC from the disc's own Biollante art), unlocked, with a fight AI.
Contents of the mod folder: edit lists and hashes only, no game data (iso_to_mod.py prepare builds the files from your game):
  ShellData\\ControlRoom.bdl     builtin bio_ui (tiles + flash script + select-grid child; our own bundle library pw_bundle.py)
  ShellData\\monsterselect.bsf   bsf-edit: she is not locked behind a monster package
  ShellData\\attributes.xfg      text-edit: her state hidden -> unlocked
  ActionData\\ConventionalGame.bsf  bsf-edit: SetupBiollanteAI (our own, built from the game's standard AI setups)
All edits are our own; nothing is taken from community mods.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso_to_mod, bsf_edit, bio_ui

REC = []

def sub(toks, old, new):
    REC.append({'old': old, 'new': new})
    try: return bsf_edit.replace(toks, old, new)
    except bsf_edit.BsfError as e: raise SystemExit(str(e))

AI = '''void SetupBiollanteAI(Monster monster){
	print("Setting up Fight AI for " + monster.getName());
	MonsterAI monster_ai;
	Monster null_monster;
	MonsterAIGoal goal;
	monster_ai = monster.getAI();
	goal = monster_ai.fightWith(null_monster, "", "", "");
	number diff_level;
	diff_level = gGame.getShellGameInfo().getDifficultyLevel();
	if(gGame.getShellGameInfo().isAttractMode() || diff_level == 2) SetupStdHardAI(goal, monster_ai);
	else if(diff_level == 0) SetupStdEasyAI(goal, monster_ai);
	else SetupStdMediumAI(goal, monster_ai);
}
'''

def edit_conventional(s):
    s = sub(s, 'void SetupAnguirusAI(Monster monster);', 'void SetupAnguirusAI(Monster monster);\nvoid SetupBiollanteAI(Monster monster);')
    s = sub(s, 'else if(are_strings_equal(monster.getName(), "orga")) SetupOrgaAI(monster);',
            'else if(are_strings_equal(monster.getName(), "biollante")) SetupBiollanteAI(monster);\n\telse if(are_strings_equal(monster.getName(), "orga")) SetupOrgaAI(monster);')
    s = sub(s, 'void SetupGodzilla90AI(Monster monster){', AI + 'void SetupGodzilla90AI(Monster monster){')
    return s

def edit_select(s):
    # extra grid row: the select script keeps its grid as a list of rows (Thumbnails); Row2 is added by the ControlRoom.bdl part (bio_ui.SLOTS)
    s = sub(s, 'Thumbnails.resize(2.000000);', 'Thumbnails.resize(3.000000);')
    s = sub(s, 'Thumbnails[1.000000] = nails.findObject("Row1");', 'Thumbnails[1.000000] = nails.findObject("Row1");\n\t\t\tThumbnails[2.000000] = nails.findObject("Row2");')
    # (lookupThumbnail, the "preselect the last monster" search, keeps scanning rows 0..1: editing its loop bound made the select script fail
    #  to initialise in the game (2026-11-21 bisect), so a monster in an extra row is simply not preselected)
    return sub(s, 'if(monster == "rodan") return 0.000000;', 'if(monster == "rodan") return 0.000000;\n\t\tif(monster == "biollante") return 0.000000;')

XFG_OLD = '<attr>biollante<state>hidden</state></attr>'
XFG_NEW = '<attr>biollante<state>unlocked</state><class>1</class></attr>'

def main(a):
    if len(a) < 2: print(__doc__); return 1
    game, out = a[0], a[1]
    mid = a[a.index('--id') + 1] if '--id' in a else 'ste.biollante'
    src = iso_to_mod.Src(game); recipes = []
    for rel, fn in (('ActionData/ConventionalGame.bsf', edit_conventional), ('ShellData/monsterselect.bsf', edit_select)):
        if not src.has(rel): print('%s is not in your game (needs the retail Xbox files)' % rel); return 1
        del REC[:]; orig = src.read(rel)
        try: res = bsf_edit.emit(fn(bsf_edit.parse(orig)))
        except bsf_edit.BsfError as e: print('%s: %s' % (rel, e)); return 1
        recipes.append((src.name(rel), orig, res, list(REC))); print('edited %s' % rel)
    rel = 'ShellData/attributes.xfg'
    if not src.has(rel): print('%s is not in your game' % rel); return 1
    orig = src.read(rel); o, n = XFG_OLD.encode(), XFG_NEW.encode()
    if orig.count(o) != 1: print('attributes.xfg: the biollante entry was not found exactly once'); return 1
    recipes.append((src.name(rel), orig, orig.replace(o, n), [{'old': XFG_OLD, 'new': XFG_NEW}], 'text-edit')); print('edited %s' % rel)
    rel = 'ShellData/ControlRoom.bdl'
    if not src.has(rel): print('%s is not in your game' % rel); return 1
    orig = src.read(rel)
    recipes.append((src.name(rel), orig, bio_ui.build(orig), 'bio_ui', 'builtin')); print('built %s' % rel)
    iso_to_mod.write_recipe_mod(out, mid, 'Biollante', 'GPL-2.0-or-later',
        'Makes Biollante a normal character (19th select-grid slot, unlocked, with fight AI). Her models, animations, HUD and sounds are on the retail disc; '
        "this mod holds only edit recipes and hashes and is built on your PC from your own game files (Prepare mods). Credits: Biollante's data is the retail game's; "
        "the third select row idea is inspired by ANILORAK's research (our own code; no code from ANILORAK used). Full credits: https://github.com/replayrevival/ste-recomp/blob/main/CREDITS.md", recipes)
    print('\nDone. Mod folder: %s (recipe only, safe to share). Install it in the launcher (Mods tab), then Prepare mods.' % out)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
