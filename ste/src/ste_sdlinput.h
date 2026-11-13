/* SPDX-License-Identifier: GPL-2.0-or-later
 * ste_sdlinput.h - SDL3 gamepad input for the Godzilla STE recomp (see ste_sdlinput.c). */
#ifndef STE_SDLINPUT_H
#define STE_SDLINPUT_H
#include "xinput_xbox.h"
#ifdef __cplusplus
extern "C" {
#endif
int         ste_sdl_init(void);                 /* 1 = SDL3 backend active, 0 = use XInput */
void        ste_buttons_resolve(int sdl_ok);    /* c355: [input] button_prompts -> STE_BUTTON_PROMPTS_RESOLVED (0 = no SDL: auto stays xbox_original) */
int         ste_sdl_active(void);
void        ste_sdl_update(void);               /* poll SDL, refresh the pad list (cheap) */
int         ste_sdl_pad_count(void);
int         ste_sdl_slot_open(int slot);
const char *ste_sdl_slot_guid(int slot);
const char *ste_sdl_map_device(int port);       /* mapping file: "auto" | "keyboard" | "none" | <guid>; NULL = no file */
int         ste_sdl_pick(unsigned claimed, const char *guid);   /* first unclaimed open pad (optionally of a GUID), connection order; -1 none */
int         ste_sdl_nth(int n);                 /* n-th open pad in connection order; -1 none */
int         ste_sdl_get_state(int slot, int port, XBOX_INPUT_STATE *st);
void        ste_sdl_rumble(int slot, int port, unsigned lo, unsigned hi);
void        ste_sdl_key(int vk, int down);      /* window procedure -> key state (focus-aware) */
void        ste_sdl_keys_clear(void);
int         ste_sdl_keyboard_apply(int port, XBOX_INPUT_STATE *st);
void        ste_sdl_chord_check(int port, const XBOX_INPUT_STATE *st);
int         ste_sdl_display_bounds(int index, int *x, int *y, int *w, int *h, int *count);   /* c252: display `index` in SDL_GetDisplays order (the launcher's `display` key); 0 = out of range / failed */
int         ste_overlay_poll(int *why, int *port);   /* 1 = a request is pending; why: 1 guide, 2 key, 3 Back+Start chord */
#ifdef __cplusplus
}
#endif
#endif
