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

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    if (off + (unsigned)size > NIC_SIZE) return;
    memcpy(s_shadow + off, &val, (size_t)size);
    s_cnt_w[off >> 2]++;
    s_last[off >> 2] = (uint32_t)val;
    if (s_lines < s_max) {
        s_lines++;
        fprintf(stderr, "[NIC] %6lu W +0x%03X/%d = 0x%llX\n", (unsigned long)(GetTickCount() - s_t0), off, size, (unsigned long long)val);
    }
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
    if (!e || !*e || *e == '0') return;
    m = getenv("STE_NIC_TRACE_MAX");
    if (m && atoi(m) > 0) s_max = (unsigned)atoi(m);
    off = xbox_GetMemoryOffset();
    if (!off) { fprintf(stderr, "  NIC trace: guest memory not mapped yet; off\n"); return; }
    if (!VirtualProtect((LPVOID)((uintptr_t)off + NIC_VA), NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  NIC trace: cannot trap 0x%08X (error %lu); off\n", NIC_VA, (unsigned long)GetLastError());
        return;
    }
    s_t0 = GetTickCount(); s_on = 1;
    atexit(xbox_NvnetReport);
    fprintf(stderr, "  NIC trace: 0x%08X..0x%08X trapped, logging only (max %u lines)\n", NIC_VA, NIC_VA + NIC_SIZE, s_max);
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
#endif
