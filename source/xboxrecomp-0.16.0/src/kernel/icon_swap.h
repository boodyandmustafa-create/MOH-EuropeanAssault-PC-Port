/**
 * icon_swap.h -- the button prompts the game draws, redrawn for the device in use.
 *
 * The game's button icons are ordinary textures (the front-end's helpBar sheet holds the Xbox
 * A/B/X/Y/Black/White/Start/Back ones). When a texture is decoded for the GPU its raw bytes are
 * hashed; a known icon is remembered with the console control it stands for, and its pixels are
 * drawn again from the current view: the Xbox or PlayStation face of the physical pad button that
 * does that job in the player's layout, or a keycap with the key that does it. The input layer
 * (mkb_input.c) publishes the view whenever the device, the style, the menu state or the bindings
 * change; the decoded copy is redrawn then, and only then (no per-frame cost).
 *
 * RECOMP_TEX_DUMP=<dir> also writes every small decoded texture once as a .png.
 */
#ifndef XBOXRECOMP_ICON_SWAP_H
#define XBOXRECOMP_ICON_SWAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console controls an icon can stand for, in the order of the physical pad buttons of
 * mkb_input.c (MKB_PB_*): A B X Y, LB (the game's Black), RB (White), LT RT, L3 R3, Start, Back,
 * the four D-pad directions. */
#define IC_COUNT   16
#define IC_KEYLEN  12

typedef struct {
    int device;                         /* 0 keyboard and mouse, 1 gamepad */
    int style;                          /* gamepad art: 0 Xbox, 1 Xbox 360, 2 PS2/PS3, 3 PS4, 4 PS5 */
    signed char pad[IC_COUNT];          /* gamepad: the physical button (IC_ order) that does the job of
                                           the game's control i, -1 none */
    char key[IC_COUNT][IC_KEYLEN];      /* keyboard: the keycap text for the control, "" none */
} IconView;

/* Any thread: the input layer's current view. Cheap when nothing changed. */
void icon_swap_publish(const IconView *v);

/* Bumped on every change of the view. */
uint32_t icon_swap_serial(void);

/* Executor thread, after decoding a texture: is it one of the game's button icons? Returns the
 * console control + 1 (0 = no). level0 = the first mip's raw bytes, `bytes` of them (DXT only).
 * `hash` is the identifier to log and dump by. */
int icon_swap_match(uint32_t w, uint32_t h, uint32_t block_bytes, const uint8_t *level0,
                    uint32_t bytes, uint64_t *hash);

/* Draw level `w` x `h` (straight ARGB, B8G8R8A8) of control `ic` (a icon_swap_match result - 1) for
 * the current view. Returns 0 when the original art should be kept (the caller restores it). */
int icon_swap_fill(int ic, uint32_t *dst, uint32_t w, uint32_t h);

/* The in-mission prompt sheet (the game's 512 x 64 controller_buttons atlas): match returns 100 for it;
 * fill draws the control cells again into a level (dst holds the game's own pixels first). */
int icon_swap_match_sheet(uint32_t w, uint32_t h, uint32_t fmt, uint64_t hash);
int icon_swap_fill_sheet(uint32_t *dst, uint32_t w, uint32_t h);

/* Sharp icons: when mods\buttons holds the button sheets (xbox.png ...), an icon texture (a match result, 100 = the prompt
 * sheet) is laid out at icon_swap_hires_size (128 px per cell instead of 32) and level 0 is filled by icon_swap_fill_hires
 * (always 1 on success; the caller makes the other mips). hires_ok = the sheets are there. */
int icon_swap_hires_ok(void);
int icon_swap_hires_size(int code, uint32_t *w, uint32_t *h);
int icon_swap_fill_hires(int code, uint32_t *dst, uint32_t w, uint32_t h);

/* Texture mods: a picture named <hash16>*.png/.tga/.bmp/.jpg in mods\textures (or mods\<pack>\textures) replaces the
 * texture whose icon_swap_dump name starts with that hash. texmod_load returns straight ARGB (malloc) or NULL;
 * texmod_downsample makes the next mip (max(w/2,1) x max(h/2,1)). */
uint32_t *texmod_load(uint64_t hash, uint32_t *w, uint32_t *h);
int texmod_has(uint64_t hash);                      /* a picture for this hash exists (the HUD run icon needs its sheet) */
void texmod_downsample(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t *dst);
uint32_t *stain_atlas_load(uint32_t *w, uint32_t *h);   /* mods\blood\atlas.png (blood_stain.c), straight ARGB malloc or NULL */
/* The same pictures loaded by worker threads, mip chain included (see icon_swap.c). 0 = none for this hash, 1 = not
 * ready yet, 2 = ready: *buf (malloc, the caller's) holds every level, level 0 = *w x *h, *total texels in all;
 * 3 = could not be read. A ready picture bigger than max_bytes is left for a later call. */
int texmod_async_take(uint64_t hash, uint64_t max_bytes, uint32_t **buf, uint32_t *w, uint32_t *h, uint32_t *total);

/* Executor thread: write a decoded texture to the dump folder (RECOMP_TEX_DUMP), once per hash. */
void icon_swap_dump(const uint32_t *argb, uint32_t w, uint32_t h, uint32_t fmt, uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif
