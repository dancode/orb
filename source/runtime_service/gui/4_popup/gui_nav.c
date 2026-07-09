/*==============================================================================================

    runtime_service/gui/4_popup/gui_nav.c -- Keyboard navigation driver.

    The per-frame brain behind the nav cursor (s_nav.id, the persistent keyboard analogue of
    hover_id).  Run once per frame from gui_ctx_begin after the popup state settles:

      1. Translate this frame's keys into a request: arrows -> a directional move, Tab -> a
         linear move, Enter/Space -> activate, Esc/Left -> close a popup level, Ctrl+Tab ->
         cycle the nav window, F6 -> hop lanes (body <-> chrome), Alt -> enter the main menu bar.
      2. Resolve the move STRUCTURALLY against the nav item list built during last frame's
         emission (nav_finish): every item of the nav window recorded itself in emission order,
         stamped with the region + line the layout engine placed it in.  Left/Right walk the
         items of the current line ordinally and stop at its ends; Up/Down choose the target
         line by band geometry over those structural lines (nearest beyond the cursor's row,
         goal column breaking near-ties between parallel regions) and land under the remembered
         goal column; Tab walks the whole list.  Geometry only ever compares whole lines the
         layout engine really built -- never free rects -- so within a region the adjacent row
         always wins and Up exactly undoes Down.  (The list is one frame old, the same deferral
         hover_win runs on; the resolve itself happens on the keypress frame.)  An adoption arms
         the scroll chase: a cursor landing outside its region's view scrolls into it
         (nav_scroll_chase, gui_widget_core.c).
      3. Choose nav_win (nav_choose_window): the top open popup if any (popups capture nav exactly
         as popup_apply_modal steals hover_win), else the explicit target window, else the
         front-most normal window.

    The per-item half lives in nav_item_register (2_interact/gui_item.c), called from widget_behavior:
    each item in nav_win appends itself to the list and -- if it is the nav cursor -- lights the
    focus ring and takes a synthesized click from an activation.  Chrome (title-bar buttons,
    dock tabs: anything not placed by a layout cell) forms its own lane: F6 hops between the
    body and the chrome strip (and back to the remembered body item), Left/Right walk the strip
    by position, Down drops back into the body.  Tab and the body arrows never touch chrome.
    Scrollbars and drag strips are no keyboard targets at all (s_scope.nav_skip).

    Included by gui.c after gui_popup.c (so the popup stack + GUI_POPUP_Z_BASE are in scope)
    and before gui_frame.c (so gui_ctx_begin can call nav_new_frame).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* The explicit nav target window (window_set_nav / Ctrl+Tab / Alt) lives in s_nav.explicit_win
   (gui_internal.h) -- per-context, so two bound contexts never stomp each other's nav target.
   0 means "follow the front-most normal window", so nav has a sensible default with no caller
   setup. */

/* A letter was used as an Alt+mnemonic during the current Alt hold, so the Alt release must not also
   toggle the menu bar -- distinguishes a bare Alt tap (toggle) from Alt+F (open the File menu). */
static bool s_nav_alt_used;

/* Last frame's cursor position -- a move drops nav_highlight (the keyboard stops being the active
   instrument), so the mouse regains the fill while the nav ring keeps its place. */
static f32 s_nav_mouse_x, s_nav_mouse_y;

/*----------------------------------------------------------------------------------------------
    nav_choose_window -- pick the 4_window/popup nav is scoped to this frame.
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
       window by z.  Overlay records (popups, the tooltip; a closed one keeps the flag) are
       skipped so they never masquerade as the front-most window, and so is a GUI_WIN_NATIVE
       frame-only shell -- it has no body items, so defaulting nav onto it would strand the
       keyboard on bare chrome. */
    gui_id_t front = GUI_ID_NONE;
    u32        frontz = 0;
    bool       have_explicit = false;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        if ( s_windows[ i ].overlay ) continue;
        if ( s_windows[ i ].id == s_nav.explicit_win ) have_explicit = true;
        if ( s_windows[ i ].flags & GUI_WIN_NATIVE ) continue;
        if ( s_windows[ i ].z >= frontz ) { frontz = s_windows[ i ].z; front = s_windows[ i ].id; }
    }
    s_nav.win = have_explicit ? s_nav.explicit_win : front;
}

/*----------------------------------------------------------------------------------------------
    nav_cycle_window -- Ctrl+Tab: move the explicit nav target to the next normal window by z.

    dir > 0 picks the next-higher z (wrapping to the lowest), dir < 0 the next-lower (wrapping to
    the highest), so repeated Ctrl+Tab walks the window stack.  The chosen window is raised to the
    front so it is visible and usable, and nav_id is cleared so the first item takes focus.
----------------------------------------------------------------------------------------------*/

/* A window Ctrl+Tab never lands on: an overlay record (popup / tooltip), a GUI_WIN_NATIVE
   frame-only shell (bare chrome, no body), or a docked window hidden behind another tab --
   cycling only visits what is visible; the hidden tabs of a node are reached locally, through
   its visible tab's chrome lane (F6, then Left/Right along the strip). */
static bool
nav_cycle_skip( const gui_window_t* w )
{
    if ( w->overlay )                     return true;
    if ( w->flags & GUI_WIN_NATIVE )      return true;

    gui_dock_node_t* dn = dock_find_window_node( w->id );
    if ( dn && !( dn->active_tab < dn->tab_count && dn->tabs[ dn->active_tab ] == w->id ) )
        return true;   /* docked but behind another tab -- not visible */

    return false;
}

static void
nav_cycle_window( i32 dir )
{
    /* Current reference z: the explicit target if live, else the front-most normal window. */
    u32  curz  = 0;
    bool found = false;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        if ( s_windows[ i ].overlay ) continue;
        if ( s_windows[ i ].id == s_nav.explicit_win ) { curz = s_windows[ i ].z; found = true; }
    }
    if ( !found )
        for ( u32 i = 0; i < s_window_count; ++i )
            if ( !s_windows[ i ].overlay && s_windows[ i ].z >= curz )
                curz = s_windows[ i ].z;

    /* Nearest visible window strictly past curz in the requested direction (see nav_cycle_skip).
       Docked windows cycle too: their record z is stale for drawing (the node draws them) but
       still orders them here. */
    gui_id_t pick  = GUI_ID_NONE;
    u32        pickz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
    for ( u32 i = 0; i < s_window_count; ++i )
    {
        u32 z = s_windows[ i ].z;
        if ( nav_cycle_skip( &s_windows[ i ] ) ) continue;
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
            if ( nav_cycle_skip( &s_windows[ i ] ) ) continue;
            if ( dir > 0 ) { if ( z <= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
            else           { if ( z >= wrapz ) { wrapz = z; pick = s_windows[ i ].id; } }
        }
    }

    if ( pick == GUI_ID_NONE ) return;

    /* Adopt + raise the picked window; first item takes focus next frame. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == pick )
            s_windows[ i ].z = surface_z_raise( s_windows[ i ].z );

    /* A floating tab group raises with its picked (visible) tab so the group surfaces. */
    {
        gui_dock_node_t* dn = dock_find_window_node( pick );
        if ( dn && dn->floating )
            dn->z = surface_z_raise( dn->z );
    }

    s_nav.explicit_win = pick;
    s_nav.id     = GUI_ID_NONE;
    s_nav.active = true;
}

/*----------------------------------------------------------------------------------------------
    Nav list resolvers -- structural movement over the item list built during last frame's
    emission (see the file header).  All of them run at request time from nav_finish, mutate
    s_nav.id, and leave the request fields (move_dir / activate) live for the emission-time
    consumers (menu_begin's Right-opens-submenu, the activation in nav_item_register).
----------------------------------------------------------------------------------------------*/

/* Index of id in the list, or -1.  Linear: the list is small and resolved once per keypress. */
static i32
nav_list_find( gui_id_t id )
{
    if ( id == GUI_ID_NONE ) return -1;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
        if ( s_nav.items[ i ].id == id )
            return (i32)i;
    return -1;
}

/* Adopt items[i] as the nav cursor.  A vertical step keeps the goal column (so a run of Up/Down
   holds its lane); any other adoption clears it -- the next vertical run re-anchors at the item.
   Every adoption arms the scroll chase: if the item sits outside its region's view when it
   registers, the region scrolls it in (nav_scroll_chase). */
static void
nav_adopt( i32 i, bool vertical )
{
    s_nav.id           = s_nav.items[ i ].id;
    s_nav.scroll_chase = true;
    if ( !vertical )
        s_nav.goal_set = false;
}

/* Tab / Shift+Tab: the next / previous placed item in emission order, wrapping.  Body lane only:
   chrome is the F6 lane's, so Tab from a title-bar button steps back into the content.  The scan
   is bounded by the list length, so an all-chrome frame simply leaves the cursor alone. */
static void
nav_resolve_tab( void )
{
    i32 n = (i32)s_nav.item_count;
    if ( n == 0 ) return;

    i32 step = ( s_nav.tab > 0 ) ? 1 : -1;
    i32 i    = nav_list_find( s_nav.id );
    for ( i32 k = 0; k < n; ++k )
    {
        if ( i < 0 )
            i = ( step > 0 ) ? 0 : n - 1;   /* no cursor: enter the list at the near end */
        else
        {
            i += step;
            if ( i < 0 )  i = n - 1;
            if ( i >= n ) i = 0;
        }
        if ( !s_nav.items[ i ].chrome )
        {
            nav_adopt( i, false );
            return;
        }
    }
}

/* Land back in the body lane: the remembered return point if it is still a live placed item,
   else the first placed item -- the same landing rule as recovery. */
static void
nav_lane_body( void )
{
    i32 i = nav_list_find( s_nav.body_id );
    if ( i >= 0 && !s_nav.items[ i ].chrome )
    {
        nav_adopt( i, false );
        return;
    }
    for ( u32 k = 0; k < s_nav.item_count; ++k )
        if ( !s_nav.items[ k ].chrome )
        {
            nav_adopt( (i32)k, false );
            return;
        }
}

/* F6: hop between the body and the chrome strip.  From the body (or from nowhere) the first
   chrome item takes the cursor and the body position is remembered; from chrome the cursor
   returns to that body position.  No chrome this frame means no hop -- the cursor stays put. */
static void
nav_resolve_lane( void )
{
    i32 cur = nav_list_find( s_nav.id );
    if ( cur >= 0 && s_nav.items[ cur ].chrome )
    {
        nav_lane_body();
        return;
    }
    for ( u32 k = 0; k < s_nav.item_count; ++k )
        if ( s_nav.items[ k ].chrome )
        {
            s_nav.body_id = s_nav.id;
            nav_adopt( (i32)k, false );
            return;
        }
}

/* The item on line (region, line) whose x-span sits best under the goal column: a span containing
   it wins (distance 0), else the nearest edge.  Returns a list index, or -1 for an empty line. */
static i32
nav_line_pick( u32 region, u32 line, f32 goal )
{
    i32 best   = -1;
    f32 best_d = 0.0f;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &s_nav.items[ i ];
        if ( it->chrome || it->region != region || it->line != line ) continue;

        f32 x0 = it->rect.x, x1 = it->rect.x + it->rect.w;
        f32 d  = ( goal < x0 ) ? ( x0 - goal ) : ( ( goal > x1 ) ? ( goal - x1 ) : 0.0f );
        if ( best < 0 || d < best_d ) { best = (i32)i; best_d = d; }
    }
    return best;
}

/* Near-tie window for the vertical line choice: two candidate lines whose edge gaps differ by
   less than this read as "the same row" (parallel regions never align to the pixel), and the
   goal column decides between them instead of the raw gap. */
#define NAV_GAP_TIE  8.0f

/* Band slack: a candidate line must start at or past the cursor line's far edge to count as
   "beyond" it; one pixel of tolerance absorbs rounding so rows that butt exactly still qualify. */
#define NAV_BAND_EPS 1.0f

/* Home / End: the first / last placed item of the cursor's region (list order == reading order).
   No cursor yet means the whole body: its first / last placed item. */
static void
nav_resolve_home( void )
{
    i32  cur        = nav_list_find( s_nav.id );
    bool use_region = ( cur >= 0 && !s_nav.items[ cur ].chrome );
    u32  region     = use_region ? s_nav.items[ cur ].region : 0;

    i32 pick = -1;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &s_nav.items[ i ];
        if ( it->chrome ) continue;
        if ( use_region && it->region != region ) continue;
        pick = (i32)i;
        if ( s_nav.home < 0 ) break;   /* Home: the first hit; End keeps scanning to the last */
    }
    if ( pick >= 0 )
        nav_adopt( pick, false );
}

/* PageUp / PageDown: a vertical hop of one view height, holding the goal column like Up/Down.
   The target is the line (same region -- paging never leaps into a side panel) whose y band lands
   nearest one page from the cursor in the requested direction; when the region has less than a
   page left the nearest-to-target rule degrades to its first / last line, so paging clamps at the
   ends by construction.  The page span approximates the region view with the nav window's height
   (right for the common full-window list; a nested child just over-steps and clamps). */
static void
nav_resolve_page( void )
{
    if ( s_nav.item_count == 0 ) return;

    i32 cur = nav_list_find( s_nav.id );
    if ( cur < 0 )
    {
        for ( u32 i = 0; i < s_nav.item_count; ++i )
            if ( !s_nav.items[ i ].chrome ) { nav_adopt( (i32)i, false ); return; }
        return;
    }
    if ( s_nav.items[ cur ].chrome ) return;
    const gui_nav_item_t c = s_nav.items[ cur ];

    f32 page = 0.0f;
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == s_nav.win ) { page = s_windows[ i ].h; break; }
    if ( page <= 0.0f ) page = 10.0f * WIDGET_H;

    if ( !s_nav.goal_set )
    {
        s_nav.goal_x   = c.rect.x + c.rect.w * 0.5f;
        s_nav.goal_set = true;
    }

    bool down   = ( s_nav.page > 0 );
    f32  cy     = c.rect.y + c.rect.h * 0.5f;
    f32  want   = cy + ( down ? page : -page );
    u32  t_line = 0;
    f32  t_d    = 0.0f;
    bool found  = false;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &s_nav.items[ i ];
        if ( it->chrome || it->region != c.region || it->line == c.line ) continue;

        f32 iy = it->rect.y + it->rect.h * 0.5f;
        if ( down ? ( iy <= cy ) : ( iy >= cy ) ) continue;   /* wrong side of the cursor */

        f32 d = ( iy > want ) ? ( iy - want ) : ( want - iy );
        if ( !found || d < t_d )
        {
            t_line = it->line;
            t_d    = d;
            found  = true;
        }
    }
    if ( !found ) return;   /* already on the region's first / last line -- a wall */

    i32 pick = nav_line_pick( c.region, t_line, s_nav.goal_x );
    if ( pick >= 0 )
        nav_adopt( pick, true );
}

/* Directional move.  Left/Right are ordinal: the neighboring placed item on the same line, and a
   line end is a wall -- a horizontal move can never read as a vertical jump.  Up/Down choose the
   target LINE geometrically over the structural lines the layout engine built: every line whose
   y band sits strictly beyond the cursor line's competes by edge gap, with the goal column
   breaking near-ties -- so within one region the adjacent row always wins (rows stack; the gap is
   decisive and Down then Up returns to the start), a child's rows slot in where the child
   visually sits, and side-by-side regions whose rows share heights resolve to the region the
   goal column is over rather than whichever emitted later.  The landing item within the target
   line is the goal-column pick. */
static void
nav_resolve_move( void )
{
    if ( s_nav.item_count == 0 ) return;

    /* No cursor in the list (fresh engage, stale id): land on the first placed item rather than
       stepping from nowhere. */
    i32 cur = nav_list_find( s_nav.id );
    if ( cur < 0 )
    {
        for ( u32 i = 0; i < s_nav.item_count; ++i )
            if ( !s_nav.items[ i ].chrome ) { nav_adopt( (i32)i, false ); return; }
        return;
    }

    /* Chrome lane: the cursor sits on a chrome item (F6, or a click on a title-bar button).
       Left/Right step to the nearest chrome item by position -- emission order is wrong here
       (the close button emits before the detach box but sits right of it), and the strip is
       one visual band, so geometry IS its order.  Down drops back into the body; Up and the
       strip ends are walls. */
    if ( s_nav.items[ cur ].chrome )
    {
        if ( s_nav.move_dir == GUI_DIR_LEFT || s_nav.move_dir == GUI_DIR_RIGHT )
        {
            bool right  = ( s_nav.move_dir == GUI_DIR_RIGHT );
            f32  cx     = s_nav.items[ cur ].rect.x + s_nav.items[ cur ].rect.w * 0.5f;
            i32  best   = -1;
            f32  best_d = 0.0f;
            for ( u32 i = 0; i < s_nav.item_count; ++i )
            {
                const gui_nav_item_t* it = &s_nav.items[ i ];
                if ( !it->chrome || (i32)i == cur ) continue;
                f32 ix = it->rect.x + it->rect.w * 0.5f;
                f32 d  = right ? ( ix - cx ) : ( cx - ix );
                if ( d <= 0.0f ) continue;                    /* not on the requested side */
                if ( best < 0 || d < best_d ) { best = (i32)i; best_d = d; }
            }
            if ( best >= 0 )
                nav_adopt( best, false );
        }
        else if ( s_nav.move_dir == GUI_DIR_DOWN )
        {
            nav_lane_body();
        }
        return;
    }

    const gui_nav_item_t c = s_nav.items[ cur ];   /* adoption rewrites s_nav.id only; copy for clarity */

    if ( s_nav.move_dir == GUI_DIR_LEFT || s_nav.move_dir == GUI_DIR_RIGHT )
    {
        i32 step = ( s_nav.move_dir == GUI_DIR_LEFT ) ? -1 : 1;
        for ( i32 i = cur + step; i >= 0 && i < (i32)s_nav.item_count; i += step )
        {
            if ( s_nav.items[ i ].chrome ) continue;
            if ( s_nav.items[ i ].region != c.region || s_nav.items[ i ].line != c.line )
                break;                       /* ran off the line -- wall */
            nav_adopt( i, false );
            return;
        }
        return;
    }

    /* Vertical: anchor the goal column on the first step of a run. */
    bool down = ( s_nav.move_dir == GUI_DIR_DOWN );

    if ( !s_nav.goal_set )
    {
        s_nav.goal_x   = c.rect.x + c.rect.w * 0.5f;
        s_nav.goal_set = true;
    }

    /* The cursor line's y band -- the union of its items' extents, so a short label and a tall
       control on one row read as one band and neither can be "beyond" its own row. */
    f32 band_y0 = c.rect.y, band_y1 = c.rect.y + c.rect.h;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &s_nav.items[ i ];
        if ( it->chrome || it->region != c.region || it->line != c.line ) continue;
        if ( it->rect.y < band_y0 )              band_y0 = it->rect.y;
        if ( it->rect.y + it->rect.h > band_y1 ) band_y1 = it->rect.y + it->rect.h;
    }

    /* Target line: every placed item strictly beyond the band competes for its line by edge gap;
       within the near-tie window the goal-column x-overlap decides (see NAV_GAP_TIE above). */
    u32  t_region = 0, t_line = 0;
    f32  t_gap    = 0.0f;
    bool t_ov     = false;
    bool found    = false;
    for ( u32 i = 0; i < s_nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &s_nav.items[ i ];
        if ( it->chrome ) continue;
        if ( it->region == c.region && it->line == c.line ) continue;   /* our own line */

        f32 iy0 = it->rect.y, iy1 = it->rect.y + it->rect.h;
        f32 gap;
        if ( down ) { if ( iy0 < band_y1 - NAV_BAND_EPS ) continue; gap = iy0 - band_y1; }
        else        { if ( iy1 > band_y0 + NAV_BAND_EPS ) continue; gap = band_y0 - iy1; }
        if ( gap < 0.0f ) gap = 0.0f;

        bool ov = ( s_nav.goal_x >= it->rect.x && s_nav.goal_x <= it->rect.x + it->rect.w );

        bool better;
        if      ( !found )                       better = true;
        else if ( gap < t_gap - NAV_GAP_TIE )    better = true;           /* clearly nearer  */
        else if ( gap > t_gap + NAV_GAP_TIE )    better = false;          /* clearly farther */
        else if ( ov != t_ov )                   better = ov;             /* near-tie: goal column */
        else                                     better = ( gap < t_gap );
        if ( better )
        {
            t_region = it->region;
            t_line   = it->line;
            t_gap    = gap;
            t_ov     = ov;
            found    = true;
        }
    }
    if ( !found ) return;   /* nothing beyond -- first / last line of the window, a wall */

    i32 pick = nav_line_pick( t_region, t_line, s_nav.goal_x );
    if ( pick >= 0 )
        nav_adopt( pick, true );
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
    s_nav.prev_win   = s_nav.explicit_win;
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
    s_nav.explicit_win   = s_nav.prev_win;
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

/* Bar/menu key handling while in menu-bar mode.  `first_prev` is last frame's first placed item,
   used to detect "Up at the top of a dropdown".  Left/Right at the bar fall through to the list
   resolver (the bar entries share one line), and Right inside a menu is handled by menu_begin
   (open submenu). */
static void
nav_menu_keys( bool down, bool up, bool left, bool esc, gui_id_t first_prev )
{
    if ( !s_nav.in_menus )
    {
        /* On the bar: the highlighted entry drops its menu (menu_begin).  Down / Enter descend into
           it; Esc leaves menu mode.  Left/Right stay as a directional move for the list resolver. */
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
        /* Down / Up (mid-list) move via the list resolver; Right opens a submenu in menu_begin;
           Enter activates through the synthesized click. */
    }
}

/*----------------------------------------------------------------------------------------------
    nav_finish -- the shared tail of nav_new_frame: resolve this frame's surviving move request
    against last frame's item list (the menu / popup handlers above it may have consumed the move
    first), then open a fresh list for the emission about to run and scope nav to its window.
    Every exit path of nav_new_frame funnels through here so the list always resets exactly once.
----------------------------------------------------------------------------------------------*/

static void
nav_finish( void )
{
    if ( s_nav.lane )
        nav_resolve_lane();
    else if ( s_nav.tab != 0 )
        nav_resolve_tab();
    else if ( s_nav.page != 0 )
        nav_resolve_page();
    else if ( s_nav.home != 0 )
        nav_resolve_home();
    else if ( s_nav.move_dir >= 0 )
        nav_resolve_move();

    s_nav.item_count = 0;   /* fresh list; this frame's items append during emission */
    nav_choose_window();
}

/*----------------------------------------------------------------------------------------------
    nav_new_frame -- the driver: recover, read keys into a request, resolve, choose nav_win.
    Called from gui_ctx_begin after popup_close_check / popup_apply_modal / window_raise_on_press.
----------------------------------------------------------------------------------------------*/

static void
nav_new_frame( void )
{
    if ( !s_fwd_caps.keyboard_nav ) return;   /* feature boundary: gui_forward_caps_t.keyboard_nav;
                                                  s_nav.win stays GUI_ID_NONE, so nav_item_register
                                                  never matches a window and mouse input is untouched */

    /* One-shot: only an adoption made THIS frame (recovery below, or a resolver in nav_finish)
       chases the cursor into view during the emission that follows. */
    s_nav.scroll_chase = false;

    /* Value-edit self-heal: the captured widget stopped emitting (window closed, tab switched) --
       drop the capture so the keyboard is not fenced on a ghost. */
    if ( s_nav.edit_id != GUI_ID_NONE && !s_nav.id_seen )
        s_nav.edit_id = GUI_ID_NONE;

    /* First-focus / recovery: nav is engaged but its cursor item was not emitted last frame
       (window just focused, popup opened, list shrank) -- land on the first placed item that was. */
    if ( s_nav.active && !s_nav.id_seen && s_nav.first_item != GUI_ID_NONE )
    {
        s_nav.id           = s_nav.first_item;
        s_nav.goal_set     = false;
        s_nav.scroll_chase = true;
    }

    /* Last frame's first placed item -- captured before the reset for the "Up at the top of a
       dropdown returns to the bar" test. */
    gui_id_t first_prev = s_nav.first_item;

    /* Reset the per-frame request + registration bookkeeping.  The item list itself stays intact:
       this frame's keys resolve against it in nav_finish, which then opens the fresh list. */
    s_nav.move_dir   = -1;
    s_nav.tab        = 0;
    s_nav.page       = 0;
    s_nav.home       = 0;
    s_nav.activate   = false;
    s_nav.lane       = false;
    s_nav.edit_dir   = 0;
    s_nav.mnemonic   = 0;
    s_nav.id_seen    = false;
    s_nav.first_item = GUI_ID_NONE;

    /* A move makes the mouse the active instrument: it drops nav_highlight, so the nav item loses
       its fill (the ring stays, via nav_active) and the mouse hover regains the fill -- the ring
       keeps marking the keyboard's last position in case the user goes back to it.  A click goes
       further: the user has committed to the mouse, so it drops nav_active too and the ring itself
       disappears (a later keyboard press brings it back at s_nav.id, unmoved).  A click additionally
       leaves menu-bar mode -- the user switched to the mouse to drive the menus (which then track
       the cursor); the open popups close on their own through popup_close_check. */
    bool mouse_moved = ( s_io.mouse_x != s_nav_mouse_x || s_io.mouse_y != s_nav_mouse_y );
    bool mouse_press = ( s_io.mouse_pressed[ 0 ] || s_io.mouse_pressed[ 1 ] || s_io.mouse_pressed[ 2 ] );
    s_nav_mouse_x = s_io.mouse_x;
    s_nav_mouse_y = s_io.mouse_y;

    if ( mouse_moved || mouse_press )
        s_nav.highlight = false;
    if ( mouse_press )
    {
        s_nav.active  = false;
        s_nav.edit_id = GUI_ID_NONE;   /* the mouse takes over -- release the value-edit capture */
        if ( s_nav.bar_win != GUI_ID_NONE )
        {
            s_nav.bar_win  = GUI_ID_NONE;
            s_nav.in_menus = false;
        }

        /* Click-to-focus: the clicked window becomes the explicit nav target, so the keyboard
           resumes where the mouse last worked.  This is what lets a DOCKED window take nav at
           all -- a click never raises a tile's z (window_raise_on_press leaves it tiled), so the
           front-most-by-z default can never reach it.  Popup-band records keep their own capture
           (nav_choose_window) and a frame-only native shell never takes the keyboard. */
        if ( s_interaction.hover_win != GUI_ID_NONE )
            for ( u32 i = 0; i < s_window_count; ++i )
                if ( s_windows[ i ].id == s_interaction.hover_win )
                {
                    if ( !s_windows[ i ].overlay
                         && !( s_windows[ i ].flags & GUI_WIN_NATIVE ) )
                        s_nav.explicit_win = s_windows[ i ].id;
                    break;
                }
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
        nav_finish();
        return;
    }

    /* A captured DRAG widget (slider / drag-box value edit) owns the keyboard the same way:
       Left/Right (repeat) step its value -- published as edit_dir, applied by the widget through
       widget_state_t.nav_adjust -- and Enter/Space/Esc release; every other nav key is fenced so
       Up/Down can never yank the cursor off a widget mid-edit. */
    if ( s_nav.edit_id != GUI_ID_NONE )
    {
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ]
             || s_io.keys_pressed[ APP_KEY_ESCAPE ] )
        {
            s_nav.edit_id = GUI_ID_NONE;
            s_io.keys_pressed[ APP_KEY_ENTER ] = false;   /* the release must not re-activate */
            s_io.keys_pressed[ APP_KEY_SPACE ] = false;
        }
        else
        {
            if ( s_io.keys_pressed_repeat[ APP_KEY_LEFT  ] ) s_nav.edit_dir = -1;
            if ( s_io.keys_pressed_repeat[ APP_KEY_RIGHT ] ) s_nav.edit_dir = +1;
            if ( s_nav.edit_dir != 0 )
            {
                s_nav.active    = true;   /* stepping keeps the keyboard the active instrument */
                s_nav.highlight = true;
            }
        }
        nav_finish();
        return;
    }

    bool ctrl  = io_ctrl();
    bool shift = io_shift();
    bool alt   = io_alt();

    /* Ctrl+Tab cycles the nav window by z-order (Shift reverses). */
    if ( ctrl && s_io.keys_pressed[ APP_KEY_TAB ] )
    {
        nav_cycle_window( shift ? -1 : +1 );
        nav_finish();
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

    /* Page keys: the scrollbar's keyboard face.  Page hops one view height (repeat, for holding
       through a long list); Home/End jump to the region's first / last item.  The scroll chase
       then brings the landing into view, so these read as page-scrolling with a cursor. */
    bool pgup = s_io.keys_pressed_repeat[ APP_KEY_PAGE_UP   ];
    bool pgdn = s_io.keys_pressed_repeat[ APP_KEY_PAGE_DOWN ];
    bool home = s_io.keys_pressed[ APP_KEY_HOME ];
    bool end  = s_io.keys_pressed[ APP_KEY_END  ];
    if ( pgup ) s_nav.page = -1;
    if ( pgdn ) s_nav.page = +1;
    if ( home ) s_nav.home = -1;
    if ( end  ) s_nav.home = +1;

    bool act = s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ];
    if ( act ) s_nav.activate = true;

    /* F6 hops between the body and the chrome strip (title-bar buttons, dock tabs) -- the pane-
       cycle key.  Not while the menu bar owns nav: its bar entries are the chrome there. */
    bool lane = s_io.keys_pressed[ APP_KEY_F6 ] && s_nav.bar_win == GUI_ID_NONE;
    if ( lane ) s_nav.lane = true;

    /* Any nav key makes the keyboard the active instrument: show the ring (nav_active) AND the fill
       (nav_highlight), and suppress mouse hover until the mouse moves again. */
    if ( up || down || left || right || tab || act || lane || pgup || pgdn || home || end )
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

    nav_finish();
}

/*----------------------------------------------------------------------------------------------
    Public: window_set_nav -- aim keyboard nav at a window by title (the explicit-focus entry).
----------------------------------------------------------------------------------------------*/

void
gui_window_set_nav( const char* title )
{
    s_nav.explicit_win  = title ? id_hash( title ) : GUI_ID_NONE;
    s_nav.id        = GUI_ID_NONE;   /* first item of the new window takes focus */
    s_nav.active    = true;
    s_nav.highlight = true;
}

// clang-format on
/*============================================================================================*/
