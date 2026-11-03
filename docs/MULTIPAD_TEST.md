# Multi-pad (local multiplayer) test for two real controllers (c192)

Build: `artifacts\ste_recomp_mission_l6_candidate192.exe` (launch with `play_low.bat`-style scripts pointed at candidate192, or copy `play.bat` and replace the exe name).

1. Plug in two Xbox-style pads (XInput). The first connected pad is Xbox port 1 (P1), the second is port 2 (P2). Log line at start: `[XPP] port map: p0=.. p1=..` (0..3 = host XInput user, 4 = keyboard, -1 = none).
2. Versus: pick P1's monster; the P2 slot says "Press START to join" -> press Start on pad 2, then choose its monster. The fight HUD shows P1 / P2 (not COM).
3. Melee: with 3-4 pads plugged in, press Start on each extra pad to join (P3/P4); all four fight.
4. Hot-plug: unplug pad 2 during a fight -> the game pauses with "MISSING CONTROLLER - please reconnect the controller to controller port 2"; replug -> the fight resumes (noticed within ~0.5 s).
5. Rumble: set `STE_RUMBLE_SELFTEST=1` (env) -> each mapped pad vibrates for 1 s at t = 60 s + 5 s x port (port 1 at 60 s, port 2 at 65 s ...). Log: `[RUMBLE] selftest on port N (host pad M)`. The title itself may never request rumble in normal play (not seen in our virtual-pad runs); `STE_RUMBLE_LOG=1` logs any request.
6. Mapping overrides: `ste_enhance.ini` `[input]` `port1..port4 = auto | xinput0..xinput3 | keyboard | none` (env `STE_INPUT_PORT1..4`). The keyboard is always merged into port 1.

Report: the `[XPP]` lines from the log (`logs\play_<stamp>.err`) and what happened.

## Real-hardware result (TESTER, 2026-10-29, c192, two real controllers on the the test PC)
- Rumble works on both controllers in-game: during a fight and when toggled in the Options menu (the title does request it on real pads; item closed).
- Melee: a real fight with both controllers worked.
- Versus: the select screen recognised controller 2 (joined). OPEN for TESTER's next session: play a full Versus fight with P2.
- Unplugging controller 2 shows the game's own disconnect screen. OPEN for TESTER's next session: confirm the fight resumes after replugging.
