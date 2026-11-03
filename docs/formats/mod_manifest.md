# mods\ folder and mod manifest (STE recomp, api 1)

Implementation: `tools\xboxrecomp-v014k\src\kernel\kernel_mods.c` (hooked into `xbox_translate_path` in kernel_path.c). Design follows the launcher survey in
`<local-folder>/` (JSON manifest per mod, explicit load order file, dependency/conflict validation, dry-run report, no silent last-write-wins).

## Layout (next to the exe, or STE_MODS_DIR)
```
mods\
  order.json                 optional: load order + enable flags
  last_report.txt            written at every start: the dry-run report
  <any folder name>\         one mod (folders starting with . or _ are ignored)
    manifest.json            required
    ActionData\...           files at GAME-RELATIVE paths (same names/case as in the game dir)
    ShellData\...
    bundle\...
    textures\...             (Q3 texture packs, see below; ignored by the file override)
    README.md, manifest.md   ignored by the loader
```
A file `mods\<folder>\<rel>` replaces the game file `<game dir>\<rel>` for every READ open of that path. The game directory is never written; opening a mod file for write is refused
(`[MODS] write open refused`). Files that do not exist in the game dir are reachable by exact name only (directory enumerations of the game dir do not list them) and are reported as `NEW` in the dry run.
Switches: `STE_MODS=0` disables everything; `STE_MODS_DIR=<dir>` moves the root; `STE_MODS_ENABLE=id1,id2` (tests/launcher) enables ONLY those ids.

## manifest.json
| field | type | required | meaning |
|---|---|---|---|
| id | string | yes | unique, `author.mod-name` style; used in order.json, dependencies, conflicts |
| name | string | no | display name |
| version | string | no | semver-style |
| api | number | no | manifest schema version (1) |
| game | string | no | `"STE-Xbox"` |
| game_hashes | string[] | no | `"sha256:<hex>"` of the game's `default.xbe`; if present and none matches the running game the mod is SKIPPED. STE retail Xbox: `sha256:6966cc8a3a22d420d05362f24f3e68d7a462c481861189855f05583c0d6de500` |
| profile | string | no | free text: `content`, `gameplay`, `texture`, `ui` |
| priority | number | no | default load position when order.json does not list the mod (lower loads earlier, later wins); default 100 |
| enabled | bool | no | default true; order.json `enabled` map and `STE_MODS_ENABLE` override it |
| dependencies | string[] | no | ids that must be ACTIVE and load before this mod, else the mod is skipped |
| optional_dependencies | string[] | no | informational (load after them when present) |
| conflicts | string[] | no | ids that must not be active; the LATER mod in load order is skipped |
| authors | string[] | no | |
| credits | object[] | no | `{name, role, source, license}` for every imported resource (also record it in docs\CREDITS_AND_SOURCES.md) |
| license | string | no | licence of the mod's own files |
| description | string | no | |
| files | string[] | no | informational list of provided game-relative paths |
| textures | object | no | Q3 reserved: `{"format": "q3-hash", "path": "textures/"}` -- the texture pack index (PNG/DDS keyed by the content hash of the guest texture data + format + size, see HD_TEXTURE_MAPPING.md) so a pack can ship files and textures together |

## order.json
```json
{ "api": 1, "order": ["ste.roundtrip-control", "stem.arena-scripts"], "enabled": {"stem.arena-scripts": true, "ste.roundtrip-control": false} }
```
Listed ids load first, in that order; the rest follow by `priority` then id. Later mods win a path conflict; every overlap is listed in the report.

## Dry-run report (stderr `[MODS]` lines and mods\last_report.txt)
```
[MODS] root ...\mods : 4 mod(s) found
[MODS] ste.roundtrip-control 1.0.0 (roundtrip_control) ACTIVE, load position 1:
[MODS]   override bundle\Godzilla2k.zip  <- ste.roundtrip-control
[MODS]   = 2 override(s), 0 new file(s)
[MODS] x.y 1.0: SKIPPED - game hash mismatch (default.xbe sha256 ...)
```
and during play `[MODS] open <rel> -> <mod file> (mod <id>)` for each redirected open (first 200).

## Title file cache and mods (c124)
The title does not read its loose script/data files from the disc every run: on first use it copies them into its own cache (guest `\Device\Harddisk0\Partition5\cache_<N>.bin`, host `<save dir>\Cache\cache_<N>.bin`;
N is the index of the file in the sorted game directory, e.g. cache_312.bin = ShellData\attributes.xfg, cache_101.bin = an ActionData arena script) and afterwards reads the cache copy.
A mod override of such a file is therefore invisible once the cache exists. Fix: while any override is active the cache directory is `Cache.mods_<hash>` (hash over every override path + size + mtime, `[MODS] cache dir tag ...`),
so each distinct mod set has its own cache which is rebuilt from the (overridden) disc files on first run; the original `Cache\` is never touched. Files read straight from the disc (bundle\*.zip, ShellData\*.bdl, movies) are overridden directly.
Debug: `STE_MODS_TRACE=1` logs every game-dir path the title opens (`[MODSTRACE]`).
