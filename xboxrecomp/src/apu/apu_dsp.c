/*
 * MCPX APU DSP (GP/EP): xemu DSP56300 interpreter (dsp/) + v0.14 doorbell ack
 *
 * STE merge: the GP/EP now run the title's DSP program (DSOUND loads dsstdfx.bin into the GP).
 * v0.14's =auto doorbell ack stays as the handshake until the GP program is running, then switches
 * itself off (the DSP program clears the doorbell). The VP mixdown below stays the audible output path
 * (monitor point is not switched to the EP). Original v0.14 header follows.
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

static const int16_t ep_silence[256][2] = { 0 };
#define IN_RANGE(a, base, count) ((a) >= (base) && (a) < (base) + (count) * 4)

static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        if (page_entry > max_sge)
            return;

        /* v0.14 physical resolution: DirectSound's SGE tables and scratch pages live in
         * MmAllocateContiguousMemory, whose physical addresses name the 0x80000000 window,
         * which here is separate storage from low RAM (see mcpx_apu_phys). */
        uint32_t prd_address = *(const uint32_t *)mcpx_apu_phys(sge_base + page_entry * 8 + 0);
        uint8_t *host = mcpx_apu_phys(((uint64_t)prd_address + offset_in_page) | mcpx_apu_phys_hint(prd_address));   /* STE: same storage as the entry's page */

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }

        if (dir) {
            memcpy(host, ptr, bytes_to_copy);
        } else {
            memcpy(ptr, host, bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }
        if (!(cur >= base && cur + bytes_to_copy <= end) || !bytes_to_copy)
            return cur;
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        if (index >= GP_OUTPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        if (index >= GP_INPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) return;
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
               (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        size_t n = len < sizeof(d->monitor.frame_buf) ? len : sizeof(d->monitor.frame_buf);
        memcpy(d->monitor.frame_buf, ptr, n);
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        if (index >= EP_OUTPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        if (index >= EP_INPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink && len <= sizeof(ep_silence)) {
            /* Since we are sinking, push silence out */
            ptr = (uint8_t *)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (end > base && cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP. addr is the offset within the GP
 * window (APU base + 0x30000). */
uint64_t mcpx_apu_gp_read(MCPXAPUState *d, hwaddr addr)
{
    uint64_t r;
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        r = dsp_read_memory(d->gp.dsp, 'X',
                            GP_DSP_MIXBUF_BASE + (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        r = dsp_read_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4);
    } else {
        r = d->gp.regs[addr & 0xFFFF];
    }
    return r;
}

void mcpx_apu_gp_write(MCPXAPUState *d, hwaddr addr, uint64_t val)
{
    qemu_mutex_lock(&d->lock);
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        dsp_write_memory(d->gp.dsp, 'X',
                         GP_DSP_MIXBUF_BASE + (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        dsp_write_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4, (uint32_t)val);
    } else if (addr == NV_PAPU_GPRST) {
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], (uint32_t)val);
        d->gp.regs[NV_PAPU_GPRST] = (uint32_t)val;
        mcpx_apu_dsp_gprst_write((uint32_t)val);
    } else {
        d->gp.regs[addr & 0xFFFF] = (uint32_t)val;
    }
    qemu_mutex_unlock(&d->lock);
}

/* Encode Processor - encoding DSP. addr is the offset within the EP window
 * (APU base + 0x50000). */
uint64_t mcpx_apu_ep_read(MCPXAPUState *d, hwaddr addr)
{
    uint64_t r;
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        r = dsp_read_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        r = dsp_read_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        r = dsp_read_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4);
    } else {
        r = d->ep.regs[addr & 0xFFFF];
    }
    return r;
}

void mcpx_apu_ep_write(MCPXAPUState *d, hwaddr addr, uint64_t val)
{
    qemu_mutex_lock(&d->lock);
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        dsp_write_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        dsp_write_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        dsp_write_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4, (uint32_t)val);
    } else if (addr == NV_PAPU_EPRST) {
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], (uint32_t)val);
        d->ep.regs[NV_PAPU_EPRST] = (uint32_t)val;
        d->ep_frame_div = 0; /* FIXME (xemu): still unsure about frame sync */
    } else {
        d->ep.regs[addr & 0xFFFF] = (uint32_t)val;
    }
    qemu_mutex_unlock(&d->lock);
}


static bool proc_running(const uint32_t *regs, hwaddr rst)
{
    return (regs[rst] & NV_PAPU_GPRST_GPRST) && (regs[rst] & NV_PAPU_GPRST_GPDSPRST);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge || s_gp_in_reset)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

static volatile int s_gp_program_running;   /* set by mcpx_apu_dsp_frame once the GP program has run */

/* Observe the GP doorbell once the ack is off: log what the title posts and whether the DSP clears it. */
static void dsp_doorbell_watch(MCPXAPUState *d)
{
    static uint32_t last = 0xFFFFFFFFu;
    static int shown;
    uint32_t *slot = dsp_ack_auto_slot(d), v;
    if (!slot) return;
    v = *slot;
    if (v != last && shown < 40) {
        shown++;
        fprintf(stderr, "[APU-DSP] doorbell: %08X -> %08X%s\n", last, v,
                (last != 0xFFFFFFFFu && last != 0 && v == 0) ? "  (cleared by the DSP program)" : "");
    }
    last = v;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;
    if (s_gp_program_running) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[APU-DSP] GP DSP program running: doorbell auto-ack OFF from here\n"); }
        dsp_doorbell_watch(d);
        return;
    }

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;

    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    dsp_set_halt_requested(d->gp.dsp, false);
    dsp_set_cycle_count(d->gp.dsp, 0);

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    dsp_set_halt_requested(d->ep.dsp, false);
    dsp_set_cycle_count(d->ep.dsp, 0);

    d->gp.realtime = false;
    d->ep.realtime = false;
    s_gp_program_running = 0;

    fprintf(stderr, "[APU] DSP GP/EP initialized (xemu DSP56300 interpreter)\n");
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    if (d->gp.dsp) dsp_destroy(d->gp.dsp);
    if (d->ep.dsp) dsp_destroy(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    {   /* GP/EP: feed the VP mixbins to the GP MIXBUF and step the DSPs (as xemu's mcpx_apu_dsp_frame) */
        bool gp_enabled = proc_running(d->gp.regs, NV_PAPU_GPRST);
        bool ep_enabled = proc_running(d->ep.regs, NV_PAPU_EPRST);
        static unsigned frames_run, nlog;
        if (gp_enabled) {
            int budget = 2000;
            for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
                uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
                for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++)
                    dsp_write_memory(d->gp.dsp, 'X', base + sample, float_to_24b(mixbins[mixbin][sample]));
            }
            dsp_start_frame(d->gp.dsp);
            dsp_set_halt_requested(d->gp.dsp, false);
            dsp_set_cycle_count(d->gp.dsp, 0);
            do {
                dsp_run(d->gp.dsp, 1000);
            } while (!dsp_get_halt_requested(d->gp.dsp) && d->gp.realtime && --budget > 0);
            g_dbg.gp.cycles = dsp_get_cycle_count(d->gp.dsp);
            frames_run++;
            if (nlog < 12 && (frames_run <= 4 || frames_run % 200 == 0)) {
                nlog++;
                fprintf(stderr, "[APU-DSP] GP frame %u: cycles=%u pc=%04X halt=%d\n", frames_run, g_dbg.gp.cycles,
                        (unsigned)d->gp.dsp->core.pc, (int)dsp_get_halt_requested(d->gp.dsp));
            }
            if (!s_gp_program_running && frames_run >= 4 && g_dbg.gp.cycles > 0) {
                /* the interpreter has been executing the GP program for several frames: the DSP handles the doorbell now */
                s_gp_program_running = 1;
            }
        }
        if (ep_enabled && d->ep_frame_div % 8 == 0) {
            int budget = 2000;
            dsp_start_frame(d->ep.dsp);
            dsp_set_halt_requested(d->ep.dsp, false);
            dsp_set_cycle_count(d->ep.dsp, 0);
            do {
                dsp_run(d->ep.dsp, 1000);
            } while (!dsp_get_halt_requested(d->ep.dsp) && d->ep.realtime && --budget > 0);
            g_dbg.ep.cycles = dsp_get_cycle_count(d->ep.dsp);
        }
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

}
