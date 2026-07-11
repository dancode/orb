/*==============================================================================================

    runtime_service/gui/nav/gui_nav.c -- Keyboard navigation driver.

    A peer service, not a client of popup/: it arbitrates focus across windows, docks, menus,
    and popups alike (nav_choose_window reads dock nodes and popups and plain windows equally).
    It lives in its own tier for that reason; it is included right after popup/gui_popup.c
    only because it reads/drives the popup stack that file just opened -- a dependency-order
    constraint, not a topical one.

    The per-frame brain behind the nav cursor (g_ctx->nav.id, the persistent keyboard analogue of
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
         (nav_scroll_chase, gui_paint_core.c).
      3. Choose nav_win (nav_choose_window): the top open popup if any (popups capture nav exactly
         as popup_apply_modal steals hover_win), else the explicit target window, else the
         front-most normal window.

    The per-item half lives in nav_item_register (interact/gui_item.c), called from widget_behavior:
    each item in nav_win appends itself to the list and -- if it is the nav cursor -- lights the
    focus ring and takes a synthesized click from an activation.  Chrome (title-bar buttons,
    dock tabs: anything not placed by a layout cell) forms its own lane: F6 hops between the
    body and the chrome strip (and back to the remembered body item), Left/Right walk the strip
    by position, Down drops back into the body.  Tab and the body arrows never touch chrome.
    Scrollbars and drag strips are no keyboard targets at all (s_scope.nav.skip).

    Included by gui.c after gui_popup.c (so the popup stack is in scope)
    and before gui_frame.c (so gui_ctx_begin can call nav_new_frame).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* The keyboard-focused window (click / window_set_nav / Ctrl+Tab / Alt) lives in
   g_ctx->nav.focused_win (gui_internal.h) -- per-context, so two bound contexts never stomp each
   other's focus.  NONE means no window has focus, so nav falls back to the front-most normal
   window, giving nav a sensible default with no caller setup. */

/* A letter was used as an Alt+mnemonic during the current Alt hold, so the Alt release must not also
   toggle the menu bar -- distinguishes a bare Alt tap (toggle) from Alt+F (open the File menu). */
static bool s_nav_alt_used;

/* Last frame's cursor position -- a move drops nav_highlight (the keyboard stops being the active
   instrument), so the mouse regains the fill while the nav ring keeps its place. */
static f32 s_nav_mouse_x, s_nav_mouse_y;

/*----------------------------------------------------------------------------------------------
    Window-pool helpers -- the recurring read over g_ctx->win.pool.  (The lookup by id is the
    surface tier's window_find, in scope here since gui_surface.c is included first.)
----------------------------------------------------------------------------------------------*/

/* Whether nav's front-most-by-z default may land on this window: not an overlay record (popup /
   tooltip -- those capture nav their own way) and not a GUI_WIN_NATIVE frame-only shell (bare
   chrome, no body item to focus, so defaulting onto it would strand the keyboard). */
static bool
nav_win_focusable( const gui_window_t* w )
{
    return !w->overlay && !( w->flags & GUI_WIN_NATIVE );
}

/*----------------------------------------------------------------------------------------------
    nav_choose_window -- pick the window/popup nav is scoped to this frame.
----------------------------------------------------------------------------------------------*/

static void
nav_choose_window( void )
{
    bool     have_popup = ( g_ctx->popup.open_count > 0 );
    gui_id_t top_popup  = have_popup ? g_ctx->popup.open[ g_ctx->popup.open_count - 1u ].id
                                     : GUI_ID_NONE;

    /* Menu-bar mode (Alt / mnemonic): nav lives on the bar window while on the entries, or on the
       top open popup once descended into the menus -- so Left/Right traverse the bar and Up/Down
       walk the open menu, both through the one nav cursor. */
    if ( g_ctx->nav.bar_win != GUI_ID_NONE )
    {
        bool descended = ( g_ctx->nav.in_menus && have_popup );
        g_ctx->nav.win = descended ? top_popup : g_ctx->nav.bar_win;
        return;
    }

    /* A popup owns nav while open (the front-most one), mirroring popup_apply_modal stealing
       hover_win -- so mouse-opened menus, combos, and context menus capture the arrows too. */
    if ( have_popup )
    {
        g_ctx->nav.win = top_popup;
        return;
    }

    /* No popup: the explicit target if it is still a live window, else the front-most normal
       window by z.  Overlay records (popups, the tooltip; a closed one keeps the flag) are
       skipped so they never masquerade as the front-most window, and so is a GUI_WIN_NATIVE
       frame-only shell -- it has no body items, so defaulting nav onto it would strand the
       keyboard on bare chrome. */
    gui_id_t front      = GUI_ID_NONE;
    u32      frontz     = 0;
    bool     have_focus = false;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
    {
        gui_window_t* w = &g_ctx->win.pool[ i ];
        if ( w->overlay ) continue;   /* an overlay never masquerades as the front-most window */
        if ( w->id == g_ctx->nav.focused_win ) have_focus = true;   /* native may hold focus */
        if ( !nav_win_focusable( w ) ) continue;
        if ( w->z >= frontz ) { frontz = w->z; front = w->id; }
    }
    g_ctx->nav.win = have_focus ? g_ctx->nav.focused_win : front;
}

/*----------------------------------------------------------------------------------------------
    nav_cycle_window -- Ctrl+Tab: move the focused window to the next normal window by z.

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
    if ( dn )
    {
        bool is_active_tab = ( dn->active_tab < dn->tab_count && dn->tabs[ dn->active_tab ] == w->id );
        if ( !is_active_tab ) return true;   /* docked but behind another tab -- not visible */
    }

    return false;
}

/* The z the cycle steps away from: the focused window if it is a live non-overlay window,
   else the front-most non-overlay window's z. */
static u32
nav_cycle_from_z( void )
{
    gui_window_t* focused_w = window_find( g_ctx->nav.focused_win );
    if ( focused_w && !focused_w->overlay )
        return focused_w->z;

    u32 top = 0;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
        if ( !g_ctx->win.pool[ i ].overlay && g_ctx->win.pool[ i ].z >= top )
            top = g_ctx->win.pool[ i ].z;
    return top;
}

/* One step around the visible-window ring by z (see nav_cycle_skip for what is on the ring): the
   nearest window strictly past fromz in dir, or -- when nothing lies past it -- the extreme
   opposite end so the ring wraps.  Both candidates fall out of a single pass.  GUI_ID_NONE if the
   ring is empty.  Docked windows cycle too: their record z is stale for drawing (the node draws
   them) but still orders them here. */
static gui_id_t
nav_cycle_step( i32 dir, u32 fromz )
{
    gui_id_t next  = GUI_ID_NONE;                    // nearest strictly past fromz
    gui_id_t wrap  = GUI_ID_NONE;                    // extreme opposite end, for the wrap
    u32      nextz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;
    u32      wrapz = ( dir > 0 ) ? 0xFFFFFFFFu : 0u;

    for ( u32 i = 0; i < g_ctx->win.count; ++i )
    {
        gui_window_t* w = &g_ctx->win.pool[ i ];
        if ( nav_cycle_skip( w ) ) continue;
        u32 z = w->z;
        if ( dir > 0 )
        {
            if ( z > fromz && z < nextz ) { nextz = z; next = w->id; }   // next higher
            if ( z <= wrapz )             { wrapz = z; wrap = w->id; }   // lowest, wraps forward
        }
        else
        {
            if ( z < fromz && z > nextz ) { nextz = z; next = w->id; }   // next lower
            if ( z >= wrapz )             { wrapz = z; wrap = w->id; }   // highest, wraps back
        }
    }
    return ( next != GUI_ID_NONE ) ? next : wrap;
}

static void
nav_cycle_window( i32 dir )
{
    gui_id_t pick = nav_cycle_step( dir, nav_cycle_from_z() );
    if ( pick == GUI_ID_NONE ) return;

    /* Adopt + raise the picked window so it surfaces; its first item takes focus next frame. */
    gui_window_t* w = window_find( pick );
    if ( w ) w->z = surface_z_raise( w->z );

    /* A floating tab group raises with its picked (visible) tab so the group surfaces. */
    gui_dock_node_t* dn = dock_find_window_node( pick );
    if ( dn && dn->floating )
        dn->z = surface_z_raise( dn->z );

    g_ctx->nav.focused_win = pick;
    g_ctx->nav.id          = GUI_ID_NONE;
    g_ctx->nav.active      = true;
}

/*----------------------------------------------------------------------------------------------
    Nav list resolvers -- structural movement over the item list built during last frame's
    emission (see the file header).  All of them run at request time from nav_finish, mutate
    g_ctx->nav.id, and leave the request fields (move_dir / activate) live for the emission-time
    consumers (menu_begin's Right-opens-submenu, the activation in nav_item_register).
----------------------------------------------------------------------------------------------*/

/* Index of id in the list, or -1.  Linear: the list is small and resolved once per keypress. */
static i32
nav_list_find( gui_id_t id )
{
    if ( id == GUI_ID_NONE ) return -1;
    for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
        if ( g_ctx->nav.items[ i ].id == id )
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
    g_ctx->nav.id           = g_ctx->nav.items[ i ].id;
    g_ctx->nav.scroll_chase = true;
    if ( !vertical )
        g_ctx->nav.goal_set = false;
}

/* Tab / Shift+Tab: the next / previous placed item in emission order, wrapping.  Body lane only:
   chrome is the F6 lane's, so Tab from a title-bar button steps back into the content.  The scan
   is bounded by the list length, so an all-chrome frame simply leaves the cursor alone. */
static void
nav_resolve_tab( void )
{
    i32 n = (i32)g_ctx->nav.item_count;
    if ( n == 0 ) return;

    i32 step = ( g_ctx->nav.tab > 0 ) ? 1 : -1;
    i32 i    = nav_list_find( g_ctx->nav.id );
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
        if ( !g_ctx->nav.items[ i ].chrome )
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
    i32 i = nav_list_find( g_ctx->nav.body_id );
    if ( i >= 0 && !g_ctx->nav.items[ i ].chrome )
    {
        nav_adopt( i, false );
        return;
    }
    for ( u32 k = 0; k < g_ctx->nav.item_count; ++k )
        if ( !g_ctx->nav.items[ k ].chrome )
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
    i32 cur = nav_list_find( g_ctx->nav.id );
    if ( cur >= 0 && g_ctx->nav.items[ cur ].chrome )
    {
        nav_lane_body();
        return;
    }
    for ( u32 k = 0; k < g_ctx->nav.item_count; ++k )
        if ( g_ctx->nav.items[ k ].chrome )
        {
            g_ctx->nav.body_id = g_ctx->nav.id;
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
    for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &g_ctx->nav.items[ i ];
        if ( it->chrome || it->region != region || it->line != line ) continue;

        f32 x0 = it->rect.x, x1 = it->rect.x + it->rect.w;
        f32 d  = ( goal < x0 ) ? ( x0 - goal ) : ( ( goal > x1 ) ? ( goal - x1 ) : 0.0f );
        if ( best < 0 || d < best_d ) { best = (i32)i; best_d = d; }
    }
    return best;
}

/*----------------------------------------------------------------------------------------------
    Type-ahead -- jump the nav cursor to the first item (in the current nav window) whose stamped
    label starts with what was just typed, the native listbox/combobox behavior.  Feeds off
    s_io.text (already-composed printable text for the frame, gui_io.c), so it never runs while a
    text field owns the keyboard -- nav_new_frame returns before reaching it in that case -- and it
    respects the keyboard layout the OS text event already resolved instead of a raw A-Z scan.

    Query state lives on g_ctx->nav (type_buf/type_len/type_last_t/type_dirty); resolution runs
    from nav_finish, against the SAME one-frame-lagged item list every other resolver (Up/Down,
    Tab, Home/End) consumes, so it composes with them for free -- no separate registry to keep in
    sync with window/popup lifetime.
----------------------------------------------------------------------------------------------*/

/* No typing for this long resets the query instead of extending it -- so "cat" typed slowly reads
   as three separate one-letter jumps (c, then a, then t), not a failed four-letter prefix. */
#define GUI_TYPEAHEAD_TIMEOUT 1.0

/* True when last frame's item list (the same one-frame-lagged list the resolver scans) has at
   least one labeled, non-chrome candidate -- i.e. type-ahead could actually land somewhere in the
   current nav window.  A window with no selectables (a form of sliders/checkboxes/text fields,
   like the font-tool bench) has none, so there is nothing for a typed letter to ever match; without
   this check nav_typeahead_feed still claimed the key and raised nav.highlight purely because a
   window was focused, silently eating every letter keystroke -- including app-level hotkeys like
   'P' -- for a search that could never succeed. */
static bool
nav_has_typeahead_targets( void )
{
    for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &g_ctx->nav.items[ i ];
        if ( !it->chrome && it->label[ 0 ] != 0 )
            return true;
    }
    return false;
}

/* Accumulate this frame's typed text into the query.  A repeated single key with no pause is the
   Explorer-style cycle case: the query stays one character (nav_resolve_typeahead then scans past
   the current cursor instead of from the top), so tapping "m" repeatedly steps through every M
   instead of re-landing on the first one. */
static void
nav_typeahead_feed( const char* text )
{
    if ( !text[ 0 ] ) return;
    if ( !nav_has_typeahead_targets() ) return;   /* nothing here can match -- let the key fall through */
    gui_nav_state_t* nav = &g_ctx->nav;

    bool first = true;
    for ( const char* ch = text; *ch; ++ch )
    {
        char c = *ch;
        if ( c >= 'A' && c <= 'Z' ) c = (char)( c + 32 );
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) ) continue;

        bool timed_out   = first && ( ( s_io.time - nav->type_last_t ) > GUI_TYPEAHEAD_TIMEOUT );
        bool repeat_same = first && !timed_out && nav->type_len == 1 && nav->type_buf[ 0 ] == c;
        if ( timed_out || repeat_same )
            nav->type_len = 0;
        first = false;

        if ( nav->type_len < (u32)sizeof( nav->type_buf ) - 1 )
            nav->type_buf[ nav->type_len++ ] = c;

        /* This character is now nav's -- claim the key edge so the same press cannot also fall
           through to an app-level hotkey later in the frame (tier 2 of the model in
           gui_want_capture_keyboard). */
        app_key_t k = ( c >= 'a' && c <= 'z' ) ? (app_key_t)( APP_KEY_A + ( c - 'a' ) )
                                                : (app_key_t)( APP_KEY_0 + ( c - '0' ) );
        key_claim( k );
    }
    if ( first ) return;   /* nothing usable in this chunk (punctuation / control chars only) */

    nav->type_buf[ nav->type_len ] = 0;
    nav->type_last_t = s_io.time;
    nav->type_dirty  = true;

    /* Every other key path (arrows, Tab, Home/End, ...) flips these in nav_new_frame right before
       nav_finish runs its resolver; nav_adopt itself never does.  Without this the cursor still
       moves internally, but nav_item_register gates the ring, the fill, AND the scroll-into-view
       chase behind active/highlight, so a typed jump would silently have no visible effect. */
    nav->active    = true;
    nav->highlight = true;
}

/* Case-sensitive prefix compare against an already-lowercased label (nav_item_stamp_label lower-
   cased it at registration; the query is lowercased as it accumulates in nav_typeahead_feed). */
static bool
nav_label_has_prefix( const char* label, const char* query, u32 qlen )
{
    for ( u32 i = 0; i < qlen; ++i )
        if ( label[ i ] != query[ i ] ) return false;
    return true;
}

/* Resolve this frame's query against last frame's item list.  A single-char query (the repeat-
   cycle case) scans starting just past the current cursor, wrapping around, so a held/tapped key
   steps forward through every match; any other query scans the whole list from the top, matching
   plain "type a few letters, land on it" expectation. */
static void
nav_resolve_typeahead( void )
{
    gui_nav_state_t* nav = &g_ctx->nav;
    nav->type_dirty = false;
    if ( nav->type_len == 0 ) return;

    bool cycle = ( nav->type_len == 1 );
    i32  cur   = cycle ? nav_list_find( nav->id ) : -1;
    u32  n     = nav->item_count;

    for ( u32 k = 0; k < n; ++k )
    {
        u32 i = cycle ? ( (u32)( cur + 1 + (i32)k ) % n ) : k;
        const gui_nav_item_t* it = &nav->items[ i ];
        if ( it->chrome || it->label[ 0 ] == 0 ) continue;
        if ( nav_label_has_prefix( it->label, nav->type_buf, nav->type_len ) )
        {
            nav_adopt( (i32)i, false );
            return;
        }
    }
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
    i32  cur        = nav_list_find( g_ctx->nav.id );
    bool use_region = ( cur >= 0 && !g_ctx->nav.items[ cur ].chrome );
    u32  region     = use_region ? g_ctx->nav.items[ cur ].region : 0;

    i32 pick = -1;
    for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &g_ctx->nav.items[ i ];
        if ( it->chrome ) continue;
        if ( use_region && it->region != region ) continue;
        pick = (i32)i;
        if ( g_ctx->nav.home < 0 ) break;   /* Home: the first hit; End keeps scanning to the last */
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
    if ( g_ctx->nav.item_count == 0 ) return;

    i32 cur = nav_list_find( g_ctx->nav.id );
    if ( cur < 0 )
    {
        for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
            if ( !g_ctx->nav.items[ i ].chrome ) { nav_adopt( (i32)i, false ); return; }
        return;
    }
    if ( g_ctx->nav.items[ cur ].chrome ) return;
    const gui_nav_item_t c = g_ctx->nav.items[ cur ];

    gui_window_t* nw   = window_find( g_ctx->nav.win );
    f32           page = nw ? nw->h : 0.0f;
    if ( page <= 0.0f ) page = 10.0f * WIDGET_H;

    if ( !g_ctx->nav.goal_set )
    {
        g_ctx->nav.goal_x   = c.rect.x + c.rect.w * 0.5f;
        g_ctx->nav.goal_set = true;
    }

    bool down   = ( g_ctx->nav.page > 0 );
    f32  cy     = c.rect.y + c.rect.h * 0.5f;
    f32  want   = cy + ( down ? page : -page );
    u32  t_line = 0;
    f32  t_d    = 0.0f;
    bool found  = false;
    for ( u32 i = 0; i < g_ctx->nav.item_count; ++i )
    {
        const gui_nav_item_t* it = &g_ctx->nav.items[ i ];
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

    i32 pick = nav_line_pick( c.region, t_line, g_ctx->nav.goal_x );
    if ( pick >= 0 )
        nav_adopt( pick, true );
}

/* Chrome-lane step: the cursor sits on a chrome item (F6, or a click on a title-bar button).
   Left/Right step to the nearest chrome item by x-center -- emission order is wrong here (the
   close button emits before the detach box but sits right of it), and the strip is one visual
   band, so geometry IS its order.  Down drops back into the body; Up and the strip ends are
   walls. */
static void
nav_move_chrome( i32 cur )
{
    gui_nav_state_t* nav = &g_ctx->nav;

    if ( nav->move_dir == GUI_DIR_LEFT || nav->move_dir == GUI_DIR_RIGHT )
    {
        bool right  = ( nav->move_dir == GUI_DIR_RIGHT );
        f32  cx     = nav->items[ cur ].rect.x + nav->items[ cur ].rect.w * 0.5f;
        i32  best   = -1;
        f32  best_d = 0.0f;
        for ( u32 i = 0; i < nav->item_count; ++i )
        {
            const gui_nav_item_t* it = &nav->items[ i ];
            if ( !it->chrome || (i32)i == cur ) continue;
            f32 ix = it->rect.x + it->rect.w * 0.5f;
            f32 d  = right ? ( ix - cx ) : ( cx - ix );
            if ( d <= 0.0f ) continue;                    /* not on the requested side */
            if ( best < 0 || d < best_d ) { best = (i32)i; best_d = d; }
        }
        if ( best >= 0 )
            nav_adopt( best, false );
    }
    else if ( nav->move_dir == GUI_DIR_DOWN )
    {
        nav_lane_body();
    }
}

/* Body-lane horizontal step: the neighboring placed item on the cursor's own (region, line),
   ordinal.  A line end is a wall -- a horizontal move can never read as a vertical jump. */
static void
nav_move_horizontal( i32 cur, gui_nav_item_t c )
{
    gui_nav_state_t* nav = &g_ctx->nav;

    i32 step = ( nav->move_dir == GUI_DIR_LEFT ) ? -1 : 1;
    for ( i32 i = cur + step; i >= 0 && i < (i32)nav->item_count; i += step )
    {
        if ( nav->items[ i ].chrome ) continue;
        if ( nav->items[ i ].region != c.region || nav->items[ i ].line != c.line )
            break;                       /* ran off the line -- wall */
        nav_adopt( i, false );
        return;
    }
}

/* Body-lane vertical step (Up/Down).  Choose the target LINE geometrically over the structural
   lines the layout engine built: every line whose y band sits strictly beyond the cursor line's
   competes by edge gap, with the goal column breaking near-ties -- so within one region the
   adjacent row always wins (rows stack; the gap is decisive and Down then Up returns to the
   start), a child's rows slot in where the child visually sits, and side-by-side regions whose
   rows share heights resolve to the region the goal column is over rather than whichever emitted
   later.  The landing item within the target line is the goal-column pick. */
static void
nav_move_vertical( gui_nav_item_t c )
{
    gui_nav_state_t* nav  = &g_ctx->nav;
    bool             down = ( nav->move_dir == GUI_DIR_DOWN );

    /* Anchor the goal column on the first step of a run. */
    if ( !nav->goal_set )
    {
        nav->goal_x   = c.rect.x + c.rect.w * 0.5f;
        nav->goal_set = true;
    }

    /* The cursor line's y band -- the union of its items' extents, so a short label and a tall
       control on one row read as one band and neither can be "beyond" its own row. */
    f32 band_y0 = c.rect.y, band_y1 = c.rect.y + c.rect.h;
    for ( u32 i = 0; i < nav->item_count; ++i )
    {
        const gui_nav_item_t* it = &nav->items[ i ];
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
    for ( u32 i = 0; i < nav->item_count; ++i )
    {
        const gui_nav_item_t* it = &nav->items[ i ];
        if ( it->chrome ) continue;
        if ( it->region == c.region && it->line == c.line ) continue;   /* our own line */

        f32 iy0 = it->rect.y, iy1 = it->rect.y + it->rect.h;
        f32 gap;
        if ( down ) { if ( iy0 < band_y1 - NAV_BAND_EPS ) continue; gap = iy0 - band_y1; }
        else        { if ( iy1 > band_y0 + NAV_BAND_EPS ) continue; gap = band_y0 - iy1; }
        if ( gap < 0.0f ) gap = 0.0f;

        bool ov = ( nav->goal_x >= it->rect.x && nav->goal_x <= it->rect.x + it->rect.w );

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

    i32 pick = nav_line_pick( t_region, t_line, nav->goal_x );
    if ( pick >= 0 )
        nav_adopt( pick, true );
}

/* Directional move: dispatch the arrow keys to the chrome lane, the body horizontal step, or the
   body vertical step.  No cursor yet lands on the first placed item rather than stepping from
   nowhere. */
static void
nav_resolve_move( void )
{
    gui_nav_state_t* nav = &g_ctx->nav;
    if ( nav->item_count == 0 ) return;

    i32 cur = nav_list_find( nav->id );
    if ( cur < 0 )
    {
        for ( u32 i = 0; i < nav->item_count; ++i )
            if ( !nav->items[ i ].chrome ) { nav_adopt( (i32)i, false ); return; }
        return;
    }

    if ( nav->items[ cur ].chrome )
    {
        nav_move_chrome( cur );
        return;
    }

    gui_nav_item_t c = nav->items[ cur ];   /* adoption rewrites nav->id only; copy for clarity */
    if ( nav->move_dir == GUI_DIR_LEFT || nav->move_dir == GUI_DIR_RIGHT )
        nav_move_horizontal( cur, c );
    else
        nav_move_vertical( c );
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
    return window_find( mb ) ? mb : GUI_ID_NONE;
}

/* Enter menu-bar mode on `bar` with the first entry highlighted; remember the prior nav target so
   Alt can toggle straight back to it. */
static void
nav_menu_enter( gui_id_t bar )
{
    g_ctx->nav.prev_win   = g_ctx->nav.focused_win;
    g_ctx->nav.prev_id    = g_ctx->nav.id;    /* remember the focus to toggle back to */
    g_ctx->nav.bar_win    = bar;
    g_ctx->nav.in_menus   = false;
    g_ctx->nav.menu_owner = GUI_ID_NONE;
    g_ctx->nav.id         = GUI_ID_NONE;   /* first bar entry takes focus */
    g_ctx->nav.active     = true;
    g_ctx->nav.highlight  = true;            /* Alt makes the keyboard the active instrument */
}

/* Leave menu-bar mode: close the menu popups and restore nav to exactly where it was before Alt. */
static void
nav_menu_exit( void )
{
    g_ctx->popup.open_count   = 0;                 /* drop the open menus */
    g_ctx->nav.bar_win    = GUI_ID_NONE;
    g_ctx->nav.in_menus   = false;
    g_ctx->nav.menu_owner = GUI_ID_NONE;
    g_ctx->nav.focused_win    = g_ctx->nav.prev_win;
    g_ctx->nav.id         = g_ctx->nav.prev_id;   /* back to the last focus location */
}

/* Ascend from the open menus back to the bar entry that owns them (the close / Up-to-bar return),
   leaving its menu dropped so Left/Right can keep traversing the bar. */
static void
nav_menu_ascend_to_bar( void )
{
    g_ctx->nav.in_menus = false;
    g_ctx->nav.id       = g_ctx->nav.menu_owner;
    g_ctx->nav.move_dir = -1;                  /* consume the move that triggered the ascend */
}

/* Bar/menu key handling while in menu-bar mode.  `first_prev` is last frame's first placed item,
   used to detect "Up at the top of a dropdown".  Left/Right at the bar fall through to the list
   resolver (the bar entries share one line), and Right inside a menu is handled by menu_begin
   (open submenu). */
static void
nav_menu_keys( bool down, bool up, bool left, bool esc, gui_id_t first_prev )
{
    if ( !g_ctx->nav.in_menus )
    {
        /* On the bar: the highlighted entry drops its menu (menu_begin).  Down / Enter descend into
           it; Esc leaves menu mode.  Left/Right stay as a directional move for the list resolver. */
        if ( down || g_ctx->nav.activate )
        {
            if ( g_ctx->popup.open_count > 0 )      /* a menu is dropped -> step into it */
            {
                g_ctx->nav.menu_owner = g_ctx->nav.id;
                g_ctx->nav.in_menus   = true;
                g_ctx->nav.id         = GUI_ID_NONE;   /* first item */
            }
            g_ctx->nav.move_dir = -1;
            g_ctx->nav.activate = false;        /* do not also "click" (toggle-close) the bar entry */
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
        u32  depth       = g_ctx->popup.open_count;
        bool in_submenu  = ( depth >= 2 );
        bool at_menu_top = ( depth <= 1 && first_prev != GUI_ID_NONE && g_ctx->nav.id == first_prev );

        if ( up && at_menu_top )
        {
            nav_menu_ascend_to_bar();
        }
        else if ( left || esc )
        {
            if ( in_submenu )                  /* close a submenu, back to its parent menu */
            {
                --g_ctx->popup.open_count;
                g_ctx->nav.id = GUI_ID_NONE;
            }
            else                               /* top level: back to the owning bar entry */
            {
                nav_menu_ascend_to_bar();
            }
            g_ctx->nav.move_dir = -1;
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
    if ( g_ctx->nav.lane )
        nav_resolve_lane();
    else if ( g_ctx->nav.tab != 0 )
        nav_resolve_tab();
    else if ( g_ctx->nav.page != 0 )
        nav_resolve_page();
    else if ( g_ctx->nav.home != 0 )
        nav_resolve_home();
    else if ( g_ctx->nav.move_dir >= 0 )
        nav_resolve_move();

    if ( g_ctx->nav.type_dirty )
        nav_resolve_typeahead();

    g_ctx->nav.item_count = 0;   /* fresh list; this frame's items append during emission */
    nav_choose_window();
}

/*----------------------------------------------------------------------------------------------
    nav_new_frame -- the driver: recover, read keys into a request, resolve, choose nav_win.
    Called from gui_ctx_begin after popup_close_check / popup_apply_modal / window_raise_on_press.
----------------------------------------------------------------------------------------------*/

static void
nav_new_frame( void )
{
     /* feature boundary: gui_forward_caps_t.keyboard_nav; g_ctx->nav.win stays GUI_ID_NONE, 
        so nav_item_register never matches a window and mouse input is untouched */

    if ( !s_fwd_caps.keyboard_nav )
        return;                             /* not using the keyboard for nav */

    /* A deaf (non-listening) context takes no input.  s_io and the interaction record are shared by
       every context (foundation/gui_ctx.c), so a passive context must not read -- much less key_claim
       -- the keyboard from them, or it would steal keys from whichever context IS listening this
       frame (and, with per-key claim, starve it of the press entirely).  The nav-driver peer of the
       widget-level deaf gate (interact/gui_item.c) and the hover-nomination gate (surface/gui_surface.c). */
    if ( !g_ctx->listening )
        return;

    /* One-shot: only an adoption made THIS frame (recovery below, or a resolver in nav_finish)
       chases the cursor into view during the emission that follows. */
    g_ctx->nav.scroll_chase = false;

    /* Value-edit self-heal: the captured widget stopped emitting (window closed, tab switched) --
       drop the capture so the keyboard is not fenced on a ghost. */
    if ( g_ctx->nav.edit_id != GUI_ID_NONE && !g_ctx->nav.id_seen )
        g_ctx->nav.edit_id = GUI_ID_NONE;

    /* First-focus / recovery: nav is engaged but its cursor item was not emitted last frame
       (window just focused, popup opened, list shrank) -- land on the first placed item that was. */
    if ( g_ctx->nav.active && !g_ctx->nav.id_seen && g_ctx->nav.first_item != GUI_ID_NONE )
    {
        g_ctx->nav.id           = g_ctx->nav.first_item;
        g_ctx->nav.goal_set     = false;
        g_ctx->nav.scroll_chase = true;
    }

    /* Last frame's first placed item -- captured before the reset for the "Up at the top of a
       dropdown returns to the bar" test. */
    gui_id_t first_prev = g_ctx->nav.first_item;

    /* Reset the per-frame request + registration bookkeeping.  The item list itself stays intact:
       this frame's keys resolve against it in nav_finish, which then opens the fresh list. */
    g_ctx->nav.move_dir   = -1;
    g_ctx->nav.tab        = 0;
    g_ctx->nav.page       = 0;
    g_ctx->nav.home       = 0;
    g_ctx->nav.activate   = false;
    g_ctx->nav.lane       = false;
    g_ctx->nav.edit_dir   = 0;
    g_ctx->nav.mnemonic   = 0;
    g_ctx->nav.id_seen    = false;
    g_ctx->nav.first_item = GUI_ID_NONE;

    /* A move makes the mouse the active instrument: it drops nav_highlight, so the nav item loses
       its fill (the ring stays, via nav_active) and the mouse hover regains the fill -- the ring
       keeps marking the keyboard's last position in case the user goes back to it.  A click goes
       further: the user has committed to the mouse, so it drops nav_active too and the ring itself
       disappears (a later keyboard press brings it back at g_ctx->nav.id, unmoved).  A click additionally
       leaves menu-bar mode -- the user switched to the mouse to drive the menus (which then track
       the cursor); the open popups close on their own through popup_close_check. */
    bool mouse_moved = ( s_io.mouse_x != s_nav_mouse_x || s_io.mouse_y != s_nav_mouse_y );
    bool mouse_press = ( s_io.mouse_pressed[ 0 ] || s_io.mouse_pressed[ 1 ] || s_io.mouse_pressed[ 2 ] );
    s_nav_mouse_x = s_io.mouse_x;
    s_nav_mouse_y = s_io.mouse_y;

    if ( mouse_moved || mouse_press )
        g_ctx->nav.highlight = false;
    if ( mouse_press )
    {
        g_ctx->nav.active  = false;
        g_ctx->nav.edit_id = GUI_ID_NONE;   /* the mouse takes over -- release the value-edit capture */
        if ( g_ctx->nav.bar_win != GUI_ID_NONE )
        {
            g_ctx->nav.bar_win  = GUI_ID_NONE;
            g_ctx->nav.in_menus = false;
        }

        /* Click-to-focus: the clicked window becomes the keyboard-focused window, so the keyboard
           resumes where the mouse last worked.  This is what lets a DOCKED window take nav at
           all -- a click never raises a tile's z (window_raise_on_press leaves it tiled), so the
           front-most-by-z default can never reach it.  Popup-band records keep their own capture
           (nav_choose_window) and a frame-only native shell never takes the keyboard. */
        gui_window_t* clicked = window_find( s_interaction.hover_win );
        if ( clicked && nav_win_focusable( clicked ) )
            g_ctx->nav.focused_win = clicked->id;
        else if ( s_interaction.hover_win == GUI_ID_NONE )
            g_ctx->nav.focused_win = GUI_ID_NONE;   /* background/viewport click -- nothing is focused */
    }

    /* Menu mode self-heals: if its bar window is gone, drop out. */
    if ( g_ctx->nav.bar_win != GUI_ID_NONE && !window_find( g_ctx->nav.bar_win ) )
        nav_menu_exit();

    /* A focused text field owns the keyboard: nav reads nothing (Tab/arrows/Enter are the editor's,
       which releases focus on Enter/Esc -- gui_text_edit.c). */
    if ( s_interaction.focused_id != GUI_ID_NONE )
    {
        nav_finish();
        return;
    }

    /* A captured DRAG widget (slider / drag-box value edit) owns the keyboard the same way:
       Left/Right (repeat) step its value -- published as edit_dir, applied by the widget through
       gui_item_state_t.nav_adjust -- and Enter/Space/Esc release; every other nav key is fenced so
       Up/Down can never yank the cursor off a widget mid-edit. */
    if ( g_ctx->nav.edit_id != GUI_ID_NONE )
    {
        bool edit_release = ( s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ]
                              || s_io.keys_pressed[ APP_KEY_ESCAPE ] );
        if ( edit_release )
        {
            g_ctx->nav.edit_id = GUI_ID_NONE;
            key_claim( APP_KEY_ENTER );    /* the release is nav's -- no re-activate, no fall-through */
            key_claim( APP_KEY_SPACE );
            key_claim( APP_KEY_ESCAPE );
        }
        else
        {
            if ( s_io.keys_pressed_repeat[ APP_KEY_LEFT  ] ) { g_ctx->nav.edit_dir = -1; key_claim( APP_KEY_LEFT  ); }
            if ( s_io.keys_pressed_repeat[ APP_KEY_RIGHT ] ) { g_ctx->nav.edit_dir = +1; key_claim( APP_KEY_RIGHT ); }
            if ( g_ctx->nav.edit_dir != 0 )
            {
                g_ctx->nav.active    = true;   /* stepping keeps the keyboard the active instrument */
                g_ctx->nav.highlight = true;
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
        key_claim( APP_KEY_TAB );   /* nav's -- do not also fall through to a Tier-3 binding */
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
                g_ctx->nav.mnemonic  = (u8)( 'A' + c );  /* menu_begin matches + opens the entry */
                s_nav_alt_used      = true;
                g_ctx->nav.active    = true;
                g_ctx->nav.highlight = true;
                if ( g_ctx->nav.bar_win == GUI_ID_NONE )
                {
                    gui_id_t mb = nav_main_bar_win();
                    if ( mb != GUI_ID_NONE ) nav_menu_enter( mb );
                }
                key_claim( (app_key_t)( APP_KEY_A + c ) );   /* consumed -- no hotkey fallthrough */
                break;
            }

    if ( ( s_io.keys_released[ APP_KEY_LALT ] || s_io.keys_released[ APP_KEY_RALT ] )
         && !s_nav_alt_used )
    {
        if ( g_ctx->nav.bar_win != GUI_ID_NONE )
            nav_menu_exit();                      /* toggle out -> restore the prior focus */
        else
        {
            gui_id_t mb = nav_main_bar_win();
            if ( mb != GUI_ID_NONE ) nav_menu_enter( mb );
        }
    }

    /* Type-ahead: any printable text typed this frame narrows/jumps the nav cursor (see the
       nav_typeahead_feed comment above).  Not while Alt is held -- those letters are menu-bar
       mnemonics, handled above -- or while the menu bar owns the keyboard, where a bare letter
       has no jump-to-row target anyway (menu items do not opt into type-ahead).  Also requires a
       focused window (click / Ctrl+Tab / Alt-mnemonic) -- without this, a fallback front-most
       window with no user interaction would steal ordinary keystrokes meant for app-level hotkeys. */
    if ( !alt && g_ctx->nav.bar_win == GUI_ID_NONE && g_ctx->nav.focused_win != GUI_ID_NONE )
        nav_typeahead_feed( s_io.text );

    /* Arrows / Tab move (repeat so a held key keeps stepping); Enter/Space activate. */
    bool down  = s_io.keys_pressed_repeat[ APP_KEY_DOWN  ];
    bool up    = s_io.keys_pressed_repeat[ APP_KEY_UP    ];
    bool left  = s_io.keys_pressed_repeat[ APP_KEY_LEFT  ];
    bool right = s_io.keys_pressed_repeat[ APP_KEY_RIGHT ];
    bool esc   = s_io.keys_pressed[ APP_KEY_ESCAPE ];

    if ( up    ) g_ctx->nav.move_dir = GUI_DIR_UP;
    if ( down  ) g_ctx->nav.move_dir = GUI_DIR_DOWN;
    if ( left  ) g_ctx->nav.move_dir = GUI_DIR_LEFT;
    if ( right ) g_ctx->nav.move_dir = GUI_DIR_RIGHT;

    bool tab = s_io.keys_pressed_repeat[ APP_KEY_TAB ];
    if ( tab ) g_ctx->nav.tab = shift ? -1 : +1;

    /* Page keys: the scrollbar's keyboard face.  Page hops one view height (repeat, for holding
       through a long list); Home/End jump to the region's first / last item.  The scroll chase
       then brings the landing into view, so these read as page-scrolling with a cursor. */
    bool pgup = s_io.keys_pressed_repeat[ APP_KEY_PAGE_UP   ];
    bool pgdn = s_io.keys_pressed_repeat[ APP_KEY_PAGE_DOWN ];
    bool home = s_io.keys_pressed[ APP_KEY_HOME ];
    bool end  = s_io.keys_pressed[ APP_KEY_END  ];
    if ( pgup ) g_ctx->nav.page = -1;
    if ( pgdn ) g_ctx->nav.page = +1;
    if ( home ) g_ctx->nav.home = -1;
    if ( end  ) g_ctx->nav.home = +1;

    bool act = s_io.keys_pressed[ APP_KEY_ENTER ] || s_io.keys_pressed[ APP_KEY_SPACE ];
    if ( act ) g_ctx->nav.activate = true;

    /* F6 hops between the body and the chrome strip (title-bar buttons, dock tabs) -- the pane-
       cycle key.  Not while the menu bar owns nav: its bar entries are the chrome there. */
    bool lane = s_io.keys_pressed[ APP_KEY_F6 ] && g_ctx->nav.bar_win == GUI_ID_NONE;
    if ( lane ) g_ctx->nav.lane = true;

    /* Any nav key makes the keyboard the active instrument: show the ring (nav_active) AND the fill
       (nav_highlight), and suppress mouse hover until the mouse moves again. */
    if ( up || down || left || right || tab || act || lane || pgup || pgdn || home || end )
    {
        g_ctx->nav.active    = true;
        g_ctx->nav.highlight = true;
    }

    /* Claim the motion keys nav actually took, so the same press cannot also drive a Tier-3 binding
       (the per-key half of the model in gui_want_capture_keyboard).  Gated on there being a nav
       target this frame: an empty item list resolves nothing, so an arrow over a targetless window
       falls through instead of being silently eaten -- the same lesson as nav_has_typeahead_targets.
       Enter/Space are NOT claimed here; they are taken at the activation seam (nav_item_register,
       interact/gui_item.c), the only place that knows an item actually consumed the activation.  Esc
       is left to the popup / menu-bar branches below, which run under want_capture_keyboard's hard
       block -- the app is already fenced from every key there, so a per-key claim would be redundant. */
    if ( g_ctx->nav.item_count > 0 )
    {
        if ( up    ) key_claim( APP_KEY_UP        );
        if ( down  ) key_claim( APP_KEY_DOWN      );
        if ( left  ) key_claim( APP_KEY_LEFT      );
        if ( right ) key_claim( APP_KEY_RIGHT     );
        if ( tab   ) key_claim( APP_KEY_TAB       );
        if ( pgup  ) key_claim( APP_KEY_PAGE_UP   );
        if ( pgdn  ) key_claim( APP_KEY_PAGE_DOWN );
        if ( home  ) key_claim( APP_KEY_HOME      );
        if ( end   ) key_claim( APP_KEY_END       );
        if ( lane  ) key_claim( APP_KEY_F6        );
    }

    /* Menu-bar mode owns the bar/menu keys (traverse, descend, ascend-to-owner, Up-to-bar). */
    if ( g_ctx->nav.bar_win != GUI_ID_NONE )
    {
        nav_menu_keys( down, up, left, esc, first_prev );
    }
    /* Generic popup keyboard (mouse-opened menus, combos, context menus): Esc closes the top level,
       Left closes a submenu back to its parent. */
    else if ( g_ctx->popup.open_count > 0 )
    {
        bool close_top     = esc;
        bool close_submenu = ( g_ctx->nav.move_dir == GUI_DIR_LEFT && g_ctx->popup.open_count >= 2 );

        if ( close_top )
        {
            --g_ctx->popup.open_count;
            g_ctx->nav.id = GUI_ID_NONE;
        }
        else if ( close_submenu )
        {
            --g_ctx->popup.open_count;
            g_ctx->nav.move_dir = -1;
            g_ctx->nav.id       = GUI_ID_NONE;
        }
    }

    nav_finish();
}

/*==============================================================================================
    Public: The single public function, used to set (keyboard) nav target window by title.
    
    The next nav_new_frame will land on its first placed item (or the first placed item
    of the front-most window if the title is not found).

    window_set_nav -- aim keyboard nav at a window by title (the explicit-focus entry).
==============================================================================================*/

void
gui_window_set_nav( const char* title )
{
    g_ctx->nav.focused_win = title ? id_hash( title ) : GUI_ID_NONE;
    g_ctx->nav.id          = GUI_ID_NONE;   /* first item of the new window takes focus */
    g_ctx->nav.active    = true;
    g_ctx->nav.highlight = true;
}

// clang-format on
/*============================================================================================*/
