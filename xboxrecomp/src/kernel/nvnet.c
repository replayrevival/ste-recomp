/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * nvnet.c -- Xbox network card (nForce NIC, MMIO at 0xFEF00000): LOGGING ONLY.
 *
 * Step 1 of the online Phase 1 plan (docs\REPORT_BETA_RESEARCH.md, section 2): find out which registers the
 * title's XNET driver really touches, in what order, before any behaviour is written.
 *
 * Opt-in: STE_NIC_TRACE=1 (otherwise nothing changes: the page stays plain RAM, as before).
 * With it set the NIC page is made inaccessible, every access is trapped by the shared MMIO decoder, logged
 * and answered from a shadow copy that behaves exactly like the plain RAM it replaces (a write is read back).
 * So the title sees the same values as without the trace; only the log is new.
 *
 * Log: stderr lines "[NIC] <ms> W|R +0xOFF/size = 0xVALUE", capped by STE_NIC_TRACE_MAX (default 20000),
 * then a per-offset summary at the report call. Own code; no code from xemu or any other project.
 */
#define _CRT_RAND_S
#include <stdlib.h>
#include "nvnet.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include "../platform/mmio_decode.h"

#define NIC_VA   0xFEF00000u
#define NIC_SIZE 0x1000u

static uint8_t  s_shadow[NIC_SIZE];
static int      s_on;
static int      s_trace;   /* STE_NIC_TRACE: register/descriptor logging (c443-c447) */
static int      s_calltrace;   /* c457: STE_NIC_CALLTRACE=1 (research call trace, see candidate457) */
static int      s_emu;     /* c448: STE_ETHERNET_LINK=up: the minimal NIC + virtual switch below (online mode only) */
static unsigned s_max = 20000, s_lines;
static unsigned s_cnt_r[NIC_SIZE / 4], s_cnt_w[NIC_SIZE / 4];
static uint32_t s_last[NIC_SIZE / 4];
static DWORD    s_t0;

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    if (off + (unsigned)size > NIC_SIZE) return 0;
    memcpy(&v, s_shadow + off, (size_t)size);
    s_cnt_r[off >> 2]++;
    /* log every read only while the same offset has not been read many times in a row (poll loops) */
    {   unsigned c = s_cnt_r[off >> 2];
        if (s_lines < s_max && c > 4 && v == s_last[off >> 2] && (c & (c - 1)) == 0) {   /* poll loop: one line at 8, 16, 32 ... reads */
            s_lines++; fprintf(stderr, "[NIC] %6lu R +0x%03X/%d = 0x%llX   (read #%u, unchanged)\n", (unsigned long)(GetTickCount() - s_t0), off, size, (unsigned long long)v, c);
        }
    }
    if (s_lines < s_max && (s_cnt_r[off >> 2] <= 4 || v != s_last[off >> 2])) {
        s_lines++;
        fprintf(stderr, "[NIC] %6lu R +0x%03X/%d = 0x%llX\n", (unsigned long)(GetTickCount() - s_t0), off, size, (unsigned long long)v);
    }
    s_last[off >> 2] = (uint32_t)v;
    return v;
}

/* c443: on each TX/RX kick (+0x144 bit 0) log every TX descriptor whose contents changed since last time
 * (8-byte legacy descriptor: u32 buffer phys, u32 flags|len; bit31 = owned by the NIC), with the first bytes of the frame. Read-only. */
/* c446: ring at +0x104 (high half of +0x108) = RX buffers (owned by the NIC, empty); ring at +0x100 (low half) = TX, where the DHCP frames appear (see the report) */
static uint32_t s_rxprev[64][2];
static void nic_dump_rx(void)
{
    uint32_t base, cfg, n, i;
    memcpy(&base, s_shadow + 0x104, 4); memcpy(&cfg, s_shadow + 0x108, 4);
    n = ((cfg >> 16) & 0x3FF) + 1; if (n > 64) n = 64;
    if (!base) return;
    __try {
        const uint32_t *d = (const uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + (0x80000000u | base));
        for (i = 0; i < n; i++) {
            uint32_t buf = d[i * 2], fl = d[i * 2 + 1];
            if (buf == s_rxprev[i][0] && fl == s_rxprev[i][1]) continue;
            s_rxprev[i][0] = buf; s_rxprev[i][1] = fl;
            if (s_lines >= s_max) continue;
            s_lines++;
            fprintf(stderr, "[NIC] %6lu RXDESC #%u buf=0x%08X flags=0x%08X%s\n", (unsigned long)(GetTickCount() - s_t0), i, buf, fl, (fl & 0x80000000u) ? " NIC-OWNS" : "");
        }
    } __except (1) { if (s_lines < s_max) { s_lines++; fprintf(stderr, "[NIC] RX ring 0x%X not readable\n", base); } }
}

/* c444: kernel PhyGetLinkState calls (first 8, then at powers of two) with the caller's return address */
void xbox_NvnetNoteLink(int verify, uint32_t result)
{
    static unsigned n;
    unsigned c;
    if (!s_on) return;
    c = ++n;
    if (s_lines < s_max && (c <= 8 || (c & (c - 1)) == 0)) {
        s_lines++;
        fprintf(stderr, "[NIC] %6lu PhyGetLinkState(verify=%d) = 0x%X   (call #%u)\n", (unsigned long)(GetTickCount() - s_t0), verify, result, c);
    }
}

static uint32_t s_txprev[64][2];
static void nic_dump_tx(void)
{
    uint32_t base, cfg, n, i;
    memcpy(&base, s_shadow + 0x100, 4); memcpy(&cfg, s_shadow + 0x108, 4);
    n = (cfg & 0x3FF) + 1; if (n > 64) n = 64;
    if (!base) return;
    __try {
        const uint32_t *d = (const uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + (0x80000000u | base));
        for (i = 0; i < n; i++) {
            uint32_t buf = d[i * 2], fl = d[i * 2 + 1];
            if (buf == s_txprev[i][0] && fl == s_txprev[i][1]) continue;
            s_txprev[i][0] = buf; s_txprev[i][1] = fl;
            if (s_lines >= s_max) continue;
            s_lines++;
            fprintf(stderr, "[NIC] %6lu TXDESC #%u buf=0x%08X flags=0x%08X%s\n", (unsigned long)(GetTickCount() - s_t0), i, buf, fl, (fl & 0x80000000u) ? " OWN" : "");
            if (fl & 0x80000000u) {
                const uint8_t *p = (const uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + (uintptr_t)buf);
                unsigned len = (fl & 0xFFFF) + 1, k; if (len > 128) len = 128;
                fprintf(stderr, "[NIC]        frame(%u+):", len);
                for (k = 0; k < len; k++) fprintf(stderr, " %02X", p[k]);
                fprintf(stderr, "\n");
            }
        }
    } __except (1) { if (s_lines < s_max) { s_lines++; fprintf(stderr, "[NIC] TX ring 0x%X not readable\n", base); } }
}

/* ======================================================================================================================
 * c448: minimal NIC model + virtual switch. ONLY with STE_ETHERNET_LINK=up (online mode); default builds never get here.
 * Own code (GPL-2.0-or-later), written from the register/descriptor behaviour recorded by the c443-c447 traces.
 *  - TX ring (+0x100, low half of +0x108): frames the title queues are taken, OWN cleared, TX_OK (0x10) raised.
 *  - RX ring (+0x104, high half): frames for the title are copied into its NIC-owned buffers, OWN cleared, RX (0x02) raised.
 *  - Interrupt status +0x000 (write-1-to-clear), mask +0x004; the title's ISR on vector 4 is called from the NIC thread.
 *  - Virtual router 10.77.0.1: answers DHCP DISCOVER/REQUEST and ARP for itself; everything else is logged ("[NICEMU]").
 * The tunnel (later) plugs into nic_tx_frame() / nic_rx_push().
 * ====================================================================================================================*/
typedef void (*nic_func_t)(void);
extern nic_func_t recomp_lookup(uint32_t xbox_va);
extern nic_func_t recomp_lookup_manual(uint32_t xbox_va);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_AllocThreadTib(void);
extern int  xbox_IrqlBlocksInterrupts(void);
extern int  xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_fs_base;

#define NIC_VECTOR   4u
#define NIC_IRQ_RX   0x02u
#define NIC_IRQ_TXOK 0x10u
static const uint8_t k_router_mac[6] = { 0x02, 0x52, 0x52, 0x00, 0x00, 0x01 };
static const uint8_t k_router_ip[4]  = { 10, 77, 0, 1 };
static uint8_t  s_my_mac[6];
static int      s_mac_ready;
static uint8_t  s_client_ip[4] = { 10, 77, 0, 2 };
static unsigned s_txi, s_rxi, s_emu_lines;
static CRITICAL_SECTION s_rxq_lock;
#define RXQ_N 32
static struct { uint8_t d[1600]; unsigned n; } s_rxq[RXQ_N];
static unsigned s_rxq_h, s_rxq_t;

#define EMU_LOG(...) do { if (s_emu_lines < 3000) { s_emu_lines++; fprintf(stderr, "[NICEMU] %6lu ", (unsigned long)(GetTickCount() - s_t0)); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while (0)

int xbox_NvnetEmuOn(void) { return s_emu; }
static __declspec(thread) int t_in_isr;
static volatile uint32_t s_nic_dpc;
int xbox_NvnetInIsr(void) { return t_in_isr; }
void xbox_NvnetNoteDpc(uint32_t dpc, uint32_t routine) { s_nic_dpc = dpc; EMU_LOG("ISR queued DPC 0x%08X routine 0x%08X", dpc, routine); }
int xbox_NvnetIsNicDpc(uint32_t dpc) { return s_emu && dpc && dpc == s_nic_dpc; }

/* Random, locally administered, unicast MAC; made once per install and kept in ste_nic.ini ([nic] mac=). Never taken from hardware. */
int xbox_NvnetGetMac(uint8_t mac[6])
{
    char buf[64]; unsigned v[6]; int i;
    if (!s_emu) return 0;
    if (!s_mac_ready) {
        GetPrivateProfileStringA("nic", "mac", "", buf, sizeof buf, ".\\ste_nic.ini");
        if (sscanf(buf, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 && (v[0] & 2) && !(v[0] & 1)) {
            for (i = 0; i < 6; i++) s_my_mac[i] = (uint8_t)v[i];
        } else {
            for (i = 0; i < 6; i += 2) { unsigned r = 0; rand_s(&r); s_my_mac[i] = (uint8_t)r; s_my_mac[i + 1] = (uint8_t)(r >> 16); }
            s_my_mac[0] = (uint8_t)((s_my_mac[0] & 0xFC) | 0x02);   /* locally administered, unicast */
            snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5]);
            WritePrivateProfileStringA("nic", "mac", buf, ".\\ste_nic.ini");
        }
        s_mac_ready = 1;
        fprintf(stderr, "  [NICEMU] NIC MAC %02x:%02x:%02x:%02x:%02x:%02x (random per install, ste_nic.ini)\n", s_my_mac[0], s_my_mac[1], s_my_mac[2], s_my_mac[3], s_my_mac[4], s_my_mac[5]);
        fflush(stderr);
    }
    memcpy(mac, s_my_mac, 6);
    return 1;
}

volatile DWORD g_nic_trace_until;   /* c457: window (GetTickCount) during which the research call trace logs; set when a frame is delivered with STE_NIC_CALLTRACE=1 */
static void nic_rx_push(const uint8_t *f, unsigned n)
{
    if (n > sizeof s_rxq[0].d) return;
    EnterCriticalSection(&s_rxq_lock);
    if (((s_rxq_t + 1) % RXQ_N) != s_rxq_h) { memcpy(s_rxq[s_rxq_t].d, f, n); s_rxq[s_rxq_t].n = n; s_rxq_t = (s_rxq_t + 1) % RXQ_N; }
    LeaveCriticalSection(&s_rxq_lock);
}

static uint16_t ip_csum(const uint8_t *p, unsigned n)
{
    uint32_t s = 0; unsigned i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* DHCP server side of the virtual router (DISCOVER -> OFFER, REQUEST -> ACK). rq = the BOOTP payload of the title's frame. */
static void nic_dhcp(const uint8_t *rq, unsigned n, const uint8_t *src_mac)
{
    uint8_t f[600]; uint8_t *bp, *o; unsigned i, type = 0, len;
    uint16_t cs;
    if (n < 240 || rq[0] != 1 || rq[236] != 99 || rq[237] != 130 || rq[238] != 83 || rq[239] != 99) return;
    for (i = 240; i + 1 < n && rq[i] != 255;) {
        if (rq[i] == 0) { i++; continue; }
        if (rq[i] == 53 && rq[i + 1] >= 1 && i + 2 < n) type = rq[i + 2];
        i += 2u + rq[i + 1];
    }
    if (type != 1 && type != 3) { EMU_LOG("DHCP type %u ignored", type); return; }
    memset(f, 0, sizeof f);
    if (rq[10] & 0x80) memset(f, 0xFF, 6); else memcpy(f, rq + 28, 6);   /* c455: BOOTP broadcast flag set -> broadcast; else unicast to chaddr (RFC 2131 4.1) */
    memcpy(f + 6, k_router_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    f[14] = 0x45; f[22] = 64; f[23] = 17;     /* IPv4, TTL 64, UDP */
    memcpy(f + 26, k_router_ip, 4); if (rq[10] & 0x80) memset(f + 30, 0xFF, 4); else memcpy(f + 30, s_client_ip, 4);
    f[34] = 0; f[35] = 67; f[36] = 0; f[37] = 68;
    bp = f + 42;
    bp[0] = 2; bp[1] = 1; bp[2] = 6; bp[3] = 0;
    memcpy(bp + 4, rq + 4, 4);                /* xid */
    memcpy(bp + 10, rq + 10, 2);              /* flags */
    memcpy(bp + 16, s_client_ip, 4);          /* yiaddr */
    memcpy(bp + 20, k_router_ip, 4);          /* siaddr */
    memcpy(bp + 28, rq + 28, 16);             /* chaddr */
    bp[236] = 99; bp[237] = 130; bp[238] = 83; bp[239] = 99;
    o = bp + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)(type == 1 ? 2 : 5);
    *o++ = 54; *o++ = 4; memcpy(o, k_router_ip, 4); o += 4;
    *o++ = 51; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x0E; *o++ = 0x10;              /* lease 3600 s */
    *o++ = 58; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x07; *o++ = 0x08;              /* T1 1800 s */
    *o++ = 59; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = 0x0C; *o++ = 0x4E;              /* T2 3150 s */
    *o++ = 1;  *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;              /* mask */
    *o++ = 3;  *o++ = 4; memcpy(o, k_router_ip, 4); o += 4;                          /* router */
    *o++ = 6;  *o++ = 4; memcpy(o, k_router_ip, 4); o += 4;                          /* dns (the router; no answers) */
    *o++ = 255;
    len = (unsigned)(o - f);
    f[16] = (uint8_t)((len - 14) >> 8); f[17] = (uint8_t)(len - 14);
    f[38] = (uint8_t)((len - 34) >> 8); f[39] = (uint8_t)(len - 34);                 /* UDP length, checksum 0 = none */
    cs = ip_csum(f + 14, 20); f[24] = (uint8_t)(cs >> 8); f[25] = (uint8_t)cs;
    {   /* c456: real UDP checksum (pseudo header + datagram) instead of 0 */
        uint8_t t[640]; unsigned ul = len - 34;
        memcpy(t, f + 26, 8); t[8] = 0; t[9] = 17; t[10] = (uint8_t)(ul >> 8); t[11] = (uint8_t)ul;
        memcpy(t + 12, f + 34, ul); t[12 + 6] = 0; t[12 + 7] = 0;
        cs = ip_csum(t, 12 + ul); if (!cs) cs = 0xFFFF;
        f[40] = (uint8_t)(cs >> 8); f[41] = (uint8_t)cs;
    }
    if (len < 60) len = 60;
    EMU_LOG("DHCP %s from %02x:%02x:%02x:%02x:%02x:%02x -> %s %u.%u.%u.%u", type == 1 ? "DISCOVER" : "REQUEST", src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5], type == 1 ? "OFFER" : "ACK", s_client_ip[0], s_client_ip[1], s_client_ip[2], s_client_ip[3]);
    nic_rx_push(f, len);
}

/* every frame the title transmits arrives here (tunnel hook point) */
static void nic_tx_frame(const uint8_t *f, unsigned n)
{
    unsigned et, k;
    if (n < 14) { EMU_LOG("runt frame %u bytes", n); return; }
    et = (unsigned)(f[12] << 8 | f[13]);
    if (et == 0x0806 && n >= 42) {                       /* ARP */
        unsigned op = (unsigned)(f[20] << 8 | f[21]);
        EMU_LOG("ARP op %u who-has %u.%u.%u.%u tell %u.%u.%u.%u", op, f[38], f[39], f[40], f[41], f[28], f[29], f[30], f[31]);
        if (op == 1 && !memcmp(f + 38, k_router_ip, 4)) {
            uint8_t r[60]; memset(r, 0, sizeof r);
            memcpy(r, f + 6, 6); memcpy(r + 6, k_router_mac, 6); r[12] = 8; r[13] = 6;
            r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4; r[21] = 2;
            memcpy(r + 22, k_router_mac, 6); memcpy(r + 28, k_router_ip, 4);
            memcpy(r + 32, f + 22, 6);       memcpy(r + 38, f + 28, 4);
            nic_rx_push(r, 60);
        }
        return;
    }
    if (et == 0x0800 && n >= 42 && f[23] == 17) {         /* IPv4/UDP */
        unsigned ihl = (unsigned)(f[14] & 15) * 4, sp, dp;
        if (ihl >= 20 && n >= 14 + ihl + 8) {
            sp = (unsigned)(f[14 + ihl] << 8 | f[15 + ihl]); dp = (unsigned)(f[16 + ihl] << 8 | f[17 + ihl]);
            if (dp == 67) { nic_dhcp(f + 14 + ihl + 8, n - 14 - ihl - 8, f + 6); return; }
            {   /* c459: sampled log (first 3 in full, then every 100th) so a long session fits: counts, rate and a full hex of a few frames */
                static unsigned nudp; static DWORD t_first;
                nudp++; if (nudp == 1) t_first = GetTickCount();
                if (nudp <= 3 || nudp % 100 == 0) {
                    EMU_LOG("UDP #%u (+%lu ms) %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len %u dst-mac %02x:%02x:%02x:%02x:%02x:%02x", nudp, (unsigned long)(GetTickCount() - t_first), f[26], f[27], f[28], f[29], sp, f[30], f[31], f[32], f[33], dp, n, f[0], f[1], f[2], f[3], f[4], f[5]);
                    if (nudp <= 3 || nudp % 500 == 0) { for (k = 0; k < n; k++) { if (k % 32 == 0) fprintf(stderr, "\n[NICEMU]   "); fprintf(stderr, "%02X ", f[k]); } fprintf(stderr, "\n"); fflush(stderr); }
                }
            }
            return;
        }
    } else {
        EMU_LOG("frame ethertype 0x%04X proto %u len %u dst-mac %02x:%02x:%02x:%02x:%02x:%02x", et, n > 23 ? f[23] : 0, n, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
    for (k = 0; k < n && k < 96 && s_emu_lines < 3000; k++) { if (k % 32 == 0) fprintf(stderr, "\n[NICEMU]   "); fprintf(stderr, "%02X ", f[k]); }
    fprintf(stderr, "\n"); fflush(stderr);
}

static int nic_call_isr(void)
{
    uint32_t kint = xbox_GetConnectedInterrupt(NIC_VECTOR), routine, context;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    nic_func_t fn; int slot, claimed;
    if (!kint) kint = xbox_GetConnectedInterrupt(0);   /* c448: the title's NIC driver connects on vector 0 here (seen in the c448 run), not 4 */
    if (!kint) return -1;
    routine = *(uint32_t *)(mem + kint); context = *(uint32_t *)(mem + kint + 4);
    if (!routine) return -1;
    fn = recomp_lookup(routine); if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) return -1;
    slot = xbox_worker_stack_alloc(); if (slot < 0) return -1;
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;
    { int il = xbox_IrqlEnterInterrupt(16); t_in_isr = 1; fn(); t_in_isr = 0; xbox_IrqlLeaveInterrupt(il); }
    claimed = (int)(g_eax & 1u);
    xbox_worker_stack_free(slot);
    return claimed;
}

static DWORD WINAPI nic_thread(LPVOID unused)
{
    static uint8_t pkt[2048]; unsigned pkt_n = 0, isr_n = 0, tx_total = 0, rx_total = 0;
    int have_tib = 0;
    (void)unused;
    for (;;) {
        uint32_t cfg, txb, rxb, ntx, nrx, i; uint8_t *mem; volatile uint32_t *st; uint32_t mask;
        Sleep(5);
        memcpy(&txb, s_shadow + 0x100, 4); memcpy(&rxb, s_shadow + 0x104, 4); memcpy(&cfg, s_shadow + 0x108, 4);
        if (!txb || !rxb) continue;
        if (!have_tib) {
            uint32_t tib = xbox_AllocThreadTib();
            if (!tib) { fprintf(stderr, "  [NICEMU] no TIB for the NIC thread; stopping\n"); return 0; }
            g_fs_base = tib; have_tib = 1;
        }
        mem = (uint8_t *)xbox_GetMemoryOffset();
        st = (volatile uint32_t *)s_shadow;
        ntx = (cfg & 0x3FF) + 1; nrx = ((cfg >> 16) & 0x3FF) + 1;
        if (ntx > 64) ntx = 64;
        if (nrx > 64) nrx = 64;
        __try {
            volatile uint32_t *td = (volatile uint32_t *)(mem + (0x80000000u | txb)), *rd = (volatile uint32_t *)(mem + (0x80000000u | rxb));
            unsigned done = 0, got = 0;
            /* TX: take every owned descriptor, in order */
            for (i = 0; i < ntx; i++) {
                uint32_t fl = td[(s_txi % ntx) * 2 + 1], buf = td[(s_txi % ntx) * 2], len;
                if (!(fl & 0x80000000u)) break;
                len = (fl & 0xFFFF) + 1;
                if (pkt_n + len <= sizeof pkt) { memcpy(pkt + pkt_n, mem + (uintptr_t)buf, len); pkt_n += len; }
                td[(s_txi % ntx) * 2 + 1] = fl & ~0xC0000000u;       /* hand back: OWN and ERROR clear */
                s_txi = (s_txi + 1) % ntx;
                if (fl & 0x00010000u) { nic_tx_frame(pkt, pkt_n); pkt_n = 0; done++; tx_total++; }
            }
            /* RX: copy queued frames into the title's empty buffers */
            for (;;) {
                uint32_t fl, buf; unsigned n;
                EnterCriticalSection(&s_rxq_lock);
                if (s_rxq_h == s_rxq_t) { LeaveCriticalSection(&s_rxq_lock); break; }
                n = s_rxq[s_rxq_h].n;
                fl = rd[(s_rxi % nrx) * 2 + 1]; buf = rd[(s_rxi % nrx) * 2];
                if (!(fl & 0x80000000u)) { LeaveCriticalSection(&s_rxq_lock); break; }   /* no empty buffer: keep it queued */
                memcpy(mem + (0x80000000u | (uintptr_t)buf), s_rxq[s_rxq_h].d, n);   /* c458: RX buffers live in the contiguous window (0x80000000|phys), not the low alias (the low alias is different memory here: c448-c457 wrote the frame into low RAM and the driver read zeros) */
                s_rxq_h = (s_rxq_h + 1) % RXQ_N;
                LeaveCriticalSection(&s_rxq_lock);
                rd[(s_rxi % nrx) * 2 + 1] = (uint32_t)n | 0x00110000u;     /* c453: OWN clear, DESCRIPTORVALID(bit16) + BIT4(bit20), length = exact frame size (behaviour per xemu nvnet, research only) */
                s_rxi = (s_rxi + 1) % nrx; got++; rx_total++; if (s_calltrace) g_nic_trace_until = GetTickCount() + 120;
            }
            if (done) InterlockedOr((volatile LONG *)st, (LONG)NIC_IRQ_TXOK);
            if (got)  InterlockedOr((volatile LONG *)st, (LONG)NIC_IRQ_RX);
            if (done || got) EMU_LOG("tx done %u (total %u), rx delivered %u (total %u), irq status 0x%X mask 0x%X", done, tx_total, got, rx_total, *st, *(volatile uint32_t *)(s_shadow + 4));
        } __except (1) { EMU_LOG("ring access fault (tx 0x%X rx 0x%X)", txb, rxb); Sleep(500); continue; }
        mask = *(volatile uint32_t *)(s_shadow + 4);
        if ((*st & mask) && !xbox_IrqlBlocksInterrupts()) {
            int c = nic_call_isr();
            if (isr_n < 20) { isr_n++; EMU_LOG("ISR %s (status 0x%X mask 0x%X)", c < 0 ? "not callable" : c ? "claimed" : "declined", *st, mask); }
        }
    }
}

static void nic_emu_write(uint32_t off, uint32_t val)
{
    (void)val;
    if (off == 0x100) s_txi = 0;    /* ring base rewritten: the NIC restarts at descriptor 0 */
    if (off == 0x104) s_rxi = 0;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    if (off + (unsigned)size > NIC_SIZE) return;
    if (s_emu && off == 0 && size == 4) {   /* c448: interrupt status is write-1-to-clear */
        InterlockedAnd((volatile LONG *)s_shadow, ~(LONG)(uint32_t)val);
        s_cnt_w[0]++;
        if (s_lines < s_max) { s_lines++; fprintf(stderr, "[NIC] %6lu W +0x000/4 = 0x%llX (clear)\n", (unsigned long)(GetTickCount() - s_t0), (unsigned long long)val); }
        return;
    }
    memcpy(s_shadow + off, &val, (size_t)size);
    s_cnt_w[off >> 2]++;
    s_last[off >> 2] = (uint32_t)val;
    if (s_lines < s_max) {
        s_lines++;
        fprintf(stderr, "[NIC] %6lu W +0x%03X/%d = 0x%llX\n", (unsigned long)(GetTickCount() - s_t0), off, size, (unsigned long long)val);
    }
    if (s_trace && off == 0x144 && (val & 1)) { nic_dump_rx(); nic_dump_tx(); }
    if (s_emu) nic_emu_write(off, (uint32_t)val);
}

void xbox_NvnetInit(void)
{
    static int done;
    ptrdiff_t off;
    DWORD old;
    const char *e, *m;
    if (done) return;
    done = 1;
    e = getenv("STE_NIC_TRACE");
    s_trace = e && *e && *e != '0';
    e = getenv("STE_ETHERNET_LINK");
    s_emu = e && !_stricmp(e, "up");
    if (!s_trace && !s_emu) return;
    m = getenv("STE_NIC_TRACE_MAX");
    if (m && atoi(m) > 0) s_max = (unsigned)atoi(m);
    if (!s_trace) s_max = 0;      /* emulation without the register trace: only the [NICEMU] lines */
    if (s_emu) {
        { const char *ct = getenv("STE_NIC_CALLTRACE"); s_calltrace = ct && *ct && *ct != 0x30; }
        const char *sl = getenv("STE_ONLINE_SLOT");   /* 1..4: which address the virtual router hands out (10.77.0.<slot+1>) */
        if (sl && atoi(sl) >= 1 && atoi(sl) <= 4) s_client_ip[3] = (uint8_t)(atoi(sl) + 1);
        InitializeCriticalSection(&s_rxq_lock);
    }
    off = xbox_GetMemoryOffset();
    if (!off) { fprintf(stderr, "  NIC trace: guest memory not mapped yet; off\n"); return; }
    if (!VirtualProtect((LPVOID)((uintptr_t)off + NIC_VA), NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  NIC trace: cannot trap 0x%08X (error %lu); off\n", NIC_VA, (unsigned long)GetLastError());
        return;
    }
    s_t0 = GetTickCount(); s_on = 1;
    if (s_trace) {
        atexit(xbox_NvnetReport);
        fprintf(stderr, "  NIC trace: 0x%08X..0x%08X trapped, logging only (max %u lines)\n", NIC_VA, NIC_VA + NIC_SIZE, s_max);
    }
    if (s_emu) {
        HANDLE th = CreateThread(NULL, 0, nic_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
        fprintf(stderr, "  NIC emulation (online mode, STE_ETHERNET_LINK=up): minimal NIC + virtual router 10.77.0.1, client %u.%u.%u.%u\n", s_client_ip[0], s_client_ip[1], s_client_ip[2], s_client_ip[3]);
    }
    fflush(stderr);
}

int xbox_NvnetOwnsAddress(uint32_t va)
{
    return s_on && va >= NIC_VA && va < NIC_VA + NIC_SIZE;
}

int xbox_NvnetHandleMmio(void *ctx, uint32_t va)
{
    static unsigned fails;
    int ok;
    if (!xbox_NvnetOwnsAddress(va)) return 0;
    ok = mmio_emulate((PCONTEXT)ctx, va - NIC_VA, NULL, nic_read, nic_write);
    if (!ok && fails++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        fprintf(stderr, "[NIC] undecoded access at +0x%03X: %02X %02X %02X %02X %02X %02X\n", va - NIC_VA, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
}

void xbox_NvnetReport(void)
{
    unsigned i, any = 0;
    if (!s_on) return;
    fprintf(stderr, "[NIC] summary (offset: reads / writes / last value):\n");
    for (i = 0; i < NIC_SIZE / 4; i++)
        if (s_cnt_r[i] || s_cnt_w[i]) { any++; fprintf(stderr, "[NIC]   +0x%03X: %u / %u / 0x%X\n", i * 4, s_cnt_r[i], s_cnt_w[i], s_last[i]); }
    if (!any) fprintf(stderr, "[NIC]   (the title never touched the NIC page)\n");
    fflush(stderr);
}

#else
void xbox_NvnetInit(void) {}
int xbox_NvnetOwnsAddress(uint32_t va) { (void)va; return 0; }
int xbox_NvnetHandleMmio(void *ctx, uint32_t va) { (void)ctx; (void)va; return 0; }
void xbox_NvnetReport(void) {}
void xbox_NvnetNoteLink(int verify, uint32_t result) { (void)verify; (void)result; }
int xbox_NvnetEmuOn(void) { return 0; }
int xbox_NvnetGetMac(uint8_t mac[6]) { (void)mac; return 0; }
int xbox_NvnetInIsr(void) { return 0; }
void xbox_NvnetNoteDpc(uint32_t dpc, uint32_t routine) { (void)dpc; (void)routine; }
int xbox_NvnetIsNicDpc(uint32_t dpc) { (void)dpc; return 0; }
#endif
