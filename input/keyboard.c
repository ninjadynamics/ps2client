/* Keyboard input module for ps2client --input keyboard (ps2link P4).
 *
 * Reads the Windows keyboard state (focus-independent) and reports one full
 * PS2 controller state per poll. Every key is sampled independently, so any
 * combination held together is reported (up to the keyboard's own rollover):
 *   arrows        D-pad
 *   W A S D       left stick (full deflection)
 *   I J K L       right stick (full deflection)
 *   1 2 3         L1 L2 L3
 *   8 9 0         R1 R2 R3
 *   Z X , .       square cross circle triangle ("," and "." are the < > keys)
 *   Enter         START
 *   Backspace     SELECT
 */
#include <string.h>
#include "ps2link-input.h"

#ifdef _WIN32
#include <windows.h>

static int down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static uint8_t axis(int negative, int positive)
{
    const int n = down(negative), p = down(positive);
    if (n == p) return PS2_PAD_AXIS_CENTRE;
    return n ? 0x00 : 0xff;
}

static int keyboard_start(void)
{
    return 0;
}

static int keyboard_poll(ps2_input_state_t *state)
{
    static const struct { int vk; uint16_t bit; } map[] = {
        { VK_UP,    PS2_PAD_UP },     { VK_DOWN,  PS2_PAD_DOWN },
        { VK_LEFT,  PS2_PAD_LEFT },   { VK_RIGHT, PS2_PAD_RIGHT },
        { '1', PS2_PAD_L1 }, { '2', PS2_PAD_L2 }, { '3', PS2_PAD_L3 },
        { '8', PS2_PAD_R1 }, { '9', PS2_PAD_R2 }, { '0', PS2_PAD_R3 },
        { 'Z', PS2_PAD_SQUARE },           { 'X', PS2_PAD_CROSS },
        { VK_OEM_COMMA, PS2_PAD_CIRCLE },  { VK_OEM_PERIOD, PS2_PAD_TRIANGLE },
        { VK_RETURN, PS2_PAD_START },      { VK_BACK, PS2_PAD_SELECT },
    };
    uint16_t buttons = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (down(map[i].vk)) buttons |= map[i].bit;
    state->buttons = buttons;
    state->l2 = (buttons & PS2_PAD_L2) ? 0xff : 0x00;   /* keys are full-pressure triggers */
    state->r2 = (buttons & PS2_PAD_R2) ? 0xff : 0x00;
    state->lx = axis('A', 'D');
    state->ly = axis('W', 'S');
    state->rx = axis('J', 'L');
    state->ry = axis('I', 'K');
    return 0;
}

static void keyboard_stop(void)
{
}

static const ps2_input_module_v1_t keyboard_module = {
    PS2_INPUT_MODULE_ABI_V1,
    sizeof(ps2_input_module_v1_t),
    "ps2client keyboard (arrows/WASD/IJKL/123/890/ZX,./Enter/Backspace)",
    keyboard_start,
    keyboard_poll,
    keyboard_stop,
};

__declspec(dllexport) const ps2_input_module_v1_t *ps2_input_module_get_v1(void)
{
    return &keyboard_module;
}
#endif
