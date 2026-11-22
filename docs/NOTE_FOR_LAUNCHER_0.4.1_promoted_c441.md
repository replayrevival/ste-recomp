# NOTE FOR THE LAUNCHER SESSION: 0.4.1 hotfix = c441 (Solo flag fix, Solo title) - NOT published, waiting for the maintainer
Exe: the c441 build artifact, SHA-256 CF08F70BC81B9AECDF07265D81884B9BF26F4109C41243C6C7978F38634E6410, version resource 0.4.1 / 0.4.1+c441. Validator PASS. Base c437 + a missing `#include <stdlib.h>` fix (an implicit `getenv` truncated the pointer: `STE_ETHERNET_LINK=up` could crash; default unaffected) + an opt-in NIC register trace (`STE_NIC_TRACE=1`, off by default). Source: publish xboxrecomp\src\kernel (nvnet.c/.h, kernel_xbox.c and others), commit follows.
## What changed for players
1. Fixed: after SOLO DESTRUCTION (or backing out of it) Versus / Action / Melee could open the Solo select screen. "SOLO DESTRUCTION is now cleared properly when you leave it." Needs the UPDATED mod: the player must run Prepare mods again.
2. Fixed: the SOLO DESTRUCTION title now has the same font, size and colour as VERSUS.
Full list and what could not be reproduced (Biollante tile on the Solo screen, black attract movie, globe on the ultrawide menu): docs\REPORT_0.4.1.md; release text: docs\RELEASE_ALPHA.md "0.4.1".
## What the launcher must do
1. Ship the updated `ste\mods\ste.solo-destruction\` (manifest + 4 edit lists + a ControlRoom.bdl `builtin:solo_ui` entry). Both mods are OFF by default.
2. Package tools next to iso_to_mod.py: the new `solo_ui.py` plus the updated `iso_to_mod.py` (BUILTINS now bio_ui, solo_ui), `build_solo_destruction.py` (optional author tool).
3. **Prepare must use `prepare-stack`** for all enabled recipe mods (as in the 0.4.0 note): Solo Destruction and Biollante now BOTH change ControlRoom.bdl, so a per-mod `prepare` cannot combine them (the later one would replace the other's Row2/tiles). Please confirm in the smoke test what Prepare runs.
4. Smoke test cases (both mods on, then each alone, then none): Solo -> Back -> Versus / Action / Melee each show their own screen; Solo again shows Solo; restart -> main menu normal; the SOLO title looks like the VERSUS title; Biollante's tile visible on every select screen (with the Biollante mod); idle until the attract movie plays (picture visible).
5. Credits line in the release notes (already in RELEASE_ALPHA): Solo Destruction is inspired by ANILORAK's research (our own code; no code from ANILORAK used). Full credits: CREDITS.md.
Media: reuse the 0.4.0 media. Machine: my game is not running.
