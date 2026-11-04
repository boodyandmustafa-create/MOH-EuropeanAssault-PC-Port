/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 8)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/* Native mouse aim: a title hook in the game's own look update calls this
 * once per update to take the mouse movement since the last call, as
 * angles in radians (yaw: positive = turn right; pitch: positive = look
 * up). Returns 0, and takes nothing, when the layer is off, released (F9)
 * or the game is not in front -- the hook then leaves the look alone.
 * While a hook is taking the mouse, the mouse stops driving the emulated
 * aim stick. Scale: 0.0022 rad per count times `sens` (mkb.ini). */
int mkb_native_aim_take(float *yaw, float *pitch);

/* Sprint, for a title hook in its movement code: 1 while the sprint key
 * (mkb.ini sprint_key, default Alt) is held and the game has the input;
 * always fills in the speed factor, the seconds a sprint lasts and the
 * seconds a full recovery takes (sprint_speed / sprint_time /
 * sprint_recover). The hook keeps the stamina. */
int mkb_sprint_query(float *speed, float *secs, float *recover);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* Mouse speed and vertical invert, live (the Recomp Settings menu). */
double mkb_get_sens(void);
void   mkb_set_sens(double v);
int    mkb_get_invert_y(void);
void   mkb_set_invert_y(int on);

/* Sprint speed factor, seconds of sprint, seconds of full recovery, live (the settings menu). */
float mkb_get_sprint_speed(void);
void  mkb_set_sprint_speed(float v);
float mkb_get_sprint_time(void);
void  mkb_set_sprint_time(float v);
float mkb_get_sprint_recover(void);
void  mkb_set_sprint_recover(float v);

/* Called with the pad state the game is about to read (port 0), after every other layer; it may
 * change it (the settings screen takes the press that opens it). NULL = none. */
extern void (*g_xbox_input_filter)(XBOX_INPUT_STATE *pState);

/* While non-zero the game reads a pad at rest (the settings overlay owns the
 * input); g_input_inject_b_until (GetTickCount) holds B for the game after it
 * closes, so a screen it opened behind the overlay backs out. */
extern volatile long  g_input_block;
extern volatile unsigned long g_input_inject_b_until;
void mkb_release_all(void);     /* forget held keys and buttons (the settings panel took their key-ups) */

/* From the sprint hook, once per move update: whether the runner is going
 * forward and whether the stamina ran out; ends the pad's toggled sprint. */
void mkb_sprint_pad_moving(int moving, int tired);

/* ---- in-game controls by function (Recomp Settings > Controls) ---------------------------
 * Keyboard / mouse: up to four key codes per function (VK codes, MKB mouse buttons 0x100.., the
 * wheel 0x105); the movement keys are separate. Gamepad: up to two physical buttons per function
 * (the MKB_PB_* indices, -1 = none). */
#define MKB_ACT_COUNT 15
#define MKB_PB_COUNT  16
int  mkb_action_count(void);
const char *mkb_action_label(int a);
int  mkb_action_keys(int a, unsigned short out[4]);              /* returns how many */
void mkb_action_set_keys(int a, const unsigned short *keys, int n);
unsigned short mkb_move_key(int dir);                            /* 0 forward 1 back 2 left 3 right */
void mkb_set_move_key(int dir, unsigned short code);
int  mkb_pad_buttons(int a, signed char pb[2]);                  /* returns how many */
void mkb_action_set_pad(int a, const signed char pb[2]);
const char *mkb_pad_label(int pb);
void mkb_key_label(unsigned code, char *out, size_t n);
void mkb_bindings_reset(void);
void mkb_bindings_write(void *file);                             /* the ini lines */
/* ---- button prompts (icon_swap.c): which device the game's icons follow, and their art ----------
 * mode: 0 the last device used (pad or keyboard + mouse), 1 always the gamepad, 2 always the keyboard;
 * style: the gamepad art, 0 Xbox, 1 PlayStation. mkb_note_pad takes the pad as the hardware reported it;
 * mkb_icons_poll is called as the game polls the pad and publishes the view when it changed. */
int  mkb_get_icon_mode(void);
void mkb_set_icon_mode(int mode);
int  mkb_get_icon_style(void);
void mkb_set_icon_style(int style);
void mkb_note_pad(const XBOX_INPUT_STATE *raw);
void mkb_icons_poll(void);

/* The pad, translated in place by those bindings (not in menus); call before the keyboard merge. */
void mkb_pad_translate(XBOX_INPUT_STATE *pState);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
