# Build recipes: mods that are built on the player's PC (manifest api 2, `format: "bsf-edit"`)

Some features change game scripts (Solo Destruction, later Biollante's roster/script changes). The compiled script bytes are game
data, so they must never be published. A *recipe mod* publishes only the edit list; the player's PC builds the files from the
player's own game during **Prepare mods**, and the result is verified by hash.

## Manifest entry
```json
{ "target": "ActionData\\Game.bsf", "format": "bsf-edit", "patch": "edits\\ActionData__Game.bsf.json",
  "base_sha256": "sha256:...", "result_sha256": "sha256:...", "size": 12345 }
```
`edits\*.json` = `{"format": "bsf-edit", "edits": [{"old": "<source snippet>", "new": "<source snippet>"}, ...]}`. Snippets are
short pieces of script source (tokens, whitespace ignored). `old` must match exactly once in the player's script, else the build
fails with "script does not match what this tool expects ... Is this the original Xbox game?". There is no other data: the mod
folder is small (about 85 KB for Solo Destruction), has no game bytes and may be shared.

## Before and after Prepare
- Before: the mod folder has `manifest.json` + `edits\` and no `_built\`. In the Mods tab show it as "needs build" (recipe entries
  have `format: "bsf-edit"`; `_built\<target>` missing).
- Prepare: `python -I iso_to_mod.py prepare <mod folder> <game folder or .iso>` writes `<mod>\_built\<target>` for every entry. The game
  reads `_built\` only when its hash equals `result_sha256` (same rule as patch mods, docs\formats\mod_patch.md).
- Exit codes: 0 = everything built; 2 = at least one file failed (messages below); 1 = bad command line / usage text.
- Messages (one line per file, printed to stdout): `<target>: built` / `<target>: not in your game, skipped` /
  `<target>: BASE MISMATCH (your file differs from the one the mod was made for)` / `<target>: script does not match what this tool expects
  (N matches for: ...). Is this the original Xbox game?` / `<target>: RESULT MISMATCH`.
- Time: under 1 second for Solo Destruction (3 scripts). A patch-mod entry for a big file takes a few seconds; zip-of entries
  rebuild a zip (seconds). Show a spinner, not a percentage.

## Tooling and requirements
Our own tools, standard-library Python 3.9+ only: `tools\mods\iso_to_mod.py` (prepare/make/diff) and `tools\mods\bsf_edit.py`
(script editor, docs\formats\bsf_script.md). No third-party tool (GZBuildr etc.) is used at install time. The launcher cannot assume
Python: either ship the two files with an embedded Python / a frozen exe, or call `prepare` through a bundled interpreter.

## Making a recipe mod (authors)
`python -I tools\mods\build_solo_destruction.py <game folder or .iso> <out mod folder>` shows the pattern (edits are applied once to
the author's own copy to compute `result_sha256`; the edit lists are written with `iso_to_mod.write_recipe_mod`).

## More entry formats (0.4)
- `format: "text-edit"`: `patch` = `edits\<name>.json` with `{"format":"text-edit","edits":[{"old","new"}]}`; each `old` must occur exactly once in the original file's text (latin-1); used for `ShellData\attributes.xfg`.
- `format: "builtin"`: `patch` = `builtin:<module>`; the result is `<module>.build(original bytes)` from the tools folder. Only the modules named in `iso_to_mod.BUILTINS` (now `bio_ui`) are accepted, a manifest cannot name any other module. Used where the build generates data (the Biollante grid tiles) and an edit list cannot express it. Same hash rules as the other formats (`base_sha256` / `result_sha256`).
- Example: `tools\mods\build_biollante.py <game> <out mod folder>` writes the Biollante recipe mod (3 edit lists + one builtin); it needs `pw_bundle.py`, `bio_ui.py`, `texlib.py`, `bsf_edit.py` next to `iso_to_mod.py`.
