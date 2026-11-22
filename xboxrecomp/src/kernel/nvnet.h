/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVNET_H
#define NVNET_H
#include <stdint.h>
/* NIC (0xFEF00000) register trace, opt-in via STE_NIC_TRACE=1. Logging only; see nvnet.c. */
void xbox_NvnetInit(void);
int  xbox_NvnetOwnsAddress(uint32_t xbox_va);
int  xbox_NvnetHandleMmio(void *ctx, uint32_t xbox_va);
void xbox_NvnetReport(void);   /* per-offset summary; call at exit */
#endif
