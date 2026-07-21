/*==============================================================================================

    runtime_service/gui/surface/gui_surface.c -- The surface service: window records as
    placed, stacked, occluding rectangles -- no layout, no chrome, no gestures.

    Tier 1 owns what a window IS before anything is drawn in it: the persistent record
    (position, size, z, open state, host viewport), the placement channel that seeds it, the
    z dispenser that stacks it, the hover-win contest that resolves which surface the cursor
    is over, and the request slot that moves a window between OS surfaces.  Everything a
    window DOES -- title-bar drags, resize grips, collapse, tear-off gestures, chrome paint --
    is window/ policy layered on these services.

    Following the house pattern, the records themselves live in gui_context_t
    (core/gui_ctx.c owns storage + frame turnover: the window pool g_ctx->win.pool /
    g_ctx->win.count / g_ctx->win.scratch, the dispenser g_ctx->win.z_counter, and the hover nominee
    fields in s_interaction, promoted to hover_win at frame turnover); this file owns their
    behavior.  The OS half of the surface story -- the viewport records (gui_viewport_t) and
    their open/close lifecycle -- stays with the context and the conductor (gui_frame.c),
    since creating a surface is an app()/rhi() operation the tiers never perform.

    Included by gui.c after core/ and before compose/ -- a root region enters the
    same hover contest a window does, so the contest must sit below both.

==============================================================================================*/
// clang-format off

/*==============================================================================================

    - Find the window for this GUID or create it from the initial geometry.
    - Never returns NULL; an overflowing table falls back to a transient scratch entry.    

==============================================================================================*/

static gui_window_t*
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
            static bool warned = false;
            if ( !warned )
            {
                printf( "[gui] WARNING: more than %u windows live on the main surface this frame -- "
                        "extra windows share one transient scratch slot and lose persisted state. "
                        "Raise gui_config.max_windows.\n", g_ctx->win.max );
                fflush( stdout );   /* flush the diagnostic before the once-assert can trap */
                warned = true;
            }
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

/*==============================================================================================
    window_find -- locate an existing window record by id, or NULL.  Unlike window_get this never
    creates one; used by the post-build reconcile (viewport_update) to reach the window a
    tear-off / merge-back gesture named, where creating a phantom record would be wrong.  A
    GUI_ID_NONE query is "no window" and short-circuits -- no real record ever carries that id.
==============================================================================================*/

static gui_window_t*
window_find( gui_id_t id )
{
    if ( id == GUI_ID_NONE ) return NULL;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
        if ( g_ctx->win.pool[ i ].id == id )
            return &g_ctx->win.pool[ i ];
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

static struct
{
    bool         has_pos, has_size;     /* a value is queued on this axis */
    gui_cond_t  pos_cond, size_cond;   /* when to apply it               */
    f32          pos_x, pos_y;
    f32          size_w, size_h;

    bool         has_viewport;          /* a viewport reassignment is queued for the next window */
    u32          viewport;              /* its target surface index                              */

} s_next_win;

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
    nav / dock / native tests key on -- so z itself stays pure paint order.
==============================================================================================*/

#define GUI_REGION_BG_Z  0x00000000u
#define GUI_REGION_Z     0x40000000u
#define GUI_Z_OVERLAY    0x80000000u
#define GUI_REGION_FG_Z  0xF0000000u

/*==============================================================================================
    surface_z_raise -- the z dispenser's single verb: bring a stacked entity to the front.

    Returns the z the entity should hold: a fresh top-of-stack value, or its own z unchanged
    when it is already the most recently raised (no value is burned re-raising the top).  The
    dispenser (g_ctx->win.z_counter) is monotonic and shared by windows, floating dock groups, and
    appearing windows alike, so every raise lands strictly above everything raised before it.
    This tier is the ONLY author of z values: window/dock raise through this verb, and the
    popup layer stamps the overlay band through surface_z_overlay below -- nothing outside this
    file touches g_ctx->win.z_counter or the band constants raw.
==============================================================================================*/

static u32
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
static u32
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

static void
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

/*==============================================================================================
    The pane -- the minimal top-level surface occupant (gui_pane_t, gui.h).

    pane_tag is THE shared open every stacked entity runs: stamp the draw state with the pane
    key (id, z, viewport, band) so every command emitted after it belongs to this pane, and
    commit the interaction scope so every item is attributed to it.  window_begin_ex, the
    docked branch, and region_begin all route through it -- they differ only in the policy
    layered around it (what rect they nominate, where their z comes from, what chrome and
    layout they add).  Hover nomination stays a separate verb (surface_hover_nominate above)
    because the NOMINATED rect is policy: a window pads it by the resize band, a collapsed
    window shrinks it to the title bar, a frame-only shell offers only its caption.
==============================================================================================*/

static void
pane_tag( gui_id_t id, u32 z, u32 vp, u32 band )
{
    draw_set_window( id );        /* stable retained-cache key: all this pane's spans share it */
    draw_set_sort_key( z );
    draw_set_viewport( vp );
    draw_set_band( band );
    s_build.win.viewport = vp;    /* ambient surface: debug capture + windows begun after inherit it */
    s_build.win.id       = id;
    s_scope.win          = id;    /* interaction scope: this pane owns the items that follow */
}

/* One-deep bracket state for the public pane: a raw pane is root-level (like a region) and
   never nests -- children of a pane are carved rects, not sub-panes. */
static struct
{
    bool open;
    bool clipped;

} s_pane;

/* pane_begin -- open the raw block for a caller building its own chrome: tag + hover
   nomination + base clip, nothing else.  No pool record, no persistence, no layout, no
   background paint: the caller owns every pixel (el_* / draw_* over carved rects) and any
   cross-frame state (open flags, dragged position) lives with the caller.  Rect-first: flow
   is available inside via flow_begin( pane.rect ) if wanted.  vp GUI_VP_INVALID = primary. */
gui_pane_t
gui_pane_begin( const char* id_str, gui_rect_t r, gui_region_tier_t tier, gui_vp_t vp,
                gui_win_flags_t flags )
{
    ORB_ASSERT_MSG_ONCE( !s_pane.open, "pane_begin while a pane is open -- panes do not nest" );

    gui_id_t id = id_hash( id_str );
    DBG_NAME( id, id_str );

    if ( vp == GUI_VP_INVALID )
        vp = 0;

    u32 z = ( tier == GUI_REGION_BG ) ? GUI_REGION_BG_Z
          : ( tier == GUI_REGION_FG ) ? GUI_REGION_FG_Z
          :                             GUI_REGION_Z;

    bool input = !( flags & GUI_WIN_NO_INPUT );

    pane_tag( id, z, vp, ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );

    if ( input )
        surface_hover_nominate( id, r, z, vp );

    /* Base clip against the pane's own surface, then the pane rect -- draw clip and hit clip
       together, exactly the docked-window pair.  NO_CLIP skips both: a pure HUD pane that
       draws outside its nominal rect (and hit-tests display-wide). */
    {
        const gui_viewport_t* vprec = &g_ctx->vp.pool[ vp ];
        draw_set_root_clip( vp_w( vprec ), vp_h( vprec ) );
    }
    if ( !( flags & GUI_WIN_NO_CLIP ) )
    {
        draw_push_clip_rect( r.x, r.y, r.w, r.h );
        s_scope.clip = r;
    }
    else
    {
        s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    }

    /* This open is not an item: a disabled latch from a prior widget must not leak in. */
    item_flags_chrome_reset();

    s_pane.open    = true;
    s_pane.clipped = !( flags & GUI_WIN_NO_CLIP );

    return ( gui_pane_t ){ .id = id, .rect = r, .z = z, .vp = (u8)vp, .input = (u8)input };
}

void
gui_pane_end( void )
{
    ORB_ASSERT_MSG_ONCE( s_pane.open, "pane_end without pane_begin" );

    if ( s_pane.clipped )
        draw_pop_clip_rect();

    /* Restore the main display's root clip for whatever paints next at root level, the same
       hand-back window_end performs. */
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );

    s_pane.open = s_pane.clipped = false;
}

/*==============================================================================================
    Surface reassignment request -- one window moving between OS surfaces.

    A window dragged by its title bar and released with the cursor outside its host surface's
    client bounds changes which surface hosts it: from the main surface (viewport 0) it tears off
    into a fresh floater; from a floater it merges back to the main surface.  The gesture
    detection that fills this slot is window/ policy (window_begin_ex / the detach button);
    the post-build reconcile gui_viewport_update (gui_frame.c) services it -- the safe point to
    create or destroy a surface, since the build is complete and no draw list is mid-flight.

    A single slot suffices: only one window can own the drag (active_id) at a time.  `title` is the
    dragged window's title string, borrowed for the same frame to name the spawned OS window (the
    immediate-mode same-frame lifetime makes this safe -- it is consumed before the frame ends).
==============================================================================================*/

static struct
{
    bool        active;     /* a request is queued this frame                                   */
    bool        by_drag;    /* true = seamless title-bar drag; false = detach-button click       */
    gui_id_t  win_id;     /* the dragged window record                                        */
    u32         from_vp;    /* surface it was on (0 = main -> tear off; else floater -> merge)   */
    const char* title;      /* window title, to label the spawned floater's OS window           */

    bool        has_home;   /* re-open of a closed floater: the spawn reads RESTORE geometry +    */
                            /* maximized state from the window record (home_*, restore_*,         */
                            /* reopen.maximized) instead of the cursor / main-relative default.   */
} s_vp_request;

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
