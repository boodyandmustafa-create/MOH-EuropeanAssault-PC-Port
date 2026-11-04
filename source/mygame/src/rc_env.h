/* Force-included into every C file of the game and the runtime (CMake: /FI); the implementation is in main.c. The CRT's getenv walks the
 * whole environment block on every call, case-insensitively; the profile showed it as ~1.5% of the game
 * thread's time, so the many RECOMP_* lookups are answered from a small per-thread cache instead. A
 * _putenv anywhere bumps a generation counter, which drops every cache. */
#pragma once
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
char *rc_getenv(const char *name);
int rc_putenv(const char *s);
#ifdef __cplusplus
}
#endif
#define getenv rc_getenv
#define _putenv rc_putenv
