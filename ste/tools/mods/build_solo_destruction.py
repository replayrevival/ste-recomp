# SPDX-License-Identifier: GPL-2.0-or-later
"""build_solo_destruction.py - build the "Solo Destruction" patch mod from YOUR OWN game files.

  python -I build_solo_destruction.py <game folder or .iso> <out mod folder> --gzbuildr <gste_script_tool_v3.py> [--id ste.solo-destruction]

What it does (nothing from the game is stored in this file): it reads three of the game's own script files
(ActionData\\ChallengeDemolition.bsf, ActionData\\Game.bsf, ShellData\\mainmenu.bsf), turns them into readable text with the
external GZBuildr script tool (GPL-3.0, https://github.com/Digitzaki/GZBuildr, a separate download, used offline), applies the
text edits below, compiles them again, and writes a manifest-api-2 patch mod (iso_to_mod.py make) into <out mod folder>.
Install the folder through the launcher's Mods tab (or put it in the mods folder) and run "Prepare mods" / iso_to_mod.py prepare.

The mode (see docs\\formats\\solo_destruction.md): a new main-menu row "SOLO DESTRUCTION" starts the normal Versus flow (monster,
rules, city); the game then runs the Demolition challenge script on the chosen monster and city. Based on ANILORAK's Godzilla
Ultra research (the idea of a solo destruction mode); the scripts are our own edits of the game's retail scripts.
"""
import os, sys, subprocess, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso_to_mod

FLAG = '4194304.000000'   # shell cheat bit 1<<22: transient flag set by the menu row, cleared by Game.bsf when it loads the script
FILES = ['ActionData/ChallengeDemolition.bsf', 'ActionData/Game.bsf', 'ShellData/mainmenu.bsf']

def sub(s, old, new):
    if s.count(old) != 1:
        raise SystemExit('script text does not match what this tool expects (%d matches for: %s ...): is this the retail Xbox game?' % (s.count(old), old[:60].strip()))
    return s.replace(old, new)

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
			if(gi.getRoundTime() >= 599.000000) gi.setRoundTime(5999.900000);
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
		sgi.setGameType(gt);
		gShell.createGame(gt, mode);
		if(gSoloNext) sgi.setShellCheats(sgi.getShellCheats() + ''' + FLAG + ''');''')
    return sub(m, '\tShellListBox ListBox;\n', '\tShellListBox ListBox;\n\tnumber gSoloNext;\n')

EDITS = {FILES[0]: edit_demolition, FILES[1]: edit_game, FILES[2]: edit_mainmenu}

def main(a):
    if len(a) < 2 or '--gzbuildr' not in a: print(__doc__); return 1
    game, out = a[0], a[1]; gz = a[a.index('--gzbuildr') + 1]
    mid = a[a.index('--id') + 1] if '--id' in a else 'ste.solo-destruction'
    if not os.path.isfile(gz): print('GZBuildr script tool not found: %s' % gz); return 1
    src = iso_to_mod.Src(game); work = tempfile.mkdtemp(prefix='solo_build_'); modsrc = os.path.join(work, 'modsrc')
    try:
        for rel in FILES:
            if not src.has(rel): print('%s is not in your game (needs the retail Xbox files)' % rel); return 1
            name = os.path.basename(rel); orig = os.path.join(work, name); txt = os.path.join(work, name + '.txt')
            open(orig, 'wb').write(src.read(rel))
            subprocess.run([sys.executable, '-I', gz, orig, '-o', txt], check=True, stdout=subprocess.DEVNULL)
            s = open(txt, encoding='utf-8').read(); s = EDITS[rel](s)
            open(txt, 'w', encoding='utf-8', newline='').write(s)
            dst = os.path.join(modsrc, rel.replace('/', os.sep)); os.makedirs(os.path.dirname(dst), exist_ok=True)
            subprocess.run([sys.executable, '-I', gz, txt, '-o', dst], check=True, stdout=subprocess.DEVNULL)
            print('edited and compiled %s' % rel)
        iso_to_mod.cmd_make([modsrc, game, out, '--id', mid, '--name', 'Solo Destruction', '--license', 'GPL-2.0-or-later', '--partial'])
        print('\nDone. Mod folder: %s\nInstall it in the launcher (Mods tab), then Prepare mods, then play: main menu > SOLO DESTRUCTION.' % out)
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
