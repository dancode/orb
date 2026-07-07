/*==============================================================================================

    engine/app/app_key_names.c

    Canonical name table for the unified digital source space (app_src_t): keyboard keys
    index-matched to app_key_t, then mouse buttons / wheel pulses / gamepad buttons at their
    APP_SRC_* codes.  The bind system (core/cmd) is a layer below app and cannot see the
    enums, so the host wires this table into it at boot:
        cmd_bind_wire_names( app_key_names(), APP_SRC_COUNT );
    Names are lowercase and stable -- they appear in config files ("bind pad_a +jump").
    Gap indexes between blocks stay NULL and read/write as bare numbers.

==============================================================================================*/

// clang-format off
static const char* const s_app_key_names[ APP_SRC_COUNT ] =
{
    "none",

    /* Letters */
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",

    /* Row digits */
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",

    /* Function keys */
    "f1", "f2", "f3",  "f4",  "f5",  "f6",
    "f7", "f8", "f9", "f10", "f11", "f12",

    /* Control */
    "escape", "enter", "space", "tab", "backspace",

    /* Arrow keys */
    "left", "right", "up", "down",

    /* Navigation */
    "insert", "delete", "home", "end", "pageup", "pagedown",

    /* Modifiers */
    "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "lsuper", "rsuper",

    /* Lock keys */
    "capslock", "numlock", "scrolllock",

    /* Numpad */
    "kp_0", "kp_1", "kp_2", "kp_3", "kp_4",
    "kp_5", "kp_6", "kp_7", "kp_8", "kp_9",
    "kp_enter", "kp_dot", "kp_add", "kp_sub", "kp_mul", "kp_div",

    /* Symbol / punctuation (US layout) */
    "grave", "minus", "equal", "lbracket", "rbracket", "backslash",
    "semicolon", "apostrophe", "comma", "period", "slash",

    /* System */
    "pause", "printscreen", "menu",

    /* Mouse -- unified source space (designated: table indexes must match app_src_t) */
    [APP_SRC_MOUSE1]    = "mouse1",
    [APP_SRC_MOUSE2]    = "mouse2",
    [APP_SRC_MOUSE3]    = "mouse3",
    [APP_SRC_MOUSE4]    = "mouse4",
    [APP_SRC_MOUSE5]    = "mouse5",
    [APP_SRC_WHEELUP]   = "wheelup",
    [APP_SRC_WHEELDOWN] = "wheeldown",

    /* Gamepad */
    [APP_SRC_PAD_A]          = "pad_a",
    [APP_SRC_PAD_B]          = "pad_b",
    [APP_SRC_PAD_X]          = "pad_x",
    [APP_SRC_PAD_Y]          = "pad_y",
    [APP_SRC_PAD_LB]         = "pad_lb",
    [APP_SRC_PAD_RB]         = "pad_rb",
    [APP_SRC_PAD_BACK]       = "pad_back",
    [APP_SRC_PAD_START]      = "pad_start",
    [APP_SRC_PAD_LS]         = "pad_ls",
    [APP_SRC_PAD_RS]         = "pad_rs",
    [APP_SRC_PAD_DPAD_UP]    = "pad_up",
    [APP_SRC_PAD_DPAD_DOWN]  = "pad_down",
    [APP_SRC_PAD_DPAD_LEFT]  = "pad_left",
    [APP_SRC_PAD_DPAD_RIGHT] = "pad_right",
    [APP_SRC_PAD_LTRIGGER]   = "pad_ltrigger",
    [APP_SRC_PAD_RTRIGGER]   = "pad_rtrigger",
};
// clang-format on

const char* const*
app_key_names( void )
{
    return s_app_key_names;
}

/*============================================================================================*/
