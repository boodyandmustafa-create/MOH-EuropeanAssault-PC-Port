/*
 * The Win32 thread shim on a POSIX host -- written to fail.
 *
 * pthreads cannot stop another thread mid-run, so the shim's SuspendThread
 * only counts. A thread that suspends *itself* is at a known point, though,
 * and Win32 parks it there until a ResumeThread. Worker pools idle that way
 * (NtSuspendThread on the current thread, resumed when work is queued); a
 * SuspendThread that returns at once turns the idle loop into a busy spin.
 *
 *   1. SuspendThread(own handle) does not return until ResumeThread
 *   2. the same through the current-thread pseudo-handle
 *   3. ResumeThread reports the count the park left (1)
 */
#include "win32_compat.h"

#include <stdio.h>
#include <time.h>

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static volatile LONG g_past_suspend;
static volatile LONG g_use_pseudo;

static DWORD WINAPI parks_itself(LPVOID unused)
{
    (void)unused;
    SuspendThread(g_use_pseudo ? (HANDLE)(LONG_PTR)-2 : GetCurrentThread());
    InterlockedExchange(&g_past_suspend, 1);
    return 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void run(int pseudo, const char *what)
{
    DWORD tid, prev, w;
    HANDLE h;

    g_past_suspend = 0;
    g_use_pseudo = pseudo;
    h = CreateThread(NULL, 0, parks_itself, NULL, 0, &tid);
    check(h != NULL, "CreateThread", NULL);
    if (!h)
        return;
    sleep_ms(100);
    check(g_past_suspend == 0, what,
          "SuspendThread returned at once; an idle pool spins");
    prev = ResumeThread(h);
    check(prev == 1, "ResumeThread reports the parked count (1)", NULL);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0 && g_past_suspend == 1,
          "the thread runs on after ResumeThread", NULL);
    CloseHandle(h);
}

int main(void)
{
    printf("win32 thread shim (POSIX)\n");
    run(0, "SuspendThread(own handle) parks the thread");
    run(1, "SuspendThread(pseudo-handle) parks the thread");
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
