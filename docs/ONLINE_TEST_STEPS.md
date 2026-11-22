<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Online Phase 1 test steps (draft, terminal only, for testers)

Status: planned. The NIC emulation and tunnel do not exist yet (see REPORT_BETA_NIC_TRACE.md); the settings names below are the proposed ones and will be confirmed when the feature lands. Nothing here needs the launcher.

## 0. Rules
- One game process per PC per player; each instance on the same PC gets its own save folder (`RECOMP_SAVE_DIR`), its own port and its own window. Never share a save folder between two instances.
- Both sides must run the same exe (compare SHA-256) and the same mods/settings.
- The invite secret is shown once by the host; do not paste it in public places.

## 1. Two instances on test PC A (first test)
1. Host (PowerShell 1):
   `$env:RECOMP_SAVE_DIR="./online_test/save_host"; $env:STE_ONLINE_HOST="127.0.0.1:27500"; .\ste-recomp.exe`  
   It prints `invite: <code>` in the log (stderr).
2. Guest (PowerShell 2, other window):
   `$env:RECOMP_SAVE_DIR="./online_test/save_guest"; $env:STE_ONLINE_JOIN="<invite>@127.0.0.1:27500"; .\ste-recomp.exe`
3. Both: Multi-player > System Link. Host creates a game, guest sees it and joins. Expect: both players on the select screen, a fight starts, inputs on both sides.
4. Record: both logs (`[NIC]`/`[TUNNEL]` lines, dropped-frame counters), any crash dump, screenshots from each side.

## 2. test PC A vs test PC B (final Phase 1 test, scheduled with the maintainer)
1. Check both machines run the same exe: `Get-FileHash .\ste-recomp.exe -Algorithm SHA256`.
2. On the host machine: allow UDP port 27500 inbound on the home LAN only (Windows Firewall rule for private networks), note its LAN address (`ipconfig`).
3. Host: `$env:STE_ONLINE_HOST="0.0.0.0:27500"`; guest: `$env:STE_ONLINE_JOIN="<invite>@<host LAN address>:27500"`.
4. Same steps as test 1 (System Link, create, join, fight). Then repeat with roles swapped.
5. Negative checks (security): from the guest side send a datagram with a wrong key to the host port (the log must show it dropped, no guest-visible effect); stop the guest mid-fight (host must survive and return to the lobby).
6. Report: round-trip feel, dropped/late frames per minute, any desync, logs from both PCs.

## 3. What to send back
Exe SHA-256, both stderr logs, the settings used, what you saw (one line per step). No game data, no invite codes.
