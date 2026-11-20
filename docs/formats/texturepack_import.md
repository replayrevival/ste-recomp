<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Importing an outside texture set into a texture pack (STE recomp, 0.2.1+)

Tool: `tools\mods\import_texture_set.py` (own code, Python 3.9+, Pillow). It reads the user's own game files read-only and writes only into the folders you name. Pack layout and loader: `docs\formats\texturepack.md`; manifest: `docs\formats\mod_manifest.md`.

## What an outside set must look like
- Replacement images are **PNG, 8-bit** (RGB or RGBA; the tool writes RGBA). Any size up to 8192 per side; the renderer builds the mip chain itself, so supply level 0 only.
- Same **aspect ratio** as the game's texture, normally a power of two (the game's own textures are power-of-two except the linear UI textures, which have odd sizes such as 448x305). A different aspect ratio or a non power of two size is written but flagged `size-mismatch` (it will be stretched onto the game's UV space). 2x / 4x / 8x of the game size is the intended use.
- **Alpha** is kept (DXT3/DXT5 and ARGB game textures carry alpha: keep it; DXT1 textures have 1-bit alpha at most).
- **Names**: the PNG file name (without .png) is the game's texture name, e.g. `Gigan_G2_512_C.png`; the match is case-insensitive and an `_anything` suffix is tolerated (`Gigan_G2_512_C_hd.png`). Alternatively a `mapping.csv` with `name,file` rows. The game's names are the texture chunk names inside its bundle files (`.cmf` monsters, `.bdl` props/UI, `.clf` city chunks, also inside the `bundle\*.zip` files). A name that exists in more than one bundle maps to every distinct key (`ambiguous`, all are written).

## How a name becomes a pack key
The renderer keys a replacement by the content of the texture it uploads: `key = mix(hash(level-0 texel bytes) ^ hash(palette, 1024 bytes, 8-bit palette textures only) ^ D3D format word * 0x9E3779B97F4A7C15 ^ width<<48 ^ height<<32)` (`texpack_key()` in `kelvin_texpack.c`). Every texture chunk of an Xbox bundle is a D3D texture header (20 used bytes: Common, Data, Lock, **Format**, **Size**) plus the texel data, so the key can be computed offline from the game files alone:
- `Format` bits 8-15 = colour code (0x0C DXT1, 0x0E DXT3, 0x0F DXT5, 0x06/0x07 ARGB32, 0x0B P8, ...), bits 16-19 = mip count, bits 20-23 / 24-27 = log2 width / height (swizzled textures);
- linear textures (colour codes 0x10, 0x11, 0x12, 0x1D, 0x1E ...: UI/menus) take width, height and pitch from `Size` (bits 0-11 = w-1, 12-23 = h-1, 24-31 = pitch/64-1);
- level-0 byte count = width*height*bytes-per-pixel (DXT1 0.5, DXT3/5 and P8 1, 16-bit 2, 32-bit 4) or pitch*height for linear textures.
**Validation (2026-11-19):** the keys the tool computes for the 11,211 texture chunks of the whole game match 422 of the 425 textures dumped by `STE_TEXDUMP` in earlier runs (99 %; the 3 others are not bundle textures). A run's own `index.tsv` (from `STE_TEXDUMP`) can be used to double-check any pack.

## Commands
```
python -I tools\mods\import_texture_set.py index  <Xbox game folder> xbox_texture_index.csv
python -I tools\mods\import_texture_set.py import <folder of PNGs | mapping.csv> xbox_texture_index.csv <out pack dir> --id my-hd-set --name "My HD set" --credits "author, source, licence"
python -I tools\mods\import_texture_set.py sample xbox_texture_index.csv <out pack dir>      (synthetic gradient images for 2-3 textures: for testing the Mods tab; no game imagery)
```
`import` writes `<key16>_<name>.png` files, `manifest.json` (`profile: "textures"`, `enabled`, `priority`, `credits`) and `report.csv` (one line per input: `matched`, `size-mismatch`, `ambiguous`, `unmatched`, `no-key`). Put the pack folder under `<exe dir>\texturepacks\` (or install it through the launcher's Mods tab); `STE_TEXPACKS_ENABLE=<id>` forces it on for a test run.

## Packaging as a mod
The manifest must carry `credits` (source and licence of every imported texture); packs are separate mods hosted by their authors and are never part of the repo or of a release (no game data, no third-party assets). A pack that holds only the author's own PNGs named by game texture name is fine to share: it contains no Xbox data.

## What is not covered yet
- PS2 / PCSX2 hash names (`<hash>.dds` replacement sets) cannot be keyed directly: those hashes are of the PS2's texture memory; the route is **name based** (the PS2 and Xbox games share the texture names): see `docs\COLLAB_GODZILLA_ULTRA.md`.
- Cubemaps and volume textures are indexed (they have keys) but their replacement semantics are untested.
- Texture chunks in the `New_*` city bundles that share one name across day/night variants map to several keys (`ambiguous`).

## End-to-end proof (2026-11-19, c430)
Two synthetic checkerboard PNGs named `Gigan_G2_512_C` and `Gigan_G2_512_B` went through `import_texture_set.py import` (both `matched`, keys 859899229af9d2ce / 31710ceea1a8eab8) into a pack; a Tokyo Godzilla 2000 vs Gigan fight with `STE_TEXTUREPACKS=<folder>` logged `[TEXPACK] gigan-synthetic ... ACTIVE ... 2 texture file(s)`, `replacement textures uploaded so far: 2`, and Gigan is drawn with the checkerboard in the recorded frames (F:\GodzillaSTE_work\clips_try\texpack_gigan.mkv, not in a repo). No crash. So name -> offline key -> pack -> game works.

## Getting the original textures out: `export` (0.2.2)
Requirements: Python 3.9+ and Pillow (`pip install Pillow`); Windows paths work as written. Offline, read-only, from the user's own game folder or Xbox disc image (.iso, plain or redump-style); never writes into the game.

    python -I tools\mods\import_texture_set.py export <game folder or .iso> <out folder> --monster Gigan
    python -I tools\mods\import_texture_set.py export <game folder or .iso> <out folder> --match Gigan,Mothra
    python -I tools\mods\import_texture_set.py export <game folder or .iso> --list --match Gigan      (names only, writes nothing)
    python -I tools\mods\import_texture_set.py export <game folder or .iso> x --monster ?            (monster names)

- `--match` = comma-separated words, case-insensitive substring of the texture name. `--monster NAME` = every texture in that monster's own bundle plus its HUD bundle. Plain `--match Gigan` misses the textures that do not carry the name (`sawblade`, `atomicbreath`, `hornkatana`, `gigan_cubemap`), so prefer `--monster`.
- Monsters in the game: Anguirus, Baragon, Biollante, Destoroyah, Ebirah, Ghidorah, Gigan, Godzilla2k, Godzilla90s, JetJaguar, MechaGodzilla2, MechaGodzilla3, Megaguirus, Megalon, MGhidorah, MOGUERA2, Mothra, Orga, Rodan, SpaceGodzilla.
- Output: `<Name>.png` (RGBA, level 0), exactly the name `import` matches, plus `export_report.csv` (name, size, format, file, status, reason). If one name has different pixels in several bundles, each is written as `<Name>__<bundle>.png`; importing any of them under the base name replaces every variant. Identical copies in several bundles are written once (the report lists where else they are).
- Skin textures of a monster: `<Monster>_G2_512_C` = colour, `<Monster>_G2_512_B` = bump/normal map. The retail monster bundles hold ONE skin set per monster: there are no separate far-LOD texture names in the Xbox game (the 21:9 / far-LOD pair reported earlier is not a naming scheme of the retail files).
- Formats decoded (incl. de-swizzling): DXT1/3/5, A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, X1R5G5B5, L8, AL8, A8, A8L8, P8 (with palette), and the linear (LIN_*) variants. Anything else is listed as skipped with the reason.
- Linear (LIN_*) textures such as the 132x204 name plates are not power-of-two: `import` flags them "size-mismatch" but writes them; keep the same size when editing.
- Friendly messages instead of tracebacks: Pillow missing, folder/iso not found or not an Xbox image, no bundles found, no match (with the closest names).

## `import` takes the game directly (0.2.2)
`import <edits folder> <index.csv | game folder | game .iso> <pack folder>`: with a folder or .iso the tool reads the game's texture list itself (about 10 s); the separate `index` step is only needed to reuse an index.csv. After export and import the tool prints the next step (install the pack folder in the launcher's Mods tab).
