/* SPDX-License-Identifier: GPL-2.0-or-later
 * Kelvin HD texture packs: dump mode, replacement lookup, texture-quality ini keys (c128).
 * Format: docs\formats\texturepack.md in the STE project tree. */
#ifndef KELVIN_TEXPACK_H
#define KELVIN_TEXPACK_H
#include <stdint.h>

/* One-time init (reads ste_video.ini [video] + env, scans texturepacks\). Safe to call repeatedly. */
void texpack_init(void);

int      texpack_packs_active(void);      /* at least one enabled pack with at least one texture */
int      texpack_dump_on(void);           /* STE_TEXDUMP / ini texture_dump set */
unsigned texpack_aniso(void);             /* 0 (off) | 2 | 4 | 8 | 16 */
float    texpack_mip_bias(void);          /* 0 = original */
int      texpack_want_mips(void);         /* build a mip chain for uploaded textures (packs, anisotropy or mip_bias) */

/* Stable identity of a guest texture: content hash (bytes + palette) mixed with format and size. Never depends on the guest address. */
uint64_t texpack_key(uint64_t content_hash, uint32_t format, uint32_t w, uint32_t h);

/* Dump mode: writes <dump dir>\<key16>.png once per key (skipped when the file already exists) and appends a line to index.tsv.
 * bgra = decoded A8R8G8B8 pixels (blue first in memory), pitch in bytes. */
void texpack_dump(uint64_t key, uint32_t w, uint32_t h, uint32_t format, uint32_t frame, uint32_t guest_offset, const uint8_t *bgra, uint32_t pitch);

/* Replacement lookup: returns a malloc'd BGRA8 image (free with texpack_free) or NULL when no enabled pack has this key. */
uint8_t *texpack_load(uint64_t key, uint32_t *w, uint32_t *h);
void     texpack_free(void *p);

/* 2x2 box downsample of a BGRA8 image (w,h >= 1; the destination is max(w/2,1) x max(h/2,1)). */
void texpack_downsample(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst);

/* Statistics for the periodic log line. */
void texpack_note_replaced(void);
void texpack_log_stats(uint32_t frame);
#endif
