/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include <stdio.h>
#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdlib.h>
#include <string.h>

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

/* Split-screen pads (RECOMP_SPLIT_PADS=1): the keyboard and mouse are player 1 alone (game port 0), and the physical pads
 * are shifted up one port: pad 1 is player 2 (game port 1), pad 2 is player 3, pad 3 is player 4. Without this the pad is
 * merged with the mouse on port 0 and a second player cannot have a pad of their own. */
static int split_pads(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPLIT_PADS");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}
/* the XInput index behind a game port, or -1 when no physical pad stands behind it */
static int xi_index(DWORD dwPort)
{
    if (!split_pads())
        return (int)dwPort;
    return dwPort == 0 ? -1 : (int)dwPort - 1;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        int xi = xi_index(i);
        DWORD result = xi < 0 ? ERROR_DEVICE_NOT_CONNECTED : XInputGetState((DWORD)xi, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

volatile int g_pad_rt_down;   /* the right trigger of port 0 as the game last read it (mygame RNG diagnostics) */

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState);

extern volatile int g_recomp_rt_last, g_recomp_input_log, g_recomp_dup_down;
extern volatile unsigned g_recomp_dup_press, g_recomp_b_press, g_recomp_a_press;
void (*g_xbox_input_filter)(XBOX_INPUT_STATE *pState);
volatile long g_input_block;                 /* the settings overlay owns the input */
volatile unsigned long g_input_inject_b_until;

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD r = input_get_state(dwPort, pState);
    if (dwPort == 0)
        mkb_icons_poll();
    if (dwPort == 0 && pState && r == ERROR_SUCCESS) {
        if (g_xbox_input_filter && !g_input_block)
            g_xbox_input_filter(pState);
        if (g_input_block) {
            memset(&pState->Gamepad, 0, sizeof pState->Gamepad);
        } else if ((long)(g_input_inject_b_until - GetTickCount()) > 0) {
            pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
            pState->dwPacketNumber++;
        }
    }
    if (dwPort == 0 && pState && r == ERROR_SUCCESS) {      /* what the game is finally given: read by the grenade countdown */
        static int bprev, rtprev;
        static unsigned short wprev;
        static unsigned char aprev[8];
        int d = (pState->Gamepad.wButtons & 1) != 0, b = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] > 40, k;
        g_recomp_rt_last = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
        if (d && !g_recomp_dup_down)
            g_recomp_dup_press++;
        g_recomp_dup_down = d;
        if (b && !bprev)
            g_recomp_b_press++;
        bprev = b;
        {
            static int aprev2;
            int a = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] > 40;
            if (a && !aprev2)
                g_recomp_a_press++;
            aprev2 = a;
        }
        (void)rtprev;
        if (g_recomp_input_log) {
            if (pState->Gamepad.wButtons != wprev)
                fprintf(stderr, "[GRENTIMER] input wButtons %04X -> %04X\n", wprev, pState->Gamepad.wButtons);
            wprev = pState->Gamepad.wButtons;
            for (k = 0; k < 8; k++) {
                unsigned char v = pState->Gamepad.bAnalogButtons[k] > 40;
                if (v != aprev[k])
                    fprintf(stderr, "[GRENTIMER] input analog %d %s\n", k, v ? "down" : "up");
                aprev[k] = v;
            }
        }
    }
    if (dwPort == 0 && pState)
        g_pad_rt_down = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] > 100;
    return r;
}

static DWORD input_get_state_impl(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    {
        int xi = xi_index(dwPort);
        result = xi < 0 ? ERROR_DEVICE_NOT_CONNECTED : XInputGetState((DWORD)xi, &xi_state);
    }
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0) {
        mkb_note_pad(pState);
        mkb_pad_translate(pState);
    }

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0) {
        mkb_merge(dwPort, pState);
    }

    return ERROR_SUCCESS;
}

/* the final fire (right trigger) value the game was given on port 0, after the pad and keyboard/mouse merge: the grenade
 * countdown reads it to know when the pin is pulled */
volatile int g_recomp_rt_last;
volatile int g_recomp_input_log = 0;
volatile int g_recomp_dup_down;                 /* D-pad up (the grenade control) held, final packet */
volatile unsigned g_recomp_dup_press;           /* counts its presses */
volatile unsigned g_recomp_b_press;
volatile unsigned g_recomp_a_press;             /* counts presses of A (Action / Reload: pulls the grenade pin) */             /* counts presses of B (change weapon) */

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get_state_impl(dwPort, pState);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    {
        int xi = xi_index(dwPort);
        if (xi < 0)
            return ERROR_DEVICE_NOT_CONNECTED;
        return XInputSetState((DWORD)xi, &xi_vib);
    }
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    {
        int xi = xi_index(dwPort);
        result = xi < 0 ? ERROR_DEVICE_NOT_CONNECTED : XInputGetCapabilities((DWORD)xi, dwFlags, &xi_caps);
    }
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static volatile int g_main_pump;

void xbox_InputSetMainPump(int on)
{
    g_main_pump = on;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            return 1;
    return 0;
}

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!g_main_pump)
        SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    if (dwPort == 0)
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
