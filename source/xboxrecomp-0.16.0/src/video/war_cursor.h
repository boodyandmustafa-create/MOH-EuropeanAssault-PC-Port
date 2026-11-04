/**
 * The game window's mouse pointer: a rifle round lying at 45 degrees, tip
 * first, drawn in code (war_cursor.c) so no resource file has to ship.
 */
#ifndef WAR_CURSOR_H
#define WAR_CURSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the pointer into an n x n array of straight-alpha 0xAARRGGBB pixels
 * (top row first) and returns the hot spot, the tip of the round. */
void war_cursor_render(uint32_t *argb, int n, int *hot_x, int *hot_y);

#if defined(_WIN32)
#include <windows.h>
/* The pointer for the window class, or NULL when the system arrow is wanted
 * (RECOMP_CURSOR=default). RECOMP_CURSOR=<file.cur> or a menu_cursor.cur in
 * the working directory replaces the drawn one. */
HCURSOR recomp_war_cursor(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
