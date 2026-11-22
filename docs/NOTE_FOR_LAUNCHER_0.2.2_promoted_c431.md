# NOTE FOR THE LAUNCHER SESSION: 0.2.2 promoted = c431
Exe: artifacts\ste_recomp_mission_l6_candidate431.exe, SHA-256 089A57CDD20662A17585CDD342F4374AC3B9D94400FDC32577CC9DAAEECA1C9F, version resource 0.2.2.431 / 0.2.2+c431, capabilities.json adds "texture_mod_packs". Validator PASS. Not published. Base c430 (0.2.1); code differences: kernel_mods.c + kelvin_texpack.c (patches\c431_texture_mods_dir.patch) and the version string.
## What a player/modder gets (lines you can quote)
1. "Texture packs installed from the Mods tab now work: the game reads them from the mods folder, follows the on/off switch and the order." Verified: checkerboard on Godzilla 2000 on/off in a Tokyo fight; logs and last_report.txt lines (docs\NOTE_FOR_LAUNCHER_0.2.2_retexture.md).
2. "Export the game's own textures as named PNGs (tools\mods\import_texture_set.py export)", then edit, import (takes the game folder or .iso directly), install.
## Not in 0.2.2
RAGE announcer, Tokyo UFO, Unlimited pop-in, ANILORAK HD sample test: moved to 0.2.3 with sound packs (docs\REPORT_0.2.md). Nothing new claimed about them.
Docs: docs\formats\texturepack.md, texturepack_import.md; RELEASE_ALPHA.md section 0.2.2. Screenshots: none new (the checkerboard shots are test images, not for the repo).
For the machine: I am not running a game now; say "launcher busy"/"launcher clear".
