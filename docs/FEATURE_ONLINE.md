<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Feasibility: online play for the Xbox recomp (2026-11-19, report only, nothing built)

Question (Miko): online play like ANILORAK's PS2 mod. PS2 used GameSpy (his lobby: qr2 UDP 27900, server browsing TCP 28910, peerchat TCP 6667; matches peer-to-peer UDP 1000/1001); the Xbox version used Xbox Live. His pack (`online/GUIDE.md`, `lobby/lobby.py`) was used as REFERENCE ONLY for the PS2 side; no code of his is used or copied. Everything about the Xbox side below comes from our own files and the Xbox scripts (decompiled copies, read-only). **No game or network test was run for this report.**

## 1. What the Xbox build contains (evidence)
| Layer | Finding | Evidence |
|---|---|---|
| XDK libraries | XDK 5849 (the last 2004 build). Libraries linked in: XAPILIB, XBOXKRNL, LIBC, D3D8, D3DX8, XGRAPHC, DSOUND, **XONLINES** (the secure Xbox Live online library), **XVOICE**, **VOICMAIL**, XMV. Code sections: **XNET 77,136 bytes** (0x0027B940), **XONLINE 136,460 bytes** (0x0028E6A0), XON_RD (5,616 bytes). All of it is ordinary x86 recompiled into our generated C like the rest of the game | `default.xbe` header scan (library table, section table) |
| game net classes | **`DNetLobby`** (53 methods: `hostGame`, `initiateJoinGame`, `serviceGameAdvertise`, `serviceBroadcastSocket` ("BSock.RecvFrom"), `send_/recv_JoinRequest, JoinResults, SetGameType, SetPlayerNotify, GameInfo, GameCancel, PlayerCancel, LobbyClosedAcknowledge, GudRequest`, `peerLostNotification`, `netDownNotification`, `sockAddrToXnAddr`, `acquireAddress`) = the game's own **LAN / System Link lobby** (broadcast advertise + join over UDP sockets); **`DNetXOnline`** (70 methods: sign-on, friends, invites, `hostGame`, `initiateQuery`, `bootPlayer`, `gamerNameToXUID`, `getSessionXNKID`, `establishHostSession`) = the **Xbox Live** layer; `ShellLobby` / `DShellLobby` = the shell-facing lobby objects; `XNetCreateKey`, `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr` are called by the lobby code; string `.4s.XBOXLIVE.COM` | xbe string scan |
| menus/scripts | `MultiPlayer.bsf`: the Multi-player screen has **System Link** (`gOnline.setLiveMultiplayer(0)` -> screen 5) and **Online / Xbox Live** (`setLiveMultiplayer(1)` -> screen 16; `getPlatform()==2` branches to the PS2 network-config screens 20/21); `NetworkPlay.bsf` (834 lines, lobby UI: `joinGame`, `lobbyNotify`, `mapPlayer`) and `GameSpyPlay.bsf` (1,450 lines, the PS2 GameSpy flow) are both in the Xbox script set (the scripts are shared between platforms); `Lobby.isGameSpy()` branches exist in CitySelect/MonsterSelect | decompiled Xbox scripts |
| what the recomp does now | The NIC (MCPX register block at `0xFEF00000`) is **not emulated**. The kernel answers the Ethernet link query with "no link" by default (`STE_ETHERNET_LINK=up` restores the old "cable connected" answer, which makes the title's XNet start-up run a DHCP exchange against a NIC that does not exist and costs ~20 s: 0.1.4). Nothing in the runtime touches the host network. No XNet/XOnline function is replaced (HLE) | `kernel_xbox.c` (PhyGetLinkState), `LAUNCHER_INTERFACE.md`, memory-layout notes |
| PS2 comparison | The PS2 program also contains the DNetLobby class (213 lobby-related functions in his map; many are 8-byte stubs on PS2), i.e. the lobby abstraction is shared engine code and the PS2 build plugs GameSpy in at `ShellLobby` level | his `functions.tsv` (names read, not copied) |

So the Xbox game already has **two complete network paths in its own code**: System Link (LAN broadcast, no account) and Xbox Live. Everything below the game's lobby (sockets, IP, ARP/DHCP, the secure Live logon) is the XDK's XNET/XONLINE code talking to the NIC through the kernel's hardware access.

## 2. Phases (proposal)
### Phase 1: System Link between two PCs (LAN or a private network/VPN)
- **Approach A (recommended): emulate the NIC at hardware level.** The game's own XNet stack (77 KB, ARP/IP/UDP/DHCP, XNKID keys) stays untouched and talks to an emulated NVIDIA nForce NIC ("nvnet": register file, MAC, PHY, TX/RX descriptor rings in DMA memory, interrupts). Frames are carried to the other PC by a **frame tunnel over one UDP port** (each side encapsulates Ethernet frames in UDP to the peer; no pcap/Npcap, no admin rights, works over LAN, Tailscale/ZeroTier or a forwarded port). Hardware emulation is not an override of game logic, so it stays inside our rules. Unknown: how much of nvnet the XDK 5849 driver needs (xemu's implementation is the reference for the behaviour; our own code must be written independently).
- Approach B (not recommended): HLE of the XNet/WSA socket functions onto host UDP sockets. Faster to a first demo but it replaces XDK library functions with our code (a "new HLE override of an XDK function" = the STOP class in CLAUDE.md, needs Miko's decision) and every XNet feature the lobby uses (XNKID, secure addresses, link-local addressing) would have to be re-implemented by hand.
- Game side needs nothing: the System Link screens exist. Our work: nvnet device (MMIO, DMA, IRQ), the tunnel, a settings row (peer address, tunnel port), a link-up answer when a peer is configured.
- **Effort: L (about 3-6 weeks).** Risk: timing/IRQ details of the NIC driver; a second PC to test with.

### Phase 2: internet play
- Xbox Live proper (XONLINES, the secure library) needs Microsoft's Xbox Live servers (long shut down; authenticated KDC/XRPC protocol): not reproducible from this game alone. Options:
  1. **Reuse the Phase 1 tunnel over the internet** with a tiny rendezvous service (we host it, or one player runs it): it only introduces two peers and relays frames/UDP-hole-punches; players then play System Link across the internet. The game still shows the System Link screens. **Effort: M after Phase 1** (a small server + client code + NAT traversal); a relay fallback for NATs that cannot hole-punch.
  2. **Insignia** (the community Xbox Live revival that xemu uses): it works at the NIC/DNS level, so Phase 1's NIC emulation is the prerequisite. **Whether Insignia supports Godzilla: Save the Earth must be checked first** (not verified here). If yes, internet play could come almost for free; if not, 1. applies.
  3. Our own replacement of the XOnline calls (HLE of XOnlineLogon, matchmaking/session functions) so the Live screens work against our service: **XL (months)** and STOP-class overrides; only if Live-style friends/invites matter.
- Recommendation: Phase 1, then option 1 (same tunnel + rendezvous), check Insignia in parallel.

### Phase 3: cross-play with PS2 (PCSX2/XBSX2 with ANILORAK's patch)
- Not shown to be possible. What is known: both games share the engine's lobby classes (DNetLobby exists in both programs), the scripts are shared, and his notes give the PS2 match layer as UDP 1000/1001 with a lobby on 1001. What would have to match: the in-match packet format (state/inputs), the lobby messages, the game versions, and the **platform value** (`getPlatform()` is 2 on PS2: scripts branch on it). A bridge (our lobby speaks his GameSpy-replacement protocol AND carries Xbox System Link frames) is only worthwhile if the in-match packets turn out to be identical; the cheap first step is a **packet capture of a PS2 match** (Miko/ANILORAK) compared with a capture of our Phase 1 match. **Effort: M for the analysis, then unknown (L-XL) for a bridge.** Treat as speculative.

## 3. Security plan (the 2004 netcode was never hardened)
- Assume every packet the game parses is hostile: the game's DNetLobby/XNet code does its own parsing and we do not patch it, so **we limit who can reach it**: (a) only frames from configured peers enter the emulated NIC; the tunnel is **authenticated** (a per-session key from the rendezvous/invite, AEAD or HMAC on every datagram; drop everything else); (b) our NIC layer validates frames before they reach guest memory: Ethernet length 60..1514, descriptor ring bounds, DMA addresses inside the guest RAM window, rate limit (packets/s, bytes/s), drop broadcasts/ICMP/anything not UDP to the game's ports, no fragments reassembly outside the guest; (c) the guest never sees host network access: no gateway to the host LAN or the internet except the tunnel (the emulated network is closed: only the other player's console exists); (d) crash isolation: if the guest faults inside a network handler, the existing crash handler writes the dump and exits cleanly (as now).
- **Ports kept to the minimum:** one UDP port for the tunnel (user-configurable, default fixed); the rendezvous service: one TCP/UDP port on its host (443-like) and nothing else; no inbound ports needed for the joiner (hole punching) when the host forwards one UDP port.
- Privacy: peers see each other's public IP (as in any peer-to-peer game); no account, no logs unless asked.
- Never forward or DMZ anything else; document this in the Online page of the launcher.

## 4. Effort summary
| Item | Effort |
|---|---|
| Phase 1: nvnet NIC emulation + UDP frame tunnel + settings (System Link works between two PCs) | L, 3-6 weeks |
| Phase 2.1: rendezvous service + NAT traversal + relay fallback (internet play through System Link) | M, 2-3 weeks after Phase 1 |
| Phase 2.2: Insignia check (research) / support | S to check, unknown to support |
| Phase 2.3: own XOnline replacement | XL |
| Phase 3: capture + compare + bridge decision | M analysis; bridge L-XL, speculative |
| Security layer (frame validation, authenticated tunnel, rate limits, fuzzing the NIC path) | M, in parallel with Phase 1 |
| Launcher: Online page (peer/invite, status, safety text) | S-M (launcher session) |

## 5. Not verified / open questions
- How much of the nvnet register interface the 5849 XNet driver uses, and whether the DHCP/ARP traffic of the game works unchanged through a pure frame tunnel (expected yes: it is all inside the game's stack).
- Whether System Link in this game needs both machines on one subnet (XNet link-local 169.254.x.x addressing + broadcast) -- the tunnel must behave as one Ethernet segment, which it does.
- Insignia support for this title; STE's Xbox Live service was shut down in 2010.
- Two-PC test setup (a second machine or two instances with the tunnel on loopback; **two game instances on one PC are forbidden by our save-lock rule**, so a second PC or a separate user session is needed).
- Packet compatibility with the PS2 match layer (needs captures).

## 6. Decision wanted from Miko
Approve Phase 1 (NIC emulation + tunnel, hardware-level, no XDK overrides) as the first build, with the security layer designed in from the start; decide whether Insignia is to be checked first; keep cross-play as a research item.

## 7. Update 2026-11-21
Insignia check, Phase 1 design details and the pop-in plan: docs\REPORT_BETA_RESEARCH.md.
