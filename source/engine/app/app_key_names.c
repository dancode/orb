/*==============================================================================================

    engine/app/app_key_names.c

    Canonical key-name table, index-matched to app_key_t.  The bind system (core/cmd) is a
    layer below app and cannot see the enum, so the host wires this table into it at boot:
        cmd_bind_wire_names( app_key_names(), APP_KEY_COUNT );
    Names are lowercase and stable -- they appear in config files ("bind f5 quicksave").

==============================================================================================*/

// clang-format off
static const char* const s_app_key_names[] =
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
};
// clang-format on

ORB_STATIC_ASSERT( sizeof( s_app_key_names ) / sizeof( s_app_key_names[ 0 ] ) == APP_KEY_COUNT,
                   "key name table must match app_key_t" );

const char* const*
app_key_names( void )
{
    return s_app_key_names;
}

/*============================================================================================*/
