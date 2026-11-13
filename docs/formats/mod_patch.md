<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Mods as patches (mod manifest api 2, item 5) - v1, approved 2026-11-12

Goal (owner decision, release decisions 3): a mod never contains game data. A content mod is a set of binary patches that turn the user's own original file into the modded file. The project ships no mods; modders host them; the launcher and the game verify and apply them.
Status: specification only. Nothing in the exe or launcher implements it yet. It extends `mod_manifest.md` (api 1, whole-file overrides stay valid and unchanged, for mods whose files are the modder's own work: scripts, textures made from scratch, text).

## 1. Rules
1. **Identify, never guess.** A patch names the ORIGINAL file it applies to by game-relative path and by SHA-256 (`base_sha256`), and states the SHA-256 of the result (`result_sha256`). If the user's file does not hash to `base_sha256` the patch is not applied (status `BASE MISMATCH`, shown with both hashes); if the result does not hash to `result_sha256` the output is discarded (status `RESULT MISMATCH`). No "best effort" application.
2. **Never touch the originals.** Patching reads `<game dir>\<rel>` (read-only, the game-owned rules apply) and writes only into the mod's own folder: `mods\<id>\_built\<rel>`. The loader treats `_built\` as the mod's file tree (same as files placed directly in the mod folder; a real file in the mod folder wins over `_built`). `_built\` is derived data: it can be removed and rebuilt at any time, and is never published. The game folder, `saves\` and the original `Cache\` are not written.
3. **Whole file in, whole file out.** Patches are per file (a game file can be a zip bundle, a .cmf, a .bdl, a script). Patching INSIDE a bundle is the modder's tool's business; the patch sees bytes.
4. **Idempotent and cached.** `_built\state.json` records, per target, `base_sha256`, `result_sha256`, patch SHA-256 and the patch tool version. Start-up (or the launcher's "Prepare mods" step) rebuilds only entries whose inputs changed; the dry-run report lists the state of every patch.
5. **Licence.** The patch container and the apply code must be GPL-2.0-or-later compatible, not Apache-2.0-only (so no zstd-only/Apache libraries are required). The patch files themselves are the modder's work and carry the modder's licence; they contain only differences, but a modder must not embed original data that does not change (see 3.3: the format has no "insert original bytes" shortcut other than COPY, which refers to the user's file).

## 2. Manifest additions (api 2)
`api` becomes 2 in a manifest that uses patches. A game/launcher that only knows api 1 skips such a mod with `SKIPPED - needs manifest api 2` (it must not load a half mod).

| field | type | required | meaning |
|---|---|---|---|
| `patches` | object[] | for patch mods | one entry per patched file |
| `patches[].target` | string | yes | game-relative path of the original, same spelling/case as in the game dir (`bundle\MechaGodzilla3.cmf`) |
| `patches[].base_sha256` | string | yes | `sha256:<hex>` of the original file this patch was made against |
| `patches[].result_sha256` | string | yes | `sha256:<hex>` expected after patching |
| `patches[].patch` | string | yes | path of the patch file inside the mod folder (`patches\MechaGodzilla3.cmf.step`) |
| `patches[].patch_sha256` | string | no | integrity of the patch file itself |
| `patches[].format` | string | no | `step1` (default, section 3) or `bsdiff40` (section 4) |
| `patches[].size` | number | no | expected size of the result in bytes (cheap pre-check) |
| `patch_tool` | string | no | informational, e.g. `step-diff 1.0` |
| `game_hashes` | string[] | recommended | as in api 1; patches are additionally tied to the files by `base_sha256`, so a mod made for another release fails per file, not silently |

Example:
```json
{ "api": 2, "id": "someone.mg3-longer-rockets", "name": "MG3 longer rockets", "version": "1.0.0", "game": "STE-Xbox",
  "game_hashes": ["sha256:6966cc8a3a22d420d05362f24f3e68d7a462c481861189855f05583c0d6de500"],
  "profile": "gameplay", "license": "CC-BY-4.0",
  "patches": [ { "target": "bundle\\MechaGodzilla3.cmf", "base_sha256": "sha256:...", "result_sha256": "sha256:...", "patch": "patches\\MechaGodzilla3.cmf.step" } ] }
```
A mod can mix `patches` and plain files; two mods that patch the same target conflict unless the second one's `base_sha256` equals the first one's `result_sha256` (patches are then chained in load order; otherwise the later mod is SKIPPED with `CONFLICT: patches the same file`).

## 3. Patch format `step1` (our own, simple, no third-party code)
Little-endian. Whole file:
```
offset  size  field
0       8     magic "STEPATC1"
8       8     base_size        (u64) size of the original
16      8     result_size      (u64)
24      32    base_sha256      (raw bytes; must equal the manifest entry)
56      32    result_sha256
88      4     op_count         (u32)
92      4     flags            (u32) bit0: op stream is zlib-compressed (deflate, RFC 1950); others 0
96      ...   ops (compressed as a whole when flags bit0)
end-4   4     crc32 of everything before it
```
Ops, each `u8 kind` followed by varint (LEB128) fields:
| kind | name | fields | effect on the output |
|---|---|---|---|
| 0 | COPY | `src_off`, `len` | append `len` bytes of the ORIGINAL starting at `src_off` |
| 1 | ADD | `src_off`, `len`, then `len` signed delta bytes | append original[src_off+i] + delta[i] (mod 256); this is the bsdiff idea that makes small float/int edits in otherwise similar data cheap |
| 2 | INSERT | `len`, then `len` literal bytes | append new bytes (these are the modder's data) |
The result must be exactly `result_size` bytes; any op that reads outside the original or overshoots is an error (`BAD PATCH`). The decoder is about 80 lines of C and has no allocation beyond the output buffer; it checks sizes before reading, so a damaged or hostile patch cannot read or write outside its buffers. The encoder (`step-diff`, tools\, planned) uses a suffix-array matcher; it is a modder tool, not part of the game.

Why not xdelta3 (Apache-2.0)? It would make the release binaries GPL-3.0-only (CLAUDE.md licence rules). Why not bsdiff only? It is fine as an interchange (section 4) but the classic container needs bzip2 and has no integrity fields; `step1` carries both hashes in the header.

## 4. Optional `bsdiff40`
The classic BSD-2-Clause bsdiff 4.3 format (`BSDIFF40`, three bzip2 blocks) is accepted when `format` says so, decoded by a clean-room reader (bzip2 is BSD-style licensed, compatible). The hash checks of section 1 are done by the loader from the manifest entry, since this container has none.

## 5. Where it runs
* **Launcher (recommended):** "Prepare mods" step before launch; it reads the user's verified game folder, checks `base_sha256`, writes `_built\`, shows per-patch status (OK, BASE MISMATCH with the two hashes, RESULT MISMATCH, BAD PATCH, SKIPPED). It passes nothing extra to the game: the game just sees files in `mods\<id>\_built\`.
* **Game (fallback, planned):** `kernel_mods.c` runs the same decoder at start when `_built\state.json` is missing or stale, so mods also work without the launcher. Output goes only to `_built\`; failures are `[MODS]` report lines, never an exit.
* The title's file cache rule from `mod_manifest.md` applies unchanged (the cache tag hashes override paths + sizes + mtimes of `_built\` files too).

## 6. Dry-run report lines (additions)
```
[MODS] someone.mg3-longer-rockets 1.0.0 ACTIVE, load position 2:
[MODS]   patch bundle\MechaGodzilla3.cmf: base OK, built OK (3,412,480 bytes, cached)
[MODS] other.mod 1.0: SKIPPED - patch bundle\Godzilla2k.cmf: BASE MISMATCH (found sha256:ab12..., wanted sha256:cd34...)
```

## 7. Decisions (owner, 2026-11-12) - all as recommended
1. **One patch per game file.** A mod lists as many patches as it needs; each is verified against its own original (SHA-256 of the original before, SHA-256 of the result after).
2. **Optional zlib compression** of patch data (zlib licence, GPL-compatible).
3. **Our own patch-making tool ships in the repo** (`tools\step-diff`, GPL-2.0-or-later). To be built with item 5; Biollante (restoring her from the on-disc files as her own roster slot) is the first real test case.
4. Hash algorithm is SHA-256 as everywhere else in the project.
