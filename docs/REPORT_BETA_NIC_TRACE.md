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

## Finding that matters for the plan (corrected 2026-11-22)
- **Correction.** An earlier version of this section blamed the link-up crash on a missing PHY model. That was wrong. The crash (access violation in the game's `stricmp`, XNET section) was a missing `#include <stdlib.h>` in `kernel_xbox.c`: `getenv` was implicitly declared `int`, so the pointer it returned was truncated and sign-extended. It depended on where the OS placed the process memory, which is why it was intermittent and independent of the trace. Fixed in 0.4.1; the default (link down) was never affected.
- **The driver never reads the PHY over MMIO.** Link, speed and duplex come from the kernel call `PhyGetLinkState`. With `STE_ETHERNET_LINK=up` it now answers `0x0B` (active, 100 Mbit, full duplex; before it answered `0x01`), and the default still answers `0`.

## What the title sends with link up (c447 trace, 2026-11-22)
- Ring layout (to confirm against more traces): the ring at `+0x100` (size = low half of `+0x108`, 11 entries) is the **TX** ring; the ring at `+0x104` (high half, 8 entries) is the **RX** ring, with 2046-byte buffers owned by the NIC and never filled. Descriptors are 8 bytes: buffer address, then length-1 with flags (bit 31 = owned by the NIC, bit 16 = last fragment).
- Right after driver start the stack sends **DHCP DISCOVER** broadcasts (342 bytes: Ethernet FF:FF:FF:FF:FF:FF, IPv4 0.0.0.0:68 -> 255.255.255.255:67, same transaction id each time), **10 of them about 2.03 s apart (1.75 s ... 20.1 s)**. Nothing is answered, so the stack gives up. **After that nothing more is sent, also on Multi-player > System Link** (reached with link up; the "games available" list stays empty). `PhyGetLinkState` is polled at roughly 300 calls/s for the whole run.
- **The source MAC in those frames is 00:00:00:00:00:00.** The driver asks the kernel for the factory Ethernet address (`ExQueryNonVolatileSetting` index `0x104`, currently "unhandled") and gets nothing. A usable MAC (locally administered, unique per player) is a prerequisite for any tunnel.
- No interrupt is ever delivered to the driver (mask `0x5F` is written; nothing raises one). Whether the stack needs completions to proceed is not yet known.

## Next (proposal; each step is reported before the next)
1. Online mode only: answer `XC_FACTORY_ETHERNET_ADDR` with a configured MAC; complete TX descriptors and raise the TX-done interrupt; feed RX buffers. Default (online off) stays byte-identical.
2. A minimal DHCP responder inside the host's virtual switch (or let the stack fall back to a link-local address) so System Link has an address; then see what System Link sends.
3. Own TX/RX descriptor code + frame validation + unit tests with synthetic frames.
4. Tunnel with Monocypher, as in [ONLINE_DESIGN.md](ONLINE_DESIGN.md) (it goes into CREDITS.md part 1 in the commit that adds it).
5. Two-instance test on test PC A, then the LAN test ([ONLINE_TEST_STEPS.md](ONLINE_TEST_STEPS.md)).

Credits: register layout knowledge from public documentation of the nForce NIC; xemu's nvnet was not used as code. Full credits: [CREDITS.md](../CREDITS.md).
