/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <shellapi.h>   /* c244: CommandLineToArgvW */
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00015586  /* XBE entry point VA */
/* c258: no built-in game path any more (release binaries carry no development paths): --game, STE_GAME_PATH, or a default.xbe next to the exe */


/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const WCHAR *path, void **out_data, size_t *out_size);

/* c244: game location for the launcher. Precedence: --game <path> (or --game=<path>), env STE_GAME_PATH, then the built-in development default above.
 * <path> is a folder that holds default.xbe. A disc image (.iso/.xiso) is recognised but cannot be mounted yet (read-only XISO support is the next step): exit code 3.
 * Exit codes: 0 normal exit, 1 startup failure (XBE load / memory layout), 2 bad command line, 3 game path unusable. */
/* c246: release version. STE_VERSION is passed by tests\make_launcher_cand.ps1 (/D STE_VERSION="x.y.z"); the same value goes into the exe's VERSIONINFO resource (FileVersion x.y.z.<candidate>,
 * ProductVersion x.y.z+c<candidate>). `ste-recomp.exe --version` prints "ste-recomp x.y.z (STE_CANDIDATE_ID=<n>;)" on stdout and exits 0 before anything else is touched. */
#ifndef STE_VERSION
#define STE_VERSION "0.0.0-dev"
#endif
extern const char ste_candidate_id[];
static int ste_early_args(void)
{
    int argc = 0, i, rc = -1; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; ++i) {
        if (!wcscmp(argv[i], L"--version")) { printf("ste-recomp %s (%s)\n", STE_VERSION, ste_candidate_id); rc = 0; break; }
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            printf("ste-recomp %s (%s)\nusage: ste-recomp.exe [--game <folder with default.xbe>] [--console] [--version] [--help]\n"
                   "  --console       open a console that mirrors the log (or env STE_CONSOLE=1); otherwise the log goes to <logs>\\ste_<time>.log.\n"
                   "  --game <path>   game folder (else env STE_GAME_PATH). Exit codes: 0 ok, 1 startup failure, 2 bad command line, 3 game path unusable.\n"
                   "  env: STE_VIDEO_INI STE_ENHANCE_INI STE_GAME_PATH STE_NO_DIALOG (see docs\\LAUNCHER_INTERFACE.md)\n", STE_VERSION, ste_candidate_id);
            rc = 0; break;
        }
    }
    if (argv) LocalFree(argv);
    return rc;
}
static WCHAR g_game_dir_w[MAX_PATH], g_xbe_path_w[MAX_PATH];
static char g_game_dir[MAX_PATH * 3];
static const char *g_game_src = "built-in default";

static int ste_fail(int code, const WCHAR *msg)
{
    fprintf(stderr, "[CONFIG] %ls\n", msg);
    if (!GetEnvironmentVariableW(L"STE_NO_DIALOG", NULL, 0)) MessageBoxW(NULL, msg, L"Godzilla STE", MB_ICONERROR);   /* STE_NO_DIALOG=1: stderr + exit code only (launcher / scripts) */
    return code;
}

static int ste_resolve_game(void)
{
    WCHAR raw[MAX_PATH] = L"", full[MAX_PATH], msg[MAX_PATH + 200]; int argc = 0, i, from_cli = 0; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; ++i) {
        if (!wcscmp(argv[i], L"--game")) {
            if (i + 1 >= argc) return ste_fail(2, L"--game needs a path (a folder that contains default.xbe)");
            wcsncpy_s(raw, MAX_PATH, argv[++i], _TRUNCATE); from_cli = 1;
        } else if (!wcsncmp(argv[i], L"--game=", 7)) { wcsncpy_s(raw, MAX_PATH, argv[i] + 7, _TRUNCATE); from_cli = 1; }
    }
    if (from_cli) g_game_src = "--game";
    else if (GetEnvironmentVariableW(L"STE_GAME_PATH", raw, MAX_PATH) > 0 && raw[0]) g_game_src = "env STE_GAME_PATH";
    else {   /* neither given: a default.xbe next to the exe */
        WCHAR *sl; GetModuleFileNameW(NULL, raw, MAX_PATH); sl = wcsrchr(raw, L'\\'); if (sl) *sl = 0;
        swprintf_s(full, MAX_PATH, L"%ls\\default.xbe", raw);
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) { if (argv) LocalFree(argv); return ste_fail(2, L"no game folder given: use --game <folder that contains default.xbe> (or set STE_GAME_PATH)"); }
        g_game_src = "exe folder";
    }
    if (argv) LocalFree(argv);
    if (!raw[0]) return ste_fail(2, L"empty game path");
    if (!GetFullPathNameW(raw, MAX_PATH, full, NULL)) return ste_fail(3, L"cannot resolve the game path");
    { size_t n = wcslen(full); while (n > 3 && (full[n - 1] == L'\\' || full[n - 1] == L'/')) full[--n] = 0; }
    {
        DWORD at = GetFileAttributesW(full);
        if (at == INVALID_FILE_ATTRIBUTES) { swprintf_s(msg, MAX_PATH + 200, L"game path not found: %ls (from %hs)", full, g_game_src); return ste_fail(3, msg); }
        if (!(at & FILE_ATTRIBUTE_DIRECTORY)) {
            const WCHAR *x = wcsrchr(full, L'.');
            if (x && (!_wcsicmp(x, L".iso") || !_wcsicmp(x, L".xiso")))
                swprintf_s(msg, MAX_PATH + 200, L"%ls is a disc image. Mounting XISO images is not implemented yet: extract it and pass the folder that contains default.xbe.", full);
            else swprintf_s(msg, MAX_PATH + 200, L"game path is a file, not a folder: %ls", full);
            return ste_fail(3, msg);
        }
    }
    swprintf_s(g_xbe_path_w, MAX_PATH, L"%ls\\default.xbe", full);
    if (GetFileAttributesW(g_xbe_path_w) == INVALID_FILE_ATTRIBUTES) { swprintf_s(msg, MAX_PATH + 200, L"no default.xbe in %ls (from %hs)", full, g_game_src); return ste_fail(3, msg); }
    wcscpy_s(g_game_dir_w, MAX_PATH, full);
    WideCharToMultiByte(CP_UTF8, 0, full, -1, g_game_dir, (int)sizeof g_game_dir, NULL, NULL);
    /* the game folder is the user's data: nothing under it may be written through the file shim (same guard the development tree has; an explicit RECOMP_GAME_OWNED_ROOT wins) */
    if (!GetEnvironmentVariableW(L"RECOMP_GAME_OWNED_ROOT", NULL, 0)) SetEnvironmentVariableW(L"RECOMP_GAME_OWNED_ROOT", full);
    fprintf(stderr, "[CONFIG] game=%s (%s)\n", g_game_dir, g_game_src);
    return 0;
}

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── c367: console-less (Windows subsystem) stdio ─────────────────────────────
 * The release exe is linked /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup (no console window). stdout/stderr then are:
 *   - the caller's redirect (launcher pipe, `> file`) when one was inherited: left alone;
 *   - else a log file  <logs>\ste_<yyyymmdd-hhmmss>.log  (<logs> = sibling "logs" of the settings folder of STE_VIDEO_INI, else <exe>\data\logs, else <exe>\logs; STE_LOG_DIR overrides);
 *   - with --console or STE_CONSOLE=1: a console (attached to the parent's when there is one, else a new one) that mirrors the same text, through a pipe + tee thread.
 * --version / --help attach to the parent console (never create one). ste_tee_drain() is called by the crash filter before it terminates the process so the last lines are not lost. */
#include <io.h>
#include <fcntl.h>
static HANDLE g_tee_rd, g_tee_con, g_tee_file;
static DWORD WINAPI ste_tee_thread(LPVOID unused)
{
    char buf[4096]; DWORD n, w; (void)unused;
    for (;;) {
        if (!ReadFile(g_tee_rd, buf, sizeof buf, &n, NULL) || !n) break;
        if (g_tee_con) WriteFile(g_tee_con, buf, n, &w, NULL);
        if (g_tee_file && g_tee_file != INVALID_HANDLE_VALUE) WriteFile(g_tee_file, buf, n, &w, NULL);
    }
    return 0;
}
void ste_tee_drain(void)
{
    /* No PeekNamedPipe here: the tee thread sits in a synchronous ReadFile on the same pipe handle and a Peek would block behind it. The thread wakes as soon as data arrives, so a short sleep lets it pass the last lines on. */
    if (g_tee_rd) Sleep(120);
}
static int ste_dir_exists(const char *d) { DWORD a = GetFileAttributesA(d); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int ste_log_path(char *out, size_t n)
{
    const char *e = getenv("STE_LOG_DIR"), *vi = getenv("STE_VIDEO_INI"); char dir[MAX_PATH], tmp[MAX_PATH], *p; SYSTEMTIME st;
    dir[0] = 0;
    if (e && e[0]) { strncpy(dir, e, sizeof dir - 1); dir[sizeof dir - 1] = 0; }
    else {
        if (vi && vi[0]) { strncpy(tmp, vi, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0; p = strrchr(tmp, '\\'); if (p) { *p = 0; p = strrchr(tmp, '\\'); if (p) { strcpy(p + 1, "logs"); if (ste_dir_exists(tmp)) strcpy(dir, tmp); } } }
        if (!dir[0]) { GetModuleFileNameA(NULL, tmp, sizeof tmp); p = strrchr(tmp, '\\'); if (p) { strcpy(p + 1, "data\\logs"); if (ste_dir_exists(tmp)) strcpy(dir, tmp); else { strcpy(p + 1, "logs"); strcpy(dir, tmp); } } }
    }
    if (!dir[0]) return 0;
    CreateDirectoryA(dir, NULL);
    if (!ste_dir_exists(dir)) return 0;
    GetLocalTime(&st);
    _snprintf(out, n, "%s\\ste_%04d%02d%02d-%02d%02d%02d.log", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond); out[n - 1] = 0;
    return 1;
}
/* In a Windows-subsystem process the CRT's stdout/stderr FILEs start with an invalid descriptor (-2), and the FILE structs cannot be re-created by hand (opaque in the UCRT): give them valid descriptors with freopen on NUL, then dup2 the real target over those descriptors. */
static void ste_rebind_files(void)
{
    FILE *fo = NULL, *fe = NULL;
    freopen_s(&fo, "NUL", "w", stdout); freopen_s(&fe, "NUL", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
}
static void ste_bind_stdio(HANDLE h)
{
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY);
    if (fd < 0) return;
    ste_rebind_files();
    _dup2(fd, _fileno(stdout)); _dup2(fd, _fileno(stderr));
    SetStdHandle(STD_OUTPUT_HANDLE, h); SetStdHandle(STD_ERROR_HANDLE, h);
}
static void ste_stdio_setup(void)
{
    int argc = 0, i, want_console = 0, attach_only = 0, redirected; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HANDLE he = GetStdHandle(STD_ERROR_HANDLE), con = NULL, file = INVALID_HANDLE_VALUE; char lp[MAX_PATH]; const char *ce = getenv("STE_CONSOLE");
    for (i = 1; argv && i < argc; ++i) {
        if (!wcscmp(argv[i], L"--console")) want_console = 1;
        else if (!wcscmp(argv[i], L"--version") || !wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) attach_only = 1;
    }
    if (argv) LocalFree(argv);
    if (ce && *ce && *ce != 0x30) want_console = 1;
    redirected = he && he != INVALID_HANDLE_VALUE && GetFileType(he) != FILE_TYPE_UNKNOWN;
    if (attach_only && !redirected) { if (AttachConsole(ATTACH_PARENT_PROCESS)) { con = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); if (con != INVALID_HANDLE_VALUE) { ste_bind_stdio(con); return; } } }
    if (redirected && !want_console) { if (_fileno(stdout) < 0 || GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) ste_bind_stdio(he); else { setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); } return; }
    if (redirected) file = he;   /* --console with a redirect: mirror into the caller's handle */
    else if (ste_log_path(lp, sizeof lp)) file = CreateFileA(lp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!want_console) { if (file != INVALID_HANDLE_VALUE) ste_bind_stdio(file); return; }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    con = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (con == INVALID_HANDLE_VALUE) { if (file != INVALID_HANDLE_VALUE) ste_bind_stdio(file); return; }
    {   HANDLE rd, wr; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
        if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { ste_bind_stdio(con); return; }
        g_tee_rd = rd; g_tee_con = con; g_tee_file = file;
        CreateThread(NULL, 0, ste_tee_thread, NULL, 0, NULL); atexit(ste_tee_drain);   /* normal exits flush the mirror too */
        ste_bind_stdio(wr);
    }
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    { int rc = ste_early_args(); if (rc >= 0) return rc; rc = ste_resolve_game(); if (rc) return rc; }
    printf("=== Godzilla STE - Static Recomp (v0.14 base) ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(g_xbe_path_w, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* STE: D3D waits on the GPU's get pointer through [dev+0x30]; mirror D3D's own put counter back (candidate1, see CHECKPOINT). */
    { extern int xbox_Nv2aMirrorFence(uint32_t, uint32_t, uint32_t); xbox_Nv2aMirrorFence(0x0022E278u, 0x2C, 0x30); }
    xbox_path_init(g_game_dir, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const WCHAR *path, void **out_data, size_t *out_size)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %ls\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ste_stdio_setup();   /* c367: no console window in the release link; see the block above */
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
