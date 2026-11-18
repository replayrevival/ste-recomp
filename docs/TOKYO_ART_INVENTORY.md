# Tokyo texture inventory (0.1.7, recreated-art pilot, step 1)

Method: `STE_TEXINV=1 STE_TEXINV_OUT=<csv>` (lib nv2a_kelvin.c, logging only, no pixels written) counts, per guest texture slot, the draws and submitted vertices that sample it, keyed by the same content hash the texture dump and pack loader use. Run: Mechagodzilla 3 vs Godzilla 2000, Tokyo (day), 1280x720 original aspect, one match of about 470 s from the title (scripted pad `tests\pads\item_d_mg3_short.pad.txt`), c415. Raw CSV: F:\GodzillaSTE_work\tokyo_inventory_raw.csv (not in the repo; it holds only hashes, sizes and counters, no game data).

Textures seen: **393** distinct content keys (283 in use during the match, 110 only in menus/loading/movies). Key 0000000000000000 = texture too large to hash (render targets / movie surfaces).

## Where the match spends its texture sampling (by size class, vertices submitted)

| size | textures | draws | share of vertices |
|---|---|---|---|
| 512x512 | 8 | 721798 | 39.4 % |
| 128x128 | 42 | 2426858 | 29.7 % |
| 32x128 | 22 | 198054 | 10.3 % |
| 64x64 | 59 | 2939663 | 9.2 % |
| 16x16 | 7 | 307843 | 3.0 % |
| 256x256 | 4 | 137836 | 2.5 % |
| 32x32 | 67 | 440568 | 1.9 % |
| 64x256 | 11 | 37293 | 1.4 % |
| 128x16 | 1 | 7919 | 0.5 % |
| 500x399 | 1 | 77566 | 0.3 % |
| 344x448 | 1 | 76312 | 0.3 % |
| 32x64 | 4 | 19185 | 0.3 % |

## Top 40 textures by vertices (candidates for recreated art, most visible first)

| # | key | size | format word | draws | vertices | share |
|---|---|---|---|---|---|---|
| 1 | F3F17CBAA62022F7 | 512x512 | 099A0E29 | 81933 | 86332869 | 13.78 % |
| 2 | 0000000000000000 | 512x512 | 099A0729 | 11632 | 70348580 | 11.23 % |
| 3 | E67F1D50650CFC6E | 512x512 | 099A0C29 | 7676 | 52143068 | 8.32 % |
| 4 | BEA2B381A75DA350 | 128x128 | 07780C29 | 1363120 | 36115976 | 5.77 % |
| 5 | 3DCB7DFFA5771B2C | 128x128 | 07780C29 | 62303 | 31772722 | 5.07 % |
| 6 | EF11DB5FDEA86627 | 128x128 | 07780C2D | 286119 | 28088829 | 4.48 % |
| 7 | AA55BA58616E7A54 | 128x128 | 07780E29 | 155977 | 21268604 | 3.40 % |
| 8 | 374AA9E32550E1FD | 16x16 | 04450E29 | 17310 | 17327310 | 2.77 % |
| 9 | C6CCDA5F070659E2 | 32x128 | 07560E29 | 21355 | 16581154 | 2.65 % |
| 10 | AB24D4A826C0F8F2 | 512x512 | 099A0E29 | 289864 | 13690380 | 2.19 % |
| 11 | 892D4E289F2C8919 | 64x64 | 06670C29 | 61508 | 13201505 | 2.11 % |
| 12 | 9CC30F49C9A031F4 | 128x128 | 07780B29 | 39212 | 12581277 | 2.01 % |
| 13 | 738ECEA49F5587BA | 512x512 | 099A0E29 | 131778 | 12097746 | 1.93 % |
| 14 | 3D3BCDA34D4F6342 | 64x64 | 06670B29 | 21447 | 10920818 | 1.74 % |
| 15 | DDB331CD56442B84 | 128x128 | 07780E29 | 120450 | 8215108 | 1.31 % |
| 16 | 450B243694DCC43B | 128x128 | 07780E29 | 33949 | 7987194 | 1.28 % |
| 17 | 8B1E0EEC717E9341 | 32x128 | 07560E29 | 10446 | 7741340 | 1.24 % |
| 18 | 4BD1D1D1C23F7499 | 512x512 | 099A0E29 | 159028 | 7741067 | 1.24 % |
| 19 | 60ABB1C3D12B3BD6 | 128x128 | 07780E29 | 66387 | 6831889 | 1.09 % |
| 20 | ED231AD73217A908 | 256x256 | 08890729 | 25546 | 5737866 | 0.92 % |
| 21 | C81269E26188A020 | 128x128 | 07780B29 | 23757 | 4854347 | 0.78 % |
| 22 | 9BADDFF3B0367EB6 | 32x128 | 07560E29 | 8439 | 4675206 | 0.75 % |
| 23 | A5924C45A50B958D | 64x64 | 06670B29 | 7919 | 4593020 | 0.73 % |
| 24 | DC3D1B9580E241E0 | 64x64 | 06670429 | 1053325 | 4274103 | 0.68 % |
| 25 | 0C571B4CDCAF87B4 | 512x512 | 099A0E29 | 31967 | 4238281 | 0.68 % |
| 26 | 96EC63B864DE199A | 128x128 | 07780E29 | 20778 | 4195907 | 0.67 % |
| 27 | EA7C9C1DA9DEA5B7 | 32x128 | 07560E29 | 21849 | 4053858 | 0.65 % |
| 28 | 211B61B7494E9AEF | 32x128 | 07560E29 | 6473 | 3951473 | 0.63 % |
| 29 | AFEB566DD3991583 | 32x128 | 07560E29 | 17280 | 3925864 | 0.63 % |
| 30 | CDF19AB5A6D6E617 | 256x256 | 08890B29 | 50561 | 3775588 | 0.60 % |
| 31 | 0506615F5473D0F2 | 128x128 | 07780B29 | 14368 | 3514944 | 0.56 % |
| 32 | 5CAD12823E667935 | 128x128 | 07780E29 | 31251 | 3336769 | 0.53 % |
| 33 | D1408D4BD333C8DC | 256x256 | 08813C29 | 44675 | 3320702 | 0.53 % |
| 34 | 5C2B1D58D6D6ABCB | 128x16 | 04750B29 | 7919 | 3262628 | 0.52 % |
| 35 | C826BCED95A0240D | 64x64 | 06670E29 | 7760 | 3142800 | 0.50 % |
| 36 | F91DDF2EF15B4D56 | 32x128 | 07560E29 | 11233 | 3136744 | 0.50 % |
| 37 | FE94214698880CDB | 32x128 | 07560E29 | 20596 | 2976373 | 0.48 % |
| 38 | 87DE45C1C6CC763B | 32x128 | 07560C29 | 11287 | 2754028 | 0.44 % |
| 39 | 099131FE5A2F1358 | 32x128 | 07560E29 | 3725 | 2721243 | 0.43 % |
| 40 | 1933F827166BF4D5 | 64x256 | 08670E29 | 10723 | 2705455 | 0.43 % |

## Observations

- 8 of the 283 match textures are 512x512 or larger and take 39.4 % of the sampled vertices (probably the city and monster atlases, to be confirmed when the keys are named). A recreated-art pass likely pays off fastest there.
- The 128x128 class (29.7 %) and the many small textures have very high draw counts (hundreds of thousands: probably sprites, particles, HUD and repeated building tiles); what they are is not known until the keys are named.
- Next steps (pilot): (1) name the keys (bundle entry names via the draw log, tests\\drawlog_tex.py) for the top 40; (2) per key pick the recreation route (own AI upscale of the Xbox texture with our tool, never shipped; or hand/AI redraw by the owner); (3) pack format = docs\\formats\\texturepack.md (key = content hash, PNG per key), tested with the existing texture-pack loader (anisotropy / mip handling already in); (4) Tokyo night and the other arenas repeat the same inventory with `STE_TIME_OF_DAY` runs.
- Limits: one match only (event textures, damage variants and the night variant are missing), vertices are a proxy for screen importance (no screen-area weighting yet).
