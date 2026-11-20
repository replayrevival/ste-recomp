/*
 * kernel_soundpack.c - sound packs (audio mods), STE 0.2.3 (c432)
 *
 * A sound pack is a mod ("profile": "sounds") whose sounds\ folder holds WAV files named after the game's sounds:
 *   <bank>_<3-digit slot>[_cue name].wav   bank = godzilla_sound | shell_sound | rail_fpm | godzilla_voice_e | godzilla_voice_j  (a bank sample)
 *   <track>.wav                            track = the name of a SoundData\*.wxv file (fight1, menu, Victory1, Gigan ...)       (a whole music/jingle stream)
 * At start the mods system calls xbox_soundpack_build() with the ACTIVE sound packs in load order (later wins). For every bank or stream with at least one
 * replacement this module reads the ORIGINAL file (game folder or the read-only XISO), builds the modified file in <mods>\_soundcache\<key>\ and reports it
 * through the emit callback, which registers it as a mods file override. The game and the audio layer are untouched: the game simply reads a different file.
 * No sound pack = this code is never called = byte-identical behaviour. Replacement WAV: PCM 16-bit, mono or stereo, any rate (others are refused and logged).
 * Format facts (docs\formats\soundpack.md): banks = .samp (per sample: 0x14-byte WAVEFORMATEX + Xbox IMA ADPCM mono 36-byte blocks) + .uber (SDIR table of
 * 16-byte slots: data offset, data length, loop flag, header offset); music = RIFF WAVE, Xbox IMA ADPCM stereo 48 kHz, block 72.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <math.h>
#include <ctype.h>
#include "xiso_vfs.h"

#define SP_MAX_SECONDS 60
static const char *k_banks[] = { "godzilla_sound", "shell_sound", "rail_fpm", "godzilla_voice_e", "godzilla_voice_j" };
#define NBANKS 5

typedef void (*sp_emit)(const WCHAR *rel_lc, const WCHAR *host, void *ctx);

/* ------------------------------------------------------------ original game file */
static uint8_t *read_original(const WCHAR *game_dir, const WCHAR *rel, uint32_t *size)
{
    WCHAR p[MAX_PATH]; uint8_t *b = NULL; *size = 0;
    swprintf_s(p, MAX_PATH, L"%s\\%s", game_dir, rel);
    if (xiso_active()) {
        XisoEntry e; uint32_t got = 0;
        if (!xiso_lookup(p, &e) || e.is_dir) return NULL;
        b = (uint8_t *)malloc(e.size ? e.size : 1); if (!b) return NULL;
        if (!xiso_read(&e, 0, b, e.size, &got) || got != e.size) { free(b); return NULL; }
        *size = e.size; return b;
    } else {
        HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); LARGE_INTEGER sz; DWORD got = 0;
        if (h == INVALID_HANDLE_VALUE) return NULL;
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFF0) { CloseHandle(h); return NULL; }
        b = (uint8_t *)malloc((size_t)sz.QuadPart + 1); if (!b) { CloseHandle(h); return NULL; }
        if (!ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart) { free(b); CloseHandle(h); return NULL; }
        CloseHandle(h); *size = (uint32_t)sz.QuadPart; return b;
    }
}

static int write_file(const WCHAR *path, const void *data, size_t n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD w = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(h, data, (DWORD)n, &w, NULL) || w != n) { CloseHandle(h); return 0; }
    CloseHandle(h); return 1;
}

/* ------------------------------------------------------------ WAV */
typedef struct { int16_t *pcm; uint32_t frames; int ch; uint32_t rate; int loop; } Wav;
static void wav_free(Wav *w) { free(w->pcm); memset(w, 0, sizeof *w); }

/* returns NULL on success or a plain-text reason */
static const char *wav_load(const uint8_t *d, size_t n, Wav *w)
{
    size_t pos = 12; int have_fmt = 0; const uint8_t *pcm = NULL; size_t pcm_n = 0; uint16_t tag = 0, ch = 0, bits = 0; uint32_t rate = 0;
    memset(w, 0, sizeof *w);
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return "not a WAV file";
    while (pos + 8 <= n) {
        uint32_t sz = *(const uint32_t *)(d + pos + 4); const uint8_t *body = d + pos + 8; size_t avail = n - pos - 8; if (sz > avail) sz = (uint32_t)avail;
        if (!memcmp(d + pos, "fmt ", 4) && sz >= 16) { tag = *(const uint16_t *)body; ch = *(const uint16_t *)(body + 2); rate = *(const uint32_t *)(body + 4); bits = *(const uint16_t *)(body + 14); have_fmt = 1; }
        else if (!memcmp(d + pos, "data", 4)) { pcm = body; pcm_n = sz; }
        else if (!memcmp(d + pos, "smpl", 4) && sz >= 60 && *(const uint32_t *)(body + 28) >= 1) w->loop = 1;
        pos += 8 + sz + (sz & 1);
    }
    if (!have_fmt || !pcm) return "no fmt/data chunk";
    if (tag != 1) return "not PCM: use PCM 16-bit (no float, no compressed WAV)";
    if (bits != 16) return "not 16-bit: use PCM 16-bit";
    if (ch != 1 && ch != 2) return "use mono or stereo";
    if (rate < 4000 || rate > 192000) return "sample rate out of range";
    w->ch = ch; w->rate = rate; w->frames = (uint32_t)(pcm_n / (2u * ch));
    if (!w->frames) return "empty";
    if ((double)w->frames / rate > SP_MAX_SECONDS) return "longer than 60 seconds";
    w->pcm = (int16_t *)malloc((size_t)w->frames * ch * 2); if (!w->pcm) return "out of memory";
    memcpy(w->pcm, pcm, (size_t)w->frames * ch * 2);
    return NULL;
}

/* to a planar float-free int16 buffer of nch channels at 'rate': channel mix + linear resample with a box pre-filter when downsampling */
static int16_t *convert(const Wav *w, int nch, uint32_t rate, uint32_t *out_frames)
{
    uint32_t nf = (uint32_t)((double)w->frames * rate / w->rate + 0.5), i; int16_t *o; double ratio = (double)w->rate / rate; int c;
    if (!nf) nf = 1;
    o = (int16_t *)malloc((size_t)nf * nch * 2); if (!o) return NULL;
    for (i = 0; i < nf; i++) {
        double pos = i * ratio, v[2] = { 0, 0 }; int taps = ratio > 1.5 ? (int)ratio : 1; int t;
        for (c = 0; c < nch; c++) {
            double acc = 0;
            for (t = 0; t < taps; t++) {
                double p = pos + t - (taps - 1) * 0.5; int64_t i0; double fr; double s0, s1;
                if (p < 0) p = 0; i0 = (int64_t)p; fr = p - (double)i0; if (i0 >= (int64_t)w->frames - 1) { i0 = (int64_t)w->frames - 1; fr = 0; }
                {   /* source value for output channel c (mono source -> both; stereo source -> mono mixes L+R) */
                    int sc0 = (w->ch == 1) ? 0 : (nch == 1 ? -1 : c);
                    #define SMP(ix) (sc0 < 0 ? ((double)w->pcm[(ix) * 2] + w->pcm[(ix) * 2 + 1]) * 0.5 : (double)w->pcm[(ix) * w->ch + sc0])
                    s0 = SMP(i0); s1 = (i0 + 1 < (int64_t)w->frames) ? SMP(i0 + 1) : s0;
                    #undef SMP
                }
                acc += s0 + (s1 - s0) * fr;
            }
            v[c] = acc / taps;
        }
        for (c = 0; c < nch; c++) { double x = v[c]; if (x > 32767) x = 32767; if (x < -32768) x = -32768; o[(size_t)i * nch + c] = (int16_t)floor(x + 0.5); }
    }
    *out_frames = nf; return o;
}

/* ------------------------------------------------------------ Xbox IMA ADPCM encoder (inverse of the APU decoder in apu_state.h) */
static const uint16_t step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const int index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* pcm: interleaved nch channels, 'frames' frames -> bytes: blocks of 64 samples per channel (4 byte header per channel + 8 words of 4 bytes per channel). *out_n = size */
static uint8_t *adpcm_encode(const int16_t *pcm, uint32_t frames, int nch, size_t *out_n)
{
    uint32_t blocks = (frames + 63) / 64, b; size_t bs = 36u * nch; uint8_t *out = (uint8_t *)malloc(bs * blocks + 1); int idx[2] = { 0, 0 };
    if (!out) return NULL;
    for (b = 0; b < blocks; b++) {
        uint8_t *blk = out + (size_t)b * bs; int c, k, g; uint8_t nib[2][64]; int32_t seg[2][65];
        for (c = 0; c < nch; c++) for (k = 0; k < 65; k++) { uint32_t f = b * 64u + k; if (f >= frames) f = frames - 1; seg[c][k] = pcm[(size_t)f * nch + c]; }
        for (c = 0; c < nch; c++) {
            int32_t pred = seg[c][0]; int ix = idx[c];
            blk[c * 4 + 0] = (uint8_t)(pred & 255); blk[c * 4 + 1] = (uint8_t)((pred >> 8) & 255); blk[c * 4 + 2] = (uint8_t)ix; blk[c * 4 + 3] = 0;
            for (k = 1; k <= 64; k++) {
                int32_t diff = seg[c][k] - pred, step = step_table[ix], d = step >> 3; int n = 0;
                if (diff < 0) { n = 8; diff = -diff; }
                if (diff >= step) { n |= 4; diff -= step; d += step; }
                step >>= 1; if (diff >= step) { n |= 2; diff -= step; d += step; }
                step >>= 1; if (diff >= step) { n |= 1; d += step; }
                pred += (n & 8) ? -d : d; if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
                ix += index_table[n & 7]; if (ix < 0) ix = 0; if (ix > 88) ix = 88;
                nib[c][k - 1] = (uint8_t)n;
            }
            idx[c] = ix;
        }
        {   /* data: 8 groups of (per channel 4 bytes = 8 nibbles, low nibble first) */
            uint8_t *p = blk + nch * 4;
            for (g = 0; g < 8; g++) for (c = 0; c < nch; c++) for (k = 0; k < 4; k++) *p++ = (uint8_t)(nib[c][g * 8 + k * 2] | (nib[c][g * 8 + k * 2 + 1] << 4));
        }
    }
    *out_n = bs * blocks; return out;
}

/* ------------------------------------------------------------ bank rebuild */
#define RD32(p) (*(const uint32_t *)(p))
#define WR32(p, v) (*(uint32_t *)(p) = (uint32_t)(v))
typedef struct { Wav w; int have; } Repl;

/* returns 1 and the new samp/uber (malloc'd) when at least one slot was replaced */
static int bank_rebuild(const uint8_t *uber, uint32_t un, const uint8_t *samp, uint32_t sn, Repl *rep, int nslots_rep, uint8_t **o_uber, uint8_t **o_samp, uint32_t *o_sn, char *why, size_t whyn)
{
    uint32_t sdir, cnt, k; uint8_t *nu, *ns; uint32_t pos = 0; int changed = 0;
    if (un < 0x30 || memcmp(uber, "UBER", 4)) { snprintf(why, whyn, "uber header"); return 0; }
    sdir = RD32(uber + 16);   /* second section offset */
    if (sdir + 16 > un || memcmp(uber + sdir, "SDIR", 4)) { snprintf(why, whyn, "no SDIR"); return 0; }
    cnt = RD32(uber + sdir + 12);
    if (sdir + 16 + (size_t)cnt * 16 > un) { snprintf(why, whyn, "SDIR size"); return 0; }
    nu = (uint8_t *)malloc(un); memcpy(nu, uber, un);
    {   size_t cap = (size_t)sn + 32u * 1024 * 1024;   /* head-room: replaced sounds are <= 60 s */
        ns = (uint8_t *)malloc(cap); if (!ns) { free(nu); return 0; }
        for (k = 0; k < cnt; k++) {
            const uint8_t *e = uber + sdir + 16 + k * 16; uint32_t doff = RD32(e), dlen = RD32(e + 4), loop = RD32(e + 8), hoff = RD32(e + 12); uint8_t *ne = nu + sdir + 16 + k * 16;
            if (doff == 0xFFFFFFFFu || !dlen) continue;   /* null entry */
            if (hoff + 0x14 > sn || doff + dlen > sn) { snprintf(why, whyn, "slot %u outside .samp", k); free(nu); free(ns); return 0; }
            if ((int)k < nslots_rep && rep[k].have) {
                uint32_t rate = *(const uint32_t *)(samp + hoff + 4), nf; int16_t *m = convert(&rep[k].w, 1, rate, &nf); size_t an; uint8_t *ad;
                if (!m) { snprintf(why, whyn, "memory"); free(nu); free(ns); return 0; }
                ad = adpcm_encode(m, nf, 1, &an); free(m); if (!ad) { snprintf(why, whyn, "memory"); free(nu); free(ns); return 0; }
                if (pos + 0x14 + an > cap) { free(ad); snprintf(why, whyn, "bank too large"); free(nu); free(ns); return 0; }
                memcpy(ns + pos, samp + hoff, 0x14); WR32(ns + pos + 8, (uint32_t)((uint64_t)rate * 36 / 64));   /* avg bytes per second */
                memcpy(ns + pos + 0x14, ad, an); free(ad);
                WR32(ne, pos + 0x14); WR32(ne + 4, an); WR32(ne + 8, rep[k].w.loop ? 1 : 0); WR32(ne + 12, pos); pos += 0x14 + (uint32_t)an; changed = 1;
            } else {
                if (pos + 0x14 + dlen > cap) { snprintf(why, whyn, "bank too large"); free(nu); free(ns); return 0; }
                memcpy(ns + pos, samp + hoff, 0x14); memcpy(ns + pos + 0x14, samp + doff, dlen);
                WR32(ne, pos + 0x14); WR32(ne + 4, dlen); WR32(ne + 8, loop); WR32(ne + 12, pos); pos += 0x14 + dlen;
            }
        }
    }
    if (!changed) { free(nu); free(ns); return 0; }
    WR32(nu + sdir + 8, pos);   /* SDIR: total .samp size */
    {   /* the cue records (POOL) carry their own loop flag: block = 10 00 <slot u16> 00 00 00 00 07 <loop> ..; set it for every replaced slot (the pack decides) */
        uint32_t pool = RD32(uber + 12), j;
        for (j = pool + 16; j + 10 <= sdir; j++) {
            if (nu[j] == 0x10 && nu[j + 1] == 0 && !nu[j + 4] && !nu[j + 5] && !nu[j + 6] && !nu[j + 7] && nu[j + 8] == 0x07) {
                uint32_t slot = nu[j + 2] | ((uint32_t)nu[j + 3] << 8);
                if ((int)slot < nslots_rep && rep[slot].have) nu[j + 9] = rep[slot].w.loop ? 1 : 0;
            }
        }
    }
    *o_uber = nu; *o_samp = ns; *o_sn = pos; return 1;
}

/* ------------------------------------------------------------ music (.wxv) */
static uint8_t *wxv_build(const Wav *w, size_t *out_n)
{
    uint32_t nf; int16_t *m = convert(w, 2, 48000, &nf); size_t an; uint8_t *ad, *f; uint32_t total;
    if (!m) return NULL;
    ad = adpcm_encode(m, nf, 2, &an); free(m); if (!ad) return NULL;
    total = 12 + 8 + 0x14 + 8 + (uint32_t)an; f = (uint8_t *)malloc(total);
    if (!f) { free(ad); return NULL; }
    memcpy(f, "RIFF", 4); WR32(f + 4, total - 8); memcpy(f + 8, "WAVEfmt ", 8); WR32(f + 16, 0x14);
    *(uint16_t *)(f + 20) = 0x69; *(uint16_t *)(f + 22) = 2; WR32(f + 24, 48000); WR32(f + 28, 54000); *(uint16_t *)(f + 32) = 72; *(uint16_t *)(f + 34) = 4; *(uint16_t *)(f + 36) = 2; *(uint16_t *)(f + 38) = 64;
    memcpy(f + 40, "data", 4); WR32(f + 44, (uint32_t)an); memcpy(f + 48, ad, an); free(ad);
    *out_n = total; return f;
}

/* ------------------------------------------------------------ driver */
static uint32_t fnv(uint32_t h, const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; size_t i; for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

typedef struct { WCHAR path[MAX_PATH]; char stem[160]; int pack; } WavFile;

/* dirs: the ACTIVE sound-pack folders in load order (later wins). Returns the number of files emitted. log (UTF-8) receives one line per problem / result. */
int xbox_soundpack_build(const WCHAR *game_dir, const WCHAR *cache_root, const WCHAR *const *dirs, const char *const *ids, int npacks, sp_emit emit, void *ctx, void (*logf)(const char *line, void *c), void *lctx)
{
    WavFile *wf = NULL; size_t nwf = 0, cap = 0; int p, emitted = 0; uint32_t key = 2166136261u; key = fnv(key, "sp2-poolloop", 12);   /* loader version: a change in the build rules must not reuse an old cache */ char line[400];
    for (p = 0; p < npacks; p++) {
        WCHAR pat[MAX_PATH]; WIN32_FIND_DATAW fd; HANDLE h; int pcount = 0; int sub;
        for (sub = 0; sub < 2; sub++) {
            if (sub == 0) swprintf_s(pat, MAX_PATH, L"%s\\sounds\\*.wav", dirs[p]); else swprintf_s(pat, MAX_PATH, L"%s\\*.wav", dirs[p]);
            h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) continue;
            do {
                WavFile *f; size_t l;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (nwf == cap) { cap = cap ? cap * 2 : 64; wf = (WavFile *)realloc(wf, cap * sizeof *wf); if (!wf) return 0; }
                f = &wf[nwf++]; f->pack = p;
                swprintf_s(f->path, MAX_PATH, sub == 0 ? L"%s\\sounds\\%s" : L"%s\\%s", dirs[p], fd.cFileName);
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, f->stem, sizeof f->stem, NULL, NULL);
                l = strlen(f->stem); if (l > 4) f->stem[l - 4] = 0;
                key = fnv(key, f->stem, strlen(f->stem)); key = fnv(key, &fd.nFileSizeLow, 4); key = fnv(key, &fd.ftLastWriteTime, 8); key = fnv(key, &p, 4); pcount++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        snprintf(line, sizeof line, "sound pack %s: %s, %d file(s)", ids[p], pcount ? "ACTIVE" : "skipped (no WAV files)", pcount); logf(line, lctx);
    }
    if (!nwf) { free(wf); return 0; }
    {   WCHAR cdir[MAX_PATH]; int b; size_t i;
        swprintf_s(cdir, MAX_PATH, L"%s\\%08x", cache_root, key);
        CreateDirectoryW(cache_root, NULL); CreateDirectoryW(cdir, NULL);
        /* banks */
        for (b = 0; b < NBANKS; b++) {
            Repl *rep = NULL; int nrep = 0, any = 0; uint8_t *u, *s; uint32_t un, sn; WCHAR rel_u[MAX_PATH], rel_s[MAX_PATH], host[MAX_PATH]; char nb[64];
            u = NULL; s = NULL;
            for (i = 0; i < nwf; i++) {
                size_t bl = strlen(k_banks[b]); int slot;
                if (_strnicmp(wf[i].stem, k_banks[b], bl) || wf[i].stem[bl] != '_' || !isdigit((unsigned char)wf[i].stem[bl + 1]) || !isdigit((unsigned char)wf[i].stem[bl + 2]) || !isdigit((unsigned char)wf[i].stem[bl + 3])) continue;
                if (wf[i].stem[bl + 4] && wf[i].stem[bl + 4] != '_') continue;
                slot = atoi(wf[i].stem + bl + 1);
                if (slot >= nrep) { rep = (Repl *)realloc(rep, (size_t)(slot + 1) * sizeof *rep); memset(rep + nrep, 0, (size_t)(slot + 1 - nrep) * sizeof *rep); nrep = slot + 1; }
                {   HANDLE h = CreateFileW(wf[i].path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); LARGE_INTEGER sz; uint8_t *buf; DWORD got = 0; const char *why;
                    if (h == INVALID_HANDLE_VALUE) continue;
                    GetFileSizeEx(h, &sz); if (sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); snprintf(line, sizeof line, "  %s: file too large, ignored", wf[i].stem); logf(line, lctx); continue; }
                    buf = (uint8_t *)malloc((size_t)sz.QuadPart + 1); ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL); CloseHandle(h);
                    if (rep[slot].have) wav_free(&rep[slot].w);   /* later pack wins */
                    why = wav_load(buf, got, &rep[slot].w); free(buf);
                    if (why) { snprintf(line, sizeof line, "  %s: refused (%s)", wf[i].stem, why); logf(line, lctx); rep[slot].have = 0; } else { size_t sl = strlen(wf[i].stem); if (sl >= 5 && !_stricmp(wf[i].stem + sl - 5, "_loop")) rep[slot].w.loop = 1; rep[slot].have = 1; any = 1; }
                }
            }
            if (!any) { free(rep); continue; }
            swprintf_s(rel_u, MAX_PATH, L"SoundData\\%S.uber", k_banks[b]); swprintf_s(rel_s, MAX_PATH, L"SoundData\\%S.samp", k_banks[b]);
            u = read_original(game_dir, rel_u, &un); s = read_original(game_dir, rel_s, &sn);
            if (!u || !s) { snprintf(line, sizeof line, "  %s: original bank not found, replacement skipped", k_banks[b]); logf(line, lctx); }
            else {
                uint8_t *nu = NULL, *ns = NULL; uint32_t nsn = 0; char why[96] = "";
                snprintf(nb, sizeof nb, "%s", k_banks[b]);
                swprintf_s(host, MAX_PATH, L"%s\\%S.samp", cdir, nb);
                if (GetFileAttributesW(host) != INVALID_FILE_ATTRIBUTES) {   /* built by an earlier start with the same pack contents */
                    WCHAR hu[MAX_PATH]; swprintf_s(hu, MAX_PATH, L"%s\\%S.uber", cdir, nb);
                    {   WCHAR lc[MAX_PATH]; swprintf_s(lc, MAX_PATH, L"sounddata\\%S.samp", nb); emit(lc, host, ctx); swprintf_s(lc, MAX_PATH, L"sounddata\\%S.uber", nb); emit(lc, hu, ctx); }
                    emitted += 2; snprintf(line, sizeof line, "  bank %s: cached build reused", nb); logf(line, lctx);
                } else if (bank_rebuild(u, un, s, sn, rep, nrep, &nu, &ns, &nsn, why, sizeof why)) {
                    WCHAR hu[MAX_PATH], lc[MAX_PATH]; swprintf_s(hu, MAX_PATH, L"%s\\%S.uber", cdir, nb);
                    if (write_file(hu, nu, un) && write_file(host, ns, nsn)) {
                        swprintf_s(lc, MAX_PATH, L"sounddata\\%S.samp", nb); emit(lc, host, ctx); swprintf_s(lc, MAX_PATH, L"sounddata\\%S.uber", nb); emit(lc, hu, ctx); emitted += 2;
                        snprintf(line, sizeof line, "  bank %s rebuilt (%u -> %u bytes)", nb, sn, nsn); logf(line, lctx);
                    } else { snprintf(line, sizeof line, "  bank %s: cannot write the cache folder", nb); logf(line, lctx); }
                    free(nu); free(ns);
                } else { snprintf(line, sizeof line, "  bank %s: not rebuilt (%s)", nb, why[0] ? why : "no usable replacement"); logf(line, lctx); }
            }
            free(u); free(s);
            { int k; for (k = 0; k < nrep; k++) if (rep[k].have) wav_free(&rep[k].w); } free(rep);
        }
        /* music / jingle streams: <name>.wav replaces SoundData\<name>.wxv */
        for (i = 0; i < nwf; i++) {
            WIN32_FIND_DATAW fd; HANDLE h; WCHAR pat[MAX_PATH], rel[MAX_PATH], host[MAX_PATH], lc[MAX_PATH]; char nm[200]; size_t j; int isbank = 0;
            for (b = 0; b < NBANKS; b++) if (!_strnicmp(wf[i].stem, k_banks[b], strlen(k_banks[b])) && wf[i].stem[strlen(k_banks[b])] == '_') isbank = 1;
            if (isbank) continue;
            for (j = i + 1; j < nwf; j++) if (!_stricmp(wf[j].stem, wf[i].stem)) break;
            if (j < nwf) continue;   /* a later pack has the same name: it wins */
            snprintf(nm, sizeof nm, "%s", wf[i].stem);
            swprintf_s(rel, MAX_PATH, L"SoundData\\%S.wxv", nm);
            {   uint32_t on; uint8_t *orig = read_original(game_dir, rel, &on);
                if (!orig) { snprintf(line, sizeof line, "  %s: no such sound in the game (%s.wxv not found), ignored", nm, nm); logf(line, lctx); continue; }
                free(orig); }
            swprintf_s(host, MAX_PATH, L"%s\\%S.wxv", cdir, nm); (void)fd; (void)h; (void)pat;
            swprintf_s(lc, MAX_PATH, L"sounddata\\%S.wxv", nm);
            if (GetFileAttributesW(host) == INVALID_FILE_ATTRIBUTES) {
                HANDLE hh = CreateFileW(wf[i].path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); LARGE_INTEGER sz; uint8_t *buf; DWORD got = 0; Wav w; const char *why; uint8_t *out; size_t on;
                if (hh == INVALID_HANDLE_VALUE) continue;
                GetFileSizeEx(hh, &sz); if (sz.QuadPart > 256 * 1024 * 1024) { CloseHandle(hh); continue; }
                buf = (uint8_t *)malloc((size_t)sz.QuadPart + 1); ReadFile(hh, buf, (DWORD)sz.QuadPart, &got, NULL); CloseHandle(hh);
                why = wav_load(buf, got, &w); free(buf);
                if (why) { snprintf(line, sizeof line, "  %s: refused (%s)", nm, why); logf(line, lctx); continue; }
                out = wxv_build(&w, &on); wav_free(&w);
                if (!out || !write_file(host, out, on)) { snprintf(line, sizeof line, "  %s: cannot build the stream", nm); logf(line, lctx); free(out); continue; }
                free(out); snprintf(line, sizeof line, "  stream %s built (%u bytes)", nm, (unsigned)on); logf(line, lctx);
            } else { snprintf(line, sizeof line, "  stream %s: cached build reused", nm); logf(line, lctx); }
            emit(lc, host, ctx); emitted++;
        }
    }
    free(wf); return emitted;
}
