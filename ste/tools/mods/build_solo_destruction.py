# SPDX-License-Identifier: GPL-2.0-or-later
"""build_solo_destruction.py - build the "Solo Destruction" patch mod from YOUR OWN game files.

  python -I build_solo_destruction.py <game folder or .iso> <out mod folder> [--id ste.solo-destruction]

What it does (nothing from the game is stored in this file): it reads three of the game's own script files
(ActionData\\ChallengeDemolition.bsf, ActionData\\Game.bsf, ShellData\\mainmenu.bsf), edits them at token level with our own
script editor (bsf_edit.py, no third-party tool needed), applies the edits below, and writes a manifest-api-2 patch mod (iso_to_mod.py make) into <out mod folder>.
Install the folder through the launcher's Mods tab (or put it in the mods folder) and run "Prepare mods" / iso_to_mod.py prepare.

The mode (see docs\\formats\\solo_destruction.md): a new main-menu row "SOLO DESTRUCTION" starts the normal Versus flow (monster,
rules, city); the game then runs the Demolition challenge script on the chosen monster and city. Based on ANILORAK's Godzilla
Ultra research (the idea of a solo destruction mode); the scripts are our own edits of the game's retail scripts.
"""
import os, sys, subprocess, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso_to_mod, bsf_edit

FLAG = '4194304.000000'   # shell cheat bit 1<<22: transient flag set by the menu row, cleared by Game.bsf when it loads the script
FLAG2 = '8388608.000000'  # shell cheat bit 1<<23: "no time limit" chosen on the solo Rules screen (read and cleared by ChallengeDemolition.bsf)
NOLIM = '1.000000'        # value of the Rules spinner "NO TIME LIMIT: YES" (the YES/NO spinner of the unused Reset-arenas row)
FILES = ['ActionData/ChallengeDemolition.bsf', 'ActionData/Game.bsf', 'ShellData/mainmenu.bsf']

REC = []   # every edit as {"old","new"} in order, written into the recipe

def sub(toks, old, new):
    REC.append({'old': old, 'new': new})
    try: return bsf_edit.replace(toks, old, new)
    except bsf_edit.BsfError as e: raise SystemExit(str(e))

def edit_demolition(s):
    s = sub(s, '''		gameLoadLevelData("ChallengeDemolition", doDay);
		gi.setPlayers(1.000000);
		if(gApp.debugSetting("game", "bShortMatches", 0.000000)){
			gi.setRoundTime(15.000000 + 0.900000);
			POINTS_GOAL = 600.000000;
		} else gi.setRoundTime(60.000000 * 3.000000 + 0.900000);
''', '''		if(gi.getGameType() == 1.000000){
			gameLoadLevelData(gi.getCityName(), gi.getDaylight());
			gi.setPlayers(1.000000);
			if(gi.getShellCheats() & ''' + FLAG2 + '''){
				gi.setShellCheats(gi.getShellCheats() - ''' + FLAG2 + ''');
				gi.setRoundTime(5999.900000);
			}
		} else{
			gameLoadLevelData("ChallengeDemolition", doDay);
			gi.setPlayers(1.000000);
			if(gApp.debugSetting("game", "bShortMatches", 0.000000)){
				gi.setRoundTime(15.000000 + 0.900000);
				POINTS_GOAL = 600.000000;
			} else gi.setRoundTime(60.000000 * 3.000000 + 0.900000);
		}
''')
    s = sub(s, 'if(gi.getGameType() != 5.000000){\n\t\t\tpd.setMonsterName("godzilla2k");', 'if(gi.getGameType() != 5.000000 && gi.getGameType() != 1.000000){\n\t\t\tpd.setMonsterName("godzilla2k");')
    s = sub(s, 'if(! gApp.debugSetting("game", "bSkipIntroAnimations", 0.000000)){\n\t\t\tcreateNITimer("IntroNI"', 'if(! gApp.debugSetting("game", "bSkipIntroAnimations", 0.000000) && gGame.getShellGameInfo().getGameType() != 1.000000){\n\t\t\tcreateNITimer("IntroNI"')
    s = sub(s, '|| (gGame.getShellGameInfo().getRoundTime() - gGame.getGameTime()) <= 0.000000){',
            '|| (gGame.getShellGameInfo().getRoundTime() > 0.000000 && (gGame.getShellGameInfo().getRoundTime() - gGame.getGameTime()) <= 0.000000)){')
    s = sub(s, '\t\tremoveKillNI = 0.000000;\n\t}\n\tvoid showSummary', '\t\tremoveKillNI = 0.000000;\n\t\tif(gGame.getShellGameInfo().getGameType() == 1.000000) POINTS_GOAL = 0.000000;\n\t}\n\tvoid showSummary')
    return s

def edit_game(g):
    return sub(g, '\t\tgGame.loadScript("ConventionalGame.bsf");\n\t}\n\tnumber sendHudMessage',
               '\t\tif(gi.getShellCheats() & ' + FLAG + '){\n\t\t\tgi.setShellCheats(gi.getShellCheats() - ' + FLAG + ');\n\t\t\tgGame.loadScript("ChallengeDemolition.bsf");\n\t\t} else gGame.loadScript("ConventionalGame.bsf");\n\t}\n\tnumber sendHudMessage')

def edit_mainmenu(m):
    m = sub(m, '''			else ListBox.findObject("SystemLinkCheat").hideObject();
		}
		ListBox.setSelection(selection);''', '''			else ListBox.findObject("SystemLinkCheat").hideObject();
		}
		{
			ShellObject sdo;
			sdo = ListBox.findObject("SystemLinkCheat");
			((ShellText) sdo).setText("SOLO DESTRUCTION");
			sdo.showObject();
		}
		ListBox.setSelection(selection);''')
    m = sub(m, '''		if(entry == "SystemLinkCheat"){
			gShell.fCallScriptF("MultiPlayer::setSilentPassThrough", 5.000000, 0.000000);
			mmGotoScreen(19.000000);
			return;
		}''', '''		if(entry == "SystemLinkCheat"){
			gSoloNext = 1.000000;
			createGame(1.000000, 0.000000);
			gSoloNext = 0.000000;
			mmGotoScreen(2.000000);
			return;
		}''')
    m = sub(m, '''		sgi = gShell.getGameInfo();
		sgi.setGameType(gt);
		gShell.createGame(gt, mode);''', '''		sgi = gShell.getGameInfo();
		if(sgi.getShellCheats() & ''' + FLAG + ''') sgi.setShellCheats(sgi.getShellCheats() - ''' + FLAG + ''');
		if(sgi.getShellCheats() & ''' + FLAG2 + ''') sgi.setShellCheats(sgi.getShellCheats() - ''' + FLAG2 + ''');
		sgi.setGameType(gt);
		gShell.createGame(gt, mode);
		if(gSoloNext){
			sgi.setShellCheats(sgi.getShellCheats() + ''' + FLAG + ''');
		}''')
    return sub(m, '\tShellListBox ListBox;\n', '\tShellListBox ListBox;\n\tnumber gSoloNext;\n')

SOLO = '(gShell.getGameInfo().getShellCheats() & ' + FLAG + ')'

SOLO = '(gShell.getGameInfo().getShellCheats() & ' + FLAG + ')'

def edit_monsterselect(m):
    """solo screen: the one-player layout (no VS graphic, no second box, no second bios, no COM step), own title, only the
    ROUND LENGTH rule. Normal Versus (flag not set) takes the original branches."""
    m = sub(m, 'else if((gt == 5.000000) || (gt == 6.000000)){ obj = OnePlayer;',
            'else if((gt == 5.000000) || (gt == 6.000000) || @S@){ obj = OnePlayer;'.replace('@S@', SOLO))
    old = """else if((gt == 5.000000) || (gt == 6.000000)){
			OnePlayer.showObject(); TwoPlayer.hideObject(); FourPlayer.hideObject();
			if(gt == 5.000000){ s = gApp.localizeString("adventure"); AMScore.showObject(); }
			else s = gApp.localizeString("survival");
			setupDifficulty();"""
    new = """else if((gt == 5.000000) || (gt == 6.000000) || @S@){
			OnePlayer.showObject(); TwoPlayer.hideObject(); FourPlayer.hideObject();
			if(gt == 5.000000){ s = gApp.localizeString("adventure"); AMScore.showObject(); }
			else if(@S@) s = "SOLO DESTRUCTION";
			else s = gApp.localizeString("survival");
			{
				ShellObject sdf;
				sdf = OnePlayer.findObject("Difficulty");
				if(@S@){ if(sdf.isValid()) sdf.hideObject(); }
				else{ if(sdf.isValid()) sdf.showObject(); setupDifficulty(); }
			}""".replace('@S@', SOLO)
    m = sub(m, old, new)
    m = sub(m, 'if(gt == 1.000000){ return rule != 2.000000; }',
            'if(gt == 1.000000){ if(@S@) return rule == 3.000000 || rule == 4.000000; return rule != 2.000000; }'.replace('@S@', SOLO))
    # the YES/NO "reset arenas" row becomes "NO TIME LIMIT" (own storage key, so Versus keeps its own setting)
    m = sub(m, 'else if(rule == 4.000000) return prefix + "reset_arenas";',
            'else if(rule == 4.000000){ if(gt == 1.000000 && @S@) return "rule_solo_notimelimit"; return prefix + "reset_arenas"; }'.replace('@S@', SOLO))
    m = sub(m, 'void updateWhichRules(){ hideUnusedRules(); setRuleCursor(0.000000);',
            'void updateWhichRules(){ if(getGameType() == 1.000000 && @S@) ((ShellText) Names[4.000000]).setText("NO TIME LIMIT"); hideUnusedRules(); setRuleCursor(0.000000);'.replace('@S@', SOLO))
    # the game starts here: set the no-limit bit from the stored choice (after every recreateGame of the rules screen)
    return sub(m, 'sgi.setPlayers(plyrs); Lobby.closeLobby();',
               '''sgi.setPlayers(plyrs);
		if(@S@){
			ShellGameInfo sg;
			sg = gShell.getGameInfo();
			if(sg.getShellCheats() & @F2@) sg.setShellCheats(sg.getShellCheats() - @F2@);
			if(sgi.getShellCheats() & @F2@) sgi.setShellCheats(sgi.getShellCheats() - @F2@);
			if((Rules -- getRuleValue(4.000000)) == @N@){
				sg.setShellCheats(sg.getShellCheats() + @F2@);
				sgi.setShellCheats(sgi.getShellCheats() + @F2@);
			}
		}
		Lobby.closeLobby();'''.replace('@S@', SOLO).replace('@F2@', FLAG2).replace('@N@', NOLIM))

FILES.append('ShellData/monsterselect.bsf')
EDITS = {FILES[0]: edit_demolition, FILES[1]: edit_game, FILES[2]: edit_mainmenu, FILES[3]: edit_monsterselect}

def main(a):
    if len(a) < 2: print(__doc__); return 1
    game, out = a[0], a[1]
    mid = a[a.index('--id') + 1] if '--id' in a else 'ste.solo-destruction'
    src = iso_to_mod.Src(game); recipes = []
    for rel in FILES:
        if not src.has(rel): print('%s is not in your game (needs the retail Xbox files)' % rel); return 1
        del REC[:]; orig = src.read(rel)
        try: res = bsf_edit.emit(EDITS[rel](bsf_edit.parse(orig)))
        except bsf_edit.BsfError as e: print('%s: %s' % (rel, e)); return 1
        recipes.append((src.name(rel), orig, res, list(REC))); print('edited %s' % rel)
    iso_to_mod.write_recipe_mod(out, mid, 'Solo Destruction', 'GPL-2.0-or-later',
        'Adds a SOLO DESTRUCTION row to the main menu: free destruction in any city with any monster. Contains only edit recipes, no game data; '
        'the scripts are built on your PC from your own game files (Prepare mods). Based on ANILORAK\'s Godzilla Ultra research.', recipes)
    print('\nDone. Mod folder: %s (recipe only, safe to share). Install it in the launcher (Mods tab), then Prepare mods, then play: main menu > SOLO DESTRUCTION.' % out)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
