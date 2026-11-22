<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Godzilla Ultra (ANILORAK, PS2) and our Xbox recomp: assessment (2026-11-19, report only, no game code changed)

Inputs: ANILORAK's published "Save the Earth modding pack" (zip SHA-256 47bca3bf4a249ec35d71cb4ef95c12ebda34f2c03b57ae9a3d5e77e42c39b2f8, tools and notes for the PS2 game SLUS-20809; no game files), the maintainer's PS2 copy (`a private game library\Godzilla - Save the Earth .zso`, read-only) and the Xbox copy (`game-owned\xbox\STE`, read-only). The pack was unzipped into `a private archive` (never into a repo), his tools were read before running and only run read-only with `python -I` on copies. Nothing of his (code, text, tables) is in our repos; what we learned is facts, credited in `CREDITS_AND_SOURCES.md` ("ANILORAK's Save the Earth modding pack: format research"). His licence excludes a named person, which is incompatible with our GPL, so his work stays his: separate mods he hosts.

## 1. Are the PS2 and Xbox files the same? (measured)
The PS2 game keeps everything in one archive `GODZILLA.VOL` (PVOL, 734,604,704 bytes, 483 entries; extracted from the .zso with our own `tools\mods\zso_extract.py`, then read with his `pvol.py`); the Xbox game has the same files loose. Comparison scripts (own code): `tools\mods\compare_ps2_xbox.py`, `compare_bundles.py`.

| Class | PS2 | Xbox | Result |
|---|---|---|---|
| Scripts `.bsf` | 162 | 162 | **152 byte-identical**; 10 differ (AttractMode, CityPurchase, ConventionalGame, fightover, GalleryPurchase, MainMenu, MonsterSelect, NetConfig, ProfileSelect, Purchase; -16 .. -1,084 bytes, platform edits; ConventionalGame differs by ONE source line, `bIgnoreTime = 1;`, PS2 only) |
| Text / config | 8 `.txt`, `attributes.xfg` | same | identical; `globals.xfg` differs (256 vs 231 bytes) |
| Bundles | 23 `.cmp` monsters, 98 `.bdp` props/UI, 8 `.clp` city chunks | 23 `.cmf`, 98 `.bdl`, 8 `.clf` | same container ("Pipeworks Bundle File version 1.3", same header, same chunk table), different file sizes (platform data); 64 file pairs matched by name for the chunk comparison |
| Bundle chunks (64 pairs) | | | **EFFECT .edf 831/835 identical**; **MONSTER_DATA 87 pairs: 52 identical, 31 same size with different numbers, 4 size differs**; ANIMATION 3,320 pairs: 1,093 identical, 2,227 same size different; SKELETON 45: 28 identical; CAMERA 22: 19 identical; RIGIDMESH / SKINNED_MESH / MATERIAL / TEXTURE / PALETTE / PWK_SOURCE: platform specific (GS/VIF packets on PS2, Xbox vertex buffers and D3D texture headers on Xbox) |
| Cities | 85 `.zip` (deflate) | 80 `.zip` + 1 | contents differ in size (PS2 `.clp` vs Xbox `.clf` chunk files inside) |
| Sound | `.mic` x39, `.samp` + `.uber` x5 | `.wxv` x39, `.samp` + `.uber` x5 | `.uber` (cue directory) same size, different bytes; `.samp` sizes differ (platform sample data); codecs differ |
| Movies | `.pss` x27 | `.xmv` | platform formats, out of scope |

Textures: of 886 texture names present in both (paired bundles) the Xbox size equals the PS2 size in 611, the PS2 is larger only by padding (multiples of 8, e.g. 448x312 vs 448x305) in 239 and the **Xbox is larger in 36, notably the monsters' main textures (e.g. Anguirus_G2_512_C: PS2 256x256 8-bit swizzled, Xbox 512x512 DXT)**. PS2 formats: PSMT8/PSMT4 swizzled, PSMCT16, PSMCT32 (his pwtex list); Xbox formats: DXT1/DXT3/DXT5, P8, ARGB32, linear UI formats. So the Xbox base textures are already at the resolution the PS2 game's file names advertise; Ultra's HD skins are replacements of both.

## 2. His tools on the Xbox files
| Tool | Result on Xbox files |
|---|---|
| `pwbundle.py` | **parses every Xbox bundle tried unchanged** (Gigan.cmf, fx.bdl, MonsterIsland0_1.clf, hud.bdl; string table, sections, chunk table) |
| `bsf.py dec` | **decompiles 161 of 162 Xbox scripts unchanged** (the 162nd, Godzilla2k.bsf, is token-stream version 1.02, which the tool refuses on the PS2 file too) |
| `mdfields.py show` | **MONSTER_DATA v24 parses unchanged** (same version word 0x10000118 and slot table) |
| `pwtex.py ls` | **fails on Xbox**: "TEXTURE main is 84 bytes, expected 260" (PS2 chunk header carries GS registers; the Xbox header is a D3D texture header) |
| `pvol.py` | PS2 only (the Xbox game has no VOL) |
| `locpack.py`, `movestat.py`, `pwrigid.py`, `meshpack.py`, `citymap.py` ... | not run (they target the PS2 data formats; `locpack` and the script/data tools are the candidates to test next) |
His `setup.py --no-map` was not run (it builds a Ghidra project; needs Java 21, Ghidra 12.1.4 and his add-on: **asking the maintainer before installing anything**); instead the disc was read with our own `zso_extract.py`.

## 3. Code-map transfer (item 2)
- His map (`program-map/script-natives.tsv`, 1,541 rows) names every script native as `Class.method` with its PS2 body address and a slot number `Class#id`. The Xbox game's native descriptors (found by scanning `default.xbe`: `{id, 0, 0x356630, parameter list, name}`) carry the same **ids and names**: **703 Xbox descriptors, 657 (93 %) match a PS2 row on method name and id** (e.g. `startRage` = native 115 = 0x73 = PS2 body 0x003CA680; `playVoice` 3, `playSound` 4, `setRageMeter` 62, `inRageMode` 39). Result: `docs\FUNCTION_NAMES_FROM_PS2.tsv` (our descriptor address, native id, the game's name, PS2 class.method, PS2 body address, confidence, method). Only game names and PS2 addresses are kept, not his invented names. The Xbox HANDLER addresses are NOT in it: the dispatch from native id to Xbox function was not located (the large pointer tables in `default.xbe` I checked are floating-point stubs, not the natives).
- His general function names (`functions.tsv`, 15,720 rows, many rule-generated) are not used: matching them to our `sub_<VA>` needs shared strings or call structure and was not done; they would add little beyond the natives for our current goals.
- Use for RAGE: `Monster.startRage` (native 115) is called by the script `StartRageMode(m)` (Game.bsf) from `onPickupPowerup` type 2 (and the type-2 pickup creates NO `Powerup_rage_grab.edf` effect, unlike the other types); no script plays an announcer line for rage, so the announcer call is in the engine, in the native; the Xbox scripts are the PS2 scripts, so the behaviour difference, if any, is in the engine or in our audio layer, not in the scripts. See `REPORT_0.2.md` 1c/1d.

## 4. Textures: his naming vs our keys (item 3)
- His packs for PCSX2 are keyed by PS2 texture-memory hashes; ours by content hash of the Xbox texture, so the keys never match. The **names do match**: both games name textures by the bundle chunk name (his `pwtex` names, our bundle chunk names), and 886 names were found in both sides of the 64 bundle pairs checked.
- Plan: (1) his HD PNGs are delivered named after the game's texture names (his `pwtex export` names) or with a CSV name->file; (2) `tools\mods\import_texture_set.py index` computes our pack key for every Xbox texture chunk straight from the game files (11,211 chunks, 11,210 with a key; **validated: 422 of 425 textures dumped by `STE_TEXDUMP` have the same key, 99 %**); (3) `import` writes `<key16>_<name>.png` + manifest + report; the loader needs nothing new (`texturepack.md`). Details: `docs\formats\texturepack_import.md`. A synthetic sample pack for testing the Mods tab: `a private archive` (generated by `import_texture_set.py sample`, 2 gradient images, no game imagery; regenerate with the command in the doc).
- Resolution caveat: a replacement larger than the game's texture works (mips are rebuilt; any size up to 8192), a non power of two or a different aspect ratio is flagged.

## 5. What could carry over, per feature class
| Feature class (Ultra) | Carry-over to our recomp | Effort | Risk / unknown |
|---|---|---|---|
| **HD textures** (monster skins, HD ground/water, UI) | Name-matched PNGs -> texture-pack mod through the importer; works today once PNGs exist | S (plumbing done) per set; the work is his PNGs | licence/hosting (his mod, hosted by him); his naming completeness; textures that exist only on PS2 (in the 64 bundle pairs: 401 texture chunk names in both, 25 only PS2, 78 only Xbox) |
| **Existing monsters re-tuned** (MONSTER_DATA numbers, moves, AI) | MONSTER_DATA v24 and the scripts are the same format; his field knowledge maps 1:1; a tuning = a patched `.cmf` MONSTER_DATA chunk or an override of values as our STEM toggles do | S-M per monster | 31 of 87 monster-data chunk pairs differ numerically between platforms already (Xbox tuning differs from PS2); need his field notes |
| **New monsters (8)** | roster entries need models, skeletons, animations, textures, MONSTER_DATA, scripts: models/animations are platform specific (GS vs Xbox vertex data; animation chunks differ in 2/3 of the cases) | L per monster (a model converter PS2 -> Xbox vertex format, or his source models if he has them in a neutral format) | skeleton/bone limits (known from Unleashed work), roster/select-screen slots |
| **Scripts, menus, new modes** | 152 of 162 scripts are identical and 161 decompile with his tool; his new modes written as BSF scripts should run on Xbox if they only use natives that exist on both (657 of 703 matched; the rest to be listed); our mod system can drop BSF files (`mod_patch.md`) | S-M per mode | natives only on one platform; PS2-only script lines (`bIgnoreTime`) |
| **Rebuilt / larger cities** (Tokyo 12 districts, New York ~760 buildings, Monster Island, 4,096 objects per city) | city data is in the `.clf` chunk bundles (Xbox) vs `.clp` (PS2); his `citymap`/`citywrite` tools are PS2-format; an Xbox equivalent must be written or his city sources used | L | engine limits in OUR build are unmeasured: object arrays, the render list query volume (we already see the pushbuffer ring at 52-73 % in Tokyo), memory (his PS2 patch needs 128 MB; the Xbox has 64 MB, our recomp can map more but the guest allocator is sized for 64 MB), collision (`.clf` collision chunks), draw cost at 1080p60 |
| **Seattle (from Unleashed)** | out of scope: assets from another game must not go into our repos or releases | - | - |
| **Code features** (his `.pnach` patches, 8 monsters' unlocks, load-pool patch etc.) | MIPS patches: re-implement the behaviour in our hooks per feature; his notes name the addresses on the PS2 side, ours need the Xbox side found (natives map helps for script-visible ones) | S-M each | the memory/object-pool changes have no direct Xbox equivalent |
| Sound | PS2 SPU samples vs Xbox sample banks: separate conversion | M | codecs differ |

## 6. What to ask ANILORAK
1. **HD textures as PNGs named exactly after the game's texture names** (his `pwtex` names), plus the list of which textures he replaced and the intended size, in a form he hosts himself; and, if he has it, the name <-> PCSX2-hash table (so his existing DDS packs can be renamed by tool).
2. His **format notes** for MONSTER_DATA fields (`mdfields` schema), the script natives (what each does), the bundle chunk types, and the sound bank layout, for our independent Xbox code (credit line agreed).
3. For maps: how a city is stored (file/chunk layout), the **object limits he found and where** (which arrays/pools the 4,096 objects change and by how much), and whether his rebuilt maps exist in a neutral source form (not PS2-specific data).
4. For new monsters: whether the 8 monsters exist as source models/animations in a neutral format, or only as PS2 data.
5. A statement of how he wants to be credited and how the packs are to be hosted/linked (his mods stay his: separate downloads, never in our repos or releases), and whether he is happy to be listed on our site.
6. Whether he can test our recomp (it is an alpha; Xbox version on PC) with a texture set so the first end-to-end test is his.

## 7. Next steps on our side (no code in the game needed for the first two)
1. Run the importer on a first real HD set from him (`texturepack_import.md`) and check in game (anisotropy, mips, alpha).
2. Test `locpack`, `movestat` and `unitadd`-style readers on Xbox data (read-only) to extend the "parses unchanged" table; list the 46 natives without a PS2 match.
3. Measure our object/pool limits (a synthetic city with many objects) before promising 4,096-object cities.
4. Locate the Xbox native handler table (needed to turn the natives map into function names for our `sub_<VA>`).
