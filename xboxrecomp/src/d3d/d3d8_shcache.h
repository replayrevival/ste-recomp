/* SPDX-License-Identifier: GPL-2.0-or-later
 * Persistent shader cache (c83): compiled vertex/pixel shader bytecode on disk (shadercache\ next to the exe), keyed by a hash of the
 * generated HLSL + target profile + compiler flags + cache version, plus an asynchronous compile queue so a cache miss never stalls a draw. */
#ifndef D3D8_SHCACHE_H
#define D3D8_SHCACHE_H
#include <stdint.h>
#include <stddef.h>

/* 64-bit key of (hlsl, profile, flags). */
uint64_t shc_key(const char *hlsl, size_t len, const char *profile, unsigned flags);
/* Disk lookup. On a hit returns a malloc'd copy of the bytecode (free with shc_free) and its size; NULL on a miss. KELVIN_SHADER_NOCACHE=1 disables the disk. */
void *shc_load(const char *kind, uint64_t key, size_t *size);
void shc_free(void *p);
void shc_store(const char *kind, uint64_t key, const void *code, size_t size);
/* Compile on a worker thread (D3DCompile, then shc_store); done(ctx, code, size) runs on the worker with the bytecode (valid only during the
 * callback) or code == NULL if the compile failed. hlsl is copied. KELVIN_SHADER_SYNC=1 runs the job inline on the caller. */
typedef void (*ShcDone)(void *ctx, const void *code, size_t size);
void shc_compile_async(const char *kind, const char *hlsl, size_t len, const char *name, const char *profile, unsigned flags, uint64_t key, ShcDone done, void *ctx);
/* counters for the report */
extern volatile long g_shc_disk_hits, g_shc_compiles, g_shc_pending;
#endif
