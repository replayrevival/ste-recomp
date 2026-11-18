/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xiso_vfs.h (0.1.7): read-only XISO reader, see xiso_vfs.c. */
#ifndef XISO_VFS_H
#define XISO_VFS_H
#include <stdint.h>
#include <wchar.h>

typedef struct { uint32_t sector, size; int is_dir; unsigned name_len; } XisoEntry;

/* mount the image read-only; virtual_root = the host-style path that stands for the image root (it need not exist). Returns 1 on success. */
int  xiso_mount(const wchar_t *iso_path, const wchar_t *virtual_root);
int  xiso_active(void);
const wchar_t *xiso_root(void);
/* resolve a host-style path under the virtual root (case-insensitive, either slash); the root itself resolves to the root directory */
int  xiso_lookup(const wchar_t *path, XisoEntry *out);
/* read up to len bytes at off; *got = bytes read (0 at/after the end). Returns 1 unless the image could not be read. */
int  xiso_read(const XisoEntry *e, uint64_t off, void *buf, uint32_t len, uint32_t *got);
/* index-th child of a directory in name order; name receives the entry name (>= 260 bytes). Returns 0 past the last entry. */
int  xiso_dir_entry(const XisoEntry *dir, int index, XisoEntry *out, char *name);
int  xiso_name_match(const char *pattern, const char *name);   /* DOS-style * and ? wildcards, case-insensitive */
uint64_t xiso_image_size(void);
#endif
