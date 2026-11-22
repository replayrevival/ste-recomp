<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Note for the launcher session: online Host / Join screens (planned, nothing to build yet)

Design: `docs\ONLINE_DESIGN.md` (player-hosted, password, no server, max 4). This note is so the launcher can prepare the screens; the game side does not exist yet (the game does not transmit on System Link so far), so **do not ship these screens until a release says online is available**.

## Screens
- **Online > Host:** fields: password (hide/show, "generate passphrase" button, warn if under 10 characters), players 2-4, port (default 47474, "advanced"). Button **Start hosting** launches the game in host mode. While running show: state, the **code** to share (Copy button), the port-mapping result (UPnP / NAT-PMP / none / failed) and, if failed, the manual-forwarding guide link; connected peers with ping.
- **Online > Join:** fields: code, password. Button **Join** launches the game in join mode. Show progress and a single plain failure message ("no answer": wrong password, closed port and host offline are deliberately not told apart).
- **Help page:** the three connection ways (automatic, manual forwarding, VPN such as Tailscale/ZeroTier), with a router port-forwarding guide for UDP 47474.
- Off by default; nothing listens unless hosting.

## Interface (draft, will be fixed when the game side exists)
The launcher writes `ste_online.ini` `[online]` keys `mode` (off/host/join), `port`, `code`, `players`, `upnp` (auto/off), `bind`, and passes the password only as the env var `STE_ONLINE_PASSWORD` to the game process (never into a file). The game writes `ste_online_status.json` (state, code, external_address, mapping, peers, error); the launcher polls it. Details and field list: `docs\ONLINE_DESIGN.md` section 5.

Example host start: `mode=host`, `players=4`, `port=47474`, `upnp=auto`, env `STE_ONLINE_PASSWORD=...`; status then shows `state=listening`, `code=<string to share>`.

## Credits and docs
Crypto will be Monocypher (public domain / BSD-2): the launcher's CREDITS.md needs it only if launcher code uses it (likely not; the game does the crypto). The novice guide gets an "Online play" chapter before the release.
