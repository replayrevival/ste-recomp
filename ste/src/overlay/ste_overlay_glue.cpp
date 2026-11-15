// SPDX-License-Identifier: GPL-2.0-or-later
// ste_overlay_glue.cpp (c375): the game side of the launcher's in-game overlay (launcher\docs\OVERLAY_FOR_GAME_SESSION.md).
//
//  - ste_overlay_frame(device, rtv, w, h) is called by the Direct3D scan-out path once per presented frame, on the render thread, AFTER the
//    game frame and any upscale and right BEFORE Present (draw last). It creates the overlay lazily, turns the input module's requests
//    (F1, Guide, Back+Start held) into show/hide, feeds pad / keyboard navigation while it is open and draws it.
//  - While it is open the game gets no input (the input module asks ste_overlay_visible()) and the game thread is held at its frame
//    hook (ste_overlay_wait_if_paused()), so the match is paused.
//  - "Return to launcher" exits with code 20, "Quit game" with 0.
//  - Settings: the overlay writes the same ini files as the launcher; apply_setting() tells it what the game applied at once.
// Disable with STE_OVERLAY=0. Resources (ui\, assets\, config\) are looked up in STE_OVERLAY_RES, else next to the exe.
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ste_overlay_host.h"

extern "C" {
int ste_overlay_poll(int* why, int* port);
int ste_sdl_slot_open(int slot);
const char* ste_sdl_slot_guid(int slot);
int ste_sdl_raw_buttons(int slot, unsigned* mask, int* lx, int* ly);   // bit0 up 1 down 2 left 3 right 4 A 5 B 6 start 7 back; stick -32768..32767 (SDL up = negative)
const char* ste_sdl_poll_source(void);                                 // name of a newly pressed pad button (SDL3 names) or NULL
void ste_sdl_keys_clear(void);
void ste_sdl_rumble(int slot, int port, unsigned lo, unsigned hi);
void ste_audio_set_master_volume(int percent);
}

namespace {
std::unique_ptr<ste_overlay::Host> g_host;
std::atomic<int> g_visible{0};
bool g_failed = false;
int g_w = 0, g_h = 0;
ULONGLONG g_last = 0;

// keyboard presses from the window procedure (main thread), drained on the render thread: vk values, 0 = empty
std::atomic<unsigned> g_keyq[64]; std::atomic<unsigned> g_kq_w{0}; unsigned g_kq_r = 0;

std::string exe_dir()
{
    char b[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, b, MAX_PATH); std::string s(b, n);
    size_t p = s.find_last_of("\\/"); return p == std::string::npos ? std::string(".") : s.substr(0, p);
}
bool dir_exists(const std::string& d) { DWORD a = GetFileAttributesA(d.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::string env(const char* k) { const char* e = getenv(k); return e ? e : ""; }


int g_cursor_mode = 0;   // 0 auto (hide after 3 s without movement), 1 always hidden over the window, 2 never
bool g_cursor_hidden = false; HCURSOR g_arrow = nullptr; POINT g_last_pt = {0, 0}; ULONGLONG g_moved = 0;
BOOL CALLBACK find_wnd(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) { RECT r; GetClientRect(h, &r); if (r.right > 100) { *(HWND*)lp = h; return FALSE; } }
    return TRUE;
}
void cursor_tick(bool overlay_open)
{
    static HWND wnd = nullptr; static ULONGLONG chk = 0; ULONGLONG t = GetTickCount64();
    if (!wnd || t - chk > 2000) { chk = t; HWND h = nullptr; EnumWindows(find_wnd, (LPARAM)&h); if (h) wnd = h; }
    if (!wnd) return;
    if (!g_arrow) g_arrow = LoadCursorA(nullptr, IDC_ARROW);
    POINT pt; GetCursorPos(&pt); if (pt.x != g_last_pt.x || pt.y != g_last_pt.y) { g_last_pt = pt; g_moved = t; }
    bool want_hide = !overlay_open && (g_cursor_mode == 1 || (g_cursor_mode == 0 && t - g_moved > 3000));
    if (want_hide != g_cursor_hidden) {
        g_cursor_hidden = want_hide;
        SetClassLongPtrA(wnd, GCLP_HCURSOR, (LONG_PTR)(want_hide ? nullptr : g_arrow));
        POINT p2; GetCursorPos(&p2); SetCursorPos(p2.x, p2.y);   // makes Windows send WM_SETCURSOR again
    }
}
void read_ini_defaults()
{
    std::string ini = env("STE_VIDEO_INI"); if (ini.empty()) return;
    char b[64] = ""; GetPrivateProfileStringA("window", "hide_cursor", "auto", b, sizeof b, ini.c_str());
    if (!_stricmp(b, "always")) g_cursor_mode = 1; else if (!_stricmp(b, "never")) g_cursor_mode = 2; else g_cursor_mode = 0;
    int v = (int)GetPrivateProfileIntA("audio", "master_volume", 100, ini.c_str()); if (v < 0) v = 0; if (v > 100) v = 100;
    ste_audio_set_master_volume(v);
    fprintf(stderr, "[SETTINGS] [audio] master_volume=%d  [window] hide_cursor=%s\n", v, g_cursor_mode == 1 ? "always" : g_cursor_mode == 2 ? "never" : "auto"); fflush(stderr);
}

ste_overlay::Applied apply_setting(const std::string& key, const std::string& value)
{
    using A = ste_overlay::Applied;
    fprintf(stderr, "[OVERLAY] setting %s = '%s'\n", key.c_str(), value.c_str()); fflush(stderr);
    auto ends = [&](const char* suf) { size_t n = strlen(suf); return key.size() >= n && key.compare(key.size() - n, n, suf) == 0; };
    if (ends("master_volume")) { int v = value.empty() ? 100 : atoi(value.c_str()); if (v < 0) v = 0; if (v > 100) v = 100; ste_audio_set_master_volume(v); return A::Now; }
    if (ends("hide_cursor")) { g_cursor_mode = (value == "always") ? 1 : (value == "never") ? 2 : 0; return A::Now; }
    if (key == "input.mapping") return A::Now;   // the input module re-reads the file on its next start; changes apply to the next match
    if (key == "input.button_prompts") return A::NextMatch;
    if (key.rfind("enhance.", 0) == 0) return A::NextMatch;   // the enhancement toggles are read when a match starts
    return A::AfterRestart;                                   // video.*, window mode, resolution ...: saved, used at the next start
}

void make(void* device)
{
    if (env("STE_OVERLAY") != "1") { fprintf(stderr, "[OVERLAY] off by default in this build (STE_OVERLAY=1 enables the experimental in-game overlay; see docs/REPORT_0.1.4.md)\n"); fflush(stderr); g_failed = true; return; }
    std::string res = env("STE_OVERLAY_RES"); if (res.empty()) res = exe_dir();
    if (!dir_exists(res + "\\ui") || !dir_exists(res + "\\config")) {
        fprintf(stderr, "[OVERLAY] resources (ui\\, assets\\, config\\) not found next to the game (%s): overlay disabled\n", res.c_str()); fflush(stderr);
        g_failed = true; return;
    }
    std::string data = env("STE_OVERLAY_DATA");
    if (data.empty()) { std::string m = env("STE_INPUT_MAP"); size_t p = m.find_last_of("\\/"); if (p != std::string::npos) data = m.substr(0, p); }
    if (data.empty()) data = exe_dir() + "\\data";
    CreateDirectoryA(data.c_str(), nullptr);
    ste_overlay::InitInfo info; info.resource_dir = res; info.data_dir = data; info.device = (ID3D11Device*)device; info.width = g_w; info.height = g_h; info.content_scale = 1.0f;
    info.callbacks.apply_setting = apply_setting;
    info.callbacks.on_show = [] { g_visible = 1; fprintf(stderr, "[OVERLAY] opened: game paused, input goes to the overlay\n"); fflush(stderr); };
    info.callbacks.on_hide = [] { g_visible = 0; ste_sdl_keys_clear(); fprintf(stderr, "[OVERLAY] closed: game resumed\n"); fflush(stderr); };
    info.callbacks.return_to_launcher = [] { fprintf(stderr, "[OVERLAY] return to launcher: exit code 20\n"); fflush(stderr); ExitProcess(20); };
    info.callbacks.quit_game = [] { fprintf(stderr, "[OVERLAY] quit game\n"); fflush(stderr); ExitProcess(0); };
    info.callbacks.rumble = [](unsigned pad_id, float strength, unsigned ms) -> bool {
        if (!pad_id || !ste_sdl_slot_open((int)pad_id - 1)) return false;
        unsigned v = (unsigned)(strength * 60000.f); ste_sdl_rumble((int)pad_id - 1, 0, v, v); Sleep(ms > 1000 ? 1000 : ms); ste_sdl_rumble((int)pad_id - 1, 0, 0, 0); return true; };
    std::string err;
    g_host = ste_overlay::Host::create(info, err);
    if (!g_host) { fprintf(stderr, "[OVERLAY] could not start: %s (overlay disabled)\n", err.c_str()); fflush(stderr); g_failed = true; return; }
    std::vector<ste_overlay::Pad> pads;
    for (int s = 0; s < 8; ++s) if (ste_sdl_slot_open(s)) { ste_overlay::Pad p; p.guid = ste_sdl_slot_guid(s); p.name = "Controller"; p.id = (unsigned)s + 1; pads.push_back(p); }
    g_host->set_pads(pads);
    fprintf(stderr, "[OVERLAY] ready (resources %s, data %s, %d pad(s))\n", res.c_str(), data.c_str(), (int)pads.size()); fflush(stderr);
}


std::string key_source(unsigned vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string("key:") + (char)vk;
    switch (vk) {
        case VK_LEFT: return "key:Left"; case VK_RIGHT: return "key:Right"; case VK_UP: return "key:Up"; case VK_DOWN: return "key:Down";
        case VK_RETURN: return "key:Return"; case VK_SPACE: return "key:Space"; case VK_TAB: return "key:Tab"; case VK_BACK: return "key:Backspace";
        case VK_SHIFT: case VK_LSHIFT: return "key:Left Shift"; case VK_RSHIFT: return "key:Right Shift"; case VK_CONTROL: case VK_LCONTROL: return "key:Left Ctrl"; case VK_RCONTROL: return "key:Right Ctrl";
        case VK_OEM_COMMA: return "key:,"; case VK_OEM_PERIOD: return "key:."; default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return "key:F" + std::to_string(vk - VK_F1 + 1);
    return std::string();
}

// edge + repeat generator for one pad direction / button
struct Rep { ULONGLONG next = 0; bool down = false; };
bool step(Rep& r, bool now, ULONGLONG t)
{
    if (!now) { r.down = false; return false; }
    if (!r.down) { r.down = true; r.next = t + 400; return true; }
    if (t >= r.next) { r.next = t + 120; return true; }
    return false;
}
void nav(ste_overlay::Nav n) { ste_overlay::Input in; in.type = ste_overlay::Input::Type::Nav; in.nav = n; g_host->feed(in); }
void source(const std::string& s) { ste_overlay::Input in; in.type = ste_overlay::Input::Type::Source; in.source = s; g_host->feed(in); }

void pump_input(ULONGLONG t)
{
    // keyboard
    for (;;) {
        unsigned w = g_kq_w.load();
        if (g_kq_r == w) break;
        unsigned vk = g_keyq[g_kq_r % 64].load(); g_kq_r++;
        if (g_host->listening()) { std::string n = vk == VK_ESCAPE ? std::string("cancel") : key_source(vk); if (!n.empty()) source(n); continue; }
        switch (vk) {
            case VK_UP: nav(ste_overlay::Nav::Up); break;       case VK_DOWN: nav(ste_overlay::Nav::Down); break;
            case VK_LEFT: nav(ste_overlay::Nav::Left); break;   case VK_RIGHT: nav(ste_overlay::Nav::Right); break;
            case VK_RETURN: case VK_SPACE: nav(ste_overlay::Nav::Confirm); break;
            case VK_ESCAPE: case VK_BACK: case VK_F1: nav(ste_overlay::Nav::Back); break;
            default: break;
        }
    }
    // pads
    static Rep up, dn, lf, rt, a, b;
    bool U = false, D = false, L = false, R = false, A = false, B = false;
    for (int s = 0; s < 8; ++s) {
        unsigned m = 0; int lx = 0, ly = 0;
        if (!ste_sdl_raw_buttons(s, &m, &lx, &ly)) continue;
        U |= (m & 1u) || ly < -16000; D |= (m & 2u) || ly > 16000; L |= (m & 4u) || lx < -16000; R |= (m & 8u) || lx > 16000; A |= (m & 16u) != 0; B |= (m & 32u) != 0;
        if (g_host->listening()) { const char* n = ste_sdl_poll_source(); if (n) source(n); }
    }
    if (g_host->listening()) return;
    if (step(up, U, t)) nav(ste_overlay::Nav::Up);   if (step(dn, D, t)) nav(ste_overlay::Nav::Down);
    if (step(lf, L, t)) nav(ste_overlay::Nav::Left); if (step(rt, R, t)) nav(ste_overlay::Nav::Right);
    if (step(a, A, t)) nav(ste_overlay::Nav::Confirm); if (step(b, B, t)) nav(ste_overlay::Nav::Back);
}
}  // namespace

extern "C" {
// window procedure -> overlay: a key went down (VK code)
void ste_overlay_key(int vk) { if (g_visible.load() && vk > 0) { unsigned w = g_kq_w.load(); g_keyq[w % 64].store((unsigned)vk); g_kq_w.store(w + 1); } }
int ste_overlay_visible(void) { return g_visible.load(); }
// the game thread calls this from its frame hook: holds the game while the overlay is open
void ste_overlay_wait_if_paused(void) { while (g_visible.load()) Sleep(8); }

void ste_overlay_frame(void* device, void* rtv, int w, int h)
{
    { static bool ini_done; if (!ini_done) { ini_done = true; read_ini_defaults(); } cursor_tick(g_visible.load() != 0); }
    if (g_failed || !device || !rtv) return;
    if (!g_host) {
        static int tries;
        if (tries++ == 0) {
            g_w = w; g_h = h;
            // RmlUi / nlohmann parse deeply: run the start-up on a thread with a big stack (the scan-out thread has a small one), then join
            struct Arg { void* dev; } arg{device};
            HANDLE th = CreateThread(nullptr, 32u << 20, [](LPVOID p) -> DWORD { make(((Arg*)p)->dev); return 0; }, &arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); } else make(device);
        }
        if (!g_host) return;
        g_last = GetTickCount64();
    }
    if (w != g_w || h != g_h) { g_w = w; g_h = h; g_host->resize(w, h, 1.0f); }
    ULONGLONG t = GetTickCount64();
    int why = 0, port = 0;
    while (ste_overlay_poll(&why, &port)) { if (g_host->visible()) g_host->hide(); else g_host->show(); }
    if (g_host->visible()) pump_input(t);
    g_host->update((double)(t - g_last) / 1000.0); g_last = t;
    g_host->render((ID3D11RenderTargetView*)rtv);
}
}
