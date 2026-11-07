# Texture packs (HD texture replacement) -- STE recomp, c128

Status: implemented in the Kelvin translator (`src/nv2a/kelvin_texpack.c`, hooked in `get_texture()` of `nv2a_kelvin.c`). Credit: stb_image v2.28 (public domain / MIT, Sean Barrett) decodes PNGs.

## Layout
```
<exe dir>\texturepacks\<any folder>\manifest.json        mod manifest (docs\formats\mod_manifest.md), "profile": "textures"
<exe dir>\texturepacks\<any folder>\<key16>.png          one replacement per texture; sub-folders (3 levels) are scanned too
```
`<key16>` = the 16 hex digits of the texture key (below), optionally followed by `_anything` (`0123456789abcdef_sky.png`). PNG only (8-bit RGB/RGBA); convert DDS with tools\mods\texmatch.py / Pillow.
Manifest keys read by the loader: `id`, `enabled` (true/false), `priority` (lower first, later wins), `version`, `name`. Every pack must carry `credits` (source + licence of each imported texture); the loader prints `[TEXPACK] <id> <version> (<name>) ACTIVE ...` per active pack.

## Texture key (stable identity)
`key = mix(content_hash ^ format*0x9E3779B97F4A7C15 ^ (w<<48) ^ (h<<32))` with `content_hash = hash_bytes(guest texture bytes at level 0, tex_geometry size) ^ hash_bytes(palette, 1024)` (palette textures only), `mix(k) = k ^= k>>29; k *= 0xBF58476D1CE4E5B9; k ^= k>>32`, `hash_bytes` = the 4-lane FNV-style hash in nv2a_kelvin.c. It does not depend on the guest address, so it is the same across runs and arenas. Changing `hash_bytes` or `texpack_key` invalidates every pack: do not.

## Dump mode
`STE_TEXDUMP=<dir>` (or `[video] texture_dump=<dir>` in ste_video.ini) writes every texture the renderer decodes, once per key, as `<dir>\<key16>.png` (full RGBA, as decoded) and appends `index.tsv` (key, width, height, format word, first frame, guest offset). One Tokyo run (menus + fight) = ~2,650 textures, ~1 GB. Existing files are not overwritten.

## Replacement
When a texture is (re)decoded its key is looked up; a hit loads the PNG (BGRA, any size up to 8192), builds a full mip chain (box filter) and uploads it instead. Shader texture coordinates stay in the original texture's space (normalised, or texel-scaled with the ORIGINAL width/height for linear textures), so HD replacements of any size sample correctly. Render targets and cube maps are not replaced.
Env: `STE_TEXTUREPACKS=<dir>` (folder of packs), `STE_TEXPACKS_ENABLE=id1,id2` (overrides the manifests' `enabled`), `STE_TEXPACKS=0` (off).

## Quality keys (ste_video.ini [video], env STE_VIDEO_ANISOTROPY / STE_VIDEO_MIP_BIAS)
- `anisotropy` = 0 | 2 | 4 | 8 | 16 (default 0 = original behaviour). `mip_bias` = float, default 0 (original), clamped to -2..2 (negative = sharper). Both apply to WORLD draws only (not the front-end, not the screen-space UI vertex program 17). Either one turns on mip chains for all uploaded textures and trilinear mip selection.
