/*==============================================================================================

    runtime_service/gui/core/gui_surface.c -- The surface service: window records as
    placed, stacked, occluding rectangles -- no layout, no chrome, no gestures.

    Tier 1 owns what a window IS before anything is drawn in it: the persistent record
    (position, size, z, open state, host viewport), the placement channel that seeds it, the
    z dispenser that stacks it, the hover-win contest that resolves which surface the cursor
    is over, and the request slot that moves a window between OS surfaces.  Everything a
    window DOES -- title-bar drags, resize grips, collapse, tear-off gestures, chrome paint --
    is window/ policy layered on these services.

    Per the house pattern the STORAGE lives with the context (core/gui_ctx.c: the window pool
    g_ctx->win.*, the z dispenser g_ctx->win.z_counter, and the hover nominee fields in
    s_interaction that frame turnover promotes to hover_win); this file owns the behavior over it.
    The OS half of the surface story -- the viewport records and their open/close lifecycle --
    stays with the frame orchestrator, since creating a surface is an app()/rhi() operation the
    tiers never perform.

    Included by gui_core.c after the ambient records (gui_ctx.c) -- a root region enters the same
    hover contest a window does, so the contest sits in the server both reach through the
    gui_core.h seams.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The window record door -- find the record for this id, or create it from the caller's initial
    geometry.  Never returns NULL: a full pool retires the oldest dormant slot, and a pool with
    nothing retirable falls back to a shared transient scratch entry.
==============================================================================================*/

gui_window_t*
window_get( gui_id_t id, f32 x, f32 y, f32 w, f32 h )
{
    /* Existing window found */
    for ( u32 i = 0; i < g_ctx->win.count; ++i ) {
        if ( g_ctx->win.pool[ i ].id == id )
            return &g_ctx->win.pool[ i ];
    }

    /* First time seen: seed from the caller's initial geometry.  z is left 0 here;
       window_begin_ex stamps a fresh z on every appearance (first frame and re-opens),
       so new and re-opened windows always land on top with no two starting at the same z. */

    gui_window_t* win;
    if ( g_ctx->win.count < g_ctx->win.max )
    {
        win = &g_ctx->win.pool[ g_ctx->win.count++ ];   /* free slot: append */
    }
    else
    {
        /* Pool full.  The pool is append-only, so it accumulates a slot for EVERY window id ever
           begun -- a UI with many entry points (menus, tools) but only a handful open at once will
           exhaust it even though the live set is small.  Reclaim the least-recently-begun DORMANT
           slot instead of thrashing on the shared scratch: retire that window, losing only its
           persisted geometry/scroll (re-seeded from the caller's initial values on its next visit);
           every cross-frame reference to it is by id (window_find, dock tabs, nav) and re-resolves.

           Two slots are off-limits as candidates:
             - last_frame == this frame: begun already this frame, so it (or a parent whose nested
               popup/child triggered this call) is live -- evicting it would corrupt the active
               window.  This is the correctness fence.
             - viewport != 0: owns an OS floater/native surface; the viewport reconcile tears that
               surface down once no record targets it, so retiring it here would drop a live window.
           When NOTHING is evictable, more than max_windows windows are genuinely live on the main
           surface this frame -- the true "raise max_windows" case -- so fall back to scratch + a
           break-once assert (same treatment as the backend render-slot overflow). */
        u32 victim  = g_ctx->win.max;   /* == none found */
        u32 oldest  = g_ctx->retained.frame;
        for ( u32 i = 0; i < g_ctx->win.count; ++i )
        {
            gui_window_t* c = &g_ctx->win.pool[ i ];
            if ( c->last_frame == g_ctx->retained.frame || c->viewport != 0 )
                continue;
            if ( victim == g_ctx->win.max || c->last_frame < oldest )
            {
                victim = i;
                oldest = c->last_frame;
            }
        }

        if ( victim != g_ctx->win.max )
        {
            win = &g_ctx->win.pool[ victim ];   /* retire the oldest dormant window in place */
        }
        else
        {
            GUI_WARN_ONCE( "more than %u windows live on the main surface this frame -- "
                           "extra windows share one transient scratch slot and lose persisted "
                           "state.  Raise gui_config.max_windows.\n", g_ctx->win.max );
            ORB_ASSERT_MSG_ONCE( false, "gui window pool overflow -- more than max_windows windows "
                                        "live at once; extras fall back to a shared scratch slot. "
                                        "Raise gui_config.max_windows" );
            win = &g_ctx->win.scratch;   /* all slots live: transient, not persisted */
        }
    }
    win->id        = id;
    win->x         = x;
    win->y         = y;
    win->w         = w;
    win->h         = h;
    win->z         = 0;
    win->viewport  = s_build.win.viewport;   /* inherit ambient; window_set_next_viewport overrides */
    win->overlay   = false;   /* a normal window until the popup layer stamps otherwise */
    win->collapsed = false;   /* reset matters only for a reused scratch slot */
    win->closed    = false;   /* a freshly seen window starts open                */
    win->reopen.floater   = false;   /* not a re-opening floater until one is closed */
    win->reopen.maximized = false;

    /* Next-window state for a fresh window: never begun (so the first begin is "appearing"), and
       ONCE / ALWAYS permitted but APPEARING withheld -- window_begin grants APPEARING only on the
       frames the window actually (re)appears.  Reset here so a reused scratch slot starts clean. */
    win->last_frame     = 0u;
    win->set_pos_allow  = (u8)( GUI_COND_ONCE | GUI_COND_ALWAYS );
    win->set_size_allow = (u8)( GUI_COND_ONCE | GUI_COND_ALWAYS );
    return win;
}

/* Locate an existing record by id, or NULL -- unlike window_get this never creates one.  The
   post-build reconcile (viewport_update) reads through it to reach the window a tear-off /
   merge-back gesture named, where creating a phantom record would be wrong.  A GUI_ID_NONE query
   is "no window" and short-circuits: no real record ever carries that id. */

gui_window_t*                  /* non-static: a cross-unit seam (core/gui_ctx.h) */
window_find( gui_id_t id )
{
    return window_find_in( g_ctx, id );
}

/* window_find against an EXPLICIT context rather than whichever is bound -- gui_viewport_update
   needs this: a tear-off request enqueued while a secondary context was bound (window_begin_ex,
   native_popin_request) must resolve against THAT context's window pool, since by the time the
   reconcile runs later in the frame g_ctx has typically rebound to the primary. */
gui_window_t*                  /* non-static: a cross-unit seam (core/gui_ctx.h) */
window_find_in( gui_context_t* ctx, gui_id_t id )
{
    if ( id == GUI_ID_NONE || !ctx ) return NULL;
    for ( u32 i = 0; i < ctx->win.count; ++i )
        if ( ctx->win.pool[ i ].id == id )
            return &ctx->win.pool[ i ];
    return NULL;
}

/*==============================================================================================
    Next-window channel -- queued geometry for the next window_begin, consumed and cleared by it.

    window_set_next_pos / window_set_next_size write here; the following window_begin applies each
    field to its target window per the field's condition (gui_cond_t), then clears the channel.
    This decouples the value from when it is applied -- the reason the geometry is a side channel
    rather than fixed window_begin parameters.  Only the next window is affected; an unconsumed
    queue (no window_begin follows) simply carries to whichever window is begun next.
==============================================================================================*/

gui_next_win_t s_next_win;   /* TYPE in core/gui_ctx.h (extern for the chrome unit) */

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
   GUI_VP_INVALID is treated as the primary (0). */
void
gui_window_set_next_viewport( gui_vp_t vp )
{
    s_next_win.has_viewport = true;
    s_next_win.viewport     = ( vp != GUI_VP_INVALID ) ? vp : 0u;
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
void
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

/*==============================================================================================
    The z band map -- every stacked entity competes in ONE contest (the hover nomination below +
    the draw sort) keyed on a plain u32 z, so the bands are pure ordering policy, all authored
    here:

        0x00000000   GUI_REGION_BG_Z    background regions -- tie the docked/base window floor
        1 ..         (dispenser)        normal windows + floating dock groups (surface_z_raise)
        0x40000000   GUI_REGION_Z       default root-region band: over windows, under popups
        0x80000000+  GUI_Z_OVERLAY      overlay band: popup depth d at OVERLAY + d, tooltip above
        0xF0000000   GUI_REGION_FG_Z    foreground regions -- above every popup depth
        0xF8000000   DOCK_OVERLAY_Z     drag-to-dock drop overlay (gui_dock_drag.c) -- topmost,
                                        in its own synthetic slot so nothing ties or outdraws it

    The dispenser never climbs anywhere near the fixed bands.  A record placed in the overlay
    band also carries win->overlay -- the TYPE fact ("an anchored overlay, not a window") the
    nav / dock / native tests key on -- so z itself stays pure paint order.  The four band macros
    live in core/gui_core.h, since the flow unit's root region and the popup layer stamp the same
    bands; the two verbs below are the only authors of a z VALUE.  Nothing outside this file
    touches g_ctx->win.z_counter or the constants raw.
==============================================================================================*/

/* Bring a stacked entity to the front: a fresh top-of-stack z, or its own z unchanged when it is
   already the most recently raised (no value burned re-raising the top).  The dispenser is
   monotonic and shared by windows, floating dock groups, and appearing windows alike, so every
   raise lands strictly above everything raised before it. */
u32
surface_z_raise( u32 z )
{
    /* 0 is the never-dispensed seed every fresh record starts at -- many entities share it, so
       it is never "already top": always dispense (the counter's first value is 1).  A NONZERO z
       equal to the counter is the unique most-recently-raised holder (dispensed values are held
       by one entity at a time), so only then is the re-raise skipped. */
    return ( z != 0u && z == g_ctx->win.z_counter ) ? z : ++g_ctx->win.z_counter;
}

/* z for an overlay-band occupant at `depth`: popups stack parent -> child, the tooltip sits at
   the maximum depth, above them all.  Stamped fresh (with win->overlay) on every popup / tooltip
   begin, so a stray raise can never sink an overlay and the dispenser never reaches this band. */
u32
surface_z_overlay( u32 depth )
{
    return GUI_Z_OVERLAY + depth;
}

/*==============================================================================================
    surface_hover_nominate -- keep the front-most (highest z) candidate the cursor is over;
    promoted to hover_win next frame (frame turnover, gui_ctx.c).  Windows (window_begin),
    floating dock groups, and root regions (gui_region_begin) all compete for hover_win in this
    one contest keyed purely on z; the winner is the single fact that gates all widget
    hit-testing (item_state's win_hover, via the interaction scope).

    The cursor lives in exactly one OS window/surface at a time (s_io.mouse_viewport, resolved
    from the win_id on mouse events).  A candidate on any other surface cannot be under the
    cursor regardless of where its rect sits in its own surface's coordinate space, so it is
    rejected before the rect test -- the "physical window is a parent hover" rule.
==============================================================================================*/

void
surface_hover_nominate( gui_id_t id, gui_rect_t r, u32 z, u32 viewport )
{
    /* Deaf context: not listening for input this frame, skip hover nomination. */
    if ( !g_ctx->listening )
        return;

    /* Surface gate first: the cursor must be in the OS window hosting this candidate's viewport. */
    if ( viewport != s_io.mouse_viewport )
        return;

    /* Cheap z test gates the rect_hit; ties keep whichever nominates last this frame. */
    if ( z >= s_interaction.next_hover_win_z && rect_hit( r ) )
    {
        s_interaction.next_hover_win   = id;
        s_interaction.next_hover_win_z = z;
    }
}

/* The pane bracket (pane_tag, gui_pane_begin/end) lives in frame/gui_pane.c: the pane is the
   go-between type, and stamping BOTH servers with it -- the render server's
   draw state (segment key, z, viewport, band, clips) and this server's interaction scope -- is
   the frame orchestrator's verb.  This file keeps only the interact-server half the bracket
   consumes: the hover contest above and the pool / z dispenser around it. */

/*==============================================================================================
    Surface reassignment request -- one window moving between OS surfaces.

    A window dragged by its title bar and released with the cursor outside its host surface's
    client bounds changes which surface hosts it: from the main surface (viewport 0) it tears off
    into a fresh floater; from a floater it merges back to the main surface.  The gesture
    detection that fills this slot is window/ policy (window_begin_ex / the detach button);
    the post-build reconcile gui_viewport_update (frame/gui_viewport.c) services it -- the safe point to
    create or destroy a surface, since the build is complete and no draw list is mid-flight.

    A single slot suffices: only one window can own the drag (active_id) at a time.  `title` is the
    dragged window's title string, borrowed for the same frame to name the spawned OS window (the
    immediate-mode same-frame lifetime makes this safe -- it is consumed before the frame ends).
==============================================================================================*/

gui_vp_request_t s_vp_request;   /* TYPE in core/gui_ctx.h (extern for the chrome unit) */

/*==============================================================================================
    Closeable windows -- open / query a window's hidden state by title.

    A CLOSEABLE window's close (X) button sets win->closed, hiding the window until the host
    re-opens it.  These reach the record by id_hash(title) -- the same key window_begin uses --
    so the host can drive the open state from a button without holding its own flag.  A window
    that has never been begun has no record yet; window_set_open then no-ops (it already opens
    by default on first begin) and window_is_open reports it open.
==============================================================================================*/

void
gui_window_set_open( const char* title, bool open )
{
    gui_window_t* win = window_find( id_hash( title ) );
    if ( !win || win->closed == !open )
        return;                 /* no edge: a host mirroring its own bool every frame must not
                                   pin the UI dirty forever -- raise only on a real change. */
    win->closed = !open;
    redraw_request();           /* show/hide lands in the NEXT build; without this a toggle driven
                                   from a menu or a hotkey freezes until the mouse moves again. */
}

bool
gui_window_is_open( const char* title )
{
    gui_window_t* win = window_find( id_hash( title ) );
    return !win || !win->closed;
}

// clang-format on
/*============================================================================================*/
