/*
 * Opens relative to a RootDirectory handle, on every host.
 *
 * NT resolves an OBJECT_ATTRIBUTES name that is not a full path against its
 * RootDirectory handle, and XAPI relies on it: the save-game code opens the
 * save directory, then each file in it by its bare name ("Data.ssx").
 * The Win32 backend joined such names to the directory handle's path; the
 * POSIX backend sent the bare name through the path table, which matched no
 * rule, and the open failed with STATUS_OBJECT_PATH_NOT_FOUND -- SSX 3 on
 * Linux said "Unable to save" when overwriting a save.
 *
 * Covers NtCreateFile (create and reopen), NtQueryFullAttributesFile and
 * NtDeleteFile with a RootDirectory, and that full paths still ignore it.
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

static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

static void ansi(XBOX_ANSI_STRING *s, XBOX_OBJECT_ATTRIBUTES *oa, HANDLE root, const char *path)
{
    s->Length = (USHORT)strlen(path);
    s->MaximumLength = (USHORT)(s->Length + 1);
    s->Buffer = (PCHAR)path;
    oa->RootDirectory = root;
    oa->ObjectName = s;
    oa->Attributes = 0x40;
}

static NTSTATUS create(HANDLE root, const char *path, ULONG disposition, ULONG options, HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    ansi(&name, &oa, root, path);
    *out = NULL;
    return xbox_NtCreateFile(out, 0xC0100080u /* GENERIC_READ|WRITE|SYNC|READ_ATTR */, &oa, &ios,
                             NULL, 0, 0, disposition, options);
}

static NTSTATUS query(HANDLE root, const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_FILE_NETWORK_OPEN_INFORMATION info;
    ansi(&name, &oa, root, path);
    return xbox_NtQueryFullAttributesFile(&oa, &info);
}

static NTSTATUS delete_file(HANDLE root, const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    ansi(&name, &oa, root, path);
    return xbox_NtDeleteFile(&oa);
}

static int host_exists(const char *path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int main(int argc, char **argv)
{
    char root[512], data[600];
    HANDLE dir, file;
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
        do snprintf(root, sizeof root, "%sfilerelative-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
        snprintf(data, sizeof data, "%s\\save1\\Data.ssx", root);
    }
#else
    {
        char tmpl[] = "/tmp/filerelativeXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(data, sizeof data, "%s/save1/Data.ssx", root);
    }
#endif
    xbox_path_init(root, root);

    printf("open relative to a directory handle\n");
    st = create(NULL, "D:\\save1", XBOX_FILE_OPEN_IF, XBOX_FILE_DIRECTORY_FILE, &dir);
    check(st == 0 && dir, "directory D:\\save1 opened", (unsigned)st);
    st = create(dir, "Data.ssx", XBOX_FILE_OVERWRITE_IF, 0x60, &file);
    check(st == 0 && file, "create Data.ssx relative to it", (unsigned)st);
    if (file) xbox_NtClose(file);
    check(host_exists(data), "... and it is inside that directory on the host", 0);
    st = create(dir, "Data.ssx", XBOX_FILE_OPEN, 0x60, &file);
    check(st == 0 && file, "reopen Data.ssx relative to it", (unsigned)st);
    if (file) xbox_NtClose(file);
    st = query(dir, "Data.ssx");
    check(st == 0, "query its attributes relative to it", (unsigned)st);
    st = create(dir, "Missing.ssx", XBOX_FILE_OPEN, 0x60, &file);
    check((uint32_t)st == 0xC0000034u, "a missing relative name: NAME_NOT_FOUND", (unsigned)st);

    printf("full paths ignore the handle\n");
    st = query(dir, "D:\\save1\\Data.ssx");
    check(st == 0, "full path with a RootDirectory set", (unsigned)st);

    printf("delete relative to the handle\n");
    st = delete_file(dir, "Data.ssx");
    check(st == 0, "delete Data.ssx relative to it", (unsigned)st);
    check(!host_exists(data), "... and it is gone on the host", 0);
    xbox_NtClose(dir);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
