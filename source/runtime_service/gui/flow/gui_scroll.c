/*==============================================================================================

    runtime_service/gui/flow/gui_scroll.c -- Scroll region engine.

    Implements the shared scrollable-region mechanism used by both window bodies and
    child_begin boxes:

        gui_region_t        persistent scroll + content-size state, keyed by id
        scroll_clamp        pin a scroll offset into [0, content - view]
        layout_push_region  open a region: reserve gutters, clamp scroll, seed a layout frame
        layout_pop_region   close a region: measure content, emit the bars, claim the wheel

    The persistent state for child_begin boxes is kept in the shared keyed pool
    (gui_state_get); window bodies pass pointers to their own gui_window_t fields.

    The scrollbar itself is a stock widget (chrome/widgets/gui_scrollbar.c) -- this engine only
    reserves its gutter and hands it the track rect + scroll slot at pop, through the forward
    declaration in flow/gui_flow.h.  Compose produces rects; the widget owns feel + look.

    Included by gui.c after gui_layout_core.c (provides cell_next, layout_frame_t, lf,
    layout_clear, layout_set_default, item_flags_chrome_reset) and before gui_layout_child.c
    and gui_layout.c which call layout_push/pop_region.

==============================================================================================*/

// clang-format off
/*==============================================================================================
    Persistent region state

    Child regions need their scroll offset and last-measured content size to survive across
    frames, keyed by id, exactly the way windows keep those fields inline in gui_window_t.
    Windows do not use this pool -- they pass pointers to their own record -- so only child_begin
    fetches a record, from the shared keyed state pool (gui_ctx.c) by region id.  No dedicated
    table or recycling logic lives here: the pool stamps the slot each frame and reclaims it once
    the id goes cold, and hands back zeroed storage on first sight so a new child opens at the top
    with no measured size.
==============================================================================================*/

/* gui_region_t is the small class's sizing tenant -- the assert lives with the tenant. */
ORB_STATIC_ASSERT( sizeof( gui_region_t ) <= GUI_STATE_CAP,
                   "gui_region_t is the small class's sizing tenant; grow GUI_STATE_CAP" );

static gui_region_t*
region_get( gui_id_t id )
{
    return GUI_STATE( gui_region_t, id );
}

/*==============================================================================================
    Nav scroll chase (here; the moment is picked by the interact server)

    Bring the nav cursor's rect into view -- the keyboard analogue of the wheel.  Runs once, on
    the frame the cursor was adopted (g_ctx->nav.scroll_chase), for the layout-placed cursor
    item: walk the open region stack innermost-out and nudge each region's scroll so the item is
    visible, correcting the rect level by level so an item deep in a nested child pulls every
    ancestor into line.  Like the wheel, the new offset only reaches the screen next frame (this
    frame's pen already used the old one), so wants_redraw forces the follow-up frame.  Invoked
    from nav_item_register (core/gui_nav_item.c) across the server's documented upward seam: walking
    regions and moving scroll is composition machinery, so the act lives here and behavior only
    picks the moment -- the same split as draw_nav_ring.
==============================================================================================*/

void
nav_scroll_chase( gui_rect_t r )
{
    const f32 pad = NAV_RING + 2.0f;   /* breathing room so the ring lands clear of the view edge */

    u32 top = ( s_layout_sp <= GUI_LAYOUT_DEPTH ) ? s_layout_sp : GUI_LAYOUT_DEPTH;
    for ( i32 i = (i32)top - 1; i >= 0; --i )
    {
        layout_frame_t* f = &s_layout_stack[ i ];
        if ( f->flags & GUI_WIN_NOSCROLL ) continue;

        f32 vx0 = f->view.x;   /* the region's resolved view rect: the same gutter-adjusted */
        f32 vy0 = f->view.y;   /* extents the region's own scrollbars are sized against     */

        /* Overshoot per axis: pull the near edge in (top-aligning an item taller than the view),
           else the far edge.  Clamped into the scroll range measured last frame, so a chase can
           never scroll past the content -- the same clamp the wheel applies. */
        f32 dy = 0.0f;
        if ( r.h > f->view.h - 2.0f * pad || r.y < vy0 + pad )
            dy = r.y - ( vy0 + pad );
        else if ( r.y + r.h > vy0 + f->view.h - pad )
            dy = ( r.y + r.h ) - ( vy0 + f->view.h - pad );

        f32 max_y  = f->scroll->content_h - f->view.h;
        if ( max_y < 0.0f ) max_y = 0.0f;
        f32 want_y = clampf( f->scroll->scroll_y + dy, 0.0f, max_y );
        dy = want_y - f->scroll->scroll_y;

        f32 dx = 0.0f;
        if ( r.w > f->view.w - 2.0f * pad || r.x < vx0 + pad )
            dx = r.x - ( vx0 + pad );
        else if ( r.x + r.w > vx0 + f->view.w - pad )
            dx = ( r.x + r.w ) - ( vx0 + f->view.w - pad );

        f32 max_x  = f->scroll->content_w - f->view.w;
        if ( max_x < 0.0f ) max_x = 0.0f;
        f32 want_x = clampf( f->scroll->scroll_x + dx, 0.0f, max_x );
        dx = want_x - f->scroll->scroll_x;

        if ( dy != 0.0f || dx != 0.0f )
        {
            f->scroll->scroll_y = want_y;
            f->scroll->scroll_x = want_x;
            r.y -= dy;   /* where the item lands next frame -- the ancestors check that position */
            r.x -= dx;
            redraw_request();
        }
    }
}

/*==============================================================================================
    scroll_clamp -- pin a scroll offset into [0, content - view].  The one place the scroll range
    is defined; shared by the gutter reservation (push), the wheel (pop), and any future caller.
==============================================================================================*/

/* Sub-quantum spill tolerance.  Lattice-quantized content (auto rows / natural sizes ceil to
   grid_quantum) meeting a free-px container (a dragged window edge, a hand-sized box) can
   overhang the view by a few px no author intended; an overflow within one quantum reads as
   fitting -- no bar, no scroll range -- instead of a region that "scrolls" by a jittery 2px.
   Genuine overflow (>= one more row) clears the tolerance by construction.  0 when the grid
   is off (q <= 1): exact comparisons, the pre-grid behavior. */
static f32
region_spill_tol( void )
{
#if GUI_GRID_LATTICE
    u32 q = s_style.grid_quantum;
    return ( q > 1 ) ? (f32)q : 0.0f;
#else
    return 0.0f;
#endif
}

static void
scroll_clamp( f32* scroll, f32 content, f32 view )
{
    f32 max = content - view;
    if ( max <= region_spill_tol() ) max = 0.0f;   /* sub-quantum spill: not scrollable */
    *scroll = clampf( *scroll, 0.0f, max );
}

/*==============================================================================================
    layout_push_region -- open a scrollable region.

    `outer` is the region box in screen space; `pad` insets the content column from its left
    and right edges; `flags` carry the scroll policy (the GUI_WIN_*SCROLL bits, reused).
    The scroll offset and the last/next content extents live in the four caller-owned f32s
    (a window record's fields, or a region-pool entry), so this engine is agnostic to where
    the persistence lives.

    Reserves the scrollbar gutters from last frame's measured content (two passes, since each
    gutter shrinks the cross view and can tip the other axis into overflow), clamps the scroll
    to that, then seeds a fresh layout frame whose pen is biased by -scroll.

    `own_clip` controls the draw-clip (scissor) stack.  A window passes false: it already
    pushed one clip spanning the whole window, content draws under it, and the chrome it draws
    last overpaints anything that scrolled under the title bar -- so a plain window stays a
    single clip / single draw command.  A child passes true: it pushes its own clip (the second
    for that window) so its contents are scissored to the box.  The interaction clip used for
    hit-testing follows the same rule -- a child narrows it, the window body inherits it.  The
    bars are drawn at pop, once this frame's content is measured.
==============================================================================================*/

void
layout_push_region( gui_id_t id, gui_rect_t outer, gui_pad_t region_pad, gui_win_flags_t flags,
                    gui_scroll_link_t* scroll, bool own_clip )
{
    /* Cap the write slot at the top of the stack so an over-deep nesting aliases the deepest
       frame rather than writing past the array; s_layout_sp still counts truthfully so each
       push stays paired with its pop (and lf() clamps its read the same way). */
    u32 slot = s_layout_sp < GUI_LAYOUT_DEPTH ? s_layout_sp : GUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    f->region_id  = id;
    f->outer      = outer;
    f->flags      = flags;
    f->scroll      = scroll;
    f->parent_clip = s_scope.clip;

    /* Seed the id scope with this region's id, so leaf widgets combine their label against it
       (identical labels in different regions never collide).  id_restore unwinds the scope -- and
       any push_id the caller left unbalanced -- at pop, so a leak cannot corrupt the parent. */
    f->id_restore = s_id_sp;
    id_push( id );

    const f32 knob = SLIDER_KNOB_W;

    /* Policy: ALWAYS_* force a static bar; NOSCROLL hides every bar (wheel still works);
       otherwise dynamic -- vertical defaults on, horizontal only when HSCROLL is requested. */
    bool no_bars  = ( flags & GUI_WIN_NOSCROLL )       != 0;
    bool v_static = ( flags & GUI_WIN_ALWAYS_VSCROLL ) != 0;
    bool h_static = ( flags & GUI_WIN_ALWAYS_HSCROLL ) != 0;
    bool v_dyn    = !no_bars && !v_static;
    bool h_dyn    = !no_bars && !h_static && ( ( flags & GUI_WIN_HSCROLL ) != 0 );

    /* View extents inside the border, before reserving any gutter. */
    f32 view_h = outer.h - WIN_BORDER;
    f32 view_w = outer.w - 2.0f * WIN_BORDER;

    /* Two-pass gutter reservation from last frame's content.  Overflow within the spill
       tolerance reads as fitting, matching the scroll range scroll_clamp allows -- a bar never
       appears for a range the clamp would zero out. */
    f32 tol    = region_spill_tol();
    f32 last_h = scroll->content_h, last_w = scroll->content_w;
    bool show_v = v_static || ( v_dyn && last_h > view_h + tol );
    bool show_h = h_static || ( h_dyn && last_w > view_w + tol );
    if ( show_v ) view_w -= knob;
    if ( show_h ) view_h -= knob;
    if ( !show_v && v_dyn && last_h > view_h + tol ) { show_v = true; view_w -= knob; }
    if ( !show_h && h_dyn && last_w > view_w + tol ) { show_h = true; view_h -= knob; }

    f->sb_w   = show_v ? knob : 0.0f;
    f->sb_h   = show_h ? knob : 0.0f;
    f->show_v = show_v;
    f->show_h = show_h;

    /* THE view rect (see layout_frame_t): the one screen-space "visible" answer every consumer
       below reads -- clips, content track, bars, scroll chase.  Sized here and only here. */
    f->view = ( gui_rect_t ){ outer.x + WIN_BORDER, outer.y, view_w, view_h };

    /* Bottom-anchor tail-follow (GUI_WIN_ANCHOR_BOTTOM): keep the scroll pinned to the content
       bottom (the newest row) so new output stays in view, until the user scrolls up; scrolling
       back to the bottom re-arms the follow.  scroll_y stays a normal 0=top offset -- the scrollbar
       and scroll_clamp below are untouched, we only choose the offset here.  Follow vs. freeze is
       driven by user intent, not geometry: an external move away from where we last pinned (a wheel
       notch, a bar drag, scroll_by) more than a row off the bottom unsticks; anything else (content
       simply growing under a pinned view) keeps following.  Uses last frame's content_h, so a line
       added this frame reaches the tail next frame -- the one-frame lag every path here runs on. */
    if ( flags & GUI_WIN_ANCHOR_BOTTOM )
    {
        f32 max = last_h - view_h;
        if ( max < 0.0f ) max = 0.0f;
        f32 bottom_tol = (f32)WIDGET_H;   /* within a row of the bottom still counts as "at the tail" */

        if ( scroll->scroll_y != scroll->pinned_y )      /* moved since we pinned it */
            scroll->unstick = ( scroll->scroll_y < max - bottom_tol );
        if ( !scroll->unstick )
            scroll->scroll_y = max;                      /* follow the tail */
    }

    /* Clamp scroll against the gutter-adjusted views (last frame's content). */
    scroll_clamp( &scroll->scroll_y, last_h, view_h );
    scroll_clamp( &scroll->scroll_x, last_w, view_w );

    /* Remember the pinned offset AFTER the clamp -- the exact value we leave the region at -- so
       next frame can tell a genuine external move from content that merely grew. */
    if ( flags & GUI_WIN_ANCHOR_BOTTOM )
        scroll->pinned_y = scroll->scroll_y;

    /* Content column + pen (the shared derivation in gui_layout_core.c).  region_pad is the inset
       between the region box and where the layout starts (l/r narrow the column, t offsets the
       first row).  Opens UNDECLARED: the first layout header in the region body (stack / columns /
       grid / ...) installs the geometry; a widget emitted before then trips the guard in
       cell_next_w. */
    layout_seed_content( f, region_pad );

    /* Own clip (children only): scissor contents to the gutter-adjusted view; draw_push_clip_rect
       intersects it with the enclosing clip so a child near an edge cannot draw past its parent.
       The window body keeps the enclosing whole-window clip instead (own_clip false), so the
       chrome it draws last overpaints content that scrolled under the title bar. */
    if ( own_clip )
    {
        /* Bound the view by the parent's interaction clip, not just the enclosing draw clip: the
           window's draw clip spans the whole window (so the chrome overpaint trick works for the
           body), but a child box scrolled up must scissor at the body seam -- its rows never
           paint across the title bar.  Draw clip and hit-test clip are then the same rect. */
        gui_rect_t clip = rect_intersect( f->view, f->parent_clip );
        draw_push_clip_rect( clip.x, clip.y, clip.w, clip.h );
        f->pushed_clip = true;
        s_scope.clip = clip;
    }
    else
    {
        f->pushed_clip  = false;
        /* No draw clip pushed; the window's single outer clip stays live so the chrome drawn last
           in window_end can overpaint content that scrolled under the title bar.  But for hit-
           testing, narrow the scope clip to the view: a widget scrolled under the title bar cannot
           be clicked through it, and nothing in the body -- widget or child region -- can claim
           hover or a press in the reserved scrollbar gutters, which belong to the bars alone.
           layout_pop_region restores parent_clip so the bars (drawn after the restore) hit-test
           against the full window rect. */
        s_scope.clip = rect_intersect( f->parent_clip, f->view );
    }
}

/*==============================================================================================
    layout_pop_region -- close the top region: measure, draw bars, restore the parent.

    Order matters: measure content from the pen travel, pop the inner content clip and restore
    the parent interaction clip *before* drawing the bars (their tracks sit in the gutter,
    outside the content view, so they must hit-test under the parent clip), then claim the wheel
    if the cursor is over this region.  The wheel is claimed here -- at pop -- so nesting works:
    pops run inner-first, so the innermost region under the cursor claims it before its parents.
    A wheel delta therefore takes effect next frame (this frame's pen bias already used the old
    offset), which is imperceptible for scrolling.  Finally advance the parent pen past `outer`.
==============================================================================================*/

void
layout_pop_region( void )
{
    layout_frame_t* f = lf();

    /* Chrome (the scrollbars below) is not an item: drop any disabled latch a trailing body widget
       left so the bars interact and paint normally. */
    item_flags_chrome_reset();

    /* Close any open line first so the measure sees the full content extent (a partially filled
       last row counts), then read the highwater: high_y is the exact content end -- gap-before
       means no trailing gap to correct for. */
    layout_row_break( f );

    /* Content extent comes from the anchor seam (content_extent_*, flow/gui_layout_core.c): the
       highwater's climb from the unscrolled origin, with the scroll bias cancelled there so this
       measure means the same thing at any scroll offset.  The region pads join it on each axis --
       the canvas the scroll range covers includes the breathing above the first item and below the
       last, so scrolling to the end leaves the same air under the content as a short region shows
       above it.  An empty region still measures 0 (the "never measured" premeasure sentinel).
       Stored for next frame's gutter + knob. */
    f32 items_h = content_extent_y( f );
    f32 items_w = content_extent_x( f );
    f32 content_h = ( items_h > 0.0f ) ? items_h + f->pad.t + f->pad.b : 0.0f;
    f32 content_w = ( items_w > 0.0f ) ? items_w + f->pad.l + f->pad.r : 0.0f;
    f->scroll->content_h = content_h;
    f->scroll->content_w = content_w;

    /* Content-rect debug layer (GUI_DBG_CONTENT): outline the raw highwater span (content_x..
       high_x, (origin_y - scroll_y)..high_y) in the SAME scrolled screen space the content itself
       just drew in, so the box lands exactly on the emitted geometry's true bounds -- if it
       doesn't hug the longest visible line, the highwater measurement (not the eye) is wrong.
       Light green, distinct from the layout layer's magenta cell rects.  Unlike the other debug
       layers this draws into the MAIN list (the box must scroll with the content it measures),
       so it is emit-gated here rather than captured into the overlay list. */
    if ( gui_debug_get_layers() & GUI_DBG_CONTENT )
    {
        f32 top = canv_from_scr_y( f, f->origin_y );   /* cross to content anchor FIRST, so the
                                                          spans below stay content-to-content */
        draw_push_rect_outline( f->content_x, top, f->high_x - f->content_x, f->high_y - top,
                                2.0f, 0, GUI_COLOR( 0xA0, 0xF0, 0xA0, 0xFF ) );
    }

    /* Region-geometry debug layer (GUI_DBG_REGION): the view rect, the reserved gutters, and
       the body's interaction clip -- captured before the restore below while s_scope.clip is
       still the clip the body widgets hit-tested under. */
    DBG_REGION( f->view, s_scope.clip, f->sb_w, f->sb_h );

    /* Pop the region's own clip if it pushed one (a child); the window body pushed none and
       leaves the whole-window clip in place for the bars + chrome.  Restore the enclosing
       interaction clip either way, so the bars (in the gutter, outside a child's box) hit-test. */
    if ( f->pushed_clip )
        draw_pop_clip_rect();
    s_scope.clip = f->parent_clip;

    /* Bars: in the reserved gutters, sitting exactly on the view's right / bottom edges (the
       gutters are what the view reservation carved out, so "just past the view" IS the gutter),
       clear of the corner.  Compose ends at the track rect -- the widget (chrome/widgets/gui_scrollbar.c)
       owns the grab and the paint. */
    if ( f->show_v )
    {
        gui_rect_t track = { f->view.x + f->view.w, f->view.y, f->sb_w, f->view.h };
        scrollbar_widget( f->region_id, track, true, content_h, f->view.h, &f->scroll->scroll_y );
    }
    if ( f->show_h )
    {
        gui_rect_t track = { f->view.x, f->view.y + f->view.h, f->view.w, f->sb_h };
        scrollbar_widget( f->region_id, track, false, content_w, f->view.w, &f->scroll->scroll_x );
    }

    /* Wheel: the hovered region consumes it (vertical by default, horizontal with Shift).
       Gated by the owning window (hover_win), unclaimed-this-frame, no in-flight drag, and the
       cursor inside this region's box.  Re-clamp against this frame's measured content. */
    bool wheel_free  = !s_build.wheel_used && !( f->flags & GUI_WIN_NOMOUSESCROLL );
    bool over_region = ( s_interaction.hover_win == s_build.win.id ) && rect_hit( f->outer );
    if ( wheel_free && over_region && interact_idle() && s_io.mouse_wheel != 0.0f )
    {
        const f32 step  = WIDGET_H * 3.0f;   /* content advanced per wheel notch (tunable) */
        bool      shift = io_shift();
        if ( shift ) f->scroll->scroll_x -= s_io.mouse_wheel * step;
        else         f->scroll->scroll_y -= s_io.mouse_wheel * step;

        /* Re-clamp against this frame's measured content. */
        scroll_clamp( &f->scroll->scroll_y, content_h, f->view.h );
        scroll_clamp( &f->scroll->scroll_x, content_w, f->view.w );

        /* The new offset only reaches the screen next frame -- this frame's items were already
           positioned from the pre-update value (layout_push_region seeds the pen from *scroll
           at push, before this pop runs).  Without forcing one more build, a frame with no other
           stimulus goes clean (retained cache sees identical output vs. last frame) and the
           update stalls until some unrelated input arrives -- every other notch appears to do
           nothing, since it is showing the previous notch's result.  Same fix as the collapse /
           close toggles above: force the guaranteed follow-up frame that flushes it. */
        redraw_request();

        s_build.wheel_used = true;
    }

    /* Pop the frame and advance the parent pen past the region box, so the parent's next
       widget lands directly below it.  The box is the parent's last "line": the pen lands at its
       exact bottom (the gap before whatever follows is owed via gap_pending, not appended), and
       line.prev_item / the line record are stamped so same_line after child_end anchors to the box.
       The root region (a window body) has no parent frame. */
    s_id_sp = f->id_restore;   /* unwind this region's id scope (and any leaked push_id) */
    gui_rect_t outer = f->outer;
    --s_layout_sp;
    if ( s_layout_sp > 0 )
    {
        layout_frame_t* p = lf();
        content_reach( p, outer.x + outer.w, outer.y + outer.h );   /* pen + highwater past the box */
        p->gap_pending = true;
        p->line.prev_item   = outer;
        p->line.cross  = outer.y;
        p->line.ext    = outer.h;
        p->line.open   = false;
    }
}

// clang-format on
/*============================================================================================*/
