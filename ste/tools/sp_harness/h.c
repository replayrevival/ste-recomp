/* offline harness: includes the loader source and drives xbox_soundpack_build on a plain folder (no xiso) */
#include "E:/GodzillaSTE/tools/xboxrecomp-v014k/src/kernel/kernel_soundpack.c"
int xiso_active(void) { return 0; }
int xiso_lookup(const wchar_t *p, XisoEntry *o) { return 0; }
int xiso_read(const XisoEntry *e, uint64_t off, void *b, uint32_t l, uint32_t *g) { return 0; }
static void em(const WCHAR *rel, const WCHAR *host, void *c) { wprintf(L"EMIT %s -> %s\n", rel, host); }
static void lg(const char *l, void *c) { printf("LOG %s\n", l); }
int wmain(int argc, wchar_t **argv)
{
    const WCHAR *dirs[1] = { argv[3] }; const char *ids[1] = { "sample-sounds" };
    int n = xbox_soundpack_build(argv[1], argv[2], dirs, ids, 1, em, NULL, lg, NULL);
    printf("emitted %d\n", n); return 0;
}
