/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"
#include "../kernel/icon_swap.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* ---- button bindings ----------------------------------------------------
 * Each Xbox control takes up to four keys. A key is a Windows VK code, or
 * MKB_MOUSE1.. for mouse buttons 1-5, or MKB_WHEEL for a wheel notch.
 * mkb.ini (beside the exe) rebinds a control with "<control>=<key>,<key>",
 * e.g. "y=space" or "white=g,mouse4"; "<control>=none" unbinds it. */
#define MKB_MOUSE1 0x100u
#define MKB_WHEEL  0x105u

enum { MKC_A, MKC_B, MKC_X, MKC_Y, MKC_BLACK, MKC_WHITE, MKC_LT, MKC_RT,
       MKC_UP, MKC_DOWN, MKC_LEFT, MKC_RIGHT, MKC_START, MKC_BACK, MKC_LS, MKC_RS,
       MKC_COUNT };

static const struct { const char *name; int analog; WORD mask; } s_ctl[MKC_COUNT] = {
    { "a",     XBOX_BUTTON_A,        0 },
    { "b",     XBOX_BUTTON_B,        0 },
    { "x",     XBOX_BUTTON_X,        0 },
    { "y",     XBOX_BUTTON_Y,        0 },
    { "black", XBOX_BUTTON_BLACK,    0 },
    { "white", XBOX_BUTTON_WHITE,    0 },   /* a modern pad's RB / R1 */
    { "lt",    XBOX_BUTTON_LTRIGGER, 0 },
    { "rt",    XBOX_BUTTON_RTRIGGER, 0 },
    { "up",    -1, XBOX_GAMEPAD_DPAD_UP },
    { "down",  -1, XBOX_GAMEPAD_DPAD_DOWN },
    { "left",  -1, XBOX_GAMEPAD_DPAD_LEFT },
    { "right", -1, XBOX_GAMEPAD_DPAD_RIGHT },
    { "start", -1, XBOX_GAMEPAD_START },
    { "back",  -1, XBOX_GAMEPAD_BACK },
    { "ls",    -1, XBOX_GAMEPAD_LEFT_THUMB },
    { "rs",    -1, XBOX_GAMEPAD_RIGHT_THUMB },
};

/* The user's MoH: European Assault layout. Enter, Backspace and the
 * arrows also drive the menus. */
static unsigned short s_bind[MKC_COUNT][4] = {
    /* a     */ { 'R', VK_RETURN },
    /* b     */ { '3', VK_BACK },
    /* x     */ { 'C' },
    /* y     */ { VK_SPACE },
    /* black */ { '1' },                /* Activate Adrenaline (the pad's LB) */
    /* white */ { 'G' },
    /* lt    */ { MKB_MOUSE1 + 1 },     /* right mouse: aim down sights */
    /* rt    */ { MKB_MOUSE1 },         /* left mouse: fire             */
    /* up    */ { '4', VK_UP },
    /* down  */ { VK_DOWN },
    /* left  */ { VK_LEFT },
    /* right */ { 0 },
    /* start */ { VK_ESCAPE },
    /* back  */ { VK_TAB },
    /* ls    */ { VK_RIGHT },           /* Position squad (the game reads the left stick's click) */
    /* rs    */ { VK_LCONTROL },
};

/* Menu bindings, used while the title shows a menu: mkb.ini names a guest
 * byte that says so ("menu_flag=<address>", "menu_value=<n>", the value it
 * holds in menus) and rebinds with "m_<control>=<keys>". A control with menu
 * keys uses only those in a menu, and a key bound here does nothing else
 * there -- a left click selects in a menu without also firing (RT). */
static unsigned short s_bind_menu[MKC_COUNT][4] = {
    /* a     */ { MKB_MOUSE1 },        /* left click selects */
    /* b     */ { MKB_MOUSE1 + 1 },    /* right click goes back */
};
static uint32_t s_menu_flag;        /* 0: no menu detection */
static int      s_menu_value = 1;
/* In a menu the mouse moves the highlight: this many counts per step
 * (mkb.ini menu_mouse; 0 leaves the mouse as the stick there too). */
static int      s_menu_mouse = 100;
/* The free pointer in menus (MoH: European Assault's EA UI, mkb.ini):
 * menu_focus = a guest dword holding a pointer to a table whose entry at
 * menu_focus_off is the focused widget; widgets are a doubly linked list
 * (next/prev at widget_next/widget_prev) with their screen y as a float at
 * widget_y, in 480-line units. 0 = no pointer (the stepping mouse instead). */
static uint32_t s_menu_focus, s_menu_focus_off = 0x10;
static uint32_t s_widget_y = 0x24, s_widget_next = 0x54, s_widget_prev = 0x58;

/* Sprint (a title hook asks mkb_sprint_query): the key, the speed factor,
 * how long it lasts and how long a full recovery takes (mkb.ini sprint_*). */
static unsigned short s_sprint_key = VK_LMENU;
static float s_sprint_speed = 1.5f, s_sprint_time = 5.0f, s_sprint_recover = 8.0f;
static void act_snapshot_defaults(void);
static int  mkb_bindings_line(const char *key, const char *val);
static unsigned short s_move[4];

/* "space", "g", "3", "mouse2", "wheel", "f5", ... -> key code; 0 if unknown. */
static unsigned mkb_key_code(const char *s)
{
    static const struct { const char *n; unsigned vk; } names[] = {
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "esc", VK_ESCAPE },
        { "tab", VK_TAB }, { "backspace", VK_BACK }, { "shift", VK_LSHIFT },
        { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT }, { "ctrl", VK_LCONTROL },
        { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL }, { "alt", VK_LMENU },
        { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "capslock", VK_CAPITAL },
        { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
        { "insert", VK_INSERT }, { "delete", VK_DELETE }, { "home", VK_HOME },
        { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
        { "wheel", MKB_WHEEL },
        { NULL, 0 }
    };
    int i;
    if (!s[0])
        return 0;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (unsigned)c;
        return 0;
    }
    if (!_strnicmp(s, "vk", 2) && s[2] && strlen(s) <= 4) {
        unsigned long v = strtoul(s + 2, NULL, 16);
        return v > 0 && v < 0x100 ? (unsigned)v : 0;
    }
    if (!_strnicmp(s, "mouse", 5) && s[5] >= '1' && s[5] <= '5' && !s[6])
        return MKB_MOUSE1 + (unsigned)(s[5] - '1');
    if ((s[0] == 'f' || s[0] == 'F') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12)
        return (unsigned)(VK_F1 + atoi(s + 1) - 1);
    for (i = 0; names[i].n; i++)
        if (!_stricmp(s, names[i].n))
            return names[i].vk;
    return 0;
}

/* "<control>=<key>,<key>": replaces that control's keys. 1 if recognised. */
static int mkb_apply_bind(const char *key, const char *val)
{
    int i, n = 0;
    char buf[128], *tok, *ctx = NULL;
    unsigned short (*table)[4] = s_bind;
    if (!_strnicmp(key, "m_", 2)) {     /* "m_a=enter": a menu binding */
        table = s_bind_menu;
        key += 2;
    }
    for (i = 0; i < MKC_COUNT; i++)
        if (!_stricmp(key, s_ctl[i].name))
            break;
    if (i == MKC_COUNT)
        return 0;
    memset(table[i], 0, sizeof table[i]);
    snprintf(buf, sizeof buf, "%s", val);
    for (tok = strtok_s(buf, ",", &ctx); tok && n < 4; tok = strtok_s(NULL, ",", &ctx)) {
        unsigned code = mkb_key_code(tok);
        if (code)
            table[i][n++] = (unsigned short)code;
        else if (_stricmp(tok, "none"))
            fprintf(stderr, "  [MKB] %s: unknown key '%s'\n", key, tok);
    }
    return 1;
}

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else if (!strcmp(key, "menu_flag")) {
        s_menu_flag = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "menu_focus")) {
        s_menu_focus = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "menu_focus_off")) {
        s_menu_focus_off = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "widget_y")) {
        s_widget_y = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "widget_next")) {
        s_widget_next = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "widget_prev")) {
        s_widget_prev = (uint32_t)strtoul(val, NULL, 0);
    } else if (!strcmp(key, "menu_mouse")) {
        s_menu_mouse = (int)strtol(val, NULL, 0);
    } else if (!strcmp(key, "menu_value")) {
        s_menu_value = (int)strtol(val, NULL, 0);
    } else if (!strcmp(key, "sprint_key")) {
        s_sprint_key = (unsigned short)mkb_key_code(val);
    } else if (!strcmp(key, "sprint_speed")) {
        d = strtod(val, NULL); if (d >= 1.0 && d <= 4.0) s_sprint_speed = (float)d;
    } else if (!strcmp(key, "sprint_time")) {
        d = strtod(val, NULL); if (d > 0.0 && d <= 600.0) s_sprint_time = (float)d;
    } else if (!strcmp(key, "sprint_recover")) {
        d = strtod(val, NULL); if (d > 0.0 && d <= 600.0) s_sprint_recover = (float)d;
    } else {
        return mkb_apply_bind(key, val);
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    act_snapshot_defaults();
    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
}

/* The in-game Recomp Settings menu (mygame/src/recomp_settings.c) keeps its
 * choices in recomp_settings.ini beside the exe; the mouse speed and the
 * vertical invert are read here, after mkb.ini, so the menu's choice wins. */
static int s_icon_mode;                      /* 0 follow the last device used, 1 gamepad, 2 keyboard and mouse */
static int s_icon_style;                     /* the gamepad art: 0 Xbox, 1 PlayStation */

static void mkb_load_rs_settings(void)
{
    char path[MAX_PATH], line[128];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path - 24);
    FILE *f;

    if (!n || n >= sizeof path - 24)
        return;
    snprintf(path + n, 24, "\\..\\recomp_settings.ini");
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "sens=", 5))
            mkb_apply_kv("sens", line + 5);
        else if (!strncmp(line, "invert_y=", 9))
            mkb_apply_kv("invert_y", line + 9);
        else if (!strncmp(line, "sprint_speed=", 13))
            mkb_apply_kv("sprint_speed", line + 13);
        else if (!strncmp(line, "sprint_time=", 12))
            mkb_apply_kv("sprint_time", line + 12);
        else if (!strncmp(line, "sprint_recover=", 15))
            mkb_apply_kv("sprint_recover", line + 15);
        else if (!strncmp(line, "icons=", 6))
            s_icon_mode = !strncmp(line + 6, "pad", 3) ? 1 : !strncmp(line + 6, "key", 3) ? 2 : 0;
        else if (!strncmp(line, "icon_style=", 11))
            s_icon_style = !strncmp(line + 11, "ps", 2) || !strncmp(line + 11, "play", 4);
        else {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                eq[1 + strcspn(eq + 1, "\r\n")] = 0;
                mkb_bindings_line(line, eq + 1);
            }
        }
    }
    fclose(f);
}

float mkb_get_sprint_speed(void)     { return s_sprint_speed; }
void  mkb_set_sprint_speed(float v)  { if (v >= 1.0f && v <= 4.0f) s_sprint_speed = v; }
float mkb_get_sprint_time(void)      { return s_sprint_time; }
void  mkb_set_sprint_time(float v)   { if (v > 0.0f && v <= 600.0f) s_sprint_time = v; }
float mkb_get_sprint_recover(void)   { return s_sprint_recover; }
void  mkb_set_sprint_recover(float v){ if (v > 0.0f && v <= 600.0f) s_sprint_recover = v; }

/* Live access for the settings menu. */
double mkb_get_sens(void)            { return s_cfg.sens; }
void   mkb_set_sens(double v)        { if (v > 0.0 && v < 100.0) s_cfg.sens = v; }
int    mkb_get_invert_y(void)        { return s_cfg.invert_y; }
void   mkb_set_invert_y(int on)      { s_cfg.invert_y = on != 0; }

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_mouse_btn;    /* 1 L, 2 R, 4 M, 8 X1, 16 X2: held */
static volatile LONG s_mouse_click;  /* pressed since the last poll      */
static volatile unsigned char s_vk_down[256];
static volatile unsigned char s_vk_tap[256];  /* pressed since the last poll */
/* F6 / F7 while playing: -1 / +1 steps of the title's own field-of-view setting.
 * The title reads and clears it (mygame recomp_manual.c). */
volatile LONG g_mkb_fov_steps;
volatile LONG g_mkb_fog_steps;          /* F2 / F3: fog distance multiplier down / up */
volatile LONG g_mkb_draw_steps;         /* F4 / F5: draw distance multiplier down / up */
volatile LONG g_mkb_pin_toggles;        /* F8: game loop on its own core on / off (mygame main.c) */
volatile LONG g_mkb_cap_toggles;        /* F12: small-object distance cap on / off (mygame recomp_manual.c) */

/* Typing a profile name: while the title's name-entry keyboard is up
 * (kb_handle sets s_kb_tick on every poll it sees it), the raw-input thread
 * queues each typed key (bit 8 = shift), and kb_handle walks the highlight
 * to that letter and presses A. */
static volatile unsigned short s_kbq[32];
static volatile LONG s_kbq_head, s_kbq_tail;
static volatile DWORD s_kb_tick;
static volatile DWORD s_kb_a_until;           /* the A button is held until this tick */
static int s_kb_on;                           /* the keyboard screen is up (this poll) */

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

/* The F2..F12 / Pause developer hotkeys (fog, draw distance, FOV, pin, cap, mouse release...) act only when the
 * environment has RECOMP_DEV_KEYS=1 (the dev launchers). A player build leaves those keys alone, so they can be
 * bound to game actions and nothing rewrites fov_saved.txt / draw_saved.txt behind the launcher's back. */
static int mkb_dev_keys_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_DEV_KEYS");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

static void ic_note_kb(void);
static void ic_note_mouse(LONG dx, LONG dy);
static int ic_pad_is_last(void);

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
            ic_note_mouse(m->lLastX, m->lLastY);
        }
        if (m->usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN
                                | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN
                                | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL))
            ic_note_kb();
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 1);
            InterlockedOr(&s_mouse_click, 1);
        }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 2);
            InterlockedOr(&s_mouse_click, 2);
        }
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~2);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 4);
            InterlockedOr(&s_mouse_click, 4);
        }
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~4);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) {
            InterlockedOr(&s_mouse_btn, 8);
            InterlockedOr(&s_mouse_click, 8);
        }
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_UP)
            InterlockedAnd(&s_mouse_btn, ~8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) {
            InterlockedOr(&s_mouse_btn, 16);
            InterlockedOr(&s_mouse_click, 16);
        }
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_UP)
            InterlockedAnd(&s_mouse_btn, ~16);
        if (m->usButtonFlags & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            InterlockedAdd(&s_wheel, d / WHEEL_DELTA);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        /* Raw input reports the generic VK_SHIFT / VK_CONTROL / VK_MENU;
         * the bindings name a side. The make code and the E0 flag say
         * which one it was. */
        if (vk == VK_SHIFT)
            vk = k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        else if (vk == VK_CONTROL)
            vk = (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            vk = (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;

        if (vk == MKB_TOGGLE_VK && mkb_dev_keys_on()) {
            if (k->Message == WM_KEYDOWN && !s_vk_down[vk & 0xFF]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN) {
                ic_note_kb();
                if (!s_vk_down[vk] && GetTickCount() - s_kb_tick < 600u) {
                    LONG h = s_kbq_head;
                    if (h - s_kbq_tail < 32) {
                        s_kbq[h & 31] = (unsigned short)(vk | ((s_vk_down[VK_LSHIFT] || s_vk_down[VK_RSHIFT]) ? 0x100 : 0));
                        s_kbq_head = h + 1;
                    }
                }
                s_vk_down[vk] = 1;
                s_vk_tap[vk] = 1;
            }
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- memory snapshots for scripted runs ---------------------------------
 * The pad script's "snap:<n>" line lands here: guest RAM 0x10000..64 MB and
 * the contiguous window 0x80000000..+64 MB, raw and back to back, into
 * <RECOMP_MKB_SNAPDIR>/snap_<n>.bin. Comparing a run's snapshots finds the
 * title's state flags (in a menu, paused, in play). */
extern ptrdiff_t g_xbox_mem_offset;

void mkb_snapshot(int n)
{
    const char *dir = getenv("RECOMP_MKB_SNAPDIR");
    char path[600];
    FILE *f;
    snprintf(path, sizeof path, "%s/snap_%d.bin", dir && *dir ? dir : ".", n);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x10000u), 1, 0x04000000u - 0x10000u, f);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x80000000u), 1, 0x04000000u, f);
    fclose(f);
    fprintf(stderr, "  [SNAP] %d -> %s\n", n, path);
    fflush(stderr);
}

/* ---- native aim (see mkb_input.h) -------------------------------------- */

static volatile LONG s_native_tick = -100000;  /* GetTickCount of the last take */

static int game_is_foreground(void);

int mkb_native_aim_take(float *yaw, float *pitch)
{
    LONG dx, dy;
    double k;

    *yaw = *pitch = 0.0f;
    if (!mkb_enabled() || !s_thread_up || !s_active || !game_is_foreground())
        return 0;
    InterlockedExchange(&s_native_tick, (LONG)GetTickCount());
    dx = InterlockedExchange(&s_dx, 0);
    dy = InterlockedExchange(&s_dy, 0);
    k = 0.0022 * s_cfg.sens;
    *yaw = (float)(dx * k);
    *pitch = (float)(-dy * k);           /* raw y grows downwards */
    if (s_cfg.invert_y)
        *pitch = -*pitch;
    return 1;
}

/* The pad's sprint is a toggle on the real left-stick click (mkb_pad_remap
 * flips it; the game never sees that click). It ends by itself when the
 * stamina runs out, when the runner stops (mkb_sprint_pad_moving), in a menu,
 * or when the move update has not asked for a while (death, a load). The
 * keyboard's sprint key stays hold-to-sprint. */
static volatile LONG s_pad_l3;          /* the toggle is on */
static volatile LONG s_pad_seen_move;   /* it has been running forward since it came on */
static volatile LONG s_pad_tick;        /* when it was last flipped or asked for (ms) */

void mkb_sprint_pad_moving(int moving, int tired)
{
    if (!s_pad_l3)
        return;
    s_pad_tick = (LONG)GetTickCount();
    if (moving)
        s_pad_seen_move = 1;
    if (tired || (!moving && s_pad_seen_move))
        s_pad_l3 = 0;
}

int mkb_sprint_query(float *speed, float *secs, float *recover)
{
    unsigned code = s_sprint_key;
    *speed = s_sprint_speed;
    *secs = s_sprint_time;
    *recover = s_sprint_recover;
    if (mkb_enabled() && s_pad_l3 && game_is_foreground())
        return 1;
    if (!mkb_enabled() || !s_thread_up || !s_active || !code || !game_is_foreground())
        return 0;
    if (code < 0x100)
        return s_vk_down[code] != 0;
    if (code < MKB_WHEEL)
        return (s_mouse_btn >> (code - MKB_MOUSE1)) & 1;
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            mkb_load_config();
            mkb_load_rs_settings();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
/* ShowCursor counts per thread, and this runs on whichever thread polls the
 * pad, not the one that owns the game window: the pointer stayed on screen.
 * So the window's class cursor becomes a blank one (the window proc leaves
 * WM_SETCURSOR to DefWindowProc, which sets the class cursor), and the
 * pointer is held at the middle of the client area, where a click cannot
 * land on the title bar or another monitor. */
static void mkb_manage_cursor(int capture)
{
    static HCURSOR blank;
    static HWND hidden_wnd;     /* window whose class cursor we replaced */
    static LONG_PTR saved;      /* its cursor before                     */
    static unsigned long last;

    unsigned long now = GetTickCount();
    HWND fg = capture ? GetForegroundWindow() : NULL;

    if (capture ? (hidden_wnd == fg && now - last < 250) : !hidden_wnd)
        return;
    last = now;

    if (capture && fg) {
        RECT r;
        POINT c;
        if (!blank) {
            BYTE and_mask[128], xor_mask[128];
            memset(and_mask, 0xFF, sizeof and_mask);
            memset(xor_mask, 0x00, sizeof xor_mask);
            blank = CreateCursor(GetModuleHandleA(NULL), 0, 0, 32, 32, and_mask, xor_mask);
        }
        if (hidden_wnd != fg) {
            if (hidden_wnd)
                SetClassLongPtrA(hidden_wnd, GCLP_HCURSOR, saved);
            saved = (LONG_PTR)GetClassLongPtrA(fg, GCLP_HCURSOR);
            if (blank)
                SetClassLongPtrA(fg, GCLP_HCURSOR, (LONG_PTR)blank);
            hidden_wnd = fg;
        }
        if (GetClientRect(fg, &r)) {
            c.x = (r.left + r.right) / 2;
            c.y = (r.top + r.bottom) / 2;
            ClientToScreen(fg, &c);
            r.left = c.x - 1; r.right = c.x + 1;
            r.top = c.y - 1;  r.bottom = c.y + 1;
            ClipCursor(&r);
            SetCursorPos(c.x, c.y);   /* a move, so the blank cursor shows now */
        }
    } else if (hidden_wnd) {
        SetClassLongPtrA(hidden_wnd, GCLP_HCURSOR, saved);
        hidden_wnd = NULL;
        ClipCursor(NULL);
    }
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Held, or pressed and released again since the last poll: a tap shorter
 * than one poll still reaches the title. `tap` is mkb_merge's snapshot. */
static int kb_eats(unsigned vk)    /* keys that type on the name-entry keyboard */
{
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE
        || vk == VK_BACK || vk == VK_RETURN || vk == VK_OEM_MINUS || vk == VK_OEM_PERIOD
        || vk == VK_LSHIFT || vk == VK_RSHIFT;
}
#define MK_VK(k)  ((unsigned)(k) < 256 && !(s_kb_on && kb_eats(k)) && (s_vk_down[(k)] || tap[(k)]))

/* ---- aim finder: RECOMP_MKB_FIND=1, then F10 while standing in a level --
 *
 * Native mouse aim needs the game's own view angle. This drives the right
 * stick (right in three steps, rest, left in three steps, rest) and keeps
 * the guest dwords that move like an angle under it: steadily one way
 * while held, still at rest, back the other way at a like rate. Each dword
 * is tried as a float, an int32 and two int16 halves (console titles often
 * keep angles as 16-bit binary angles, which wrap), since the float search
 * found only the view matrix. Survivors are logged; the write watch is
 * armed on the best one outside the known view matrices and the stack. */
extern ptrdiff_t g_xbox_mem_offset;
int xbox_WatchArmAt(uint32_t va, unsigned max_reports, int reads);

#define FIND_LO 0x00010000u
#define FIND_HI 0x04000000u
#define FIND_MAX_CAND (2u << 20)
#define FIND_NS 9

typedef struct { uint32_t va; uint32_t v[FIND_NS]; } FindCand;

static int       s_find_on = -1;
static int       s_find_phase = -1;
static DWORD     s_find_t0;
static uint8_t  *s_find_snap;
static FindCand *s_find_c;
static unsigned  s_find_n;

typedef struct { DWORD len; int x; } FindStep;
static const FindStep s_find_sched[FIND_NS] = {
    { 400, 0 }, { 250, 1 }, { 250, 1 }, { 250, 1 }, { 400, 0 },
    { 250, -1 }, { 250, -1 }, { 250, -1 }, { 400, 0 },
};

/* value of interpretation t (0 float, 1 int32, 2 low int16, 3 high int16)
 * as a double; differences of int16 wrap */
static double find_as(uint32_t raw, int t)
{
    float f;
    switch (t) {
    case 0: memcpy(&f, &raw, 4); return f;
    case 1: return (double)(int32_t)raw;
    case 2: return (double)(int16_t)(raw & 0xFFFF);
    default: return (double)(int16_t)(raw >> 16);
    }
}
static double find_d(uint32_t a, uint32_t b, int t)
{
    if (t >= 2) {
        int16_t x = (int16_t)(t == 2 ? (a & 0xFFFF) : (a >> 16));
        int16_t y = (int16_t)(t == 2 ? (b & 0xFFFF) : (b >> 16));
        return (double)(int16_t)(uint16_t)(y - x);
    }
    return find_as(b, t) - find_as(a, t);
}
static int find_like(double a, double b)  /* same way, like size */
{
    return a * b > 0 && fabs(b) > 0.33 * fabs(a) && fabs(b) < 3.0 * fabs(a);
}

/* 0 = not angle-like; else a score, smaller is better (asymmetry) */
static double find_score(const FindCand *c, int t)
{
    double d[FIND_NS];
    int k;
    if (t == 0) {
        for (k = 0; k < FIND_NS; k++) {
            double v = find_as(c->v[k], 0);
            if (!(v == v) || fabs(v) > 1e5) return 0;
        }
    }
    for (k = 1; k < FIND_NS; k++)
        d[k] = find_d(c->v[k - 1], c->v[k], t);
    if (t >= 1 && fabs(d[1]) < 2) return 0;          /* integers: real steps */
    if (!find_like(d[1], d[2]) || !find_like(d[2], d[3])) return 0;
    if (fabs(d[4]) > 0.5 * fabs(d[3])) return 0;
    if (!find_like(-d[1], d[5]) || !find_like(d[5], d[6]) || !find_like(d[6], d[7])) return 0;
    if (fabs(d[8]) > 0.5 * fabs(d[7])) return 0;
    {
        double up = d[1] + d[2] + d[3], back = d[5] + d[6] + d[7];
        return 1e-9 + fabs(up + back) / fabs(up);
    }
}

static int find_known(uint32_t va)
{
    return (va >= 0x003789A0u && va < 0x00378A40u)     /* camera matrix */
        || (va >= 0x003BCA00u && va < 0x003BCD00u)     /* view matrices */
        || (va >= 0x00F00000u && va < 0x00F80000u);    /* game thread stack */
}

/* Guest memory to search: RAM as the title addresses it, and the
 * contiguous window at 0x80000000, which this runtime keeps as separate
 * storage (MmAllocateContiguousMemory pools live there, not in low RAM). */
static const struct { uint32_t lo, hi; } s_find_rng[2] = {
    { FIND_LO, FIND_HI },
    { 0x80000000u, 0x84000000u },
};
#define FIND_SNAP_BYTES ((FIND_HI - FIND_LO) + 0x04000000u)

static void find_sample(int k)
{
    unsigned i, r, base;
    static const char *tn[4] = { "float", "int32", "int16lo", "int16hi" };

    if (k == 0) {
        for (r = 0, base = 0; r < 2; base += s_find_rng[r].hi - s_find_rng[r].lo, r++)
            memcpy(s_find_snap + base,
                   (const void *)((uintptr_t)g_xbox_mem_offset + s_find_rng[r].lo),
                   s_find_rng[r].hi - s_find_rng[r].lo);
        return;
    }
    if (k == 1) {
        s_find_n = 0;
        for (r = 0, base = 0; r < 2; base += s_find_rng[r].hi - s_find_rng[r].lo, r++) {
            const uint8_t *mem = (const uint8_t *)((uintptr_t)g_xbox_mem_offset + s_find_rng[r].lo);
            uint32_t len = s_find_rng[r].hi - s_find_rng[r].lo;
            for (i = 0; i + 4 <= len && s_find_n < FIND_MAX_CAND; i += 4) {
                uint32_t a, b;
                memcpy(&a, s_find_snap + base + i, 4);
                memcpy(&b, mem + i, 4);
                if (a != b) {
                    FindCand *c = &s_find_c[s_find_n++];
                    c->va = s_find_rng[r].lo + i;
                    c->v[0] = a;
                    c->v[1] = b;
                }
            }
        }
        fprintf(stderr, "  [FIND] angle: %u dwords changed in the first step\n", s_find_n);
        return;
    }
    for (i = 0; i < s_find_n; i++)
        s_find_c[i].v[k] = *(const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + s_find_c[i].va);
    if (k < FIND_NS - 1)
        return;
    {
        unsigned shown = 0, best_i = 0;
        int best_t = -1;
        double best = 1e30;
        for (i = 0; i < s_find_n; i++) {
            int t;
            for (t = 0; t < 4; t++) {
                double s = find_score(&s_find_c[i], t);
                const FindCand *c = &s_find_c[i];
                if (s <= 0)
                    continue;
                if (shown < 150) {
                    shown++;
                    fprintf(stderr, "  [FIND] angle cand %08X %-7s sym %.3f: %.4f | %.4f %.4f %.4f | %.4f | %.4f %.4f %.4f | %.4f\n",
                            c->va, tn[t], s, find_as(c->v[0], t), find_as(c->v[1], t), find_as(c->v[2], t),
                            find_as(c->v[3], t), find_as(c->v[4], t), find_as(c->v[5], t), find_as(c->v[6], t),
                            find_as(c->v[7], t), find_as(c->v[8], t));
                }
                if (!find_known(c->va) && s < best) {
                    best = s;
                    best_i = i;
                    best_t = t;
                }
            }
        }
        fprintf(stderr, "  [FIND] angle: %u candidates\n", shown);
        if (best_t >= 0) {
            fprintf(stderr, "  [FIND] angle best: %08X %s\n", s_find_c[best_i].va, tn[best_t]);
            xbox_WatchArmAt(s_find_c[best_i].va, 300, 0);
        }
    }
    fflush(stderr);
}

/* Called once per poll; may set the right stick. Returns 1 while running. */
/* RECOMP_MKB_FIND_AUTO=<ms>: start the finder by itself that long after the
 * first pad poll, for a scripted run with nobody at the keyboard. The run
 * then also ignores focus and leaves the pointer alone. */
static long s_find_auto = -2;
static int mkb_find_auto(void)
{
    if (s_find_auto == -2) {
        const char *e = getenv("RECOMP_MKB_FIND_AUTO");
        s_find_auto = e && *e ? strtol(e, NULL, 0) : -1;
    }
    return s_find_auto >= 0;
}

static int find_step(const unsigned char *tap, SHORT *rx, SHORT *ry)
{
    DWORD t, end = 0;
    int p;
    static DWORD first_poll;
    static int auto_fired;
    if (s_find_on < 0) {
        const char *e = getenv("RECOMP_MKB_FIND");
        s_find_on = e && *e && *e != '0';
    }
    if (!s_find_on)
        return 0;
    if (!first_poll)
        first_poll = GetTickCount();
    if (s_find_phase < 0) {
        const char *rw = getenv("RECOMP_MKB_RWATCH");
        int fire = tap[VK_F10];
        if (!fire && mkb_find_auto() && !auto_fired
                && GetTickCount() - first_poll >= (DWORD)s_find_auto) {
            fire = 1;
            auto_fired = 1;
        }
        if (!fire)
            return 0;
        /* RECOMP_MKB_RWATCH=<va>: F10 only arms the read watch there, for
         * following a value already found to its readers. */
        if (rw && *rw) {
            xbox_WatchArmAt((uint32_t)strtoul(rw, NULL, 0), 1500, 1);
            fflush(stderr);
            return 0;
        }
        /* RECOMP_MKB_WWATCH=<va>: F10 only arms the write watch there */
        rw = getenv("RECOMP_MKB_WWATCH");
        if (rw && *rw) {
            xbox_WatchArmAt((uint32_t)strtoul(rw, NULL, 0), 400, 0);
            fflush(stderr);
            return 0;
        }
        if (!s_find_snap) s_find_snap = (uint8_t *)malloc(FIND_SNAP_BYTES);
        if (!s_find_c) s_find_c = (FindCand *)malloc(FIND_MAX_CAND * sizeof(FindCand));
        if (!s_find_snap || !s_find_c) {
            fprintf(stderr, "  [FIND] out of memory\n");
            s_find_on = 0;
            return 0;
        }
        fprintf(stderr, "  [FIND] started: hands off the mouse and keys for 3 s\n");
        s_find_phase = 0;
        s_find_t0 = GetTickCount();
    }
    t = GetTickCount() - s_find_t0;
    for (p = 0; p <= s_find_phase && p < FIND_NS; p++)
        end += s_find_sched[p].len;
    while (s_find_phase < FIND_NS && t >= end) {
        find_sample(s_find_phase);
        s_find_phase++;
        if (s_find_phase < FIND_NS)
            end += s_find_sched[s_find_phase].len;
    }
    if (s_find_phase >= FIND_NS) {
        fprintf(stderr, "  [FIND] done\n");
        fflush(stderr);
        s_find_phase = -1;
        return 0;
    }
    *rx = (SHORT)(s_find_sched[s_find_phase].x * 32767);
    *ry = 0;
    return 1;
}

static uint32_t g32(uint32_t va)
{
    return *(const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}
static float gf(uint32_t va)
{
    return *(const volatile float *)((uintptr_t)g_xbox_mem_offset + va);
}
static int widget_ok(uint32_t w)
{
    float y;
    if (!((w >= 0x00010000u && w < 0x03FF0000u) || (w >= 0x80000000u && w < 0x83FF0000u)))
        return 0;
    y = gf(w + s_widget_y);
    return y == y && y > -50.0f && y < 600.0f;
}

/* A widget's height on the screen: its own y plus its parent's (+0x4C). Most
 * screens hang their rows straight off the root (y 0), but the profile list
 * sits in a container at y 213 and its rows count from there. */
static float widget_abs_y(uint32_t w)
{
    float y = gf(w + s_widget_y);
    uint32_t par = g32(w + 0x4C);
    if (par != w && widget_ok(par)) {
        float py = gf(par + s_widget_y);
        if (py == py && py > -50.0f && py < 600.0f)
            y += py;
    }
    return y;
}

/* The name-entry keyboard (a typewriter): five rows of keys at heights 184,
 * 225, 268, 311 and 351 --
 *   1234567890 / abcdefghij / OK klmnopqrst / uvwxyz_-.@ CLEAR / SPACE BACKSPACE CAPS.
 * `items`/`ys` are the screen's widget list. Returns 1 when this is that
 * screen (then the pointer leaves it alone), after adding to *b the d-pad
 * press that walks the highlight towards the next typed key; the A press
 * goes through s_kb_a_until. Closed loop: every decision reads where the
 * highlight is now, so whatever the title does at a row's end is harmless. */
typedef struct { float y; int cnt; uint32_t w[12]; float x[12]; } KbRow;

static int kb_handle(const uint32_t *items, const float *ys, int n, uint32_t tab, WORD *b)
{
    static const char *k_row[4] = { "1234567890", "abcdefghij", "?klmnopqrst", "uvwxyz_-.@" };
    static const int k_len[5] = { 10, 10, 11, 11, 3 };
    KbRow rows[8];
    static DWORD hold_until, next_at;
    static WORD held;
    static int presses;
    static unsigned short pend;      /* the key being walked to, 0 = none yet */
    static uint32_t last_focus, last_sig;
    static int awaiting;             /* a press is out and the highlight has not shown it */
    static DWORD await_until;
    static int mdl_r = 2, mdl_c;     /* where the highlight is, as far as the presses say */
    static int prev_r, prev_c;       /* ... before the press in flight */
    static DWORD mdl_settle;         /* no picture correction before this tick */
    DWORD now = GetTickCount(), s_kb_tick_prev = s_kb_tick;
    int nr = 0, i, j, r, cr = -1, cc = -1, best = -1, nbest = 0;
    unsigned top = 0;

    for (i = 0; i < n; i++) {
        if (ys[i] < 150.0f || ys[i] > 380.0f)
            continue;
        for (r = 0; r < nr && fabsf(rows[r].y - ys[i]) > 3.0f; r++) {}
        if (r == nr) {
            if (nr == 8) return 0;
            rows[nr].y = ys[i];
            rows[nr].cnt = 0;
            nr++;
        }
        if (rows[r].cnt == 12) return 0;
        rows[r].w[rows[r].cnt] = items[i];
        rows[r].x[rows[r].cnt] = gf(items[i] + 0x20);
        rows[r].cnt++;
    }
    if (nr != 5)
        return 0;
    for (i = 1; i < nr; i++) {                    /* rows by height */
        KbRow t = rows[i];
        for (j = i; j > 0 && rows[j - 1].y > t.y; j--) rows[j] = rows[j - 1];
        rows[j] = t;
    }
    for (r = 0; r < 5; r++) {
        if (rows[r].cnt != k_len[r])
            return 0;
        for (i = 1; i < rows[r].cnt; i++) {       /* keys by x */
            uint32_t tw = rows[r].w[i];
            float tx = rows[r].x[i];
            for (j = i; j > 0 && rows[r].x[j - 1] > tx; j--) {
                rows[r].w[j] = rows[r].w[j - 1];
                rows[r].x[j] = rows[r].x[j - 1];
            }
            rows[r].w[j] = tw;
            rows[r].x[j] = tx;
        }
    }
    if (now - s_kb_tick > 600u) {                 /* the screen has just opened */
        s_kbq_tail = s_kbq_head;
        pend = 0;
        presses = 0;
    }
    s_kb_tick = now;
    if ((LONG)(hold_until - now) > 0)
        *b |= held;
    if ((LONG)(next_at - now) > 0)
        return 1;

    /* The highlighted key, by dead reckoning: the screen opens on OK, and
     * every press moves the model the way the title does (up/down to the key
     * of the next row nearest in x, left/right to the next key). The keys'
     * states only ever say "this one or the one just left" (the focus table
     * is empty at rest), so they correct the model only once no press is in
     * flight, and only when it is not one of the two. */
    {
        uint32_t S[3];
        unsigned topst = 0;
        int ns = 0;
        (void)tab;
        if (now - s_kb_tick_prev > 600u) { mdl_r = 2; mdl_c = 0; mdl_settle = 0; }
        for (r = 0; r < 5; r++)
            for (i = 0; i < rows[r].cnt; i++) {
                unsigned st = (g32(rows[r].w[i] + 4) >> 16) & 0xF;
                if (st > topst) { topst = st; ns = 0; }
                if (st == topst && ns < 3) S[ns++] = rows[r].w[i];
            }
        if (awaiting) {
            /* a press is out: it is confirmed when the key it should have
             * reached shows as highlighted; if nothing has changed by the
             * deadline the press was lost (back to where it was), and if
             * something else did, the picture decides */
            uint32_t sigS = (ns > 0 ? S[0] : 0) ^ (ns == 2 ? S[1] * 31u : 0);
            uint32_t ew = rows[mdl_r].w[mdl_c];
            if ((ns >= 1 && S[0] == ew) || (ns == 2 && S[1] == ew))
                awaiting = 0;
            else if ((LONG)(await_until - now) > 0)
                return 1;
            else {
                awaiting = 0;
                if (sigS == last_sig) {
                    mdl_r = prev_r;
                    mdl_c = prev_c;
                    if (getenv("RECOMP_MENU_DEBUG"))
                        fprintf(stderr, "  [KB] press lost: back to row %d col %d\n", mdl_r, mdl_c);
                }
            }
        }
        if (!awaiting && (ns == 1 || ns == 2)) {
            uint32_t mw = rows[mdl_r].w[mdl_c];
            if (!(ns >= 1 && mw == S[0]) && !(ns == 2 && mw == S[1])) {
                float mx = gf(mw + 0x20), my2 = gf(mw + 0x24), bd = 1e30f;
                int k2;
                for (k2 = 0; k2 < ns; k2++) {         /* the nearest of the two to the model */
                    float dx2 = gf(S[k2] + 0x20) - mx, dy2 = gf(S[k2] + 0x24) - my2;
                    if (dx2 * dx2 + dy2 * dy2 < bd) {
                        bd = dx2 * dx2 + dy2 * dy2;
                        for (r = 0; r < 5; r++)
                            for (i = 0; i < rows[r].cnt; i++)
                                if (rows[r].w[i] == S[k2]) { mdl_r = r; mdl_c = i; }
                    }
                }
                if (getenv("RECOMP_MENU_DEBUG"))
                    fprintf(stderr, "  [KB] model corrected to row %d col %d\n", mdl_r, mdl_c);
            }
        }
        cr = mdl_r;
        cc = mdl_c;
        best = cr * 16 + cc;
        nbest = 1;
    }
#if 0
    {
        uint32_t S[3], fw = 0, sig;
        int ns = 0;
        (void)tab;
        for (r = 0; r < 5; r++)
            for (i = 0; i < rows[r].cnt; i++) {
                unsigned st = (g32(rows[r].w[i] + 4) >> 16) & 0xF;
                if (st > top) { top = st; ns = 0; }
                if (st == top && ns < 3) S[ns++] = rows[r].w[i];
            }
        if (now - s_kb_tick_prev > 600u)
            last_focus = last_sig = 0;
        if (ns == 1) {
            fw = S[0];
            sig = S[0];
            if (awaiting && sig == last_sig && (LONG)(await_until - now) > 0)
                return 1;
        }
        else if (ns == 2) {
            sig = S[0] ^ (S[1] * 31u);
            /* after a press the highlight shows the move a moment late;
             * deciding on the old picture presses twice */
            if (awaiting && sig == last_sig && (LONG)(await_until - now) > 0)
                return 1;
            if (sig == last_sig && (last_focus == S[0] || last_focus == S[1])) fw = last_focus;
            else if (last_focus == S[0]) fw = S[1];
            else if (last_focus == S[1]) fw = S[0];
            else {
                /* neither is where it was: it moved twice, and the current
                 * one is the one further from where it was */
                float lx, ly, d0, d1;
                if (!last_focus) return 1;
                lx = gf(last_focus + 0x20);
                ly = gf(last_focus + 0x24);
                d0 = (gf(S[0] + 0x20) - lx) * (gf(S[0] + 0x20) - lx) + (gf(S[0] + 0x24) - ly) * (gf(S[0] + 0x24) - ly);
                d1 = (gf(S[1] + 0x20) - lx) * (gf(S[1] + 0x20) - lx) + (gf(S[1] + 0x24) - ly) * (gf(S[1] + 0x24) - ly);
                fw = d1 > d0 ? S[1] : S[0];
            }
        } else {
            static DWORD said;
            if (getenv("RECOMP_MENU_DEBUG") && now - said > 500u) {
                said = now;
                fprintf(stderr, "  [KB] %d keys at top state %u\n", ns, top);
            }
            return 1;                             /* mid-animation: wait */
        }
        if (awaiting && sig != last_sig)
            awaiting = 0;                         /* the move has shown */
        if ((fw != last_focus || sig != last_sig) && getenv("RECOMP_MENU_DEBUG")) {
            int rr = -1, cc2 = -1;
            for (r = 0; r < 5; r++)
                for (i = 0; i < rows[r].cnt; i++)
                    if (rows[r].w[i] == fw) { rr = r; cc2 = i; }
            fprintf(stderr, "  [KB] focus row %d col %d (%d at top state %u)\n", rr, cc2, ns, top);
        }
        last_focus = fw;
        last_sig = sig;
        for (r = 0; r < 5; r++)
            for (i = 0; i < rows[r].cnt; i++)
                if (rows[r].w[i] == fw) { best = r * 16 + i; nbest = 1; }
    }
#endif

    if (!pend) {
        if (s_kbq_tail == s_kbq_head)
            return 1;
        {
            unsigned short q = s_kbq[s_kbq_tail & 31];
            unsigned vk = q & 0xFF;
            int sh = (q & 0x100) != 0;
            s_kbq_tail++;
            if (vk >= 'A' && vk <= 'Z') pend = (unsigned short)(vk - 'A' + 'a');
            else if (vk == '2' && sh) pend = '@';
            else if (vk >= '0' && vk <= '9') pend = (unsigned short)vk;
            else if (vk == VK_OEM_MINUS) pend = sh ? '_' : '-';
            else if (vk == VK_OEM_PERIOD) pend = '.';
            else if (vk == VK_SPACE) pend = ' ';
            else if (vk == VK_BACK) pend = 8;
            else if (vk == VK_RETURN) pend = 13;
            presses = 0;
            if (!pend)
                return 1;
        }
    }
    {
        int tr = -1, tc = -1;
        if (pend == ' ')      { tr = 4; tc = 0; }
        else if (pend == 8)   { tr = 4; tc = 1; }
        else if (pend == 13)  { tr = 2; tc = 0; }
        else
            for (r = 0; r < 4 && tr < 0; r++) {
                const char *p = strchr(k_row[r], (int)pend);
                if (p && pend != '?') { tr = r; tc = (int)(p - k_row[r]); }
            }
        {   /* RECOMP_MENU_DEBUG=1: each step of the typing */
            static int dbg = -1;
            if (dbg < 0) { const char *e = getenv("RECOMP_MENU_DEBUG"); dbg = e && *e == '1'; }
            if (dbg)
                fprintf(stderr, "  [KB] key %c(%d) at row %d col %d -> row %d col %d presses %d\n",
                        pend > 32 ? (char)pend : '.', (int)pend, cr, cc, tr, tc, presses);
        }
        if (tr < 0 || presses > 40) {             /* no such key, or lost: drop it */
            pend = 0;
            return 1;
        }
        if (cr == tr && cc == tc) {               /* on it: press A */
            s_kb_a_until = now + 100u;
            next_at = now + 300u;
            pend = 0;
            return 1;
        }
        held = cr != tr ? (tr > cr ? XBOX_GAMEPAD_DPAD_DOWN : XBOX_GAMEPAD_DPAD_UP)
                        : (tc > cc ? XBOX_GAMEPAD_DPAD_RIGHT : XBOX_GAMEPAD_DPAD_LEFT);
        prev_r = cr;
        prev_c = cc;
        {   /* what the picture shows now, to tell a lost press from a slow one */
            uint32_t s0 = 0, s1 = 0;
            unsigned tp = 0;
            int q, i2;
            for (q = 0; q < 5; q++)
                for (i2 = 0; i2 < rows[q].cnt; i2++) {
                    unsigned st = (g32(rows[q].w[i2] + 4) >> 16) & 0xF;
                    if (st > tp) { tp = st; s0 = rows[q].w[i2]; s1 = 0; }
                    else if (st == tp && !s1) s1 = rows[q].w[i2];
                }
            last_sig = s0 ^ (s1 ? s1 * 31u : 0);
        }
        if (cr != tr) {                           /* the model follows the press */
            int nr2 = cr + (tr > cr ? 1 : -1), bj = 0;
            float x = rows[cr].x[cc], bd = 1e30f;
            for (j = 0; j < rows[nr2].cnt; j++)
                if (fabsf(rows[nr2].x[j] - x) < bd) { bd = fabsf(rows[nr2].x[j] - x); bj = j; }
            mdl_r = nr2;
            mdl_c = bj;
        } else
            mdl_c = cc + (tc > cc ? 1 : -1);
        mdl_settle = now + 900u;
        presses++;
        hold_until = now + 80u;
        next_at = now + 200u;
        awaiting = 1;
        await_until = now + 600u;
        *b |= held;
    }
    return 1;
}

/* The free pointer: the Windows cursor stays visible in a menu, and while it
 * moves over an item of the focused list the highlight is walked there with
 * d-pad presses (the menu's own way of moving, so its sounds and state stay
 * right); the clicks are the menu table's. 0 when the focused widget cannot
 * be found -- the caller then steps with the mouse instead. */
/* The pointer found a row of items to aim at on its last poll. */
static int s_hover_ok;

/* A horizontal menu (the pause menu: Resume Options Restart Quit): 3 to 8
 * widgets at one height, evenly spaced in x; the focused one is the widget in
 * the focus table's slot menu_focus_off. The highlight is walked to the item
 * under the pointer with left/right presses, like the lists are with up/down. Returns 1 when this is
 * such a screen (the pointer then leaves the vertical logic alone). */
static volatile LONG s_hm_btn;          /* the mouse buttons as of this poll (mkb_merge) */
/* How long the pause menu takes to answer the pointer: when a menu came up,
 * and how far the pointer got since ([PAUSE] lines in the log). */
static DWORD s_menu_on_tick;
static int s_menu_stage;

static int hmenu_handle(uint32_t tab, WORD *b)
{
    static DWORD on_until, off_until, settle_until, pend_until;
    static WORD pend;                   /* a click on an arrow of the objectives line */
    static int prev_cl;
    static WORD held;
    static POINT last;
    static int moved, presses, primed, prev_cur = -2, pressed_from = -9;
    static uint32_t prev_first;
    static DWORD since;
    uint32_t it[64], row[8], first = 0;
    float xs[8], ys[64], gy = 0.0f, d = 0.0f;
    int st[8], n = 0, gn = 0, k, i, j, cur = -1, t = -1;
    DWORD now = GetTickCount();
    POINT pt;
    RECT rc;
    HWND fg = mkb_find_auto() ? FindWindowA("XboxRecompFramebuffer", NULL) : GetForegroundWindow();

    if (!fg)
        return 0;
    for (k = 0; k < 8 && !gn; k++) {
        uint32_t w, k0 = g32(tab + 4u * (uint32_t)k);
        int guard;
        if (!widget_ok(k0))
            continue;
        for (w = k0, guard = 0; guard < 64; guard++) {
            uint32_t pv = g32(w + s_widget_prev);
            if (!widget_ok(pv) || g32(pv + s_widget_next) != w)
                break;
            w = pv;
        }
        n = 0;
        while (n < 64 && widget_ok(w)) {
            uint32_t nx;
            it[n] = w;
            ys[n] = widget_abs_y(w);
            n++;
            nx = g32(w + s_widget_next);
            if (!widget_ok(nx) || g32(nx + s_widget_prev) != w || nx == it[0])
                break;
            w = nx;
        }
        /* a group: widgets of the same height, state 4 or more, x spread out */
        for (i = 0; i < n && !gn; i++) {
            int m = 0;
            float a[8];
            uint32_t r[8];
            int s[8];
            if (ys[i] < 100.0f || ys[i] > 450.0f || ((g32(it[i] + 4) >> 16) & 0xF) < 4)
                continue;
            for (j = 0; j < n && m < 8; j++) {
                float x = gf(it[j] + 0x20);
                int sj = (int)((g32(it[j] + 4) >> 16) & 0xF);
                if (fabsf(ys[j] - ys[i]) >= 1.0f || sj < 4 || !(x > 0.0f && x < 1000.0f))
                    continue;
                a[m] = x; r[m] = it[j]; s[m] = sj; m++;
            }
            if (m < 3 || m > 8 || it[i] != r[0])
                continue;               /* take each group from its first member */
            for (j = 1; j < m; j++) {   /* sort by x */
                float xv = a[j]; uint32_t rv = r[j]; int sv = s[j], q = j - 1;
                while (q >= 0 && a[q] > xv) { a[q + 1] = a[q]; r[q + 1] = r[q]; s[q + 1] = s[q]; q--; }
                a[q + 1] = xv; r[q + 1] = rv; s[q + 1] = sv;
            }
            d = (a[m - 1] - a[0]) / (float)(m - 1);
            if (d < 40.0f || d > 260.0f)
                continue;
            for (j = 1; j < m && fabsf((a[j] - a[j - 1]) - d) < 0.15f * d; j++) {}
            if (j < m)
                continue;
            gn = m; gy = ys[i]; first = r[0];
            for (j = 0; j < m; j++) { xs[j] = a[j]; row[j] = r[j]; st[j] = s[j]; }
        }
    }
    if (!gn) {
        static DWORD nolog;
        if (now - s_menu_on_tick < 8000u && now - nolog >= 500u) {
            nolog = now;
            fprintf(stderr, "  [PAUSE] +%lu ms: no button row yet (table %08X, slots %08X %08X %08X %08X)\n",
                    (unsigned long)(now - s_menu_on_tick), tab, g32(tab), g32(tab + 4), g32(tab + 8), g32(tab + 12));
        }
        return 0;
    }
    {   /* the focus table's slot menu_focus_off holds the focused widget: one of
         * the items, or (on the row above them) something else -- cur stays -1 */
        uint32_t fw = g32(tab + s_menu_focus_off);
        for (i = 0; i < gn; i++)
            if (row[i] == fw)
                cur = i;
    }
    if (s_menu_stage < 1) {
        s_menu_stage = 1;
        fprintf(stderr, "  [PAUSE] +%lu ms: button row found (%d buttons, focus %d)\n",
                (unsigned long)(now - s_menu_on_tick), gn, cur);
    }
    if (first != prev_first) { prev_first = first; moved = 0; presses = 0; pressed_from = -9; since = now; }
    {   /* a left click on an arrow (the ends of the objectives line, which shows
         * the mission's objectives one by one) is a left or right press there;
         * the hover has taken the highlight up to that line by then */
        int cl = (int)(s_hm_btn & 1);
        if (cl && !prev_cl) {
            POINT q;
            RECT r2;
            GetCursorPos(&q);
            ScreenToClient(fg, &q);
            if (GetClientRect(fg, &r2) && r2.right > 0 && r2.bottom > 0) {
                float cy = (float)q.y * 480.0f / (float)r2.bottom;
                float cx = (float)q.x * 640.0f / (float)r2.right;
                if (cy >= gy - 40.0f && cy < gy - 13.0f && (cx < 130.0f || cx > 510.0f)) {
                    pend = cx < 130.0f ? XBOX_GAMEPAD_DPAD_LEFT : XBOX_GAMEPAD_DPAD_RIGHT;
                    pend_until = now + 600u;
                }
            }
        }
        prev_cl = cl;
    }
    if ((LONG)(on_until - now) > 0) { *b |= held; return 1; }
    if ((LONG)(off_until - now) > 0) return 1;
    if (pend && (LONG)(pend_until - now) > 0 && cur < 0) {
        held = pend;
        pend = 0;
        on_until = now + 35;
        off_until = now + 70;
        *b |= held;
        return 1;
    }
    if ((LONG)(pend_until - now) <= 0)
        pend = 0;
    if (cur != prev_cur) {              /* a move our own press made needs no dwell */
        since = cur >= 0 && pressed_from != -9 && cur != pressed_from ? now - 150u : now;
        prev_cur = cur;
    }
    if (cur == pressed_from && (LONG)(settle_until - now) > 0)
        return 1;                       /* the press has not shown yet */
    if (cur != pressed_from)
        pressed_from = -9;
    if (now - since < 150u)             /* the focus is still changing */
        return 1;
    GetCursorPos(&pt);
    ScreenToClient(fg, &pt);
    if (!GetClientRect(fg, &rc) || rc.bottom <= 0 || rc.right <= 0)
        return 1;
    if (!primed) { primed = 1; last = pt; }
    if (pt.x != last.x || pt.y != last.y) { moved = 1; presses = 0; last = pt; }
    {
        /* The title draws its 640 x 480 layout over the whole window (also
         * 16:9, stretched), so x counts in 640ths of the width; the widget's x
         * sits 107 right of its label's centre. */
        float my = (float)pt.y * 480.0f / (float)rc.bottom;
        float mx = (float)pt.x * 640.0f / (float)rc.right;
        float best = 1e9f;
        /* the button row is the strip between two red lines (y 377..405); the
         * line of the mission objectives is the strip above it (y 350..377) */
        if (fabsf(my - (gy + 1.0f)) <= 14.0f)
            for (i = 0; i < gn; i++)
                if (fabsf(xs[i] - 107.0f - mx) < best) { best = fabsf(xs[i] - 107.0f - mx); t = i; }
        if (t >= 0 && best > 0.5f * d)
            t = -1;
        if (t < 0 && my >= gy - 40.0f && my < gy - 13.0f)
            t = -2;                     /* over the line above the buttons */
        if (getenv("RECOMP_MENU_DEBUG") && moved)
            fprintf(stderr, "  [AIM-H] mx %.0f my %.0f cur %d t %d of %d (y %.0f d %.0f) presses %d\n",
                    mx, my, cur, t, gn, gy, d, presses);
    }
    s_hover_ok = t >= 0 || t == -2;
    if (t == -1 || !moved)
        return 1;
    if (t == -2 && cur < 0) { moved = 0; return 1; }   /* already on the objectives line */
    if (t == cur || ++presses > 2 * gn + 2) { moved = 0; return 1; }
    /* off the row (on the line of the mission above it): down enters it, and
     * from the row, the pointer over that line takes the highlight back up */
    held = t == -2 ? XBOX_GAMEPAD_DPAD_UP
         : cur < 0 ? XBOX_GAMEPAD_DPAD_DOWN
                   : xs[t] > xs[cur] ? XBOX_GAMEPAD_DPAD_RIGHT : XBOX_GAMEPAD_DPAD_LEFT;
    if (s_menu_stage < 2) {
        s_menu_stage = 2;
        fprintf(stderr, "  [PAUSE] +%lu ms: first pointer press (cur %d -> target %d)\n",
                (unsigned long)(now - s_menu_on_tick), cur, t);
    }
    on_until = now + 35;
    off_until = now + 55;
    pressed_from = cur;
    settle_until = now + 250;
    *b |= held;
    return 1;
}

static int menu_hover(WORD *b)
{
    static DWORD on_until, off_until;
    static WORD held;
    static POINT last;
    static int moved;
    uint32_t tab, f, w, items[64];
    float ys[64], my, best = 1e9f, sp, fy;
    int n = 0, cur = -1, t = -1, i, k, guard, lo, hi;
    static int presses;
    static uint32_t pressed_from;   /* the focus a press left from */
    static WORD pressed_dir;        /* ... its direction, from height, to height */
    static float pressed_y, pressed_ty;
    static float dead_y[8];         /* heights the highlight never reaches */
    static int ndead;
    static uint32_t dead_owner;     /* ... on the screen whose list starts here */
    static DWORD dead_time;         /* ... as of when the last was found */
    DWORD *settle_deadline = NULL;
    POINT pt;
    RECT rc;
    HWND fg = mkb_find_auto() ? FindWindowA("XboxRecompFramebuffer", NULL)   /* scripted test */
                              : GetForegroundWindow();
    DWORD now = GetTickCount();

    if (!s_menu_focus || !fg)
        return 0;
    {   /* RECOMP_MENU_DEBUG=1: the whole focus table whenever it changes */
        static int dbg = -1;
        static uint32_t last[8];
        if (dbg < 0) { const char *e = getenv("RECOMP_MENU_DEBUG"); dbg = e && *e == '1'; }
        if (dbg) {
            uint32_t tb = g32(s_menu_focus), v[8];
            int k, ch = 0;
            for (k = 0; k < 8; k++) {
                v[k] = (tb >= 0x80000000u && tb < 0x83FF0000u) ? g32(tb + 4u * k) : 0;
                if (v[k] != last[k]) ch = 1;
                last[k] = v[k];
            }
            if (ch) {
                fprintf(stderr, "  [MENU] table %08X:", tb);
                for (k = 0; k < 8; k++)
                    fprintf(stderr, " %08X(y%.0f,s%X)", v[k],
                            widget_ok(v[k]) ? gf(v[k] + s_widget_y) : -1.0f,
                            v[k] >= 0x80000000u && v[k] < 0x83FF0000u ? (g32(v[k] + 4) >> 16) & 0xF : 0);
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
    }
    /* The focus table is the UI script's scratch: its slots hold widgets of
     * the screen on show, but which slot is the focused one changes from
     * screen to screen. So any widget there is only a way in: walk its
     * list of siblings, take the run of evenly spaced rows, and the focused
     * row is the one whose state (bits 16-19 of +4) differs from the rest. */
    tab = g32(s_menu_focus);
    if (!((tab >= 0x00010000u && tab < 0x03FF0000u) || (tab >= 0x80000000u && tab < 0x83FF0000u)))
        return 0;
    /* Two ways the menus mark the focus, and each screen uses one:
     *  - the main menu and Options: the widget in focus table slot
     *    menu_focus_off is the focused row;
     *  - the sub-menus: the focused row's state (bits 16-19 of +4) is one
     *    above its neighbours'.
     * So: take the row list of the slot widget (or, failing that, of any
     * widget in the table), find the evenly spaced run of rows, and use a
     * single row in a higher state when there is one, else the slot row. */
    f = 0;
    {   /* the name-entry keyboard first: its 48 keys are one list, reachable
         * from any widget of the screen in the table, focused or not */
        uint32_t kitems[64];
        float kys[64];
        for (k = 0; k < 8; k++) {
            uint32_t k0 = g32(tab + 4u * (uint32_t)k);
            int kn = 0;
            if (!widget_ok(k0))
                continue;
            for (w = k0, guard = 0; guard < 64; guard++) {
                uint32_t pv = g32(w + s_widget_prev);
                if (!widget_ok(pv) || g32(pv + s_widget_next) != w)
                    break;
                w = pv;
            }
            while (kn < 64 && widget_ok(w)) {
                uint32_t nx;
                kitems[kn] = w;
                kys[kn] = widget_abs_y(w);
                kn++;
                nx = g32(w + s_widget_next);
                if (!widget_ok(nx) || g32(nx + s_widget_prev) != w || nx == kitems[0])
                    break;
                w = nx;
            }
            if (kn >= 40 && kb_handle(kitems, kys, kn, tab, b)) {
                s_hover_ok = 0;
                return 1;
            }
        }
    }
    if (hmenu_handle(tab, b))               /* a row of items side by side: the pause menu */
        return 1;
    {
        uint32_t sw = g32(tab + s_menu_focus_off);
        fy = widget_ok(sw) ? widget_abs_y(sw) : NAN;
    }
    for (k = -1; k < 8 && !f; k++) {
        uint32_t f0 = g32(tab + (k < 0 ? s_menu_focus_off : 4u * (uint32_t)k)), st[64];
        int a, z, ci = -1;
        if (!widget_ok(f0))
            continue;
        for (w = f0, guard = 0; guard < 64; guard++) {
            uint32_t pv = g32(w + s_widget_prev);
            if (!widget_ok(pv) || g32(pv + s_widget_next) != w)
                break;
            w = pv;
        }
        n = 0;
        while (n < 64 && widget_ok(w)) {
            uint32_t nx;
            items[n] = w;
            ys[n] = widget_abs_y(w);
            st[n] = (g32(w + 4) >> 16) & 0xF;
            if (w == f0) ci = n;
            n++;
            nx = g32(w + s_widget_next);
            if (!widget_ok(nx) || g32(nx + s_widget_prev) != w || nx == items[0])
                break;
            w = nx;
        }
        /* a run may begin on the row that ended the last one (Options:
         * the title gap ends on the first row) */
        for (a = 0; a + 1 < n && !f; a = z) {
            float d = fabsf(ys[a + 1] - ys[a]);
            int hi_i = -1, his = 0, lo_s = 99, mi = -1;
            uint32_t top = 0;
            for (z = a + 1; z + 1 < n && fabsf(fabsf(ys[z + 1] - ys[z]) - d) < 0.25f * d; z++) {}
            if (d < 8.0f || d > 120.0f || z - a < 1)
                continue;
            for (i = a; i <= z; i++) {
                if (st[i] > top) { top = st[i]; hi_i = i; his = 1; }
                else if (st[i] == top) his++;
                if ((int)st[i] < lo_s) lo_s = (int)st[i];
            }
            /* The slot widget need not be a row of this list (the profile
             * screen's is the container its rows hang from, at the height of
             * the focused row), but when its height is that of one row it
             * names the focus, whatever the rows' own states say (there the
             * first row always keeps a state above the others). */
            if (fy == fy) {
                float md = 2.0f;
                for (i = a; i <= z; i++)
                    if (fabsf(ys[i] - fy) < md) { md = fabsf(ys[i] - fy); mi = i; }
            }
            if (z - a >= 2 && mi >= 0) {
                f = items[mi];
                cur = mi;
            } else if (z - a >= 2 && his == 1 && (int)top == lo_s + 1) {
                f = items[hi_i];          /* a single row a state above: focus */
                cur = hi_i;
            } else if (k < 0 && ci >= a && ci <= z) {
                f = f0;                   /* the slot row, inside the run */
                cur = ci;
            }
        }
    }
    if (!f || cur < 0 || n < 2) {
        /* The sub-menus (Game Settings, Audio/Video) leave their rows out
         * of the focus table until the first d-pad press: it holds only the
         * screen's container then, and the pointer has no focus to walk
         * from. So when the mouse moves over such a screen, one down press
         * fills the table in, and the pointer then walks the highlight to
         * the mouse. Once per table contents (a screen without rows, such as
         * Credits, gets one harmless press), at most every 3 s, and only
         * once the table has held still for 300 ms (a screen opening). */
        static uint32_t last_sig, nudged_sig;
        static DWORD sig_since, nudged_at;
        static POINT nlast;
        static int nprimed;
        uint32_t sig = 0;
        POINT q;
        RECT cr;
        s_hover_ok = 0;
        if ((LONG)(on_until - now) > 0) { *b |= held; return 0; }
        if ((LONG)(off_until - now) > 0) return 0;
        for (k = 0; k < 8; k++)
            sig = sig * 31u + g32(tab + 4u * (uint32_t)k);
        if (sig != last_sig) { last_sig = sig; sig_since = now; }
        GetCursorPos(&q);
        if (!nprimed) { nprimed = 1; nlast = q; }
        if (q.x == nlast.x && q.y == nlast.y)
            return 0;
        nlast = q;
        ScreenToClient(fg, &q);
        if (!GetClientRect(fg, &cr) || q.x < 0 || q.y < 0 || q.x >= cr.right || q.y >= cr.bottom)
            return 0;
        if (sig == nudged_sig || now - sig_since < 300u || now - nudged_at < 3000u)
            return 0;
        nudged_sig = sig;
        nudged_at = now;
        held = XBOX_GAMEPAD_DPAD_DOWN;
        on_until = now + 35;
        off_until = now + 70;
        *b |= held;
        if (getenv("RECOMP_MENU_DEBUG"))
            fprintf(stderr, "  [AIM] nudge: no focus in table %08X\n", tab);
        return 0;
    }
    if (kb_handle(items, ys, n, tab, b)) {     /* the name-entry keyboard: typing, not pointing */
        s_hover_ok = 0;
        return 1;
    }
    {   /* RECOMP_MENU_DEBUG=1: every row's state whenever one changes */
        static int dbg = -1;
        static uint32_t last_sum;
        uint32_t sum = 0;
        if (dbg < 0) { const char *e = getenv("RECOMP_MENU_DEBUG"); dbg = e && *e == '1'; }
        if (dbg) {
            for (k = 0; k < n; k++)
                sum = sum * 31u + ((g32(items[k] + 4) >> 16) & 0xF) + (uint32_t)k;
            if (sum != last_sum) {
                last_sum = sum;
                fprintf(stderr, "  [MENU] rows");
                for (k = 0; k < n; k++)
                    fprintf(stderr, " %X:%08X@%.0f", (g32(items[k] + 4) >> 16) & 0xF,
                            g32(items[k] + 4), ys[k]);
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
    }
    {   /* one line whenever the focus moves, for checking a menu */
        static uint32_t last_f;
        if (f != last_f) {
            last_f = f;
            fprintf(stderr, "  [MKB] menu focus %d of %d (y %.0f, items %.0f..%.0f) states", cur, n,
                    ys[cur], ys[0], ys[n - 1]);
            for (k = 0; k < n; k++)
                fprintf(stderr, " %X@%.0f", (g32(items[k] + 4) >> 16) & 0xF, ys[k]);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }

    if ((LONG)(on_until - now) > 0) { *b |= held; return 1; }
    if ((LONG)(off_until - now) > 0) return 1;
    /* After a press, the next step waits until the menu has moved (or a
     * quarter second) -- deciding on the old focus overshoots. And the focus
     * must read the same on two polls: a screen opening reads in between. */
    {
        static uint32_t prev_f;
        static DWORD settle_until;
        int stable = (f == prev_f);
        prev_f = f;
        if (pressed_from && f == pressed_from && (LONG)(settle_until - now) > 0)
            return 1;
        if (pressed_from && f != pressed_from) {
            settle_until = now;
            /* The press wrapped round (down landed higher, up lower): the
             * target is not a row the highlight reaches (Audio/Video keeps
             * a hidden one under Subtitles). Never aim there again on this
             * screen, and press back to undo the wrap. */
            if (pressed_dir == XBOX_GAMEPAD_DPAD_DOWN ? ys[cur] < pressed_y - 1.0f
                                                      : ys[cur] > pressed_y + 1.0f) {
                if (dead_owner != items[0]) { dead_owner = items[0]; ndead = 0; }
                if (ndead < 8) dead_y[ndead++] = pressed_ty;
                dead_time = now;
                moved = 0;
                pressed_from = 0;
                held = pressed_dir == XBOX_GAMEPAD_DPAD_DOWN ? XBOX_GAMEPAD_DPAD_UP : XBOX_GAMEPAD_DPAD_DOWN;
                pressed_dir = 0;
                on_until = now + 35;
                off_until = now + 70;
                *b |= held;
                return 1;
            }
        }
        pressed_from = 0;
        if (!stable)
            return 1;
        settle_deadline = &settle_until;
    }

    GetCursorPos(&pt);
    ScreenToClient(fg, &pt);
    if (!GetClientRect(fg, &rc) || rc.bottom <= 0)
        return 1;
    {
        static int primed;
        if (!primed) { primed = 1; last = pt; }  /* no move until the mouse moves */
    }
    if (pt.x != last.x || pt.y != last.y) { moved = 1; presses = 0; last = pt; }
    my = (float)pt.y * 480.0f / (float)rc.bottom;
    /* The selectable items: the run around the focused one whose rows are
     * evenly spaced. The list also holds titles and prompts, which the
     * highlight cannot reach -- aiming at one would press forever. */
    /* The rows to aim at: the longest evenly spaced chain (by height)
     * through the focused row. The list's own order need not be the
     * screen's (Audio/Video keeps SFX, Cinematic, Music, then Subtitles),
     * and it also holds titles, prompts, boxes behind a row and the
     * decorations between rows (main menu), which the highlight cannot
     * reach (aiming at one would press forever). So neither list order nor
     * the nearest neighbour gives the spacing: below the main menu's last
     * row sits the prompt, 66 lines down, and taking that as the spacing
     * left the pointer unable to go back up. Chains are sought among rows
     * in state 5 or above and among all elements, and the longest wins:
     * after a while every row but the focused one drops to state 4, and
     * then the state-5 elements left (the prompt, a box) make a false
     * chain of their own. */
    {
        int cand[64], run[64], m, pass, c, j, s, cnt = 0, loose;
        sp = 0.0f;
        /* Evenly spaced rows first; only when that finds fewer than four
         * (a map's Mission Select) is the spacing allowed to vary -- with it
         * loose from the start, the decorations between the main menu's
         * rows make a longer chain than the rows do. */
        for (loose = 0; loose < 2 && cnt < 4; loose++)
        for (pass = 0; pass < 2; pass++) {
            m = 0;
            for (i = 0; i < n; i++) {
                if (i == cur || fabsf(ys[i] - ys[cur]) < 4.0f)
                    continue;               /* a box behind the focused row */
                if (((g32(items[i] + 4) >> 16) & 0xF) < (pass == 0 ? 5 : 4))
                    continue;               /* state 1: decorations, list ends */
                if (ys[i] < 100.0f || ys[i] > 390.0f)
                    continue;               /* the title above, the button bar below */
                cand[m++] = i;
            }
            for (c = 0; c < m; c++) {
                float d = fabsf(ys[cand[c]] - ys[cur]);
                int chain[64], k = 1;
                if (d < 8.0f || d > 120.0f)
                    continue;
                chain[0] = cur;
                for (s = -1; s <= 1; s += 2) {
                    float y = ys[cur];
                    while (k < 64) {
                        /* a map's rows (Mission Select) are 62, 46 and 48
                         * lines apart, so the spacing is only roughly kept */
                        float want = y + (float)s * d, e = (loose ? 0.4f : 0.15f) * d;
                        int hit = -1;
                        for (j = 0; j < m; j++)
                            if (fabsf(ys[cand[j]] - want) <= e) { e = fabsf(ys[cand[j]] - want); hit = cand[j]; }
                        if (hit < 0)
                            break;
                        chain[k++] = hit;
                        y = ys[hit];
                    }
                }
                if (k > cnt || (k == cnt && d < sp)) {
                    cnt = k;
                    sp = d;
                    memcpy(run, chain, sizeof(int) * (size_t)k);
                }
            }
        }
        s_hover_ok = 0;
        if (cnt < 2) {
            static int dbg2 = -1;
            if (dbg2 < 0) { const char *e = getenv("RECOMP_MENU_DEBUG"); dbg2 = e && *e == '1'; }
            if (dbg2 && moved)
                fprintf(stderr, "  [AIM] no run: cur %d(y%.0f)\n", cur, ys[cur]);
            return 1;
        }
        lo = 0;
        hi = cnt - 1;
        /* what was found unreachable may unlock later (a mission): forget it */
        if (dead_owner != items[0] || now - dead_time > 20000u) { dead_owner = items[0]; ndead = 0; }
        for (c = 0; c < cnt; c++) {
            float d = fabsf(ys[run[c]] - my);
            for (j = 0; j < ndead && fabsf(ys[run[c]] - dead_y[j]) > 1.0f; j++) {}
            if (j < ndead)
                continue;               /* found unreachable */
            if (d < best) { best = d; t = run[c]; }
        }
    }
    {   /* RECOMP_MENU_DEBUG=1: each decision, when the mouse has moved */
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("RECOMP_MENU_DEBUG"); dbg = e && *e == '1'; }
        if (dbg && moved)
            fprintf(stderr, "  [AIM] my %.0f cur %d(y%.0f) t %d(y%.0f) best %.0f sp %.0f rows %d presses %d\n",
                    my, cur, ys[cur], t, t >= 0 ? ys[t] : -1.0f, best, sp, hi + 1, presses);
    }
    if (t < 0 || best > 0.6f * sp || hi <= lo)
        return 1;                       /* not over an item: leave it */
    s_hover_ok = 1;                     /* over an item */
    if (!moved)
        return 1;
    if (t == cur || ++presses > 2 * (hi - lo + 1)) { moved = 0; return 1; }
    held = ys[t] > ys[cur] ? XBOX_GAMEPAD_DPAD_DOWN : XBOX_GAMEPAD_DPAD_UP;
    on_until = now + 35;
    off_until = now + 70;
    pressed_from = f;
    pressed_dir = held;
    pressed_y = ys[cur];
    pressed_ty = ys[t];
    *settle_deadline = now + 250;
    *b |= held;
    return 1;
}

/* The mouse as the d-pad in a menu: movement adds up, and every
 * s_menu_mouse counts one way is one press that way (held 70 ms, then
 * 70 ms released, so the title sees each press). A wheel notch is a step. */
static void menu_mouse(LONG dx, LONG dy, LONG wheel, WORD *b)
{
    static long ax, ay;
    static DWORD on_until, off_until;
    static WORD held;
    DWORD now = GetTickCount();
    long step = s_menu_mouse, lim = 3 * (long)s_menu_mouse;

    ax += dx;
    (void)wheel;                        /* the user wants no wheel stepping */
    ay += dy;
    if (ax > lim) ax = lim;
    if (ax < -lim) ax = -lim;
    if (ay > lim) ay = lim;
    if (ay < -lim) ay = -lim;
    if ((LONG)(on_until - now) > 0) {
        *b |= held;
        return;
    }
    if ((LONG)(off_until - now) > 0)
        return;
    held = 0;
    if (ay <= -step)      { held = XBOX_GAMEPAD_DPAD_UP;    ay += step; ax = 0; }
    else if (ay >= step)  { held = XBOX_GAMEPAD_DPAD_DOWN;  ay -= step; ax = 0; }
    /* no left/right: on a row with a value (Invert Y, volumes) sideways
     * changes the value, which only the arrow keys should do */
    if (held) {
        on_until = now + 70;
        off_until = now + 140;
        *b |= held;
    }
}

/* Is this key bound to anything in the menu table? */
static int mkb_menu_key(unsigned code)
{
    int i, j;
    for (i = 0; i < MKC_COUNT; i++)
        for (j = 0; j < 4 && s_bind_menu[i][j]; j++)
            if (s_bind_menu[i][j] == code)
                return 1;
    return 0;
}

/* Forget every key, button and look delta. A window that takes the keys while they are held (the Recomp
 * Settings panel) gets their key-ups, so the game would otherwise keep walking or firing after it closes. */
void mkb_release_all(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        s_vk_down[i] = 0;
        s_vk_tap[i] = 0;
    }
    InterlockedExchange(&s_mouse_btn, 0);
    InterlockedExchange(&s_mouse_click, 0);
    InterlockedExchange(&s_dx, 0);
    InterlockedExchange(&s_dy, 0);
    s_kbq_tail = s_kbq_head;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    unsigned char tap[256];
    int in_menu = 0;
    static DWORD y_until;       /* the wheel's Y press, held this long */
    int any = 0, i;
    static DWORD packet;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    /* The settings panel (F1) owns the input while it is up: no pointer capture, no look, no taps. */
    if (g_input_block) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        return 0;
    }

    if (!s_active) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!mkb_find_auto() && !game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        return 0;
    }
    /* ---- a menu up (mkb.ini menu_flag) or play ---- */
    {
        static int was_menu = -1;
        in_menu = s_menu_flag
            && *(const volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + s_menu_flag)
               == (uint8_t)s_menu_value;
        if (in_menu != was_menu) {
            was_menu = in_menu;
            s_menu_on_tick = GetTickCount();
            s_menu_stage = 0;
            fprintf(stderr, "  [MKB] %s (t=%lu)\n", in_menu ? "menu keys" : "play keys",
                    (unsigned long)s_menu_on_tick);
            fflush(stderr);
        }
    }
    /* the pointer is free in a menu with the free pointer configured */
    if (!mkb_find_auto())
        /* the pointer is shown in a menu only while the mouse or keyboard was the last device used: a pad press
         * hides it (the capture also parks it), a mouse move or key brings it back */
        mkb_manage_cursor(!(in_menu && s_menu_focus) || ic_pad_is_last());

    /* While the title's look hook takes the mouse (mkb_native_aim_take),
     * the deltas are its; the stick gets them only when no hook has asked
     * for a moment (menus, cutscenes, other titles). */
    if (GetTickCount() - (DWORD)s_native_tick < 300u) {
        dx = dy = 0;
    } else {
        dx    = InterlockedExchange(&s_dx, 0);
        dy    = InterlockedExchange(&s_dy, 0);
    }
    wheel = InterlockedExchange(&s_wheel, 0);
    /* Buttons as held, plus any press since the last poll. Draining the
     * held bits here (as this did) turned a held button into a one-poll
     * tap: aim-down-sights opened and dropped at once, and fire never
     * kept firing. */
    btn   = InterlockedCompareExchange(&s_mouse_btn, 0, 0)
          | InterlockedExchange(&s_mouse_click, 0);
    for (i = 0; i < 256; i++) {
        tap[i] = s_vk_tap[i];
        if (tap[i]) s_vk_tap[i] = 0;
    }
    if (!mkb_dev_keys_on()) {
        /* nothing below acts without RECOMP_DEV_KEYS=1 */
    } else {
    if (tap[VK_F2]) InterlockedDecrement(&g_mkb_fog_steps);
    if (tap[VK_F3]) InterlockedIncrement(&g_mkb_fog_steps);
    if (tap[VK_F4]) InterlockedDecrement(&g_mkb_draw_steps);
    if (tap[VK_F5]) InterlockedIncrement(&g_mkb_draw_steps);
    if (tap[VK_F6]) InterlockedDecrement(&g_mkb_fov_steps);
    if (tap[VK_F7]) InterlockedIncrement(&g_mkb_fov_steps);
    if (tap[VK_F8] && !getenv("RECOMP_MKB_SNAPDIR")) InterlockedIncrement(&g_mkb_pin_toggles);
    if (tap[VK_F12]) InterlockedIncrement(&g_mkb_cap_toggles);
    }
    if (mkb_dev_keys_on() && tap[VK_F8] && getenv("RECOMP_MKB_SNAPDIR")) {      /* F8: a memory snapshot by hand */
        static int snap_n;
        mkb_snapshot(++snap_n);
    }
    s_kb_on = in_menu && GetTickCount() - s_kb_tick < 400u;   /* typing a profile name */

    memset(analog, 0, sizeof analog);

    /* ---- keyboard: WASD movement ----
     * Full throw, straight to the stick. Through mkb_aim_vector (the mouse
     * curve) one key came out at 1 % of throw plus the dead-zone floor,
     * under the title's own dead zone, so only diagonals moved. */
    {
        int mx = 0, my = 0;
        if (s_move[0] && MK_VK(s_move[0])) my += 1;
        if (s_move[1] && MK_VK(s_move[1])) my -= 1;
        if (s_move[2] && MK_VK(s_move[2])) mx -= 1;
        if (s_move[3] && MK_VK(s_move[3])) mx += 1;
        if (mx || my) {
            double k = (mx && my) ? 0.70710678 * 32767.0 : 32767.0;
            mov_x = (SHORT)(mx * k);
            mov_y = (SHORT)(my * k);
        }
    }

    /* In a menu the mouse moves the highlight and the wheel steps it;
     * the clicks are the menu table's (select, back). */
    if (in_menu && s_menu_focus) {        /* the free pointer; never the stepping mouse */
        s_hm_btn = btn;
        menu_hover(&b);
        if ((LONG)(s_kb_a_until - GetTickCount()) > 0)
            analog[XBOX_BUTTON_A] = 255;  /* the keyboard screen's A press */
        dx = dy = 0;
        wheel = 0;
    } else if (in_menu && s_menu_mouse > 0) {
        menu_mouse(dx, dy, wheel, &b);
        dx = dy = 0;
        wheel = 0;
    }

    /* ---- mouse: aim ---- */
    if (dx || dy) {
        double ax =  (double)dx / (double)s_cfg.full;
        double ay = -(double)dy / (double)s_cfg.full;
        if (s_cfg.invert_y) ay = -ay;
        mkb_aim_vector(ax, ay, &aim_x, &aim_y);
    }
    /* the aim finder, when running, owns the right stick */
    if (find_step(tap, &aim_x, &aim_y))
        mov_x = mov_y = 0;

    /* ---- buttons: the binding table (mkb_bind_defaults, mkb.ini) ----
     * A wheel notch is an event, not a held button: whatever it is bound
     * to is held for 80 ms so the title sees one whole press. */
    if (wheel)
        y_until = GetTickCount() + 80;
    for (i = 0; i < MKC_COUNT; i++) {
        int j, on = 0;
        const unsigned short *keys = in_menu && s_bind_menu[i][0] ? s_bind_menu[i] : s_bind[i];
        for (j = 0; j < 4 && keys[j] && !on; j++) {
            unsigned code = keys[j];
            if (in_menu && keys == s_bind[i] && mkb_menu_key(code))
                continue;                   /* that key belongs to the menu */
            if (in_menu && (code == VK_UP || code == VK_DOWN || code == VK_LEFT || code == VK_RIGHT)
                && i != MKC_UP && i != MKC_DOWN && i != MKC_LEFT && i != MKC_RIGHT)
                continue;                   /* an arrow is only the D-pad in a menu */
            if (code < 0x100)
                on = MK_VK(code);
            else if (code < MKB_WHEEL)
                on = (btn >> (code - MKB_MOUSE1)) & 1;
            else
                on = (LONG)(y_until - GetTickCount()) > 0;
        }
        if (!on)
            continue;
        if (s_ctl[i].analog >= 0)
            analog[s_ctl[i].analog] = 255;
        else
            b |= s_ctl[i].mask;
    }

    /* The arrows always move in a menu, whatever the in-game keys are (the D-pad right is no
     * longer bound to a key: the right arrow is the in-game Position squad). Nothing else about
     * the menu keyboard changed. */
    if (in_menu) {
        if (MK_VK(VK_UP))     b |= XBOX_GAMEPAD_DPAD_UP;
        if (MK_VK(VK_DOWN))   b |= XBOX_GAMEPAD_DPAD_DOWN;
        if (MK_VK(VK_LEFT))   b |= XBOX_GAMEPAD_DPAD_LEFT;
        if (MK_VK(VK_RIGHT))  b |= XBOX_GAMEPAD_DPAD_RIGHT;
    }

    /* Before any play, a screen with no row to aim at (the title,
     * "Press Start"): a left click is Start there (Esc still is). */
    {
        static int ever_played;
        if (!in_menu && GetTickCount() - (DWORD)s_native_tick < 300u)
            ever_played = 1;            /* real play: the look hook took the mouse */
        if (in_menu && s_menu_focus && !ever_played && !s_hover_ok && (btn & 1))
            b |= XBOX_GAMEPAD_START;
    }

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}

static int mkb_in_menu(void)
{
    return s_menu_flag
        && *(const volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + s_menu_flag)
           == (uint8_t)s_menu_value;
}

/* ======================================================================== */
/* In-game controls by function (the Recomp Settings > Controls screens).    */
/* Keyboard and mouse: each function takes up to four keys of the binding    */
/* table above. Gamepad: each function takes up to two physical buttons;    */
/* mkb_pad_translate turns them into the console buttons the game reads.     */
/* ======================================================================== */

enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_L3, PB_R3, PB_START, PB_BACK,
       PB_DUP, PB_DDOWN, PB_DLEFT, PB_DRIGHT };

static const char *const k_pb_label[MKB_PB_COUNT] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3 (left stick)", "R3 (right stick)", "Menu", "View",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right"
};
static const char *const k_pb_id[MKB_PB_COUNT] = {
    "a", "b", "x", "y", "lb", "rb", "lt", "rt", "l3", "r3", "start", "back",
    "dup", "ddown", "dleft", "dright"
};

/* ctl = the console control the game reads for it (-1: the sprint, which is not the game's) */
static const struct { const char *id, *label; int ctl; } k_act[MKB_ACT_COUNT] = {
    { "jump",       "Jump / Stand up",   MKC_Y },
    { "action",     "Action / Reload",   MKC_A },
    { "crouch",     "Crouch / Prone",    MKC_X },
    { "weapon",     "Change weapon",     MKC_B },
    { "adrenaline", "Activate adrenaline", MKC_BLACK },
    { "medkit",     "Use medkit",        MKC_WHITE },
    { "aim",        "Aim",               MKC_LT },
    { "fire",       "Fire",              MKC_RT },
    { "grenade",    "Grenade",           MKC_UP },
    { "recall",     "Recall squad",      MKC_DOWN },
    { "position",   "Position squad",    MKC_LS },
    { "melee",      "Melee",             MKC_RS },
    { "pause",      "Pause",             MKC_START },
    { "map",        "Mission map",       MKC_BACK },
    { "sprint",     "Sprint",            -1 },
};

static const signed char k_pad_default[MKB_ACT_COUNT][2] = {
    { PB_A, -1 }, { PB_X, PB_Y }, { PB_B, -1 }, { PB_DLEFT, -1 }, { PB_LB, -1 }, { PB_RB, -1 },
    { PB_LT, -1 }, { PB_RT, -1 }, { PB_DUP, -1 }, { PB_DDOWN, -1 }, { PB_DRIGHT, -1 }, { PB_R3, -1 },
    { PB_START, -1 }, { PB_BACK, -1 }, { PB_L3, -1 },
};

static signed char s_pad_act[MKB_ACT_COUNT][2];
static unsigned short s_bind_def[MKC_COUNT][4];
static unsigned short s_sprint_key_def;
static unsigned short s_move[4] = { 'W', 'S', 'A', 'D' };       /* forward, back, left, right */
static const unsigned short k_move_def[4] = { 'W', 'S', 'A', 'D' };
static int s_act_ready;

static void act_init(void)
{
    if (s_act_ready)
        return;
    s_act_ready = 1;
    memcpy(s_pad_act, k_pad_default, sizeof s_pad_act);
}

/* Remember the code's defaults: called before mkb.ini and the settings file are read. */
static void act_snapshot_defaults(void)
{
    memcpy(s_bind_def, s_bind, sizeof s_bind_def);
    s_sprint_key_def = s_sprint_key;
}

int mkb_action_count(void)                { return MKB_ACT_COUNT; }
const char *mkb_action_label(int a)       { return a >= 0 && a < MKB_ACT_COUNT ? k_act[a].label : ""; }
const char *mkb_pad_label(int pb)         { return pb >= 0 && pb < MKB_PB_COUNT ? k_pb_label[pb] : "-"; }

int mkb_action_keys(int a, unsigned short out[4])
{
    int n = 0, j;
    if (a < 0 || a >= MKB_ACT_COUNT)
        return 0;
    if (k_act[a].ctl < 0) {
        out[0] = s_sprint_key;
        return s_sprint_key ? 1 : 0;
    }
    for (j = 0; j < 4; j++)
        if (s_bind[k_act[a].ctl][j])
            out[n++] = s_bind[k_act[a].ctl][j];
    return n;
}

/* a code is taken away from every function (and the movement) but the one named */
static void act_unbind_code(unsigned code, int except_act, int except_move)
{
    int a, j, m;
    if (!code)
        return;
    for (a = 0; a < MKB_ACT_COUNT; a++) {
        if (a == except_act)
            continue;
        if (k_act[a].ctl < 0) {
            if (s_sprint_key == code)
                s_sprint_key = 0;
            continue;
        }
        for (j = 0; j < 4; j++)
            if (s_bind[k_act[a].ctl][j] == code) {
                int k;
                for (k = j; k < 3; k++)
                    s_bind[k_act[a].ctl][k] = s_bind[k_act[a].ctl][k + 1];
                s_bind[k_act[a].ctl][3] = 0;
                j--;
            }
    }
    for (m = 0; m < 4; m++)
        if (m != except_move && s_move[m] == code)
            s_move[m] = 0;
}

void mkb_action_set_keys(int a, const unsigned short *keys, int n)
{
    int j;
    if (a < 0 || a >= MKB_ACT_COUNT)
        return;
    for (j = 0; j < n && j < 4; j++)
        act_unbind_code(keys[j], a, -1);
    if (k_act[a].ctl < 0) {
        s_sprint_key = n > 0 ? keys[0] : 0;
        return;
    }
    memset(s_bind[k_act[a].ctl], 0, sizeof s_bind[0]);
    for (j = 0; j < n && j < 4; j++)
        s_bind[k_act[a].ctl][j] = keys[j];
}

unsigned short mkb_move_key(int d)        { return d >= 0 && d < 4 ? s_move[d] : 0; }

void mkb_set_move_key(int d, unsigned short code)
{
    if (d < 0 || d >= 4 || code >= 0x100)
        return;
    act_unbind_code(code, -1, d);
    s_move[d] = code;
}

int mkb_pad_buttons(int a, signed char pb[2])
{
    act_init();
    if (a < 0 || a >= MKB_ACT_COUNT) {
        pb[0] = pb[1] = -1;
        return 0;
    }
    pb[0] = s_pad_act[a][0];
    pb[1] = s_pad_act[a][1];
    return (pb[0] >= 0) + (pb[1] >= 0);
}

void mkb_action_set_pad(int a, const signed char pb[2])
{
    int o, k, j;
    act_init();
    if (a < 0 || a >= MKB_ACT_COUNT)
        return;
    for (j = 0; j < 2; j++) {
        if (pb[j] < 0)
            continue;
        for (o = 0; o < MKB_ACT_COUNT; o++)
            if (o != a)
                for (k = 0; k < 2; k++)
                    if (s_pad_act[o][k] == pb[j])
                        s_pad_act[o][k] = -1;
    }
    s_pad_act[a][0] = pb[0];
    s_pad_act[a][1] = pb[1];
    if (s_pad_act[a][0] < 0) {                   /* keep the used slot first */
        s_pad_act[a][0] = s_pad_act[a][1];
        s_pad_act[a][1] = -1;
    }
}

void mkb_bindings_reset(void)
{
    act_init();
    memcpy(s_bind, s_bind_def, sizeof s_bind);
    s_sprint_key = s_sprint_key_def;
    memcpy(s_move, k_move_def, sizeof s_move);
    memcpy(s_pad_act, k_pad_default, sizeof s_pad_act);
}

/* A key code -> the name mkb.ini reads back, and a name to show. */
static void key_id(unsigned code, char *out, size_t n)
{
    static const struct { const char *n; unsigned vk; } names[] = {
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "esc", VK_ESCAPE }, { "tab", VK_TAB },
        { "backspace", VK_BACK }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
        { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU },
        { "capslock", VK_CAPITAL }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT },
        { "right", VK_RIGHT }, { "insert", VK_INSERT }, { "delete", VK_DELETE }, { "home", VK_HOME },
        { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { NULL, 0 }
    };
    int i;
    if (code == MKB_WHEEL) { snprintf(out, n, "wheel"); return; }
    if (code >= MKB_MOUSE1 && code < MKB_WHEEL) { snprintf(out, n, "mouse%u", code - MKB_MOUSE1 + 1); return; }
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) { snprintf(out, n, "%c", (char)(code >= 'A' ? code + 32 : code)); return; }
    if (code >= VK_F1 && code <= VK_F12) { snprintf(out, n, "f%u", code - VK_F1 + 1); return; }
    for (i = 0; names[i].n; i++)
        if (names[i].vk == code) { snprintf(out, n, "%s", names[i].n); return; }
    snprintf(out, n, "vk%02x", code);
}

void mkb_key_label(unsigned code, char *out, size_t n)
{
    if (!code) { snprintf(out, n, "-"); return; }
    if (code == MKB_WHEEL) { snprintf(out, n, "Mouse wheel"); return; }
    if (code >= MKB_MOUSE1 && code < MKB_WHEEL) {
        static const char *const mn[5] = { "Left mouse", "Right mouse", "Middle mouse", "Mouse 4", "Mouse 5" };
        snprintf(out, n, "%s", mn[code - MKB_MOUSE1]);
        return;
    }
    switch (code) {
    case VK_LSHIFT: snprintf(out, n, "Left Shift"); return;
    case VK_RSHIFT: snprintf(out, n, "Right Shift"); return;
    case VK_LCONTROL: snprintf(out, n, "Left Ctrl"); return;
    case VK_RCONTROL: snprintf(out, n, "Right Ctrl"); return;
    case VK_LMENU: snprintf(out, n, "Left Alt"); return;
    case VK_RMENU: snprintf(out, n, "Right Alt"); return;
    case VK_SPACE: snprintf(out, n, "Space"); return;
    case VK_RETURN: snprintf(out, n, "Enter"); return;
    case VK_ESCAPE: snprintf(out, n, "Esc"); return;
    case VK_BACK: snprintf(out, n, "Backspace"); return;
    case VK_UP: snprintf(out, n, "Up arrow"); return;
    case VK_DOWN: snprintf(out, n, "Down arrow"); return;
    case VK_LEFT: snprintf(out, n, "Left arrow"); return;
    case VK_RIGHT: snprintf(out, n, "Right arrow"); return;
    }
    {
        LONG sc = (LONG)MapVirtualKeyA(code, MAPVK_VK_TO_VSC) << 16;
        if (code == VK_INSERT || code == VK_DELETE || code == VK_HOME || code == VK_END
            || code == VK_PRIOR || code == VK_NEXT)
            sc |= 1L << 24;
        if (sc && GetKeyNameTextA(sc, out, (int)n) > 0)
            return;
    }
    key_id(code, out, n);
}

/* the "kb_*", "pad_*" and "move" lines of recomp_settings.ini */
void mkb_bindings_write(void *fp)
{
    FILE *f = (FILE *)fp;
    int a, j;
    char id[24];
    unsigned short keys[4];
    act_init();
    for (a = 0; a < MKB_ACT_COUNT; a++) {
        int n = mkb_action_keys(a, keys);
        fprintf(f, "kb_%s=", k_act[a].id);
        for (j = 0; j < n; j++) {
            key_id(keys[j], id, sizeof id);
            fprintf(f, "%s%s", j ? "," : "", id);
        }
        if (!n)
            fprintf(f, "none");
        fprintf(f, "\n");
    }
    for (a = 0; a < MKB_ACT_COUNT; a++) {
        int first = 1;
        fprintf(f, "pad_%s=", k_act[a].id);
        for (j = 0; j < 2; j++)
            if (s_pad_act[a][j] >= 0) {
                fprintf(f, "%s%s", first ? "" : ",", k_pb_id[s_pad_act[a][j]]);
                first = 0;
            }
        if (first)
            fprintf(f, "none");
        fprintf(f, "\n");
    }
    fprintf(f, "move=");
    for (j = 0; j < 4; j++) {
        key_id(s_move[j], id, sizeof id);
        fprintf(f, "%s%s", j ? "," : "", s_move[j] ? id : "none");
    }
    fprintf(f, "\n");
}

/* one line of that file; 1 when it was one of ours */
static int mkb_bindings_line(const char *key, const char *val)
{
    int a, j;
    char buf[128], *tok, *ctx = NULL;

    act_init();
    if (!strcmp(key, "move")) {
        j = 0;
        snprintf(buf, sizeof buf, "%s", val);
        for (tok = strtok_s(buf, ",", &ctx); tok && j < 4; tok = strtok_s(NULL, ",", &ctx), j++) {
            unsigned c = mkb_key_code(tok);
            s_move[j] = c < 0x100 ? (unsigned short)c : 0;
        }
        return 1;
    }
    if (!strncmp(key, "kb_", 3)) {
        for (a = 0; a < MKB_ACT_COUNT; a++)
            if (!strcmp(key + 3, k_act[a].id)) {
                unsigned short ks[4];
                int n = 0;
                snprintf(buf, sizeof buf, "%s", val);
                for (tok = strtok_s(buf, ",", &ctx); tok && n < 4; tok = strtok_s(NULL, ",", &ctx)) {
                    unsigned c = mkb_key_code(tok);
                    if (c)
                        ks[n++] = (unsigned short)c;
                }
                if (k_act[a].ctl < 0) {
                    s_sprint_key = n ? ks[0] : 0;
                } else {
                    memset(s_bind[k_act[a].ctl], 0, sizeof s_bind[0]);
                    for (j = 0; j < n; j++)
                        s_bind[k_act[a].ctl][j] = ks[j];
                }
                return 1;
            }
        return 0;
    }
    if (!strncmp(key, "pad_", 4)) {
        for (a = 0; a < MKB_ACT_COUNT; a++)
            if (!strcmp(key + 4, k_act[a].id)) {
                int n = 0;
                s_pad_act[a][0] = s_pad_act[a][1] = -1;
                snprintf(buf, sizeof buf, "%s", val);
                for (tok = strtok_s(buf, ",", &ctx); tok && n < 2; tok = strtok_s(NULL, ",", &ctx))
                    for (j = 0; j < MKB_PB_COUNT; j++)
                        if (!strcmp(tok, k_pb_id[j])) {
                            s_pad_act[a][n++] = (signed char)j;
                            break;
                        }
                return 1;
            }
    }
    return 0;
}

/* The real pad's layout, never in a menu (the game reads a menu's pad as it is). Called on the
 * pad's own state BEFORE the keyboard is merged, so the keyboard keeps its own bindings. Each
 * function takes the physical button(s) it is bound to and shows up as the console button the
 * game reads for it. The sprint is not the game's: its button is hidden from the game and
 * toggles the pad's sprint (the game hook asks mkb_sprint_query). */
void mkb_pad_translate(XBOX_INPUT_STATE *st)
{
    static int prev_sprint;
    unsigned char ph[MKB_PB_COUNT];
    BYTE *an;
    WORD w;
    int a, k, sp = 0;
    DWORD now;

    if (!st || !mkb_enabled() || !s_thread_up)
        return;
    if (mkb_in_menu()) {
        s_pad_l3 = 0;
        prev_sprint = 0;
        return;
    }
    act_init();
    an = st->Gamepad.bAnalogButtons;
    w = st->Gamepad.wButtons;
    ph[PB_A] = an[XBOX_BUTTON_A];  ph[PB_B] = an[XBOX_BUTTON_B];
    ph[PB_X] = an[XBOX_BUTTON_X];  ph[PB_Y] = an[XBOX_BUTTON_Y];
    ph[PB_LB] = an[XBOX_BUTTON_BLACK];  ph[PB_RB] = an[XBOX_BUTTON_WHITE];
    ph[PB_LT] = an[XBOX_BUTTON_LTRIGGER];  ph[PB_RT] = an[XBOX_BUTTON_RTRIGGER];
    ph[PB_L3] = (w & XBOX_GAMEPAD_LEFT_THUMB) ? 255 : 0;
    ph[PB_R3] = (w & XBOX_GAMEPAD_RIGHT_THUMB) ? 255 : 0;
    ph[PB_START] = (w & XBOX_GAMEPAD_START) ? 255 : 0;
    ph[PB_BACK] = (w & XBOX_GAMEPAD_BACK) ? 255 : 0;
    ph[PB_DUP] = (w & XBOX_GAMEPAD_DPAD_UP) ? 255 : 0;
    ph[PB_DDOWN] = (w & XBOX_GAMEPAD_DPAD_DOWN) ? 255 : 0;
    ph[PB_DLEFT] = (w & XBOX_GAMEPAD_DPAD_LEFT) ? 255 : 0;
    ph[PB_DRIGHT] = (w & XBOX_GAMEPAD_DPAD_RIGHT) ? 255 : 0;

    st->Gamepad.wButtons &= (WORD)~0x00FF;
    memset(an, 0, 8);
    for (a = 0; a < MKB_ACT_COUNT; a++) {
        unsigned char val = 0;
        int ctl = k_act[a].ctl;
        for (k = 0; k < 2; k++)
            if (s_pad_act[a][k] >= 0 && ph[s_pad_act[a][k]] > val)
                val = ph[s_pad_act[a][k]];
        if (!val)
            continue;
        if (ctl < 0) {
            sp = 1;
        } else if (s_ctl[ctl].analog >= 0) {
            if (val > an[s_ctl[ctl].analog])
                an[s_ctl[ctl].analog] = val;
        } else if (val > 100) {
            st->Gamepad.wButtons |= s_ctl[ctl].mask;
        }
    }
    now = GetTickCount();
    if (sp && !prev_sprint) {                    /* a press toggles the pad's sprint */
        s_pad_l3 = !s_pad_l3;
        s_pad_seen_move = 0;
        s_pad_tick = (LONG)now;
    }
    prev_sprint = sp;
    if (s_pad_l3 && (LONG)(now - (DWORD)s_pad_tick) > 1500)
        s_pad_l3 = 0;                            /* the move update stopped asking */
}

/* ======================================================================== */
/* Button prompts: which device is in use, and what each prompt should show  */
/* (icon_swap.c redraws the game's icons from the view published here).      */
/* ======================================================================== */

static volatile LONG s_ic_kb_tick, s_ic_pad_tick;   /* GetTickCount of the last real input, 0 = never */

int  mkb_get_icon_mode(void)         { return s_icon_mode; }
void mkb_set_icon_mode(int m)        { s_icon_mode = m < 0 || m > 2 ? 0 : m; }
int  mkb_get_icon_style(void)        { return s_icon_style; }
void mkb_set_icon_style(int s)       { s_icon_style = s ? 1 : 0; }

/* The keyboard or the mouse did something (raw-input thread); only while the game is in front. */
static void ic_note_kb(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 40u)
        return;
    last = now;
    if (!game_is_foreground())
        return;
    s_ic_kb_tick = (LONG)(now | 1u);
}

/* 1 when the pad was the last device used (real input newer than the keyboard's and mouse's). */
static int ic_pad_is_last(void)
{
    LONG kt = s_ic_kb_tick, pt = s_ic_pad_tick;
    return pt && (!kt || (LONG)(pt - kt) > 0);
}

/* Mouse movement counts as "the player is on the mouse" once enough of it adds up (a 1000 Hz mouse
 * reports one or two counts per event, and a hand resting on it jitters). */
static void ic_note_mouse(LONG dx, LONG dy)
{
    static DWORD t0;
    static LONG sum;
    DWORD now = GetTickCount();
    if (now - t0 > 250u) {
        t0 = now;
        sum = 0;
    }
    sum += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (sum >= 30) {
        sum = 0;
        ic_note_kb();
    }
}

/* The pad as the hardware reported it (before any remapping): a press, a trigger, or a stick well
 * past its dead zone is "the player is using the pad"; drift is not. */
void mkb_note_pad(const XBOX_INPUT_STATE *raw)
{
    const BYTE *an;
    int i;
    int on;

    if (!raw)
        return;
    an = raw->Gamepad.bAnalogButtons;
    on = (raw->Gamepad.wButtons & 0x00FF) != 0
         || raw->Gamepad.sThumbLX > 20000 || raw->Gamepad.sThumbLX < -20000
         || raw->Gamepad.sThumbLY > 20000 || raw->Gamepad.sThumbLY < -20000
         || raw->Gamepad.sThumbRX > 20000 || raw->Gamepad.sThumbRX < -20000
         || raw->Gamepad.sThumbRY > 20000 || raw->Gamepad.sThumbRY < -20000;
    for (i = 0; i < 8 && !on; i++)
        on = an[i] > 100;
    if (on)
        s_ic_pad_tick = (LONG)(GetTickCount() | 1u);
}

/* the keycap text for a bound key or mouse button (icon_swap.c draws a mouse for "\1L" and so on, and
 * the arrows for the control characters 0x18 0x19 0x1b 0x1a) */
static void ic_key_text(unsigned code, char *out)
{
    out[0] = 0;
    if (!code)
        return;
    if (code >= MKB_MOUSE1 && code < MKB_WHEEL) {
        static const char k_m[5] = { 'L', 'R', 'M', '4', '5' };
        out[0] = 1;
        out[1] = k_m[code - MKB_MOUSE1];
        out[2] = 0;
        return;
    }
    if (code == MKB_WHEEL) { snprintf(out, IC_KEYLEN, "Wheel"); return; }
    switch (code) {
    case VK_LSHIFT: case VK_RSHIFT: snprintf(out, IC_KEYLEN, "Shift"); return;
    case VK_LCONTROL: case VK_RCONTROL: snprintf(out, IC_KEYLEN, "Ctrl"); return;
    case VK_LMENU: case VK_RMENU: snprintf(out, IC_KEYLEN, "Alt"); return;
    case VK_SPACE: snprintf(out, IC_KEYLEN, "Space"); return;
    case VK_RETURN: snprintf(out, IC_KEYLEN, "Enter"); return;
    case VK_ESCAPE: snprintf(out, IC_KEYLEN, "Esc"); return;
    case VK_BACK: snprintf(out, IC_KEYLEN, "Bksp"); return;
    case VK_TAB: snprintf(out, IC_KEYLEN, "Tab"); return;
    case VK_CAPITAL: snprintf(out, IC_KEYLEN, "Caps"); return;
    case VK_UP: out[0] = 0x18; out[1] = 0; return;
    case VK_DOWN: out[0] = 0x19; out[1] = 0; return;
    case VK_LEFT: out[0] = 0x1b; out[1] = 0; return;
    case VK_RIGHT: out[0] = 0x1a; out[1] = 0; return;
    case VK_INSERT: snprintf(out, IC_KEYLEN, "Ins"); return;
    case VK_DELETE: snprintf(out, IC_KEYLEN, "Del"); return;
    case VK_HOME: snprintf(out, IC_KEYLEN, "Home"); return;
    case VK_END: snprintf(out, IC_KEYLEN, "End"); return;
    case VK_PRIOR: snprintf(out, IC_KEYLEN, "PgUp"); return;
    case VK_NEXT: snprintf(out, IC_KEYLEN, "PgDn"); return;
    }
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) {
        out[0] = (char)code;
        out[1] = 0;
        return;
    }
    if (code >= VK_F1 && code <= VK_F12) {
        snprintf(out, IC_KEYLEN, "F%u", code - VK_F1 + 1);
        return;
    }
    {
        char lab[32];
        mkb_key_label(code, lab, sizeof lab);
        snprintf(out, IC_KEYLEN, "%.4s", lab);
    }
}

/* Called as the game polls port 0 (any thread of the game's): about ten times a second it works
 * out the device, and when what a prompt should show changed it publishes the new view. */
void mkb_icons_poll(void)
{
    static const signed char k_mkc[IC_COUNT] = {
        MKC_A, MKC_B, MKC_X, MKC_Y, MKC_BLACK, MKC_WHITE, MKC_LT, MKC_RT,
        MKC_LS, MKC_RS, MKC_START, MKC_BACK, MKC_UP, MKC_DOWN, MKC_LEFT, MKC_RIGHT };
    static DWORD last_chk, flip_tick;
    static int dev;                           /* the device the prompts follow (auto) */
    static IconView prev;
    static int have_prev;
    IconView v;
    DWORD now = GetTickCount();
    int ic, in_menu;

    if (now - last_chk < 100u || !mkb_enabled() || !s_thread_up)
        return;
    last_chk = now;
    act_init();

    if (s_icon_mode == 1) {
        dev = 1;
    } else if (s_icon_mode == 2) {
        dev = 0;
    } else {
        LONG kt = s_ic_kb_tick, pt = s_ic_pad_tick;
        int want = (pt && (!kt || (LONG)(pt - kt) > 0)) ? 1 : 0;
        if (want != dev && now - flip_tick > 800u) {
            dev = want;
            flip_tick = now;
        }
    }

    in_menu = mkb_in_menu();
    memset(&v, 0, sizeof v);
    v.device = dev;
    v.style = s_icon_style;
    for (ic = 0; ic < IC_COUNT; ic++) {
        int mkc = k_mkc[ic], a;
        v.pad[ic] = -1;
        if (in_menu) {
            v.pad[ic] = (signed char)ic;                     /* a menu reads the pad as it is */
        } else {
            for (a = 0; a < MKB_ACT_COUNT; a++)
                if (k_act[a].ctl == mkc) {
                    v.pad[ic] = s_pad_act[a][0];
                    break;
                }
        }
        /* the first key the game's control answers to, as the merge in mkb_merge reads its tables */
        {
            const unsigned short *keys = in_menu && s_bind_menu[mkc][0] ? s_bind_menu[mkc] : s_bind[mkc];
            unsigned mouse = 0, key = 0;
            int j;
            for (j = 0; j < 4 && keys[j]; j++) {
                unsigned code = keys[j];
                if (in_menu && keys == s_bind[mkc] && mkb_menu_key(code))
                    continue;
                if (in_menu && (code == VK_UP || code == VK_DOWN || code == VK_LEFT || code == VK_RIGHT)
                    && mkc != MKC_UP && mkc != MKC_DOWN && mkc != MKC_LEFT && mkc != MKC_RIGHT)
                    continue;
                if (code < 0x100) { key = code; break; }
                if (!mouse)
                    mouse = code;
            }
            ic_key_text(key ? key : mouse, v.key[ic]);
        }
    }
    if (!have_prev || memcmp(&prev, &v, sizeof v)) {
        prev = v;
        have_prev = 1;
        icon_swap_publish(&v);
    }
}
