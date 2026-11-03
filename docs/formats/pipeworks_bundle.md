# Pipeworks bundle format

Credit: Digitzaki / GZBuildr (`GZ_3.0.2`, `GZ_DAMMSDAY`). This is a working format note, not a complete proprietary specification.

## Header

- Bytes `0x00..`: ASCII magic `Pipeworks Bundle File version 1.3` or `version 1.4` (observed; parser checks the first nine bytes for `Pipeworks`).
- `0x2C`, 16-bit endian probe: GZBuildr reads it little-endian and treats zero as big-endian (from GZBuildr source). This identifies the Wii big-endian form in the observed v1.4 bundles (observed).
- `0x34`, 32-bit string-table offset (from GZBuildr source).
- `0x62`, 16-bit file/TOC entry count (from GZBuildr source).
- `0x64`, 32-bit metadata-table offset (from GZBuildr source).
- `0x68`, 32-bit main-data offset (from GZBuildr source).
- `0x70`, 32-bit resource-data offset (from GZBuildr source).
- Other bytes between the version text and these offsets are retained by GZBuildr but are not assigned a stable meaning here (inferred).

## Directory / entry tables

- The TOC begins at `0x78` and has `0x12` bytes per entry (from GZBuildr source).
- TOC offsets: `+0x00` file number (`u16`), `+0x02` main-data relative offset (`u32`), `+0x06` main-data size (`u32`), `+0x0A` resource relative offset (`u32`), `+0x0E` resource size (`u32`) (from GZBuildr source).
- Main entry data address is `main_data_offset + relative_offset`; resource data uses `resource_data_offset + relative_offset` (from GZBuildr source).
- Metadata has `0x10` bytes per file number (from GZBuildr source). Its known fields are file number at `+0x00` (`u16`), type at `+0x02` (`u8`), and string ID at `+0x04` (`u32`); remaining metadata bytes are preserved but not semantically decoded (from GZBuildr source / inferred).
- A nonzero resource size creates a second logical `.resource` entry (from GZBuildr source).

## String tables

- At `string_offset`, a 32-bit count is followed by `count` 32-bit relative string offsets (from GZBuildr source).
- GZBuildr reads these string-table count/offset integers little-endian even when the bundle data/TOC is big-endian (from GZBuildr source).
- Each string address is `string_offset + relative_string_offset`; strings are NUL-terminated printable ASCII names (from GZBuildr source).
- The string table occupies the region before `metadata_offset` (from GZBuildr source).

## Known entry types

| Type | Meaning | Status |
|---:|---|---|
| 2 | `MONSTER_DATA` / `Character_Data` | observed; source names `2/character_data` and `2/000monster_data` |
| 3 | skeleton | observed in parsed character bundles |
| 4 | animation | observed in parsed character bundles |
| 6 | material / map-related data | observed; exact subtype is inferred from names |
| 9 | texture | observed in parsed bundles |
| 13 | palette | observed in community tooling / inferred from extension mapping |
| 16 | PWK data | observed from `.PWK` names |
| 17 | mesh | observed from mesh names |
| 20 | EDF effect | from GZBuildr source; parser appends `.edf` when needed |
| 22 | newer v1.4 data type | observed in Unleashed Wii v1.4; exact semantic subtype not established |
| 0, 1, 5, 7, 8, 10–12, 14–15, 18–19, 21, 23+ | other/reserved or title-specific types | observed where present; meanings not established |

The type-directory prefix in extracted names is decimal (from GZBuildr source). Type meanings beyond the explicitly identified rows are not inferred as certain.

## Endianness and versions

- Xbox and PS2 Pipeworks bundles use little-endian numeric fields; this is observed in STE/PS2 parsing and is the parser default (observed / from GZBuildr source).
- Wii Unleashed bundles use big-endian numeric fields; GZBuildr detects this and successfully parsed Wii v1.4 character bundles (observed).
- STE Xbox data observed here uses the `version 1.3` lineage; Unleashed Wii character bundles observed here use `version 1.4` (observed).
- v1.4 is not merely a byte-order rewrite: Wii bundles contain type 22 and different entry sets/sizes, while common type numbers retain their meanings (observed).
- Exact differences in padding, reserved header fields, and newly assigned types remain incomplete (inferred).

## Editing rules learned from the Xbox ControlRoom.bdl mod work (2026-10-23, STE Xbox c126; evidence in docs\community\Q2_MODS_AND_BIOLLANTE.md)
- The title takes a bundle's byte size from the directory listing of the game folder (NtQueryDirectoryFile), not from the file: a mod file larger than the disc file is read truncated unless the directory entry reports the new size (runtime fix c126). Bundles are read sequentially in 32 KB chunks.
- Entry metadata (16 B) = [u16 file number][u16 type][u32 NAME ID][u32 1][u32 0, or the byte size for text entries]. The NAME ID is the entry's index in the bundle's name table, which is sorted case-insensitively (lowercase).
- **Do not insert new names in the middle of the name table.** GZBuildr remaps the ids of every later name when you do; four inserted bitmap names in the middle made the title crash at match start (resource lookup by id/type, garbage bucket pointer), while one inserted name worked and the SAME four names placed at the END of the sorted table (prefix `Zz_` so they still sort last) worked. Likely cause: ids of existing names are referenced from data GZBuildr does not patch (it only patches type-2 text). Safest rule for added entries: choose names that sort after every existing name.
- Resource-less TOC entries carry a running resource cursor in their resource-offset field (the original keeps it; GZBuildr zeroes it): harmless in tests. Padding the file with trailing bytes, rebuilding without changes, and inserting a text entry are all harmless.
- The nine 32-byte pool records after the TOC (header words 0x3C..0x5C) hold the end of the resource section (and +0x40); leaving them stale is also harmless in the crash test (the variant without updated pool records crashed for the same reason as the updated one).
