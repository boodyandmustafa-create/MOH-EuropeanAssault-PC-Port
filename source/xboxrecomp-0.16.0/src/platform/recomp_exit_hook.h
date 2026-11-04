/*
 * recomp_exit_hook.h - work that must run when a window ends the process.
 *
 * The Win32 windows end the run with ExitProcess, which skips atexit. The
 * kernel's exit-time reports (the missing-file summary) register here, and
 * each window calls recomp_exit_hook_run before ExitProcess. It lives in the
 * platform layer because the D3D library is linked without the kernel in
 * its standalone tests, so it cannot call the kernel by name.
 */
#ifndef RECOMP_EXIT_HOOK_H
#define RECOMP_EXIT_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*recomp_exit_hook_fn)(const char *why);

/* One hook; a second call replaces the first. Any thread. */
void recomp_exit_hook_set(recomp_exit_hook_fn fn);

/* The game's own hook, run after the first one. */
void recomp_exit_hook_set2(recomp_exit_hook_fn fn);

/* Calls the hook, if one is set, with why ("window"). */
void recomp_exit_hook_run(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EXIT_HOOK_H */
