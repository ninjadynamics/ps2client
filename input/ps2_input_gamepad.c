/* Gamepad input module for ps2client --input gamepad (ps2link P4).
 *
 * Any controller SDL3 knows (DualSense and DualShock over USB or Bluetooth,
 * Xbox, Switch Pro, generic HID with a mapping) is reported as a PS2 pad by
 * button position, so the Xbox A button is CROSS:
 *   D-pad            D-pad
 *   left/right stick lx ly / rx ry, clicks L3 / R3
 *   shoulders        L1 R1
 *   triggers         analog l2 r2; the L2/R2 bits past half travel
 *   south east west north   cross circle square triangle
 *   start / back (Create)   START / SELECT
 * SDL is linked statically and owned by the poll thread: it starts on the
 * first poll, and controllers are followed across hot-plugging. The first
 * connected gamepad drives the console; its removal promotes the next.
 */
#include <stdio.h>
#include <string.h>
#include "ps2link-input.h"

#ifdef _WIN32
#include <SDL3/SDL.h>

#define TRIGGER_BUTTON_THRESHOLD 0x80u   /* of 0xff */

static SDL_Gamepad *pad;
static int sdl_ready;

static uint8_t stick(Sint16 v)
{
    return (uint8_t)(((int)v + 32768) >> 8);   /* 0x80 = centre */
}

static uint8_t trigger(Sint16 v)
{
    return v > 0 ? (uint8_t)(((int)v * 255 + 16383) / 32767) : 0;
}

static void open_first(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count && !pad; i++)
        pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (pad)
        fprintf(stderr, "ps2-input-gamepad: using %s\n", SDL_GetGamepadName(pad));
}

static int gamepad_start(void)
{
    return 0;
}

static int gamepad_poll(ps2_input_state_t *state)
{
    static const struct { SDL_GamepadButton sdl; uint16_t bit; } map[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP,    PS2_PAD_UP },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN,  PS2_PAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT,  PS2_PAD_LEFT },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PS2_PAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_SOUTH, PS2_PAD_CROSS },
        { SDL_GAMEPAD_BUTTON_EAST,  PS2_PAD_CIRCLE },
        { SDL_GAMEPAD_BUTTON_WEST,  PS2_PAD_SQUARE },
        { SDL_GAMEPAD_BUTTON_NORTH, PS2_PAD_TRIANGLE },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  PS2_PAD_L1 },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PS2_PAD_R1 },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK,  PS2_PAD_L3 },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, PS2_PAD_R3 },
        { SDL_GAMEPAD_BUTTON_START, PS2_PAD_START },
        { SDL_GAMEPAD_BUTTON_BACK,  PS2_PAD_SELECT },
    };
    SDL_Event event;

    if (!sdl_ready) {
        /* No window: controllers must report while another app has focus. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "ps2-input-gamepad: SDL_Init failed: %s\n", SDL_GetError());
            return -1;
        }
        sdl_ready = 1;
        open_first();
        if (!pad)
            fprintf(stderr, "ps2-input-gamepad: waiting for a controller\n");
    }

    /* Pumps the controllers and reports hot-plugging. */
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED && pad &&
            event.gdevice.which == SDL_GetGamepadID(pad)) {
            fprintf(stderr, "ps2-input-gamepad: %s disconnected\n", SDL_GetGamepadName(pad));
            SDL_CloseGamepad(pad);
            pad = NULL;
            open_first();
        } else if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
            open_first();
        }
    }
    if (!pad)
        return 0;   /* the caller's released state */

    uint16_t buttons = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (SDL_GetGamepadButton(pad, map[i].sdl))
            buttons |= map[i].bit;
    state->l2 = trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state->r2 = trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (state->l2 >= TRIGGER_BUTTON_THRESHOLD)
        buttons |= PS2_PAD_L2;
    if (state->r2 >= TRIGGER_BUTTON_THRESHOLD)
        buttons |= PS2_PAD_R2;
    state->buttons = buttons;
    state->lx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    state->ly = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    state->rx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    state->ry = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    return 0;
}

/* Called after the poll thread has been joined. */
static void gamepad_stop(void)
{
    if (pad)
        SDL_CloseGamepad(pad);
    pad = NULL;
    if (sdl_ready)
        SDL_Quit();
    sdl_ready = 0;
}

static const ps2_input_module_v1_t gamepad_module = {
    PS2_INPUT_MODULE_ABI_V1,
    sizeof(ps2_input_module_v1_t),
    "ps2client gamepad (SDL3: DualSense, DualShock, Xbox and other controllers)",
    gamepad_start,
    gamepad_poll,
    gamepad_stop,
};

__declspec(dllexport) const ps2_input_module_v1_t *ps2_input_module_get_v1(void)
{
    return &gamepad_module;
}
#endif
