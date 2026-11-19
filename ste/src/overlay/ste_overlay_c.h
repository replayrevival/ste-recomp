/* In-game overlay as a DLL with a plain C interface (docs/OVERLAY.md, "Hosting from a plain C game").
 *
 * Why a DLL: the overlay is C++ (RmlUi, the C++ standard library). Linked into the game program it needs MSVCP140.dll and
 * VCRUNTIME140_1.dll next to the exe, and any library built with another layout than the one the game links against crashes
 * inside the game (this happened once, see docs/NOTE_FOR_GAME_SESSION_overlay_blocker_fixed.md). As ste_overlay.dll the game program
 * stays plain C (only VCRUNTIME140.dll, as before) and loads the overlay at run time, only when it is wanted:
 *
 *     ste_overlay_api api;
 *     if (ste_overlay_load(&api, L"ste_overlay.dll")) { ... api.create(&info, err, sizeof err) ... }
 *
 * The DLL, msvcp140.dll, vcruntime140_1.dll, ui\, assets\ and config\ ship next to the game program in the launcher package.
 * The API is C89-clean; this header includes nothing but <stddef.h>. No Direct3D headers: the device and the render target view
 * are passed as void* (ID3D11Device*, ID3D11RenderTargetView*).
 *
 * Threads: create/destroy/render/resize on the render thread; feed/update/show/hide on the thread that owns input.
 * One overlay per process. Licence: GPL-2.0-or-later, like the rest of the launcher.
 */
#ifndef STE_OVERLAY_C_H
#define STE_OVERLAY_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when any struct or function below changes. ste_overlay_load() refuses a DLL with another number. */
#define STE_OVERLAY_ABI 1

typedef struct ste_overlay_handle ste_overlay_handle;

/* What the game could do with a changed setting (return value of apply_setting). */
#define STE_APPLIED_NOW 0           /* applied immediately */
#define STE_APPLIED_NEXT_MATCH 1    /* used when the next match / level starts */
#define STE_APPLIED_AFTER_RESTART 2 /* saved, used the next time the game starts */
#define STE_APPLIED_REFUSED 3       /* cannot use this value; the overlay says it is saved for the next start */

/* Input device kinds for set_input_device() and ste_overlay_pad.kind. */
#define STE_DEVICE_KEYBOARD 0
#define STE_DEVICE_XBOX 1
#define STE_DEVICE_PLAYSTATION 2
#define STE_DEVICE_SWITCH 3
#define STE_DEVICE_GENERIC 4

typedef struct ste_overlay_callbacks {
    void* user;  /* passed back as the first argument of every callback below */
    /* A setting changed. `key` is the launcher's key id ("video.fps_cap", "audio.master_volume"); `value` is what was written to the
     * ini file ("" = back to the game's default). Called after the ini file was written. May be NULL (then AFTER_RESTART). */
    int (*apply_setting)(void* user, const char* key, const char* value);
    void (*on_show)(void* user);            /* opened: pause the game, stop sending it input. May be NULL. */
    void (*on_hide)(void* user);            /* closed: resume. May be NULL. */
    void (*return_to_launcher)(void* user); /* the player confirmed "Return to launcher": exit with code 20. May be NULL. */
    void (*quit_game)(void* user);          /* the player confirmed "Quit game". May be NULL. */
    /* Vibration test: the `id` given in ste_overlay_pad, strength 0..1, milliseconds. Return non-zero if that pad can rumble. */
    int (*rumble)(void* user, unsigned pad_id, float strength, unsigned ms);
} ste_overlay_callbacks;

typedef struct ste_overlay_init {
    size_t struct_size;        /* sizeof(ste_overlay_init) */
    const char* resource_dir;  /* UTF-8: the folder holding ui\, assets\ and config\ (next to the game program) */
    const char* data_dir;      /* UTF-8: the launcher's data folder (game_settings\ste_*.ini, input_mapping.json below it) */
    void* device;              /* ID3D11Device* (feature level 10 or better, needs ID3D11Device1: Windows 8+) */
    int width, height;         /* back buffer size in pixels */
    float content_scale;       /* user / OS scale, 1.0 = normal */
    ste_overlay_callbacks callbacks;
} ste_overlay_init;

/* Input in neutral form. Coordinates are back buffer pixels. */
#define STE_INPUT_NAV 0          /* nav = STE_NAV_*; key repeats too (not releases) */
#define STE_INPUT_MOUSE_MOVE 1   /* x, y */
#define STE_INPUT_MOUSE_BUTTON 2 /* x, y, button (0 left, 1 right, 2 middle), down */
#define STE_INPUT_MOUSE_WHEEL 3  /* wheel: notches, positive = up */
#define STE_INPUT_SOURCE 4       /* only while ste_overlay_listening(): `source` = SDL3 name of the pressed button ("a", "leftshoulder",
                                    "lefttrigger", "key:<SDL_GetKeyName>", "cancel"). Do not also send it as NAV. */
#define STE_INPUT_NAV_RELEASE 5  /* nav = STE_NAV_*: that button/key was released (0.1.8). Once the game sends one, the overlay repeats a held
                                    NAV itself (after 400 ms, every 90 ms) and the game must send only the first press, not its own repeats. */
#define STE_NAV_UP 0
#define STE_NAV_DOWN 1
#define STE_NAV_LEFT 2
#define STE_NAV_RIGHT 3
#define STE_NAV_CONFIRM 4
#define STE_NAV_BACK 5
#define STE_NAV_PREV_TAB 6  /* LB / L1: previous page (0.1.8) */
#define STE_NAV_NEXT_TAB 7  /* RB / R1: next page (0.1.8) */

typedef struct ste_overlay_input {
    int type;           /* STE_INPUT_* */
    int nav;            /* STE_NAV_* */
    float x, y;
    int button, down;
    float wheel;
    const char* source; /* UTF-8, only for STE_INPUT_SOURCE */
} ste_overlay_input;

typedef struct ste_overlay_pad {
    const char* guid;  /* SDL_GUIDToString(...) or any stable id of the device model */
    const char* name;
    int kind;          /* STE_DEVICE_* */
    unsigned id;       /* your own handle, passed back in rumble (0 = none) */
} ste_overlay_pad;

/* The functions, as a table filled by ste_overlay_load(). */
typedef struct ste_overlay_api {
    int abi;  /* STE_OVERLAY_ABI of the loaded DLL */
    /* Starts the overlay. Returns NULL and writes a plain-language message into err[errcap] on failure. */
    ste_overlay_handle* (*create)(const ste_overlay_init* info, char* err, size_t errcap);
    void (*destroy)(ste_overlay_handle*);
    void (*show)(ste_overlay_handle*);
    void (*hide)(ste_overlay_handle*);
    int (*visible)(const ste_overlay_handle*);
    int (*feed)(ste_overlay_handle*, const ste_overlay_input*); /* non-zero = the overlay used the event */
    void (*set_input_device)(ste_overlay_handle*, int kind);
    void (*set_pads)(ste_overlay_handle*, const ste_overlay_pad* pads, int count);
    int (*listening)(const ste_overlay_handle*);
    void (*update)(ste_overlay_handle*, double seconds);
    void (*render)(ste_overlay_handle*, void* render_target_view); /* ID3D11RenderTargetView*; no-op while hidden; never presents */
    void (*resize)(ste_overlay_handle*, int width, int height, float content_scale);
    const char* (*build_id)(void); /* e.g. "0.1.5 2026-11-15", for logs */
} ste_overlay_api;

/* The DLL's exports (what ste_overlay_load() looks up). */
#define STE_OVERLAY_EXPORT_ABI "ste_overlay_abi"
#define STE_OVERLAY_EXPORT_FILL "ste_overlay_fill_api"
typedef int (*ste_overlay_abi_fn)(void);
typedef int (*ste_overlay_fill_api_fn)(ste_overlay_api* out, size_t out_size);

#ifdef _WIN32
/* Loads the DLL and fills `api`. Returns 1 on success, 0 on failure (missing DLL or one of its runtime DLLs, wrong ABI). `error`
 * (may be NULL) receives a short message. Header-only so a plain C program needs no import library. Include <windows.h> first. */
#ifdef STE_OVERLAY_WANT_LOADER
static int ste_overlay_load(ste_overlay_api* api, const wchar_t* dll_path, char* error, size_t errcap) {
    HMODULE m;
    ste_overlay_abi_fn abi;
    ste_overlay_fill_api_fn fill;
    if (error && errcap) error[0] = 0;
    m = LoadLibraryExW(dll_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);  /* its runtime DLLs are looked up next to it */
    if (!m) {
        if (error && errcap) {
            unsigned long e = GetLastError();
            const char* p = "could not load the overlay DLL (is it, msvcp140.dll and vcruntime140_1.dll next to the game?), error ";
            size_t n = 0;
            while (p[n] && n + 12 < errcap) { error[n] = p[n]; ++n; }
            { char d[12]; int k = 0, j; if (!e) d[k++] = '0'; while (e) { d[k++] = (char)('0' + e % 10); e /= 10; } for (j = 0; j < k && n + 1 < errcap; ++j) error[n++] = d[k - 1 - j]; }
            error[n] = 0;
        }
        return 0;
    }
    abi = (ste_overlay_abi_fn)GetProcAddress(m, STE_OVERLAY_EXPORT_ABI);
    fill = (ste_overlay_fill_api_fn)GetProcAddress(m, STE_OVERLAY_EXPORT_FILL);
    if (!abi || !fill || abi() != STE_OVERLAY_ABI || !fill(api, sizeof *api)) {
        if (error && errcap) { const char* p = "the overlay DLL does not match this game (interface version)"; size_t n = 0; while (p[n] && n + 1 < errcap) { error[n] = p[n]; ++n; } error[n] = 0; }
        FreeLibrary(m);
        return 0;
    }
    return 1;
}
#endif /* STE_OVERLAY_WANT_LOADER */
#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif
#endif /* STE_OVERLAY_C_H */
