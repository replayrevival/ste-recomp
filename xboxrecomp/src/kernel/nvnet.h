/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NVNET_H
#define NVNET_H
#include <stdint.h>
/* NIC (0xFEF00000) register trace, opt-in via STE_NIC_TRACE=1. Logging only; see nvnet.c. */
void xbox_NvnetInit(void);
int  xbox_NvnetOwnsAddress(uint32_t xbox_va);
int  xbox_NvnetHandleMmio(void *ctx, uint32_t xbox_va);
void xbox_NvnetNoteLink(int verify, uint32_t result);   /* c444: trace of PhyGetLinkState calls (no-op unless tracing) */
int  xbox_NvnetEmuOn(void);                  /* c448: online mode (STE_ETHERNET_LINK=up): minimal NIC + virtual router active */
int  xbox_NvnetGetMac(uint8_t mac[6]);       /* c448: this install's random locally administered MAC; 0 when not in online mode */
int  xbox_NvnetInIsr(void);                      /* c454: logging hooks for the NIC DPC path */
void xbox_NvnetNoteDpc(uint32_t dpc, uint32_t routine);
int  xbox_NvnetIsNicDpc(uint32_t dpc);
void xbox_NvnetReport(void);   /* per-offset summary; call at exit */
#endif
