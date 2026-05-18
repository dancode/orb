/*==============================================================================================

    sandbox/sb_engine_app.c -- Menu-driven feature tests for the engine/app layer.

    Start by selecting a test from the numbered menu (1-9, 0 = test 10).
    Each mode focuses on one feature, prints targeted diagnostic output, and
    tells you exactly what to try and what correct output looks like.

    ESC returns to the menu from any test.  Q quits from anywhere.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys.h"
#include "engine/app/app.h"

/* clang-format off */
/*==============================================================================================
    Pretty-printers
==============================================================================================*/

static const char*
event_type_name( i32 t )
{
    switch ( t )
    {
        case APP_EV_KEY_DOWN:    return "KEY_DOWN";
        case APP_EV_KEY_UP:      return "KEY_UP";
        case APP_EV_CHAR:        return "CHAR";
        case APP_EV_MOUSE_MOVE:  return "MOUSE_MOVE";
        case APP_EV_MOUSE_DOWN:  return "MOUSE_DOWN";
        case APP_EV_MOUSE_UP:    return "MOUSE_UP";
        case APP_EV_MOUSE_WHEEL: return "MOUSE_WHEEL";
        case APP_EV_WIN_FOCUS:   return "WIN_FOCUS";
        case APP_EV_WIN_BLUR:    return "WIN_BLUR";
        case APP_EV_WIN_RESIZE:  return "WIN_RESIZE";
        case APP_EV_WIN_CLOSE:   return "WIN_CLOSE";
        case APP_EV_QUIT:        return "QUIT";
        default:                 return "NONE";
    }
}

static const char*
mouse_button_name( i32 b )
{
    switch ( b )
    {
        case APP_MOUSE_LEFT:   return "LEFT";
        case APP_MOUSE_RIGHT:  return "RIGHT";
        case APP_MOUSE_MIDDLE: return "MIDDLE";
        case APP_MOUSE_X1:     return "X1";
        case APP_MOUSE_X2:     return "X2";
        default:               return "?";
    }
}

static const char*
key_name( i32 k )
{
    static char buf[ 16 ];

    if ( k >= APP_KEY_A    && k <= APP_KEY_Z    ) { buf[ 0 ] = (char)( 'A' + ( k - APP_KEY_A ) ); buf[ 1 ] = 0; return buf; }
    if ( k >= APP_KEY_0    && k <= APP_KEY_9    ) { buf[ 0 ] = (char)( '0' + ( k - APP_KEY_0 ) ); buf[ 1 ] = 0; return buf; }
    if ( k >= APP_KEY_F1   && k <= APP_KEY_F12  ) { snprintf( buf, sizeof buf, "F%d",  1 + ( k - APP_KEY_F1   ) ); return buf; }
    if ( k >= APP_KEY_NP_0 && k <= APP_KEY_NP_9 ) { snprintf( buf, sizeof buf, "NP%d",     ( k - APP_KEY_NP_0 ) ); return buf; }

    switch ( k )
    {
        case APP_KEY_ESCAPE:       return "ESCAPE";
        case APP_KEY_ENTER:        return "ENTER";
        case APP_KEY_SPACE:        return "SPACE";
        case APP_KEY_TAB:          return "TAB";
        case APP_KEY_BACKSPACE:    return "BACKSPACE";
        case APP_KEY_LEFT:         return "LEFT";
        case APP_KEY_RIGHT:        return "RIGHT";
        case APP_KEY_UP:           return "UP";
        case APP_KEY_DOWN:         return "DOWN";
        case APP_KEY_INSERT:       return "INSERT";
        case APP_KEY_DELETE:       return "DELETE";
        case APP_KEY_HOME:         return "HOME";
        case APP_KEY_END:          return "END";
        case APP_KEY_PAGE_UP:      return "PAGE_UP";
        case APP_KEY_PAGE_DOWN:    return "PAGE_DOWN";
        case APP_KEY_LSHIFT:       return "LSHIFT";
        case APP_KEY_RSHIFT:       return "RSHIFT";
        case APP_KEY_LCTRL:        return "LCTRL";
        case APP_KEY_RCTRL:        return "RCTRL";
        case APP_KEY_LALT:         return "LALT";
        case APP_KEY_RALT:         return "RALT";
        case APP_KEY_LSUPER:       return "LSUPER";
        case APP_KEY_RSUPER:       return "RSUPER";
        case APP_KEY_CAPS_LOCK:    return "CAPS_LOCK";
        case APP_KEY_NUM_LOCK:     return "NUM_LOCK";
        case APP_KEY_SCROLL_LOCK:  return "SCROLL_LOCK";
        case APP_KEY_NP_ENTER:     return "NP_ENTER";
        case APP_KEY_NP_DOT:       return "NP_DOT";
        case APP_KEY_NP_ADD:       return "NP_ADD";
        case APP_KEY_NP_SUB:       return "NP_SUB";
        case APP_KEY_NP_MUL:       return "NP_MUL";
        case APP_KEY_NP_DIV:       return "NP_DIV";
        case APP_KEY_GRAVE:        return "GRAVE";
        case APP_KEY_MINUS:        return "MINUS";
        case APP_KEY_EQUAL:        return "EQUAL";
        case APP_KEY_LBRACKET:     return "LBRACKET";
        case APP_KEY_RBRACKET:     return "RBRACKET";
        case APP_KEY_BACKSLASH:    return "BACKSLASH";
        case APP_KEY_SEMICOLON:    return "SEMICOLON";
        case APP_KEY_APOSTROPHE:   return "APOSTROPHE";
        case APP_KEY_COMMA:        return "COMMA";
        case APP_KEY_PERIOD:       return "PERIOD";
        case APP_KEY_SLASH:        return "SLASH";
        case APP_KEY_PAUSE:        return "PAUSE";
        case APP_KEY_PRINT_SCREEN: return "PRINT_SCREEN";
        case APP_KEY_MENU:         return "MENU";
        default:                   return "NONE";
    }
}

static void
print_mod( app_mod_t m )
{
    if ( !m.bits )
        return;
    const char* sep = " [";
    if ( m.ctrl  ) { printf( "%sctrl",  sep ); sep = "+"; }
    if ( m.shift ) { printf( "%sshift", sep ); sep = "+"; }
    if ( m.alt   ) { printf( "%salt",   sep ); sep = "+"; }
    if ( m.super ) { printf( "%ssuper", sep ); sep = "+"; }
    printf( "]" );
}

static void
print_event( const app_event_t* ev )
{
    printf( "[ev #%-4d win=%d %-11s]", ev->event_id, ev->win_id, event_type_name( ev->type ) );

    switch ( ev->type )
    {
        case APP_EV_KEY_DOWN:
        case APP_EV_KEY_UP:
            printf( " key=%s", key_name( ev->data.key.key ) );
            print_mod( ev->data.key.mod );
            break;

        case APP_EV_CHAR:
            if ( ev->data.text.codepoint >= 0x20 && ev->data.text.codepoint < 0x7F )
                printf( " '%c' (U+%04X)", (char)ev->data.text.codepoint, ev->data.text.codepoint );
            else
                printf( " (U+%04X)", ev->data.text.codepoint );
            print_mod( ev->data.text.mod );
            break;

        case APP_EV_MOUSE_MOVE:
            printf( " xy=(%d,%d) d=(%d,%d)", ev->data.mouse_move.x, ev->data.mouse_move.y,
                    ev->data.mouse_move.dx, ev->data.mouse_move.dy );
            break;

        case APP_EV_MOUSE_DOWN:
        case APP_EV_MOUSE_UP:
            printf( " btn=%s xy=(%d,%d)", mouse_button_name( ev->data.mouse_btn.button ),
                    ev->data.mouse_btn.x, ev->data.mouse_btn.y );
            break;

        case APP_EV_MOUSE_WHEEL:
            printf( " delta=%+d xy=(%d,%d)", ev->data.mouse_wheel.delta,
                    ev->data.mouse_wheel.x, ev->data.mouse_wheel.y );
            break;

        case APP_EV_WIN_RESIZE:
            printf( " %dx%d", ev->data.win_resize.w, ev->data.win_resize.h );
            break;

        default: break;
    }

    printf( "\n" );
}

/*==============================================================================================
    State printing
==============================================================================================*/

static void
print_state_full( win_id_t id )
{
    if ( !app()->window_is_valid( id ) )
    {
        printf( "[win %d]  (invalid)\n", id );
        return;
    }

    app_win_state_t s = app()->window_state( id );
    printf( "[win %d]  %s  %s  %s  %s  %s  %s  %s  %s\n", id,
            s.focused    ? "+focus"   : "-focus",
            s.minimized  ? "+min"     : "-min",
            s.maximized  ? "+max"     : "-max",
            s.restored   ? "+restore" : "-restore",
            s.fillscreen ? "+fill"    : "-fill",
            s.hovered    ? "+hover"   : "-hover",
            s.captured   ? "+capture" : "-capture",
            s.hidden     ? "+hidden"  : "-hidden" );

    i32 mx = 0, my = 0;
    app()->mouse_position( &mx, &my );
    printf( "         mouse (%d,%d)  L:%s R:%s M:%s X1:%s X2:%s  paint:%s\n",
            mx, my,
            app()->mouse_button_down( APP_MOUSE_LEFT   ) ? "1" : "0",
            app()->mouse_button_down( APP_MOUSE_RIGHT  ) ? "1" : "0",
            app()->mouse_button_down( APP_MOUSE_MIDDLE ) ? "1" : "0",
            app()->mouse_button_down( APP_MOUSE_X1     ) ? "1" : "0",
            app()->mouse_button_down( APP_MOUSE_X2     ) ? "1" : "0",
            app()->window_paint_enabled( id ) ? "on" : "off" );
}

/* Prints only bits within mask that changed between prev and now. */
static void
print_state_diff( win_id_t id, app_win_state_t prev, app_win_state_t now, u32 mask )
{
    u32 changed = ( prev.bits ^ now.bits ) & mask;
    if ( !changed )
        return;
    printf( "[state win=%d]", id );
    if ( changed & 0x01 ) printf( "  %sfocus",   now.focused    ? "+" : "-" );
    if ( changed & 0x02 ) printf( "  %smin",     now.minimized  ? "+" : "-" );
    if ( changed & 0x04 ) printf( "  %smax",     now.maximized  ? "+" : "-" );
    if ( changed & 0x08 ) printf( "  %srestore", now.restored   ? "+" : "-" );
    if ( changed & 0x10 ) printf( "  %sfill",    now.fillscreen ? "+" : "-" );
    if ( changed & 0x20 ) printf( "  %shover",   now.hovered    ? "+" : "-" );
    if ( changed & 0x40 ) printf( "  %scapture", now.captured   ? "+" : "-" );
    if ( changed & 0x80 ) printf( "  %shidden",  now.hidden     ? "+" : "-" );
    printf( "\n" );
}

/*==============================================================================================
    Test modes
==============================================================================================*/

typedef enum
{
    MODE_MENU          = 0,
    MODE_WIN_STATE     = 1,    /* all state bit transitions                         */
    MODE_FILLSCREEN    = 2,    /* F / Alt+Enter fullscreen toggle                   */
    MODE_KEYS_GAME     = 3,    /* KEY_DOWN / KEY_UP, repeat suppressed              */
    MODE_KEYS_TEXT     = 4,    /* KEY_DOWN / KEY_UP, OS key-repeat enabled          */
    MODE_CHAR_INPUT    = 5,    /* CHAR events, printable codepoints, Ctrl combos    */
    MODE_MOUSE_MOVE    = 6,    /* MOUSE_MOVE with client-area xy and frame delta    */
    MODE_MOUSE_BUTTONS = 7,    /* MOUSE_DOWN / MOUSE_UP / MOUSE_WHEEL               */
    MODE_MOUSE_CAPTURE = 8,    /* captured bit -- click-hold and drag outside        */
    MODE_WIN_HOVER     = 9,    /* hover bit -- cursor enter / leave client area      */
    MODE_MULTI_WIN     = 10,   /* two windows, focus transfer                       */
} test_mode_t;

static test_mode_t     g_mode       = MODE_MENU;
static win_id_t        g_main_win   = APP_WIN_INVALID;
static win_id_t        g_extra_win  = APP_WIN_INVALID;
static app_win_state_t g_prev_main  = { 0 };
static app_win_state_t g_prev_extra = { 0 };

/* Which state bits to diff and print for each mode (0 = none). */
static u32
mode_state_mask( test_mode_t m )
{
    switch ( m )
    {
        case MODE_WIN_STATE:     return 0xFF;         /* all bits                   */
        case MODE_FILLSCREEN:    return 0x18;         /* fill(0x10) | restore(0x08) */
        case MODE_MOUSE_CAPTURE: return 0x40;         /* captured                   */
        case MODE_WIN_HOVER:     return 0x20;         /* hovered                    */
        case MODE_MULTI_WIN:     return 0xFF;         /* all bits, both windows     */
        default:                 return 0x00;
    }
}

/* Which event types to print for each mode. */
static bool
mode_show_event( test_mode_t m, i32 type )
{
    switch ( m )
    {
        case MODE_WIN_STATE:
            return type == APP_EV_WIN_FOCUS  || type == APP_EV_WIN_BLUR   ||
                   type == APP_EV_WIN_RESIZE || type == APP_EV_WIN_CLOSE;

        case MODE_FILLSCREEN:
            return type == APP_EV_WIN_RESIZE;

        case MODE_KEYS_GAME:
        case MODE_KEYS_TEXT:
            return type == APP_EV_KEY_DOWN || type == APP_EV_KEY_UP;

        case MODE_CHAR_INPUT:
            return type == APP_EV_CHAR;

        case MODE_MOUSE_MOVE:
            return type == APP_EV_MOUSE_MOVE;

        case MODE_MOUSE_BUTTONS:
            return type == APP_EV_MOUSE_DOWN  || type == APP_EV_MOUSE_UP ||
                   type == APP_EV_MOUSE_WHEEL;

        case MODE_MOUSE_CAPTURE:
            return type == APP_EV_MOUSE_DOWN || type == APP_EV_MOUSE_UP;

        case MODE_WIN_HOVER:
            return false; /* state diff only -- no raw events */

        case MODE_MULTI_WIN:
            return type == APP_EV_WIN_FOCUS  || type == APP_EV_WIN_BLUR   ||
                   type == APP_EV_WIN_RESIZE || type == APP_EV_WIN_CLOSE;

        default:
            return false;
    }
}

/*==============================================================================================
    Menu
==============================================================================================*/

static void
print_menu( void )
{
    printf( "\n=== sb_engine_app -- select a test ===========================\n" );
    printf( "  1  Window State Monitor      (focus/min/max/restore/fill/hide)\n" );
    printf( "  2  Fillscreen Toggle         (F key / Alt+Enter)\n" );
    printf( "  3  Key Events -- Game Mode    (no repeat; left/right modifiers)\n" );
    printf( "  4  Key Repeat -- Text Mode    (OS key-repeat enabled)\n" );
    printf( "  5  Char / Text Input         (CHAR events, Ctrl+C, Ctrl+V, Ctrl+Z)\n" );
    printf( "  6  Mouse Move Tracking       (client-area xy and per-frame delta)\n" );
    printf( "  7  Mouse Buttons & Wheel     (all 5 buttons; signed scroll delta)\n" );
    printf( "  8  Mouse Capture             (click-hold, drag outside window)\n" );
    printf( "  9  Window Hover              (cursor enter / leave client area)\n" );
    printf( "  0  Multi-Window              (two windows, focus transfer)\n" );
    printf( "\n" );
    printf( "  S  Full state snapshot  (works in any mode)\n" );
    printf( "  Q  Quit\n" );
    printf( "=============================================================\n\n" );
}

/*==============================================================================================
    Mode transitions
==============================================================================================*/

static void
close_extra_win( void )
{
    if ( app()->window_is_valid( g_extra_win ) )
    {
        app()->window_close( g_extra_win );
        printf( "[info] closed extra window\n" );
    }
    g_extra_win  = APP_WIN_INVALID;
    g_prev_extra = ( app_win_state_t ){ 0 };
}

static void
leave_mode( test_mode_t m )
{
    if ( m == MODE_KEYS_TEXT )
        app()->key_repeat_set( false );
    if ( m == MODE_MULTI_WIN )
        close_extra_win();
}

static void
enter_mode( test_mode_t m )
{
    leave_mode( g_mode );

    /* flush stale ring events so the new mode starts clean */
    app_event_t discard;
    while ( app()->next_event( &discard ) ) {}

    g_mode = m;
    printf( "\n" );

    switch ( m )
    {
        case MODE_WIN_STATE:
            printf( "--- [1] Window State Monitor -----------------------------------\n" );
            printf( "  Diffs:    focus / min / max / restore / fill / hover / capture / hidden\n" );
            printf( "  Events:   WIN_FOCUS, WIN_BLUR, WIN_RESIZE, WIN_CLOSE\n" );
            printf( "  Try:      click the title bar -- +focus should already be set\n" );
            printf( "  Try:      minimize (taskbar or Win+Down) -- +min, -restore\n" );
            printf( "  Try:      maximize (title-bar button or Win+Up) -- +max, -restore\n" );
            printf( "  Try:      restore -- -max, +restore\n" );
            printf( "  Try:      Alt+Tab away -- -focus, WIN_BLUR; back -- +focus, WIN_FOCUS\n" );
            printf( "  Correct:  exactly one state-diff line per transition, no spurious lines\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_FILLSCREEN:
            printf( "--- [2] Fillscreen Toggle --------------------------------------\n" );
            printf( "  Controls: F key (handled here) or Alt+Enter (handled in window proc)\n" );
            printf( "  State:    +fill / -restore on enter;  -fill / +restore on exit\n" );
            printf( "  Events:   WIN_RESIZE fires with the new client dimensions\n" );
            printf( "  Try:      press F -- window should cover entire monitor, no chrome\n" );
            printf( "  Try:      press Alt+Enter -- same result via keyboard shortcut\n" );
            printf( "  Try:      toggle twice -- verify return to original size and state\n" );
            printf( "  NOTE:     Alt+Enter is consumed by the window proc before reaching the\n" );
            printf( "            event ring -- you will NOT see a KEY_DOWN for it here\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_KEYS_GAME:
            printf( "--- [3] Key Events -- Game Mode (no repeat) ---------------------\n" );
            printf( "  Repeat:   OFF -- holding a key produces exactly one KEY_DOWN\n" );
            printf( "  Try:      press and hold any letter -- verify exactly ONE KEY_DOWN fires\n" );
            printf( "  Try:      release -- verify exactly ONE KEY_UP fires\n" );
            printf( "  Try:      Shift+A -- mod field should show [shift]; check key=A\n" );
            printf( "  Try:      Ctrl+Z -- mod shows [ctrl]; key=Z\n" );
            printf( "  Try:      LShift vs RShift -- distinct key codes LSHIFT / RSHIFT\n" );
            printf( "  Try:      LAlt vs RAlt, LCtrl vs RCtrl -- same left/right distinction\n" );
            printf( "  NOTE:     Alt+F4 and Alt+Enter are consumed before this ring\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            app()->key_repeat_set( false );
            break;

        case MODE_KEYS_TEXT:
            printf( "--- [4] Key Repeat -- Text Mode ----------------------------------\n" );
            printf( "  Repeat:   ON -- holding a key fires repeated KEY_DOWN events\n" );
            printf( "  Try:      hold any letter -- watch the stream of repeat KEY_DOWNs\n" );
            printf( "  Try:      hold Backspace or Delete -- repeat should fire\n" );
            printf( "  Try:      hold a modifier key alone (Shift) -- modifier also repeats\n" );
            printf( "  NOTE:     initial delay and repeat rate are OS keyboard settings\n" );
            printf( "  NOTE:     repeat = UP then DOWN pair per tick so key_pressed re-fires\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            app()->key_repeat_set( true );
            break;

        case MODE_CHAR_INPUT:
            printf( "--- [5] Char / Text Input ---------------------------------------\n" );
            printf( "  Events:   CHAR only -- printable UTF-32 codepoints from WM_CHAR\n" );
            printf( "  Try:      type letters -- each key: one CHAR with the expected glyph\n" );
            printf( "  Try:      Shift+1 through Shift+= for ! @ # $  ^ & * ( ) + _\n" );
            printf( "  Try:      Ctrl+C -- CHAR U+0003 [ctrl] (ETX, ASCII 3)\n" );
            printf( "  Try:      Ctrl+V -- CHAR U+0016 [ctrl] (SYN, ASCII 22)\n" );
            printf( "  Try:      Ctrl+Z -- CHAR U+001A [ctrl] (SUB, ASCII 26)\n" );
            printf( "  Try:      Alt+A  -- posts via SC_KEYMENU path; appears as CHAR U+0061 [alt]\n" );
            printf( "  NOTE:     arrows, F-keys, modifiers alone do NOT generate CHAR events\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_MOUSE_MOVE:
            printf( "--- [6] Mouse Move Tracking -------------------------------------\n" );
            printf( "  Events:   MOUSE_MOVE with client-area (x,y) and per-frame (dx,dy)\n" );
            printf( "  Try:      move slowly across the window -- small, smooth deltas\n" );
            printf( "  Try:      move quickly -- larger deltas, positions should still be accurate\n" );
            printf( "  Try:      reach each corner -- verify (0,0) top-left, (w-1,h-1) bottom-right\n" );
            printf( "  Try:      move outside the window -- events should stop (no capture)\n" );
            printf( "  NOTE:     (dx,dy) is relative to the previous MOUSE_MOVE frame position\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_MOUSE_BUTTONS:
            printf( "--- [7] Mouse Buttons & Wheel -----------------------------------\n" );
            printf( "  Events:   MOUSE_DOWN, MOUSE_UP, MOUSE_WHEEL\n" );
            printf( "  Try:      left click -- DOWN then UP; position = click coords\n" );
            printf( "  Try:      right click -- same DOWN/UP pair\n" );
            printf( "  Try:      middle button (click the scroll wheel)\n" );
            printf( "  Try:      X1 / X2 extra buttons if your mouse has them (btn=X1/X2)\n" );
            printf( "  Try:      scroll forward -- delta NEGATIVE (away from user)\n" );
            printf( "  Try:      scroll backward -- delta POSITIVE (toward user)\n" );
            printf( "  NOTE:     MOUSE_WHEEL xy is in client-area coordinates\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_MOUSE_CAPTURE:
            printf( "--- [8] Mouse Capture -------------------------------------------\n" );
            printf( "  State:    'capture' bit -- set by WM_MOUSEMOVE while any button is held\n" );
            printf( "  Events:   MOUSE_DOWN and MOUSE_UP (to see the button sequence)\n" );
            printf( "  Try:      left-click and hold inside window, then move -- expect +capture\n" );
            printf( "  Try:      drag cursor OUTSIDE the window while holding -- events continue\n" );
            printf( "  Try:      release the button outside -- -capture fires, then MOUSE_UP\n" );
            printf( "  Try:      repeat with right and middle buttons\n" );
            printf( "  NOTE:     capture is set on the first WM_MOUSEMOVE after a button down,\n" );
            printf( "            not on the button-down itself -- you must move to trigger it\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_WIN_HOVER:
            printf( "--- [9] Window Hover --------------------------------------------\n" );
            printf( "  State:    'hover' bit only -- tracks cursor inside client area\n" );
            printf( "  Try:      move cursor INTO the window -- expect +hover\n" );
            printf( "  Try:      move cursor OUT of the window -- expect -hover\n" );
            printf( "  Try:      cross the border quickly -- should still toggle cleanly\n" );
            printf( "  Try:      hover over the TITLE BAR -- NOT client area, -hover expected\n" );
            printf( "  Try:      hover over resize borders -- also not client area\n" );
            printf( "  NOTE:     hover uses WM_MOUSEHOVER (1ms) + WM_MOUSELEAVE via TrackMouseEvent\n" );
            printf( "  NOTE:     TrackMouseEvent is one-shot; re-armed on each WM_MOUSEMOVE entry\n" );
            printf( "  S = full snapshot   ESC = back to menu\n" );
            break;

        case MODE_MULTI_WIN:
        {
            printf( "--- [0] Multi-Window --------------------------------------------\n" );
            printf( "  Tracks:   both windows' state bits; tagged events from either window\n" );
            printf( "  Events:   WIN_FOCUS, WIN_BLUR, WIN_RESIZE, WIN_CLOSE\n" );
            printf( "  Try:      click extra window -- focus transfers: -focus on main, +focus on extra\n" );
            printf( "  Try:      click main window back -- reverse transfer\n" );
            printf( "  Try:      minimize one while the other has focus\n" );
            printf( "  Try:      close extra via X -- WIN_CLOSE fires; O reopens it\n" );
            printf( "  NOTE:     each event line shows win_id so you can tell which window fired\n" );
            printf( "  O = reopen extra   S = full snapshot   ESC = back to menu\n" );
            if ( !app()->window_is_valid( g_extra_win ) )
            {
                g_extra_win = app()->window_open( "sb_engine_app: extra", 120, 80, 480, 320,
                                                      APP_WIN_DEFAULT );
                if ( g_extra_win != APP_WIN_INVALID )
                {
                    g_prev_extra = app()->window_state( g_extra_win );
                    printf( "[info] opened extra window id=%d\n", g_extra_win );
                }
                else
                    printf( "[warn] window_open failed for extra window\n" );
            }
            break;
        }

        default: break;
    }

    printf( "\n" );
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static_load( "sys", sys_get_mod_desc() );
    mod_static_load( "app", app_get_mod_desc() );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    g_main_win = app()->window_open( "sb_engine_app", 0, 0, 0, 0, APP_WIN_DEFAULT );
    if ( g_main_win == APP_WIN_INVALID )
    {
        fprintf( stderr, "window_open failed\n" );
        mod_system_exit();
        return 1;
    }

    g_prev_main = app()->window_state( g_main_win );

    print_menu();

    while ( app()->pump_events() )
    {
        /* --- quit (any mode) ---------------------------------------------- */
        if ( app()->key_pressed( APP_KEY_Q ) )
        {
            printf( "[host] quit\n" );
            break;
        }

        /* --- state snapshot (any mode) ------------------------------------ */
        if ( app()->key_pressed( APP_KEY_S ) )
        {
            print_state_full( g_main_win );
            if ( app()->window_is_valid( g_extra_win ) )
                print_state_full( g_extra_win );
        }

        /* --- menu: select mode -------------------------------------------- */
        if ( g_mode == MODE_MENU )
        {
            for ( int i = 1; i <= 9; i++ )
            {
                if ( app()->key_pressed( (app_key_t)( APP_KEY_0 + i ) ) )
                {
                    enter_mode( (test_mode_t)i );
                    break;
                }
            }
            if ( app()->key_pressed( APP_KEY_0 ) )
                enter_mode( MODE_MULTI_WIN );

            sys_sleep_milliseconds( 16 );
            continue;
        }

        /* --- test mode: ESC returns to menu ------------------------------- */
        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
        {
            leave_mode( g_mode );
            g_mode = MODE_MENU;
            print_menu();
            sys_sleep_milliseconds( 16 );
            continue;
        }

        /* --- mode-specific controls --------------------------------------- */
        if ( g_mode == MODE_FILLSCREEN && app()->key_pressed( APP_KEY_F ) )
        {
            app()->window_toggle_fillscreen( g_main_win );
            printf( "[fill] F key -- fillscreen toggled\n" );
        }

        if ( g_mode == MODE_MULTI_WIN && app()->key_pressed( APP_KEY_O ) )
        {
            if ( !app()->window_is_valid( g_extra_win ) )
            {
                g_extra_win = app()->window_open( "sb_engine_app: extra", 120, 80, 480, 320,
                                                      APP_WIN_DEFAULT );
                if ( g_extra_win != APP_WIN_INVALID )
                {
                    g_prev_extra = app()->window_state( g_extra_win );
                    printf( "[info] opened extra window id=%d\n", g_extra_win );
                }
                else
                    printf( "[warn] window_open failed\n" );
            }
            else
                printf( "[info] extra window already open (id=%d)\n", g_extra_win );
        }

        /* --- drain typed events (mode-filtered) --------------------------- */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            /* Close the extra window when it receives WIN_CLOSE in multi-win mode. */
            if ( g_mode == MODE_MULTI_WIN && ev.type == APP_EV_WIN_CLOSE &&
                 ev.win_id == g_extra_win )
            {
                print_event( &ev );    /* print before invalidating */
                close_extra_win();
                continue;
            }

            if ( mode_show_event( g_mode, ev.type ) )
                print_event( &ev );
        }

        /* --- state diffs -------------------------------------------------- */
        u32 mask = mode_state_mask( g_mode );
        if ( app()->window_is_valid( g_main_win ) )
        {
            app_win_state_t now = app()->window_state( g_main_win );
            if ( mask )
                print_state_diff( g_main_win, g_prev_main, now, mask );
            g_prev_main = now;
        }
        if ( app()->window_is_valid( g_extra_win ) )
        {
            app_win_state_t now = app()->window_state( g_extra_win );
            if ( mask )
                print_state_diff( g_extra_win, g_prev_extra, now, mask );
            g_prev_extra = now;
        }

        sys_sleep_milliseconds( 16 );
    }

    leave_mode( g_mode );
    if ( app()->window_is_valid( g_main_win ) )
        app()->window_close( g_main_win );

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
/* clang-format on */
