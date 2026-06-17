/*==============================================================================================

    runtime_service/imgui/imgui_nav.c -- Keyboard navigation driver.

    The per-frame brain behind the nav cursor (s_ctx.nav_id, the persistent keyboard analogue of
    hover_id).  Three jobs, run once per frame from imgui_new_frame after the popup state settles:

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

    The per-item half lives in nav_item_register (imgui_widget_core.c), called from widget_behavior:
    each item in nav_win records its rect, scores itself as a move candidate, and -- if it is the
    nav cursor -- lights the focus ring and takes a synthesized click from an activation.

    Included by imgui.c after imgui_popup.c (so the popup stack + IMGUI_POPUP_Z_BASE are in scope)
    and before imgui_api.c (so imgui_new_frame can call nav_new_frame).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* The explicit nav target window (set_nav_window / Ctrl+Tab / Alt).  0 means "follow the
   front-most normal window", so nav has a sensible default with no caller setup. */
static imgui_id_t s_nav_window;

/* A letter was used as an Alt+mnemonic during the current Alt hold, so the Alt release must not also
   toggle the menu bar -- distinguishes a bare Alt tap (toggle) from Alt+F (open the File menu). */
static bool s_nav_alt_used;

/*----------------------------------------------------------------------------------------------
    nav_choose_window -- pick the window/popup nav is scoped to this frame.
----------------------------------------------------------------------------------------------*/

static void
nav_choose_window( void )
{
    /* Menu-bar mode (Alt / mnemonic): nav lives on the bar window while on the entries, or on the
       top open popup once descended into the menus -- so Left/Right traverse the bar and Up/Down
       walk the open menu, both through the one nav cursor. */
    if ( s_ctx.nav_bar_win != IMGUI_ID_NONE )
    {
        s_ctx.nav_win = ( s_ctx.nav_in_menus && s_popup_open_count > 0 )
                      ? s_popups_open[ s_popup_open_count - 1u ].id
                      : s_ctx.nav_bar_win;
        return;
    }

    /* A popup owns nav while open (the front-most one), mirroring popup_apply_modal stealing
       hover_win -- so mouse-opened menus, combos, and context menus capture the arrows too. */
    if ( s_popup_open_count > 0 )
    {
        s_ctx.nav_win = s_popups_open[ s_popup_open_count - 1u ].id;
        return;
    }

    /* No popup: the explicit target if it is still a live window, else the front-most normal
       window by z.  Popup-band records (a closed popup's stale high z) are skipped so they never
       masquerade as the front-most window. */
    imgui_id_t front = IMGUI_ID_NONE;
    u32        frontz = 0;
    bool       have_explicit = false;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        if ( s_windows[ i ].z >= IMGUI_POPUP_Z_BASE ) continue;
        if ( s_windows[ i ].id == s_nav_window )      have_explicit = true;
        if ( s_windows[ i ].z >= frontz ) { frontz = s_windows[ i ].z; front = s_windows[ i ].id; }
    }
    s_ctx.nav_win = have_explicit ? s_nav_window : front;
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
        if ( s_windows[ i ].z >= IMGUI_POPUP_Z_BASE ) continue;
        if ( s_windows[ i ].id == s_nav_window ) { curz = s_windows[ i ].z; found = true; }
    }
    if ( !found )
        for ( u32 i = 0; i < s_window_count; ++i )
            if ( s_windows[ i ].z < IMGUI_POPUP_Z_BASE && s_windows[ i ].z >= curz )
                curz = s_windows[ i ].z;

    /* Nearest window strictly past curz in the requested direction. */
    imgui_id_t pick  = IMGUI_ID_NONE;
    u32        pickz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        u32 z = s_windows[ i ].z;
        if ( z >= IMGUI_POPUP_Z_BASE ) continue;
        if ( dir > 0 ) { if ( z > curz && z < pickz ) { pickz = z; pick = s_windows[ i ].id; } }
        else           { if ( z < curz && z > pickz ) { pickz = z; pick = s_windows[ i ].id; } }
    }

    /* None past it -- wrap to the extreme opposite end (lowest for forward, highest for back). */
    if ( pick == IMGUI_ID_NONE )
    {
        u32 wrapz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
        for ( u32 i = 0; i < s_window_count; ++i )
        {
            u32 z = s_windows[ i ].z;
            if ( z >= IMGUI_POPUP_Z_BASE ) continue;
            if ( dir > 0 ) { if ( z <= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
            else           { if ( z >= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
        }
    }

    if ( pick == IMGUI_ID_NONE ) return;

    /* Adopt + raise the picked window; first item takes focus next frame. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == pick && s_windows[ i ].z != s_z_counter )
            s_windows[ i ].z = ++s_z_counter;

    s_nav_window     = pick;
    s_ctx.nav_id     = IMGUI_ID_NONE;
    s_ctx.nav_active = true;
}

/*----------------------------------------------------------------------------------------------
    nav_commit_prev -- apply the move resolved by last frame's emission into nav_id.
----------------------------------------------------------------------------------------------*/

static void
nav_commit_prev( void )
{
    /* Directional winner -> adopt it and carry its rect as the next scoring origin. */
    if ( s_ctx.nav_move_dir >= 0 && s_ctx.nav_move_best != IMGUI_ID_NONE )
    {
        s_ctx.nav_id       = s_ctx.nav_move_best;
        s_ctx.nav_ref_rect = s_ctx.nav_move_rect;
        s_ctx.nav_id_seen  = true;
    }
    /* Tab winner (emission order; wraps through the first item). */
    else if ( s_ctx.nav_tab != 0 )
    {
        imgui_id_t t = ( s_ctx.nav_tab > 0 )
            ? ( s_ctx.nav_tab_next != IMGUI_ID_NONE ? s_ctx.nav_tab_next : s_ctx.nav_tab_first )
            : ( s_ctx.nav_tab_prev != IMGUI_ID_NONE ? s_ctx.nav_tab_prev : s_ctx.nav_tab_first );
        if ( t != IMGUI_ID_NONE ) s_ctx.nav_id = t;
    }

    /* No directional move this commit: keep the scoring origin pinned to where the cursor item
       actually sat this past frame (it may have scrolled / the window moved). */
    if ( s_ctx.nav_move_dir < 0 && s_ctx.nav_id_seen )
        s_ctx.nav_ref_rect = s_ctx.nav_self_rect;

    /* First-focus / recovery: nav is engaged but its cursor item was not emitted (window just
       focused, popup opened, list shrank) -- land on the first eligible item in nav_win. */
    if ( s_ctx.nav_active && !s_ctx.nav_id_seen && s_ctx.nav_move_best == IMGUI_ID_NONE
         && s_ctx.nav_tab_first != IMGUI_ID_NONE )
        s_ctx.nav_id = s_ctx.nav_tab_first;
}

/*----------------------------------------------------------------------------------------------
    Menu-bar mode -- enter / exit + the bar/menu key handling (issues: bar traversal, Up-to-bar,
    close-returns-to-owner, Alt toggle, Alt+letter mnemonics).
----------------------------------------------------------------------------------------------*/

/* The main menu bar window id, or 0 if no main menu bar exists this session. */
static imgui_id_t
nav_main_bar_win( void )
{
    imgui_id_t mb = id_hash( "##MainMenuBar" );
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == mb )
            return mb;
    return IMGUI_ID_NONE;
}

/* Enter menu-bar mode on `bar` with the first entry highlighted; remember the prior nav target so
   Alt can toggle straight back to it. */
static void
nav_menu_enter( imgui_id_t bar )
{
    s_ctx.nav_prev_win   = s_nav_window;
    s_ctx.nav_prev_id    = s_ctx.nav_id;    /* remember the focus to toggle back to */
    s_ctx.nav_bar_win    = bar;
    s_ctx.nav_in_menus   = false;
    s_ctx.nav_menu_owner = IMGUI_ID_NONE;
    s_ctx.nav_id         = IMGUI_ID_NONE;   /* first bar entry takes focus */
    s_ctx.nav_active     = true;
}

/* Leave menu-bar mode: close the menu popups and restore nav to exactly where it was before Alt. */
static void
nav_menu_exit( void )
{
    s_popup_open_count   = 0;                 /* drop the open menus */
    s_ctx.nav_bar_win    = IMGUI_ID_NONE;
    s_ctx.nav_in_menus   = false;
    s_ctx.nav_menu_owner = IMGUI_ID_NONE;
    s_nav_window         = s_ctx.nav_prev_win;
    s_ctx.nav_id         = s_ctx.nav_prev_id;   /* back to the last focus location */
}

/* Ascend from the open menus back to the bar entry that owns them (the close / Up-to-bar return),
   leaving its menu dropped so Left/Right can keep traversing the bar. */
static void
nav_menu_ascend_to_bar( void )
{
    s_ctx.nav_in_menus = false;
    s_ctx.nav_id       = s_ctx.nav_menu_owner;
    s_ctx.nav_move_dir = -1;                  /* consume the move that triggered the ascend */
}

/* Bar/menu key handling while in menu-bar mode.  `first_prev` is last frame's first emitted item,
   used to detect "Up at the top of a dropdown".  Left/Right at the bar fall through to the scorer
   (the bar is horizontal), and Right inside a menu is handled by begin_menu (open submenu). */
static void
nav_menu_keys( bool down, bool up, bool left, bool esc, imgui_id_t first_prev )
{
    if ( !s_ctx.nav_in_menus )
    {
        /* On the bar: the highlighted entry drops its menu (begin_menu).  Down / Enter descend into
           it; Esc leaves menu mode.  Left/Right stay as a directional move for the scorer. */
        if ( down || s_ctx.nav_activate )
        {
            if ( s_popup_open_count > 0 )      /* a menu is dropped -> step into it */
            {
                s_ctx.nav_menu_owner = s_ctx.nav_id;
                s_ctx.nav_in_menus   = true;
                s_ctx.nav_id         = IMGUI_ID_NONE;   /* first item */
            }
            s_ctx.nav_move_dir = -1;
            s_ctx.nav_activate = false;        /* do not also "click" (toggle-close) the bar entry */
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

        if ( up && depth <= 1 && first_prev != IMGUI_ID_NONE && s_ctx.nav_id == first_prev )
        {
            nav_menu_ascend_to_bar();
        }
        else if ( left || esc )
        {
            if ( depth >= 2 )                  /* close a submenu, back to its parent menu */
            {
                --s_popup_open_count;
                s_ctx.nav_id = IMGUI_ID_NONE;
            }
            else                               /* top level: back to the owning bar entry */
            {
                nav_menu_ascend_to_bar();
            }
            s_ctx.nav_move_dir = -1;
        }
        /* Down / Up (mid-list) move via the scorer; Right opens a submenu in begin_menu; Enter
           activates through the synthesized click. */
    }
}

/*----------------------------------------------------------------------------------------------
    nav_new_frame -- the driver: commit, read keys into a fresh request, choose nav_win.
    Called from imgui_new_frame after popup_close_check / popup_apply_modal / window_raise_on_press.
----------------------------------------------------------------------------------------------*/

static void
nav_new_frame( void )
{
    nav_commit_prev();

    /* Last frame's first emitted item -- captured before the reset for the "Up at the top of a
       dropdown returns to the bar" test. */
    imgui_id_t first_prev = s_ctx.nav_tab_first;

    /* Reset the per-frame request + scoring accumulator (kept intact until here so the commit
       above could read last frame's result). */
    s_ctx.nav_move_dir   = -1;
    s_ctx.nav_tab        = 0;
    s_ctx.nav_activate   = false;
    s_ctx.nav_mnemonic   = 0;
    s_ctx.nav_move_best  = IMGUI_ID_NONE;
    s_ctx.nav_move_score = NAV_SCORE_REJECT;
    s_ctx.nav_id_seen    = false;
    s_ctx.nav_tab_first  = IMGUI_ID_NONE;
    s_ctx.nav_tab_prev   = IMGUI_ID_NONE;
    s_ctx.nav_tab_next   = IMGUI_ID_NONE;
    s_ctx.nav_tab_take   = false;

    /* A mouse click (any button) drops back to mouse mode -- the nav highlight clears and hover
       re-enables this frame.  A bare mouse *move* leaves nav engaged, so the highlight persists
       until a click (the requested behavior, vs dimming on every move). */
    if ( s_io.mouse_pressed[ 0 ] || s_io.mouse_pressed[ 1 ] || s_io.mouse_pressed[ 2 ] )
    {
        s_ctx.nav_active = false;
        if ( s_ctx.nav_bar_win != IMGUI_ID_NONE )
        {
            s_ctx.nav_bar_win  = IMGUI_ID_NONE;   /* leave menu mode; popups close on their own */
            s_ctx.nav_in_menus = false;
        }
    }

    /* Menu mode self-heals: if its bar window is gone, drop out. */
    if ( s_ctx.nav_bar_win != IMGUI_ID_NONE )
    {
        bool alive = false;
        for ( u32 i = 0; i < s_window_count; ++i )
            if ( s_windows[ i ].id == s_ctx.nav_bar_win ) { alive = true; break; }
        if ( !alive ) nav_menu_exit();
    }

    /* A focused text field owns the keyboard: nav reads nothing (Tab/arrows/Enter are the editor's,
       which releases focus on Enter/Esc -- imgui_text_edit.c). */
    if ( s_ctx.focused_id != IMGUI_ID_NONE )
    {
        nav_choose_window();
        return;
    }

    bool ctrl  = s_io.keys_down[ APP_KEY_LCTRL  ] || s_io.keys_down[ APP_KEY_RCTRL  ];
    bool shift = s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ];
    bool alt   = s_io.keys_down[ APP_KEY_LALT   ] || s_io.keys_down[ APP_KEY_RALT   ];

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
                s_ctx.nav_mnemonic = (u8)( 'A' + c );   /* begin_menu matches + opens the entry */
                s_nav_alt_used     = true;
                s_ctx.nav_active   = true;
                if ( s_ctx.nav_bar_win == IMGUI_ID_NONE )
                {
                    imgui_id_t mb = nav_main_bar_win();
                    if ( mb != IMGUI_ID_NONE ) nav_menu_enter( mb );
                }
                break;
            }

    if ( ( s_io.keys_released[ APP_KEY_LALT ] || s_io.keys_released[ APP_KEY_RALT ] )
         && !s_nav_alt_used )
    {
        if ( s_ctx.nav_bar_win != IMGUI_ID_NONE )
            nav_menu_exit();                      /* toggle out -> restore the prior focus */
        else
        {
            imgui_id_t mb = nav_main_bar_win();
            if ( mb != IMGUI_ID_NONE ) nav_menu_enter( mb );
        }
    }

    /* Arrows / Tab move (repeat so a held key keeps stepping); Enter/Space activate. */
    bool down  = s_io.keys_pressed_repeat[ APP_KEY_DOWN  ];
    bool up    = s_io.keys_pressed_repeat[ APP_KEY_UP    ];
    bool left  = s_io.keys_pressed_repeat[ APP_KEY_LEFT  ];
    bool right = s_io.keys_pressed_repeat[ APP_KEY_RIGHT ];
    bool esc   = s_io.keys_pressed[ APP_KEY_ESCAPE ];

    if ( up    ) { s_ctx.nav_move_dir = IMGUI_DIR_UP;    s_ctx.nav_active = true; }
    if ( down  ) { s_ctx.nav_move_dir = IMGUI_DIR_DOWN;  s_ctx.nav_active = true; }
    if ( left  ) { s_ctx.nav_move_dir = IMGUI_DIR_LEFT;  s_ctx.nav_active = true; }
    if ( right ) { s_ctx.nav_move_dir = IMGUI_DIR_RIGHT; s_ctx.nav_active = true; }
    if ( s_io.keys_pressed_repeat[ APP_KEY_TAB ] ) { s_ctx.nav_tab = shift ? -1 : +1; s_ctx.nav_active = true; }
    if ( s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ] )
    { s_ctx.nav_activate = true; s_ctx.nav_active = true; }

    /* Menu-bar mode owns the bar/menu keys (traverse, descend, ascend-to-owner, Up-to-bar). */
    if ( s_ctx.nav_bar_win != IMGUI_ID_NONE )
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
            s_ctx.nav_id = IMGUI_ID_NONE;
        }
        else if ( s_ctx.nav_move_dir == IMGUI_DIR_LEFT && s_popup_open_count >= 2 )
        {
            --s_popup_open_count;
            s_ctx.nav_move_dir = -1;
            s_ctx.nav_id       = IMGUI_ID_NONE;
        }
    }

    nav_choose_window();
}

/*----------------------------------------------------------------------------------------------
    Public: set_nav_window -- aim keyboard nav at a window by title (the explicit-focus entry).
----------------------------------------------------------------------------------------------*/

void
imgui_set_nav_window( const char* title )
{
    s_nav_window     = title ? id_hash( title ) : IMGUI_ID_NONE;
    s_ctx.nav_id     = IMGUI_ID_NONE;   /* first item of the new window takes focus */
    s_ctx.nav_active = true;
}

// clang-format on
/*============================================================================================*/
