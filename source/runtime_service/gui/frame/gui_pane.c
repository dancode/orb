/*==============================================================================================

    runtime_service/gui/frame/gui_pane.c -- The pane bracket: the go-between verb.

    The pane (gui_pane_t, gui.h) is the go-between type of the two-server model:
    id + rect + z + viewport + input.  This file owns the one verb that
    hands a pane to BOTH servers at once -- which is why it lives with the frame orchestrator
    and not in either server:

        render server    draw_set_window/sort_key/viewport/band + the clip pushes -- every
                         command emitted after the tag belongs to this pane's draw segment.
        interact server  s_build.win + s_scope.win/clip -- every item emitted after the tag
                         is attributed to this pane in the hover/z contest.

    pane_tag is THE shared open every stacked entity runs: window_begin_ex, the docked branch,
    and region_begin all route through it -- they differ only in the policy layered around it
    (what rect they nominate, where their z comes from, what chrome and layout they add).
    Hover nomination stays a separate interact-server verb (surface_hover_nominate,
    core/gui_surface.c) because the NOMINATED rect is policy: a window pads it by the resize
    band, a collapsed window shrinks it to the title bar, a frame-only shell offers only its
    caption.

    Included by gui.c in the frame group.

==============================================================================================*/
// clang-format off

void
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

    return ( gui_pane_t ){ .id = id, .rect = r, .z = z, .vp = (u8)vp };
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

// clang-format on
/*============================================================================================*/
