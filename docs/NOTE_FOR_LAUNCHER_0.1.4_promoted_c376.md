<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: 0.1.4 promoted as c376 (2026-11-14)

**PROMOTED: c376, SHA-256 549EDD9645348C82C67E5D543B094D6028EDA9CF14DA38657225449E5BFC5876** (artifacts\ste-recomp.exe, FileVersion 0.1.4.376 / FILEVERSION 0,1,4,376, ProductVersion "0.1.4+c376", `--version` string 0.1.4). Replaces c370 (0.1.3a, kept as artifacts\ste-recomp_c370.exe). capabilities.json is unchanged. The exe needs only the runtime DLLs c370 needed (VCRUNTIME140.dll, no MSVCP140), no new files next to it.

What is new (details in RELEASE_ALPHA.md "What changed since 0.1.3a" and LAUNCHER_INTERFACE.md "0.1.4 additions")
1. **About 20 s faster start-up.** The game waited ~20 s on every launch for an Ethernet link that does not exist (we answered "cable connected", so its network start-up ran DHCP against nothing). The runtime now answers "no cable" (`STE_ETHERNET_LINK=up` restores the old answer). First game file opened at +1.7 s (was +22 s), first intro movie at +16 s (was +36 s). If the launcher has a "game is starting" timeout based on the old time it can be shorter. There is a `[BOOT]` timeline in every log.
2. **Patch mods.** The game uses an enabled api-2 mod's `mods\<id>\_built\<target>` only when its SHA-256 equals the manifest's `patches[].result_sha256`; a missing file or a wrong hash falls back to the original and logs `[MODS]   patch <target>: not built ...` / `RESULT MISMATCH ...`. The game does not build `_built` itself: that is the launcher's Prepare mods step (or `ste\tools\step-diff\step_diff.py make/apply`, the GPL tool in the repo; `step1` format = docs\formats\mod_patch.md). Self-test mod: ste\tests\mods\ste.test-patch (changes one default Top Ten name; contains only a 125-byte patch).
3. **Settings keys** `[audio] master_volume` (0..100) and `[window] hide_cursor` (auto | always | never) in the settings ini; add rows if you want them in the Video page.
4. Crash reports list the last memory-protection calls; debug switches `STE_DISK_SLOW_MS`, `STE_PROTECT_LOG`, `STE_BOOT_SAMPLE`.

Not in this release (what slipped and why, for the README/known issues): the **in-game overlay** is written but not linked (see NOTE_FOR_LAUNCHER_overlay_blocker_0.1.4.md: its start-up crashes in your library when it runs inside the game process; I need you for that), button words and PC wording ("Press START", "Xbox console": the game looks text up by the English string, editing it breaks loading), keyboard typing on the profile name screen, the Mechagodzilla 3 missile fix (spawn works, the head sprite is never drawn, still open), and the per-monster effects sweep.

Verification: validator 0 FAIL; mixed soak of c376 (7 scenarios x 420 s: versus in 4 cities, melee, an action mission, idle/attract) 7/7 alive, 0 crash dumps, 0 ICALL/STUB, no audio starvation, frame-time p99 21.8-23.9 ms (c370: 21.9-23.4 ms); the final exe (plain link, no overlay) repeated 4 of them: 4/4 alive, 0 dumps. Slow-disk tests (5 ms, 0-40 ms random, 60 ms per read): 0 crashes. Patch mod test: good / not built / corrupt built file all behave as above.

Screenshots: refreshed per the release rule (set and captions in NOTE_FOR_LAUNCHER_0.1.4_screenshots.md). The previous set moved to docs\screenshots\archive\0.1.3a\.

Decisions made that the project owner should review (the evidence is in the work-directory report): reporting "Ethernet link down" (changes what an emulated kernel call answers; no network ever existed), shipping without the overlay, and the experimental text re-wording left off.
