/*==============================================================================================

    runtime_service/gui/popup/gui_nav.c -- Keyboard navigation driver.

    The per-frame brain behind the nav cursor (s_nav.id, the persistent keyboard analogue of
    hover_id).  Three jobs, run once per frame from gui_ctx_begin after the popup state settles:

      1. Commit the move resolved during the previous frame's emission (nav_commit_prev).  The
         directional / Tab winner the items scored into the accumulator becomes the new nav_id --
         the one-frame-deferred resolve, the same shape hover_win uses (a request this frame, the
         geometry known only as items emit, the result applied next frame).
      2. Translate this frame's keys into a fresh request: arrows -> a directional move, Tab ->
         a linear move, Enter/Space -> activate, Esc/Left -> close a popup level, Ctrl+Tab ->
         cycle the nav window, Alt -> enter the main menu bar.
      3. Choose nav_win (nav_choose_window): the top open popup if any (popups capture nav exactly
         as popup_apply_modal steals hover_win), else the explicit target window, else the
         front-most normal window.

    The per-item half lives in nav_item_register (gui_widget_core.c), called from widget_behavior:
    each item in nav_win records its rect, scores itself as a move candidate, and -- if it is the
    nav cursor -- lights the focus ring and takes a synthesized click from an activation.

    Included by gui.c after gui_popup.c (so the popup stack + GUI_POPUP_Z_BASE are in scope)
    and before gui_api.c (so gui_ctx_begin can call nav_new_frame).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* The explicit nav target window (window_set_nav / Ctrl+Tab / Alt).  0 means "follow the
   front-most normal window", so nav has a sensible default with no caller setup. */
static gui_id_t s_nav_window;

/* A letter was used as an Alt+mnemonic during the current Alt hold, so the Alt release must not also
   toggle the menu bar -- distinguishes a bare Alt tap (toggle) from Alt+F (open the File menu). */
static bool s_nav_alt_used;

/* Last frame's cursor position -- a move drops nav_highlight (the keyboard stops being the active
   instrument), so the mouse regains the fill while the nav ring keeps its place. */
static f32 s_nav_mouse_x, s_nav_mouse_y;

/*----------------------------------------------------------------------------------------------
    nav_choose_window -- pick the window/popup nav is scoped to this frame.
----------------------------------------------------------------------------------------------*/

static void
nav_choose_window( void )
{
    /* Menu-bar mode (Alt / mnemonic): nav lives on the bar window while on the entries, or on the
       top open popup once descended into the menus -- so Left/Right traverse the bar and Up/Down
       walk the open menu, both through the one nav cursor. */
    if ( s_nav.bar_win != GUI_ID_NONE )
    {
        s_nav.win = ( s_nav.in_menus && s_popup_open_count > 0 )
                      ? s_popups_open[ s_popup_open_count - 1u ].id
                      : s_nav.bar_win;
        return;
    }

    /* A popup owns nav while open (the front-most one), mirroring popup_apply_modal stealing
       hover_win -- so mouse-opened menus, combos, and context menus capture the arrows too. */
    if ( s_popup_open_count > 0 )
    {
        s_nav.win = s_popups_open[ s_popup_open_count - 1u ].id;
        return;
    }

    /* No popup: the explicit target if it is still a live window, else the front-most normal
       window by z.  Popup-band records (a closed popup's stale high z) are skipped so they never
       masquerade as the front-most window. */
    gui_id_t front = GUI_ID_NONE;
    u32        frontz = 0;
    bool       have_explicit = false;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        if ( s_windows[ i ].z >= GUI_POPUP_Z_BASE ) continue;
        if ( s_windows[ i ].id == s_nav_window )      have_explicit = true;
        if ( s_windows[ i ].z >= frontz ) { frontz = s_windows[ i ].z; front = s_windows[ i ].id; }
    }
    s_nav.win = have_explicit ? s_nav_window : front;
}

/*----------------------------------------------------------------------------------------------
    nav_cycle_window -- Ctrl+Tab: move the explicit nav target to the next normal window by z.

    dir > 0 picks the next-higher z (wrapping to the lowest), dir < 0 the next-lower (wrapping to
    the highest), so repeated Ctrl+Tab walks the window stack.  The chosen window is raised to the
    front so it is visible and usable, and nav_id is cleared so the first item takes focus.
----------------------------------------------------------------------------------------------*/

static void
nav_cycle_window( i32 dir )
{
    /* Current reference z: the explicit target if live, else the front-most normal window. */
    u32  curz  = 0;
    bool found = false;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        if ( s_windows[ i ].z >= GUI_POPUP_Z_BASE ) continue;
        if ( s_windows[ i ].id == s_nav_window ) { curz = s_windows[ i ].z; found = true; }
    }
    if ( !found )
        for ( u32 i = 0; i < s_window_count; ++i )
            if ( s_windows[ i ].z < GUI_POPUP_Z_BASE && s_windows[ i ].z >= curz )
                curz = s_windows[ i ].z;

    /* Nearest window strictly past curz in the requested direction. */
    gui_id_t pick  = GUI_ID_NONE;
    u32        pickz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        u32 z = s_windows[ i ].z;
        if ( z >= GUI_POPUP_Z_BASE ) continue;
        if ( dir > 0 ) { if ( z > curz && z < pickz ) { pickz = z; pick = s_windows[ i ].id; } }
        else           { if ( z < curz && z > pickz ) { pickz = z; pick = s_windows[ i ].id; } }
    }

    /* None past it -- wrap to the extreme opposite end (lowest for forward, highest for back). */
    if ( pick == GUI_ID_NONE )
    {
        u32 wrapz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
        for ( u32 i = 0; i < s_window_count; ++i )
        {
            u32 z = s_windows[ i ].z;
            if ( z >= GUI_POPUP_Z_BASE ) continue;
            if ( dir > 0 ) { if ( z <= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
            else           { if ( z >= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
        }
    }

    if ( pick == GUI_ID_NONE ) return;

    /* Adopt + raise the picked window; first item takes focus next frame. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == pick && s_windows[ i ].z != s_z_counter )
            s_windows[ i ].z = ++s_z_counter;

    s_nav_window     = pick;
    s_nav.id     = GUI_ID_NONE;
    s_nav.active = true;
}

/*----------------------------------------------------------------------------------------------
    nav_commit_prev -- apply the move resolved by last frame's emission into nav_id.
----------------------------------------------------------------------------------------------*/

static void
nav_commit_prev( void )
{
    /* Directional winner -> adopt it and carry its rect as the next scoring origin. */
    if ( s_nav.move_dir >= 0 && s_nav.move_best != GUI_ID_NONE )
    {
        s_nav.id       = s_nav.move_best;
        s_nav.ref_rect = s_nav.move_rect;
        s_nav.id_seen  = true;
    }
    /* Tab winner (emission order; wraps through the first item). */
    else if ( s_nav.tab != 0 )
    {
        gui_id_t t = ( s_nav.tab > 0 )
            ? ( s_nav.tab_next != GUI_ID_NONE ? s_nav.tab_next : s_nav.tab_first )
            : ( s_nav.tab_prev != GUI_ID_NONE ? s_nav.tab_prev : s_nav.tab_first );
        if ( t != GUI_ID_NONE ) s_nav.id = t;
    }

    /* No directional move this commit: keep the scoring origin pinned to where the cursor item
       actually sat this past frame (it may have scrolled / the window moved). */
    if ( s_nav.move_dir < 0 && s_nav.id_seen )
        s_nav.ref_rect = s_nav.self_rect;

    /* First-focus / recovery: nav is engaged but its cursor item was not emitted (window just
       focused, popup opened, list shrank) -- land on the first eligible item in nav_win. */
    if ( s_nav.active && !s_nav.id_seen && s_nav.move_best == GUI_ID_NONE
         && s_nav.tab_first != GUI_ID_NONE )
        s_nav.id = s_nav.tab_first;
}

/*----------------------------------------------------------------------------------------------
    Menu-bar mode -- enter / exit + the bar/menu key handling (issues: bar traversal, Up-to-bar,
    close-returns-to-owner, Alt toggle, Alt+letter mnemonics).
----------------------------------------------------------------------------------------------*/

/* The main menu bar window id, or 0 if no main menu bar exists this session. */
static gui_id_t
nav_main_bar_win( void )
{
    gui_id_t mb = id_hash( "##MainMenuBar" );
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == mb )
            return mb;
    return GUI_ID_NONE;
}

/* Enter menu-bar mode on `bar` with the first entry highlighted; remember the prior nav target so
   Alt can toggle straight back to it. */
static void
nav_menu_enter( gui_id_t bar )
{
    s_nav.prev_win   = s_nav_window;
    s_nav.prev_id    = s_nav.id;    /* remember the focus to toggle back to */
    s_nav.bar_win    = bar;
    s_nav.in_menus   = false;
    s_nav.menu_owner = GUI_ID_NONE;
    s_nav.id         = GUI_ID_NONE;   /* first bar entry takes focus */
    s_nav.active     = true;
    s_nav.highlight  = true;            /* Alt makes the keyboard the active instrument */
}

/* Leave menu-bar mode: close the menu popups and restore nav to exactly where it was before Alt. */
static void
nav_menu_exit( void )
{
    s_popup_open_count   = 0;                 /* drop the open menus */
    s_nav.bar_win    = GUI_ID_NONE;
    s_nav.in_menus   = false;
    s_nav.menu_owner = GUI_ID_NONE;
    s_nav_window         = s_nav.prev_win;
    s_nav.id         = s_nav.prev_id;   /* back to the last focus location */
}

/* Ascend from the open menus back to the bar entry that owns them (the close / Up-to-bar return),
   leaving its menu dropped so Left/Right can keep traversing the bar. */
static void
nav_menu_ascend_to_bar( void )
{
    s_nav.in_menus = false;
    s_nav.id       = s_nav.menu_owner;
    s_nav.move_dir = -1;                  /* consume the move that triggered the ascend */
}

/* Bar/menu key handling while in menu-bar mode.  `first_prev` is last frame's first emitted item,
   used to detect "Up at the top of a dropdown".  Left/Right at the bar fall through to the scorer
   (the bar is horizontal), and Right inside a menu is handled by menu_begin (open submenu). */
static void
nav_menu_keys( bool down, bool up, bool left, bool esc, gui_id_t first_prev )
{
    if ( !s_nav.in_menus )
    {
        /* On the bar: the highlighted entry drops its menu (menu_begin).  Down / Enter descend into
           it; Esc leaves menu mode.  Left/Right stay as a directional move for the scorer. */
        if ( down || s_nav.activate )
        {
            if ( s_popup_open_count > 0 )      /* a menu is dropped -> step into it */
            {
                s_nav.menu_owner = s_nav.id;
                s_nav.in_menus   = true;
                s_nav.id         = GUI_ID_NONE;   /* first item */
            }
            s_nav.move_dir = -1;
            s_nav.activate = false;        /* do not also "click" (toggle-close) the bar entry */
        }
        else if ( esc )
        {
            nav_menu_exit();
        }
    }
    else
    {
        /* Inside the menus.  Up at the first item of a top-level menu returns to the bar; Left /
           Esc close one level, ascending to the owning bar entry at the top level. */
        u32 depth = s_popup_open_count;

        if ( up && depth <= 1 && first_prev != GUI_ID_NONE && s_nav.id == first_prev )
        {
            nav_menu_ascend_to_bar();
        }
        else if ( left || esc )
        {
            if ( depth >= 2 )                  /* close a submenu, back to its parent menu */
            {
                --s_popup_open_count;
                s_nav.id = GUI_ID_NONE;
            }
            else                               /* top level: back to the owning bar entry */
            {
                nav_menu_ascend_to_bar();
            }
            s_nav.move_dir = -1;
        }
        /* Down / Up (mid-list) move via the scorer; Right opens a submenu in menu_begin; Enter
           activates through the synthesized click. */
    }
}

/*----------------------------------------------------------------------------------------------
    nav_new_frame -- the driver: commit, read keys into a fresh request, choose nav_win.
    Called from gui_ctx_begin after popup_close_check / popup_apply_modal / window_raise_on_press.
----------------------------------------------------------------------------------------------*/

static void
nav_new_frame( void )
{
    if ( !s_fwd_caps.keyboard_nav ) return;   /* feature boundary: gui_forward_caps_t.keyboard_nav;
                                                  s_nav.win stays GUI_ID_NONE, so nav_item_register
                                                  never matches a window and mouse input is untouched */

    nav_commit_prev();

    /* Last frame's first emitted item -- captured before the reset for the "Up at the top of a
       dropdown returns to the bar" test. */
    gui_id_t first_prev = s_nav.tab_first;

    /* Reset the per-frame request + scoring accumulator (kept intact until here so the commit
       above could read last frame's result). */
    s_nav.move_dir   = -1;
    s_nav.tab        = 0;
    s_nav.activate   = false;
    s_nav.mnemonic   = 0;
    s_nav.move_best  = GUI_ID_NONE;
    s_nav.move_score = NAV_SCORE_REJECT;
    s_nav.id_seen    = false;
    s_nav.tab_first  = GUI_ID_NONE;
    s_nav.tab_prev   = GUI_ID_NONE;
    s_nav.tab_next   = GUI_ID_NONE;
    s_nav.tab_take   = false;

    /* Any mouse activity makes the mouse the active instrument: a move or a click drops
       nav_highlight, so the nav item loses its fill (the ring stays, via nav_active) and the mouse
       hover regains the fill.  A click additionally leaves menu-bar mode -- the user switched to
       the mouse to drive the menus (which then track the cursor); the open popups close on their
       own through popup_close_check. */
    bool mouse_moved = ( s_io.mouse_x != s_nav_mouse_x || s_io.mouse_y != s_nav_mouse_y );
    bool mouse_press = ( s_io.mouse_pressed[ 0 ] || s_io.mouse_pressed[ 1 ] || s_io.mouse_pressed[ 2 ] );
    s_nav_mouse_x = s_io.mouse_x;
    s_nav_mouse_y = s_io.mouse_y;

    if ( mouse_moved || mouse_press )
        s_nav.highlight = false;
    if ( mouse_press && s_nav.bar_win != GUI_ID_NONE )
    {
        s_nav.bar_win  = GUI_ID_NONE;
        s_nav.in_menus = false;
    }

    /* Menu mode self-heals: if its bar window is gone, drop out. */
    if ( s_nav.bar_win != GUI_ID_NONE )
    {
        bool alive = false;
        for ( u32 i = 0; i < s_window_count; ++i )
            if ( s_windows[ i ].id == s_nav.bar_win ) { alive = true; break; }
        if ( !alive ) nav_menu_exit();
    }

    /* A focused text field owns the keyboard: nav reads nothing (Tab/arrows/Enter are the editor's,
       which releases focus on Enter/Esc -- gui_text_edit.c). */
    if ( s_interaction.focused_id != GUI_ID_NONE )
    {
        nav_choose_window();
        return;
    }

    bool ctrl  = io_ctrl();
    bool shift = io_shift();
    bool alt   = io_alt();

    /* Ctrl+Tab cycles the nav window by z-order (Shift reverses). */
    if ( ctrl && s_io.keys_pressed[ APP_KEY_TAB ] )
    {
        nav_cycle_window( shift ? -1 : +1 );
        nav_choose_window();
        return;
    }

    /* Alt mnemonics + toggle.  A letter pressed while Alt is held targets the bar entry whose label
       starts with it (issue: Alt+F); a bare Alt tap toggles menu mode on / off, resolved on release
       so it never fights an Alt+letter combo (s_nav_alt_used tracks whether the hold was a combo). */
    if ( s_io.keys_pressed[ APP_KEY_LALT ] || s_io.keys_pressed[ APP_KEY_RALT ] )
        s_nav_alt_used = false;

    if ( alt )
        for ( u32 c = 0; c < 26u; ++c )
            if ( s_io.keys_pressed[ APP_KEY_A + c ] )
            {
                s_nav.mnemonic  = (u8)( 'A' + c );  /* menu_begin matches + opens the entry */
                s_nav_alt_used      = true;
                s_nav.active    = true;
                s_nav.highlight = true;
                if ( s_nav.bar_win == GUI_ID_NONE )
                {
                    gui_id_t mb = nav_main_bar_win();
                    if ( mb != GUI_ID_NONE ) nav_menu_enter( mb );
                }
                break;
            }

    if ( ( s_io.keys_released[ APP_KEY_LALT ] || s_io.keys_released[ APP_KEY_RALT ] )
         && !s_nav_alt_used )
    {
        if ( s_nav.bar_win != GUI_ID_NONE )
            nav_menu_exit();                      /* toggle out -> restore the prior focus */
        else
        {
            gui_id_t mb = nav_main_bar_win();
            if ( mb != GUI_ID_NONE ) nav_menu_enter( mb );
        }
    }

    /* Arrows / Tab move (repeat so a held key keeps stepping); Enter/Space activate. */
    bool down  = s_io.keys_pressed_repeat[ APP_KEY_DOWN  ];
    bool up    = s_io.keys_pressed_repeat[ APP_KEY_UP    ];
    bool left  = s_io.keys_pressed_repeat[ APP_KEY_LEFT  ];
    bool right = s_io.keys_pressed_repeat[ APP_KEY_RIGHT ];
    bool esc   = s_io.keys_pressed[ APP_KEY_ESCAPE ];

    if ( up    ) s_nav.move_dir = GUI_DIR_UP;
    if ( down  ) s_nav.move_dir = GUI_DIR_DOWN;
    if ( left  ) s_nav.move_dir = GUI_DIR_LEFT;
    if ( right ) s_nav.move_dir = GUI_DIR_RIGHT;

    bool tab = s_io.keys_pressed_repeat[ APP_KEY_TAB ];
    if ( tab ) s_nav.tab = shift ? -1 : +1;

    bool act = s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ];
    if ( act ) s_nav.activate = true;

    /* Any nav key makes the keyboard the active instrument: show the ring (nav_active) AND the fill
       (nav_highlight), and suppress mouse hover until the mouse moves again. */
    if ( up || down || left || right || tab || act )
    {
        s_nav.active    = true;
        s_nav.highlight = true;
    }

    /* Menu-bar mode owns the bar/menu keys (traverse, descend, ascend-to-owner, Up-to-bar). */
    if ( s_nav.bar_win != GUI_ID_NONE )
    {
        nav_menu_keys( down, up, left, esc, first_prev );
    }
    /* Generic popup keyboard (mouse-opened menus, combos, context menus): Esc closes the top level,
       Left closes a submenu back to its parent. */
    else if ( s_popup_open_count > 0 )
    {
        if ( esc )
        {
            --s_popup_open_count;
            s_nav.id = GUI_ID_NONE;
        }
        else if ( s_nav.move_dir == GUI_DIR_LEFT && s_popup_open_count >= 2 )
        {
            --s_popup_open_count;
            s_nav.move_dir = -1;
            s_nav.id       = GUI_ID_NONE;
        }
    }

    nav_choose_window();
}

/*----------------------------------------------------------------------------------------------
    Public: window_set_nav -- aim keyboard nav at a window by title (the explicit-focus entry).
----------------------------------------------------------------------------------------------*/

void
gui_window_set_nav( const char* title )
{
    s_nav_window        = title ? id_hash( title ) : GUI_ID_NONE;
    s_nav.id        = GUI_ID_NONE;   /* first item of the new window takes focus */
    s_nav.active    = true;
    s_nav.highlight = true;
}

// clang-format on
/*============================================================================================*/
