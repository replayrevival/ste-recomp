# 0.1.6 promoted: c408

- Build: `artifacts\ste_recomp_mission_l6_candidate408.exe`, SHA-256 E8C1037BE10EA22D6A0D1A94F0C05829E531BD0B741E98B800A1D94DC0BAC5A6, version resource 0.1.6.408 (`ste-recomp 0.1.6`). Same recompiled code objects as 0.1.5a (c381); only runtime lib and our exe-side sources changed. Validator: 0 failures.
- Fixed: Mechagodzilla 3 back-missile heads and smoke trails (and every other point-sprite effect): sprites sampled stage 3 at texture coordinate (0,0). Proof: item D report (REPORT_0.1.6) and the new_in_0.1.6_missiles clip.
- New: keyboard name entry on the on-screen profile keyboard (PC keys typed, Tab = END; `STE_TEXT_ENTRY=0` off). Opt-in recorder `STE_RECORD` (docs\RECORDING.md). `[STALLFRAME]` log for frames over 250 ms.
- Package: take the 0.1.5a layout, replace `ste-recomp.exe` with c408 (ste_overlay.dll and the rest are unchanged). Launcher interface unchanged.
- Known / not done: Mechagodzilla 2 finger rockets not specifically re-checked; PlayStation "PRESS START" keeps the existing OPTIONS-then-MENU fallback; menu clip shows the Versus screen, not a fight; 4:3 aspect not swept for effects; the rare 4K water-stage stall was not caught (log in place).
- Publish repo synced locally (signed commit, not pushed).
