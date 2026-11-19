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
