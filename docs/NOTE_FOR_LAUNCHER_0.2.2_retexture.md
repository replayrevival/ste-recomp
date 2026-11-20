# Note for the launcher session: retexture path (0.2.2)
What changed (see docs\formats\texturepack.md and texturepack_import.md):
1. `import_texture_set.py export <game folder|.iso> <out> --monster Gigan` (or `--match`, `--list`, `--monster ?`): original textures as `<Name>.png` + export_report.csv. Retail monsters have ONE skin set (`_C` colour, `_B` bump), no separate far-LOD textures.
2. `import <edits> <game folder|.iso|index.csv> <pack folder> --id ... --name ...`: no separate index step; prints the next step.
3. Mods-dir texture packs (candidate c431): the game scans STE_MODS_DIR for `"profile": "textures"` mods, follows order.json (enabled/order), applies them after <exe>\texturepacks, later wins; no restart-time setting needed. Log `[TEXPACK] mod <id> ACTIVE ...`; last_report.txt gets `texture pack <id>: ACTIVE, K file(s)`.
Example: export Gigan, edit Gigan_G2_512_C.png, `import gigan_edits "<game folder>" gigan_pack --id my-gigan --name "My Gigan"`, install gigan_pack in the Mods tab, enable, start the game.
Capability for the launcher: "texture_mod_packs" (candidate c431 onward). No ini key added.
