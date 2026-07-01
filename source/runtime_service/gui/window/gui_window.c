/*==============================================================================================

    runtime_service/gui/window/gui_window.c -- Persistent per-window state.

    Windows are keyed by id_hash(title).  On first appearance the registry seeds the record
    from any queued window_set_next_pos / _size (ONCE condition), falling back to a default
    60x60 origin and 240x320 size.  From then on this registry owns the window's position so
    it survives across frames -- the foundation for dragging (now) and for collapse / scroll /
    saved-layout state (later).

    Drag / resize / scrollbar interaction lives in gui_widget_window.c alongside
    window_begin / window_end, where the layout dimensions (title-bar height, padding) are in
    scope.  This file owns the table, the active drag mode, the in-flight drag and resize
    offsets, and raise-to-front -- the state those interactions read and write.

    Included by gui.c after gui_ctx.c so s_interaction (hover_win) and s_io are in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* gui_window_t and GUI_MAX_WINDOWS are defined early in gui.c so the bound context can hold
   the window pool by value.  The pool itself -- s_windows / s_window_count / s_window_scratch and
   the z dispenser s_z_counter -- are members of gui_context_t (gui_ctx.c), reached here through
   the g_ctx aliases; the pool is per-context retained.  This file owns only their behavior. */

/* Drag configuration + in-flight drag offset (mouse - window pos at grab time).
   The window currently being dragged is tracked via s_interaction.active_id == window id. */

static gui_win_drag_t     s_win_drag_mode = GUI_WIN_DRAG_TITLEBAR;
static f32                  s_drag_off_x, s_drag_off_y;

/* Merge-back edge latch.  A floater merges back into the main surface only on a genuine ENTER --
   the cursor crossing from outside the main window into it.  Without this, a floater spawned over
   its parent (cursor already inside the main rect) would merge back the instant it is grabbed,
   making it impossible to drag away.  s_vp_drag_id marks which window the latch belongs to so a
   fresh drag re-arms it; s_vp_merge_armed turns true once the cursor has been clear of the main
   window during this drag, gating the merge until then. */
static gui_id_t           s_vp_drag_id = GUI_ID_NONE;
static bool                 s_vp_merge_armed;

/* Native title-bar drag-threshold state: records a pending title-bar press until the cursor
   moves far enough to commit it as a drag (vs. a click or a double-click).  Prevents the OS
   modal move loop (frame_only) or active_id set (floater) from triggering on click-1 of a
   double-click attempt, which would swallow click-2 before gui can test mouse_double. */
#define TITLEBAR_DRAG_THRESH 4.0f
static bool       s_titlebar_drag_pending;
static bool       s_titlebar_drag_os;     /* true: dispatch to OS (frame_only); false: gui drag (floater) */
static win_id_t   s_titlebar_drag_os_id;  /* OS win_id for frame_only dispatch */
static gui_id_t s_titlebar_drag_gui;  /* gui window id -- guards threshold check to the right window */
static f32        s_titlebar_drag_px;
static f32        s_titlebar_drag_py;

/* In-flight edge resize.  The window being resized holds active_id == (id ^ RESIZE_SALT).
   The s_resize_* in-flight state (edges, offsets, far-edge pins) is owned by the shared
   resize mechanism in core/gui_resize.c -- a resizeable child_begin (gui_layout_child.c)
   reads it too, so it cannot live here; only the window-record apply / grab / fit stay in
   gui_widget_window.c. */

/*----------------------------------------------------------------------------------------------
    window_get -- find the window for this id, or create it from the initial geometry.
    Never returns NULL; an overflowing table falls back to a transient scratch entry.
----------------------------------------------------------------------------------------------*/

static gui_window_t*
window_get( gui_id_t id, f32 x, f32 y, f32 w, f32 h )
{
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == id )
            return &s_windows[ i ];

    /* First time seen: seed from the caller's initial geometry.  z is left 0 here;
       window_begin_ex stamps a fresh z on every appearance (first frame and re-opens),
       so new and re-opened windows always land on top with no two starting at the same z. */
    gui_window_t* win = ( s_window_count < g_ctx->max_windows )
                        ? &s_windows[ s_window_count++ ]
                        : &s_window_scratch;   /* table full: transient, not persisted */
    win->id        = id;
    win->x         = x;
    win->y         = y;
    win->w         = w;
    win->h         = h;
    win->z         = 0;
    win->viewport  = s_build.cur_viewport;   /* inherit ambient; window_set_next_viewport overrides */
    win->collapsed = false;   /* reset matters only for a reused scratch slot */
    win->closed    = false;   /* a freshly seen window starts open                */
    win->reopen_floater   = false;   /* not a re-opening floater until one is closed */
    win->reopen_maximized = false;

    /* Next-window state for a fresh window: never begun (so the first begin is "appearing"), and
       ONCE / ALWAYS permitted but APPEARING withheld -- window_begin grants APPEARING only on the
       frames the window actually (re)appears.  Reset here so a reused scratch slot starts clean. */
    win->last_frame     = 0u;
    win->set_pos_allow  = (u8)( GUI_COND_ONCE | GUI_COND_ALWAYS );
    win->set_size_allow = (u8)( GUI_COND_ONCE | GUI_COND_ALWAYS );
    return win;
}

/*----------------------------------------------------------------------------------------------
    window_find -- locate an existing window record by id, or NULL.  Unlike window_get this never
    creates one; used by the post-build reconcile (viewport_update) to reach the window a
    tear-off / merge-back gesture named, where creating a phantom record would be wrong.
----------------------------------------------------------------------------------------------*/

static gui_window_t*
window_find( gui_id_t id )
{
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == id )
            return &s_windows[ i ];
    return NULL;
}

/*----------------------------------------------------------------------------------------------
    Next-window channel -- queued geometry for the next window_begin, consumed and cleared by it.

    window_set_next_pos / window_set_next_size write here; the following window_begin applies each
    field to its target window per the field's condition (gui_cond_t), then clears the channel.
    This decouples the value from when it is applied -- the reason the geometry is a side channel
    rather than fixed window_begin parameters.  Only the next window is affected; an unconsumed
    queue (no window_begin follows) simply carries to whichever window is begun next.
----------------------------------------------------------------------------------------------*/

static struct
{
    bool         has_pos, has_size;     /* a value is queued on this axis */
    gui_cond_t pos_cond, size_cond;   /* when to apply it               */
    f32          pos_x, pos_y;
    f32          size_w, size_h;

    bool         has_viewport;          /* a viewport reassignment is queued for the next window */
    u32          viewport;              /* its target surface index                              */

} s_next_win;

/*----------------------------------------------------------------------------------------------
    Tear-off / merge-back request

    A window dragged by its title bar and released with the cursor outside its host surface's
    client bounds changes which surface hosts it: from the main surface (viewport 0) it tears off
    into a fresh floater; from a floater it merges back to the main surface.  window_begin_ex (in
    gui_widget_window.c) detects the released-outside gesture and fills this slot; the post-build
    reconcile gui_viewport_update (gui_frame.c) services it -- the safe point to create
    or destroy a surface, since the build is complete and no draw list is mid-flight.

    A single slot suffices: only one window can own the drag (active_id) at a time.  `title` is the
    dragged window's title string, borrowed for the same frame to name the spawned OS window (the
    immediate-mode same-frame lifetime makes this safe -- it is consumed before the frame ends).
----------------------------------------------------------------------------------------------*/

static struct
{
    bool        active;     /* a request is queued this frame                                   */
    bool        by_drag;    /* true = seamless title-bar drag; false = detach-button click       */
    gui_id_t  win_id;     /* the dragged window record                                        */
    u32         from_vp;    /* surface it was on (0 = main -> tear off; else floater -> merge)   */
    const char* title;      /* window title, to label the spawned floater's OS window           */

    bool        has_home;   /* re-open of a closed floater: the spawn reads RESTORE geometry +    */
                            /* maximized state from the window record (home_*, restore_*,         */
                            /* reopen_maximized) instead of the cursor / main-relative default.   */
} s_vp_request;

void
gui_window_set_next_pos( f32 x, f32 y, gui_cond_t cond )
{
    s_next_win.has_pos  = true;
    s_next_win.pos_cond = cond ? cond : GUI_COND_ALWAYS;   /* unset cond -> force */
    s_next_win.pos_x    = x;
    s_next_win.pos_y    = y;
}

void
gui_window_set_next_size( f32 w, f32 h, gui_cond_t cond )
{
    s_next_win.has_size  = true;
    s_next_win.size_cond = cond ? cond : GUI_COND_ALWAYS;
    s_next_win.size_w    = w;
    s_next_win.size_h    = h;
}

/* Queue the surface the NEXT window_begin paints into.  Sticky: it lands on the window record and
   persists across frames until reassigned.  Omit to use the ambient viewport (most recently emitted).
   Invalid or negative vp is treated as the primary (0). */
void
gui_window_set_next_viewport( gui_vp_t vp )
{
    s_next_win.has_viewport = true;
    s_next_win.viewport     = ( vp >= 0 ) ? (u32)vp : 0u;
}

/* Resolve one queued axis against the window's remaining permissions.  Returns whether to apply
   the value this frame; on a match it consumes the one-shot conditions (keeping only ALWAYS, so a
   forced value keeps firing while ONCE / APPEARING fire just the once). */
static bool
window_cond_apply( u8* allow, gui_cond_t cond )
{
    if ( !( (u8)cond & *allow ) ) return false;
    *allow &= (u8)GUI_COND_ALWAYS;
    return true;
}

/* Apply (and clear) the next-window channel onto win.  `appearing` gates the one-shot APPEARING
   permission: granted on (re)appearance frames and withheld otherwise, so an APPEARING-conditioned
   value fires on exactly those frames and never on a steady-state one. */
static void
window_apply_next( gui_window_t* win, bool appearing )
{
    if ( appearing )
    {
        win->set_pos_allow  |= (u8)GUI_COND_APPEARING;
        win->set_size_allow |= (u8)GUI_COND_APPEARING;
    }
    else
    {
        win->set_pos_allow  &= (u8)~GUI_COND_APPEARING;
        win->set_size_allow &= (u8)~GUI_COND_APPEARING;
    }

    if ( s_next_win.has_pos && window_cond_apply( &win->set_pos_allow, s_next_win.pos_cond ) )
    {
        win->x = s_next_win.pos_x;
        win->y = s_next_win.pos_y;
    }
    if ( s_next_win.has_size && window_cond_apply( &win->set_size_allow, s_next_win.size_cond ) )
    {
        win->w = s_next_win.size_w;
        win->h = s_next_win.size_h;
    }

    /* Viewport reassignment is unconditional (no ONCE/ALWAYS/APPEARING) -- it simply lands and
       sticks until the next window_set_next_viewport on this window. */
    if ( s_next_win.has_viewport )
        win->viewport = s_next_win.viewport;

    s_next_win.has_pos = s_next_win.has_size = s_next_win.has_viewport = false;   /* the queue targets only the next window */
}

/*----------------------------------------------------------------------------------------------
    window_raise_on_press -- a press brings the window under the cursor to the front.

    hover_win (the window the cursor is over) was resolved last frame, so this runs at the
    top of the frame -- before any window_begin stamps its z -- and the raise therefore
    takes effect this same frame: clicking a window's exposed area brings it up at once.
    Called from gui_ctx_begin() right after ctx_new_frame() promotes hover_win.
----------------------------------------------------------------------------------------------*/

static void
window_raise_on_press( void )
{
    /* Either button raises: left for the normal click/drag, middle for the convenience move
       grab (gui_widget.c window_end), so a middle-grabbed window also comes to the front. */
    if ( ( !s_io.mouse_pressed[ 0 ] && !s_io.mouse_pressed[ 2 ] )
         || s_interaction.hover_win == GUI_ID_NONE )
        return;

    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == s_interaction.hover_win )
        {
            /* A frame-only native shell (GUI_WIN_NATIVE) is the borderless viewport's backdrop
               frame: it must stay behind the windows living inside it, so it never raises. */
            if ( s_windows[ i ].flags & GUI_WIN_NATIVE )
                break;

            /* A docked window is tiled by its node, not stacked: it draws in a low z band behind the
               free-floating windows and a click must never reorder the tile.  Leave its z untouched. */
            if ( dock_find_window_node( s_windows[ i ].id ) )
                break;

            /* Already on top?  Don't burn a z value. */
            if ( s_windows[ i ].z != s_z_counter )
                s_windows[ i ].z = ++s_z_counter;
            break;
        }
}

/*----------------------------------------------------------------------------------------------
    window_set_drag -- select the global drag mode; call between frames.
----------------------------------------------------------------------------------------------*/

void
gui_window_set_drag( gui_win_drag_t mode )
{
    s_win_drag_mode = mode;
}

/*----------------------------------------------------------------------------------------------
    Closeable windows -- open / query a window's hidden state by title.

    A CLOSEABLE window's close (X) button sets win->closed, hiding the window until the host
    re-opens it.  These reach the record by id_hash(title) -- the same key window_begin uses --
    so the host can drive the open state from a button without holding its own flag.  A window
    that has never been begun has no record yet; window_set_open then no-ops (it already opens
    by default on first begin) and window_is_open reports it open.
----------------------------------------------------------------------------------------------*/

void
gui_window_set_open( const char* title, bool open )
{
    gui_window_t* win = window_find( id_hash( title ) );
    if ( win )
        win->closed = !open;
}

bool
gui_window_is_open( const char* title )
{
    gui_window_t* win = window_find( id_hash( title ) );
    return !win || !win->closed;
}

// clang-format on
/*============================================================================================*/
