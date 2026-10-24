#include "../../src/kernel/kernel_file.c"
/* Include the file backend to drive its private save guard directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }
volatile int g_fn_watch_arm;   /* the game defines it; kernel_path.c references it */

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put(const WCHAR *p, const char *s)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(h, s, (DWORD)strlen(s), &n, NULL);
    CloseHandle(h);
}
static int has(const WCHAR *p, const char *s)
{
    char buf[64] = {0};
    DWORD n = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    return strcmp(buf, s) == 0;
}
static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int count(const WCHAR *pattern)
{
    WIN32_FIND_DATAW fd;
    int n = 0;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do n++; while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

int main(void)
{
    char root[MAX_PATH], game[MAX_PATH], save[MAX_PATH];
    WCHAR dir[MAX_PATH], f[MAX_PATH], tmp[MAX_PATH], pend[MAX_PATH], bak[MAX_PATH];
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const WCHAR *rel;
    HANDLE h;
    DWORD n;
    int i;

    GetTempPathA(MAX_PATH, root);
    sprintf(game, "%ssave-guard-test-%lu", root, GetCurrentProcessId());
    sprintf(save, "%s\\save", game);
    CreateDirectoryA(game, NULL);
    xbox_path_init(game, save);
    swprintf_s(dir, MAX_PATH, L"%S\\UDATA", game); CreateDirectoryW(dir, NULL);
    swprintf_s(dir, MAX_PATH, L"%S\\UDATA\\4d530046", game); CreateDirectoryW(dir, NULL);
    swprintf_s(f, MAX_PATH, L"%s\\profile.dat", dir);
    swprintf_s(tmp, MAX_PATH, L"%s.~new", f);
    swprintf_s(pend, MAX_PATH, L"%s.~pend", f);
    put(f, "OLD");

    CHECK(save_guard_temp_name(L"a.dat.~new") && save_guard_temp_name(L"a.~PEND") && !save_guard_temp_name(L"a.dat"));
    rel = save_rel(f);
    CHECK(rel && _wcsicmp(rel, L"\\UDATA\\4d530046\\profile.dat") == 0);
    CHECK(save_rel(L"C:\\elsewhere\\UDATA\\x") == NULL);

    /* Truncating rewrite: the old save stays whole until the close. */
    h = save_guard_open(f, rel, GENERIC_WRITE, share, XBOX_FILE_OVERWRITE_IF, FILE_ATTRIBUTE_NORMAL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(has(f, "OLD") && exists(tmp));
    WriteFile(h, "NEWDATA", 7, &n, NULL);
    CHECK(has(f, "OLD"));
    CHECK(save_guard_close(h));
    CHECK(has(f, "NEWDATA") && !exists(tmp) && !exists(pend));
    swprintf_s(bak, MAX_PATH, L"%S_backup\\UDATA\\4d530046\\profile.dat.%s", save, s_session_stamp);
    CHECK(has(bak, "OLD"));

    /* In-place write without truncation works on a copy of the old bytes;
     * the session backup is not replaced by the newer save. */
    h = save_guard_open(f, rel, GENERIC_WRITE, share, XBOX_FILE_OPEN, FILE_ATTRIBUTE_NORMAL);
    CHECK(h != INVALID_HANDLE_VALUE);
    WriteFile(h, "X", 1, &n, NULL);
    CHECK(save_guard_open(f, rel, GENERIC_WRITE, share, XBOX_FILE_OPEN, 0) == INVALID_HANDLE_VALUE);   /* one writer per save */
    CHECK(save_guard_close(h));
    CHECK(has(f, "XEWDATA") && has(bak, "OLD"));

    /* Dispositions that must fail on the real file are left to it. */
    swprintf_s(dir, MAX_PATH, L"%s.missing", f);
    CHECK(save_guard_open(dir, rel, GENERIC_WRITE, share, XBOX_FILE_OPEN, 0) == INVALID_HANDLE_VALUE && !exists(dir));
    CHECK(save_guard_open(f, rel, GENERIC_WRITE, share, XBOX_FILE_CREATE, 0) == INVALID_HANDLE_VALUE);
    CHECK(!save_guard_close((HANDLE)(uintptr_t)0x1234));

    /* A killed run: a stray .~new is dropped, a finished .~pend is applied. */
    put(tmp, "HALF");
    put(pend, "PEND");
    swprintf_s(dir, MAX_PATH, L"%S\\UDATA", game);
    save_guard_sweep(dir, wcslen(dir));
    CHECK(!exists(tmp) && !exists(pend) && has(f, "PEND"));

    /* Backups keep the newest SAVE_BACKUP_KEEP per file. */
    for (i = 0; i < 7; i++) {
        swprintf_s(bak, MAX_PATH, L"%S_backup\\UDATA\\4d530046\\profile.dat.2000010%d-000000", save, i + 1);
        put(bak, "OLDER");
    }
    wcscpy_s(s_session_stamp, 16, L"29991231-235959");
    save_backup(f, rel);
    swprintf_s(bak, MAX_PATH, L"%S_backup\\UDATA\\4d530046\\profile.dat.*", save);
    CHECK(count(bak) == SAVE_BACKUP_KEEP);
    swprintf_s(bak, MAX_PATH, L"%S_backup\\UDATA\\4d530046\\profile.dat.29991231-235959", save);
    CHECK(has(bak, "PEND"));

    if (!fails) printf("save guard: all checks passed\n");
    return fails != 0;
}
