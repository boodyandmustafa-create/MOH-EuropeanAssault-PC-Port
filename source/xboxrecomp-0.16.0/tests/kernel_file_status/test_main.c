/*
 * What a failed open returns, on every host.
 *
 * The console, and the Win32 backend through ERROR_PATH_NOT_FOUND, tell a
 * missing file (STATUS_OBJECT_NAME_NOT_FOUND) from a file under a missing
 * directory (STATUS_OBJECT_PATH_NOT_FOUND). The POSIX backend used to answer
 * NAME_NOT_FOUND for both, since ENOENT covers both, and left the Win32 error
 * the bridge prints at 0. A title can branch on the status, so the two hosts
 * must agree. Attribute queries answer NAME_NOT_FOUND for both on both
 * backends today, and stay that way.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* kernel_file.c; the bridge declares it the same way. */
uint32_t xbox_LastFileError(void);

static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

static void ansi(XBOX_ANSI_STRING *s, XBOX_OBJECT_ATTRIBUTES *oa, const char *path)
{
    s->Length = (USHORT)strlen(path);
    s->MaximumLength = (USHORT)(s->Length + 1);
    s->Buffer = (PCHAR)path;
    oa->RootDirectory = NULL;
    oa->ObjectName = s;
    oa->Attributes = 0x40;
}

static NTSTATUS open_read(const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    ansi(&name, &oa, path);
    return xbox_NtCreateFile(&h, 0x80100080u /* GENERIC_READ|SYNC|READ_ATTR */, &oa, &ios,
                             NULL, 0, 1 /* share read */, XBOX_FILE_OPEN, 0x60);
}

static NTSTATUS query(const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_FILE_NETWORK_OPEN_INFORMATION info;
    ansi(&name, &oa, path);
    return xbox_NtQueryFullAttributesFile(&oa, &info);
}

int main(int argc, char **argv)
{
    char root[512], adx[600];
    NTSTATUS st;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        unsigned n = 0;
        /* Wine reuses process ids from one run to the next, so the id
         * alone can name a directory an earlier run left behind. */
        do snprintf(root, sizeof root, "%sfilestatus-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
        snprintf(adx, sizeof adx, "%s\\adx", root);
        CreateDirectoryA(adx, NULL);
    }
#else
    {
        char tmpl[] = "/tmp/filestatusXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(adx, sizeof adx, "%s/adx", root);
        mkdir(adx, 0755);
    }
#endif
    xbox_path_init(root, root);

    printf("failed open status\n");
    st = open_read("D:\\adx\\music_TITLE.adx");
    check((uint32_t)st == 0xC0000034u, "missing file, directory present: NAME_NOT_FOUND", (unsigned)st);
    check(xbox_LastFileError() == 2u, "... and win32 err 2 (ERROR_FILE_NOT_FOUND)", xbox_LastFileError());
    st = open_read("D:\\media\\event\\CLIP0001.BIN");
    check((uint32_t)st == 0xC000003Au, "missing directory: PATH_NOT_FOUND", (unsigned)st);
    check(xbox_LastFileError() == 3u, "... and win32 err 3 (ERROR_PATH_NOT_FOUND)", xbox_LastFileError());

    printf("attribute query status (unchanged)\n");
    st = query("D:\\adx\\music_TITLE.adx");
    check((uint32_t)st == 0xC0000034u, "missing file: NAME_NOT_FOUND", (unsigned)st);
    st = query("D:\\media\\event\\CLIP0001.BIN");
    check((uint32_t)st == 0xC0000034u, "missing directory: NAME_NOT_FOUND too", (unsigned)st);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
