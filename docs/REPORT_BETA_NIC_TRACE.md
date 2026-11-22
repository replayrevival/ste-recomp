<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Online Phase 1, step 1: NIC register trace (2026-11-21, logging only)

Maintainer decisions (2026-11-21): Phase 1 go (logging first, report before behaviour changes); tunnel crypto = Monocypher; test on two instances on test PC A first, final test test PC A vs test PC B; Insignia: no contact by us (an internal message draft is kept privately); pop-in: measure first.

## What was built (candidate c438 = c437 + trace, nothing else)
`xboxrecomp/src/kernel/nvnet.c/.h` (own code): with `STE_NIC_TRACE=1` the NIC page (0xFEF00000, 4 KB) is made inaccessible, every access is trapped by the shared MMIO decoder (`platform/mmio_decode.h`), logged (`[NIC] <ms> R|W +offset/size = value`) and answered from a shadow copy that behaves like the plain RAM it replaces. Without the variable nothing changes. `STE_NIC_TRACE_MAX` caps the log (default 20000 lines); a per-offset summary is printed at exit. Hooked into `xbox_VehMmioFault`. Validator: all checks PASS. c438 SHA-256 `F272840FECFF4A78A6B0C2B7F5BF7BDB737AE0895C21686C1607EA8866255DD2`. Not a release build.

## What the title's XNET driver does (4 identical runs, `STE_ETHERNET_LINK=up`)
The driver is active right after boot (about 1.6 s after the first NIC access), before any menu. 52 accesses, always the same order (only the random seed at +0x09C differs):
1. Quiesce: writes 0 to +0x110, +0x094, +0x084 (transmit/receive control), reads +0x098 and +0x088 (status) as bytes, a pulse sequence on +0x144 (0x4 polled four times, then 0, 0x10, 0), then clears +0x184, +0x004 (interrupt mask), +0x200, +0x008, +0x100..+0x110, and read-then-write-back of +0x088, +0x098, +0x180, +0x000 (status/interrupt registers cleared by writing back).
2. MAC address: writes +0x0A8/+0x0AC (zero here, the MAC is not supplied), multicast filter +0x0B0..+0x0BC = all ones.
3. Configuration: +0x090 = 0x5EE (max frame size 1518), +0x08C = 0x7F0020, +0x080 = 0x3B0F3E, +0x09C (random seed), +0x0A0 = 0x16070F, +0x0A4 = 0x16.
4. Rings: **+0x100 = 0x0075E000 (RX ring physical address), +0x104 = 0x0075E800 (TX ring), +0x108 = 0x0007000A (ring sizes, 8 and 11 entries minus one)**.
5. PHY: +0x140 = +0x13C = 0x300010, then MII access: writes +0x188 = 0x01040000, +0x18C = 0x105, then reads +0x188 and polls until its busy bit (bit 16) clears; the shadow keeps it set, so the driver polls and then takes its failure path.
Never reached (because nothing answers the MII access): enabling the receiver/transmitter, the interrupt unmask, any descriptor writes, any reads of the MAC.
Offsets are consistent with the public nForce NIC register layout (the Linux `forcedeth` documentation: rings at 0x100/0x104/0x108, MII at 0x180..0x190); names will be added only where confirmed by what the game does. Raw log kept privately.

## Finding that matters for the plan
- **With the link forced up and no PHY answer the title crashes** (access violation in the game's `stricmp` on a garbage pointer, return addresses in the XNET section 0x0027xxxx..0x0028xxxx) in 4/4 trace runs, and intermittently without the trace (1 of 2 runs on c437/c438, so it is not caused by the trace). That is the failure the default `STE_ETHERNET_LINK=down` avoids (0.1.4) and the reason a real PHY model is needed: the driver expects the MII busy bit to clear and the link state register to answer.
- So the first behaviour step is small and well defined: a minimal PHY (MII busy clears at once, PHY id and link status registers answer "100 Mbit full duplex, link up", auto-negotiation complete). That is behaviour, so it waits for the maintainer's OK (no change was made).

## Next (proposal, not started)
1. (needs OK) PHY model so the driver finishes initialisation with link up; log again to see the rest (enable, interrupt unmask, ring use, the first transmitted frame: expect DHCP/ARP on the System Link screen).
2. Own TX/RX descriptor code + frame validation + unit tests with synthetic frames.
3. Tunnel with Monocypher (not yet in the tree: **it goes into CREDITS.md part 1 (version, licence, hash, files) in the commit that adds it**; listing it before it is included would be wrong).
4. Two-instance test on test PC A (separate `RECOMP_SAVE_DIR`s, loopback), then the LAN test ([ONLINE_TEST_STEPS.md](ONLINE_TEST_STEPS.md)).

Credits: register layout knowledge from public documentation of the nForce NIC; xemu's nvnet was not used as code. Inspired by no one's code. Full credits: [CREDITS.md](../CREDITS.md).
