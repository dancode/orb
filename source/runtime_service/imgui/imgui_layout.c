/*==============================================================================================

    runtime_service/imgui/imgui_layout.c -- Scrollable layout regions + the layout stack engine.

    A "region" is a rectangular area with its own layout pen, content column, clip, and
    scrollbars.  Both the window body and a begin_child box are regions: begin_window and
    begin_child resolve a box, then call layout_push_region; their matching end calls
    layout_pop_region.  Regions nest on the layout-frame stack (imgui_ctx.c), so a list box
    with its own scrollbar can live inside a window and the parent layout resumes below it.

    The scroll mechanics here -- two-pass gutter reservation, wheel handling, clamping, content
    measurement, and the scrollbar widget -- were lifted out of the window so windows and child
    regions share one engine.  region_scrollbar() is axis-generic and id-parameterized; it is
    used for every bar on every region.

    Included by imgui.c after imgui_widget_core.c so widget_behavior, widget_bg_color, the COL_*
    palette, the WIDGET_/WIN_ layout macros, the layout-frame stack (lf / s_layout_stack), and
    the draw + region helpers are all in scope.  Included before imgui_widget_window.c, which
    calls layout_push_region / layout_pop_region for the window body.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Scrollbar ids -- distinct salts so a region's vertical and horizontal bars never share an
    id, nor collide with a label-hashed widget in the same window.  Applied to a region id.
----------------------------------------------------------------------------------------------*/

#define IMGUI_SCROLLBAR_SALT  0x5C011B01u
#define IMGUI_HSCROLLBAR_SALT 0x5C011B02u

/*----------------------------------------------------------------------------------------------
    Persistent region state

    Child regions need their scroll offset and last-measured content size to survive across
    frames, keyed by id, exactly the way windows keep those fields inline in imgui_window_t.
    Windows do not use this pool -- they pass pointers to their own record -- so only begin_child
    fetches a record, from the shared keyed state pool (imgui_ctx.c) by region id.  No dedicated
    table or recycling logic lives here: the pool stamps the slot each frame and reclaims it once
    the id goes cold, and hands back zeroed storage on first sight so a new child opens at the top
    with no measured size.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32 scroll_x, scroll_y;     /* persisted scroll offset            */
    f32 content_w, content_h;   /* content extent measured last frame */
    f32 user_w, user_h;         /* user-resized size (CHILD_RESIZE_*); 0 = none, use the passed w/h */

} imgui_region_t;

static imgui_region_t*
region_get( imgui_id_t id )
{
    return IMGUI_STATE( imgui_region_t, id );
}

/*----------------------------------------------------------------------------------------------
    scroll_clamp -- pin a scroll offset into [0, content - view].  The one place the scroll range
    is defined; shared by the gutter reservation (push), the wheel (pop), and any future caller.
----------------------------------------------------------------------------------------------*/

static void
scroll_clamp( f32* scroll, f32 content, f32 view )
{
    f32 max = content - view;
    if ( max < 0.0f ) max = 0.0f;
    *scroll = clampf( *scroll, 0.0f, max );
}

/*----------------------------------------------------------------------------------------------
    region_scrollbar -- one scrollbar track + knob along an axis; folds a knob drag into *scroll.

    `vertical` picks the axis; `track` is the full track rect, `content`/`view` the measured and
    visible extents along that axis, and `mouse_along` the live cursor coordinate on it.  The
    knob length tracks the visible fraction (min-clamped so it stays grabbable) and the drag
    maps the cursor back into scroll, mirroring slider_float.  Shared by every region's bars.
----------------------------------------------------------------------------------------------*/

static void
region_scrollbar( imgui_id_t id, imgui_rect_t track, bool vertical,
                  f32 content, f32 view, f32 mouse_along, f32* scroll )
{
    f32 max_scroll = content - view;
    if ( max_scroll < 0.0f ) max_scroll = 0.0f;

    f32 track_len = vertical ? track.h : track.w;
    f32 track_org = vertical ? track.y : track.x;

    widget_state_t st = widget_behavior( id, track, WIDGET_KIND_DRAG );

    /* Knob length is the visible fraction of the track, clamped to a grabbable minimum
       and never longer than the track itself (content <= view => full-length knob).  The
       min-then-cap order matters: a track shorter than the minimum collapses to track_len,
       so it is not folded into one clampf (whose bounds would invert). */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = SLIDER_KNOB_W;
    if ( knob_len < min_len )   knob_len = min_len;
    if ( knob_len > track_len ) knob_len = track_len;
    f32 travel = track_len - knob_len;

    /* Drag maps the cursor (knob centre) back into the scroll offset. */
    if ( st.active && travel > 0.0f )
    {
        f32 t = saturate( ( mouse_along - track_org - knob_len * 0.5f ) / travel );
        *scroll = t * max_scroll;
    }

    f32 t_cur    = ( max_scroll > 0.0f ) ? *scroll / max_scroll : 0.0f;
    f32 knob_off = track_org + t_cur * travel;

    draw_push_rect_filled( track.x, track.y, track.w, track.h, 0,0,1,1, 0, COL_SLIDER_TRACK );
    if ( vertical )
        draw_push_rect_filled( track.x, knob_off, track.w, knob_len, 0,0,1,1, 0, widget_bg_color( st ) );
    else
        draw_push_rect_filled( knob_off, track.y, knob_len, track.h, 0,0,1,1, 0, widget_bg_color( st ) );
}

/*----------------------------------------------------------------------------------------------
    layout_push_region -- open a scrollable region.

    `outer` is the region box in screen space; `pad` insets the content column from its left
    and right edges; `flags` carry the scroll policy (the IMGUI_WIN_*SCROLL bits, reused).
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
----------------------------------------------------------------------------------------------*/

static void
layout_push_region( imgui_id_t id, imgui_rect_t outer, imgui_pad_t region_pad, imgui_win_flags_t flags,
                    f32* scroll_x, f32* scroll_y, f32* content_w, f32* content_h, bool own_clip )
{
    /* Cap the write slot at the top of the stack so an over-deep nesting aliases the deepest
       frame rather than writing past the array; s_layout_sp still counts truthfully so each
       push stays paired with its pop (and lf() clamps its read the same way). */
    u32 slot = s_layout_sp < IMGUI_LAYOUT_DEPTH ? s_layout_sp : IMGUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    f->region_id  = id;
    f->outer      = outer;
    f->flags      = flags;
    f->scroll_x   = scroll_x;
    f->scroll_y   = scroll_y;
    f->pcontent_w = content_w;
    f->pcontent_h = content_h;
    f->parent_clip = s_ctx.clip_rect;

    /* Seed the id scope with this region's id, so leaf widgets combine their label against it
       (identical labels in different regions never collide).  id_restore unwinds the scope -- and
       any push_id the caller left unbalanced -- at pop, so a leak cannot corrupt the parent. */
    f->id_restore = s_id_sp;
    id_push( id );

    const f32 knob = SLIDER_KNOB_W;

    /* Policy: ALWAYS_* force a static bar; NOSCROLL hides every bar (wheel still works);
       otherwise dynamic -- vertical defaults on, horizontal only when HSCROLL is requested. */
    bool no_bars  = ( flags & IMGUI_WIN_NOSCROLL )       != 0;
    bool v_static = ( flags & IMGUI_WIN_ALWAYS_VSCROLL ) != 0;
    bool h_static = ( flags & IMGUI_WIN_ALWAYS_HSCROLL ) != 0;
    bool v_dyn    = !no_bars && !v_static;
    bool h_dyn    = !no_bars && !h_static && ( ( flags & IMGUI_WIN_HSCROLL ) != 0 );

    /* View extents inside the border, before reserving any gutter. */
    f32 view_h = outer.h - WIN_BORDER;
    f32 view_w = outer.w - 2.0f * WIN_BORDER;

    /* Two-pass gutter reservation from last frame's content. */
    f32 last_h = *content_h, last_w = *content_w;
    bool show_v = v_static || ( v_dyn && last_h > view_h );
    bool show_h = h_static || ( h_dyn && last_w > view_w );
    if ( show_v ) view_w -= knob;
    if ( show_h ) view_h -= knob;
    if ( !show_v && v_dyn && last_h > view_h ) { show_v = true; view_w -= knob; }
    if ( !show_h && h_dyn && last_w > view_w ) { show_h = true; view_h -= knob; }

    f->sb_w   = show_v ? knob : 0.0f;
    f->sb_h   = show_h ? knob : 0.0f;
    f->show_v = show_v;
    f->show_h = show_h;
    f->view_w = view_w;
    f->view_h = view_h;

    /* Clamp scroll against the gutter-adjusted views (last frame's content). */
    scroll_clamp( scroll_y, last_h, view_h );
    scroll_clamp( scroll_x, last_w, view_w );

    /* Content column + pen.  region_pad is the inset between the region box and where the layout
       starts (l/r narrow the column, t offsets the first row); origin_* is the unscrolled
       top-left used to measure extent at pop; the live pen is biased by -scroll so widgets slide
       under the clip.  (region_pad.b reserves bottom space only in a fixed grid, none yet.) */
    f->origin_x      = outer.x + region_pad.l;
    f->origin_y      = outer.y + region_pad.t;
    f->content_x     = outer.x + region_pad.l - *scroll_x;
    f->content_w     = outer.w - region_pad.l - region_pad.r - f->sb_w;
    f->cursor_x      = f->content_x;
    f->cursor_y      = outer.y + region_pad.t - *scroll_y;
    f->content_max_x = f->content_x;   /* seed extent at the origin -> an empty body measures 0 */

    /* Bottom of the content area (mirror of content_w on the vertical axis): the end of a grid's
       band, so a grid fills from the pen down to here.  Unscrolled -- grids do not scroll. */
    f->content_y_max = outer.y + outer.h - region_pad.b - f->sb_h;

    /* Open UNDECLARED: no template, mode NONE (this also clears the same_line anchor).  The first
       layout header in the region body (stack / columns / grid / ...) installs the geometry; a
       widget emitted before then trips the guard in widget_next_rect_w. */
    layout_clear( f );

    /* Own clip (children only): scissor contents to the gutter-adjusted view; draw_push_clip_rect
       intersects it with the enclosing clip so a child near an edge cannot draw past its parent.
       The window body keeps the enclosing whole-window clip instead (own_clip false), so the
       chrome it draws last overpaints content that scrolled under the title bar. */
    if ( own_clip )
    {
        imgui_rect_t clip = { outer.x + WIN_BORDER, outer.y, view_w, view_h };
        draw_push_clip_rect( clip.x, clip.y, clip.w, clip.h );
        f->pushed_clip = true;
        s_ctx.clip_rect = rect_intersect( f->parent_clip, clip );   /* hit-test clip = the box */
    }
    else
    {
        f->pushed_clip  = false;
        /* s_ctx.clip_rect stays the enclosing clip -- the window body inherits it. */
    }
}

/*----------------------------------------------------------------------------------------------
    layout_pop_region -- close the top region: measure, draw bars, restore the parent.

    Order matters: measure content from the pen travel, pop the inner content clip and restore
    the parent interaction clip *before* drawing the bars (their tracks sit in the gutter,
    outside the content view, so they must hit-test under the parent clip), then claim the wheel
    if the cursor is over this region.  The wheel is claimed here -- at pop -- so nesting works:
    pops run inner-first, so the innermost region under the cursor claims it before its parents.
    A wheel delta therefore takes effect next frame (this frame's pen bias already used the old
    offset), which is imperceptible for scrolling.  Finally advance the parent pen past `outer`.
----------------------------------------------------------------------------------------------*/

static void
layout_pop_region( void )
{
    layout_frame_t* f = lf();

    /* Chrome (the scrollbars below) is not an item: drop any disabled latch a trailing body widget
       left so the bars interact and paint normally. */
    item_flags_chrome_reset();

    /* Content extent = how far the pen travelled from the unscrolled origin (add the scroll
       back to cancel the bias).  Stored for next frame's gutter decision + knob proportions. */
    f32 content_h = ( f->cursor_y      + *f->scroll_y ) - f->origin_y;
    f32 content_w = ( f->content_max_x + *f->scroll_x ) - f->origin_x;
    *f->pcontent_h = content_h;
    *f->pcontent_w = content_w;

    /* Pop the region's own clip if it pushed one (a child); the window body pushed none and
       leaves the whole-window clip in place for the bars + chrome.  Restore the enclosing
       interaction clip either way, so the bars (in the gutter, outside a child's box) hit-test. */
    if ( f->pushed_clip )
        draw_pop_clip_rect();
    s_ctx.clip_rect = f->parent_clip;

    /* Bars: inset by the border, in the reserved gutters, clear of the corner. */
    if ( f->show_v )
    {
        imgui_rect_t track = { f->outer.x + f->outer.w - WIN_BORDER - f->sb_w,
                               f->outer.y, f->sb_w, f->view_h };
        region_scrollbar( id_combine( f->region_id, IMGUI_SCROLLBAR_SALT ), track, true,
                          content_h, f->view_h, s_io.mouse_y, f->scroll_y );
    }
    if ( f->show_h )
    {
        imgui_rect_t track = { f->outer.x + WIN_BORDER,
                               f->outer.y + f->outer.h - WIN_BORDER - f->sb_h, f->view_w, f->sb_h };
        region_scrollbar( id_combine( f->region_id, IMGUI_HSCROLLBAR_SALT ), track, false,
                          content_w, f->view_w, s_io.mouse_x, f->scroll_x );
    }

    /* Wheel: the hovered region consumes it (vertical by default, horizontal with Shift).
       Gated by the owning window (hover_win), unclaimed-this-frame, no in-flight drag, and the
       cursor inside this region's box.  Re-clamp against this frame's measured content. */
    if ( !s_ctx.wheel_used
         && !( f->flags & IMGUI_WIN_NOMOUSESCROLL )
         && s_ctx.win_id == s_ctx.hover_win
         && s_ctx.active_id == IMGUI_ID_NONE
         && s_io.mouse_wheel != 0.0f
         && rect_hit( f->outer ) )
    {
        const f32 step  = WIDGET_H * 3.0f;   /* content advanced per wheel notch (tunable) */
        bool      shift = s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ];
        if ( shift ) *f->scroll_x -= s_io.mouse_wheel * step;
        else         *f->scroll_y -= s_io.mouse_wheel * step;

        /* Re-clamp against this frame's measured content. */
        scroll_clamp( f->scroll_y, content_h, f->view_h );
        scroll_clamp( f->scroll_x, content_w, f->view_w );

        s_ctx.wheel_used = true;
    }

    /* Pop the frame and advance the parent pen past the region box, so the parent's next
       widget lands directly below it.  The root region (a window body) has no parent frame. */
    s_id_sp = f->id_restore;   /* unwind this region's id scope (and any leaked push_id) */
    imgui_rect_t outer = f->outer;
    --s_layout_sp;
    if ( s_layout_sp > 0 )
    {
        layout_frame_t* p = lf();
        p->cursor_y = outer.y + outer.h + WIDGET_GAP;
        if ( outer.x + outer.w > p->content_max_x )
            p->content_max_x = outer.x + outer.w;
    }
}

/*----------------------------------------------------------------------------------------------
    begin_child / end_child -- a nested scrollable region inside the current layout.

    Carves a box of height `h` (width `w`, or the remaining content width when w <= 0) from the
    parent pen, draws its frame, and opens a region inside it.  Fill it with any widgets (e.g.
    selectable rows for a list box); they scroll and clip within the box and get their own
    scrollbar.  Always pair with end_child; the parent layout resumes directly below the box.

    CHILD_RESIZE_X / _Y add a draggable grip on the right / bottom border (flow children only).
    The size on a resizeable axis is then user-owned and persisted in the region record -- seeded
    once from the begin_child w/h, thereafter driven by the drag -- so it overrides the passed
    value and survives across frames, exactly the way a real window owns its geometry.  The grip
    grows from the top-left (origin pinned), so only the right and bottom edges are live.

    set_next_window_size_constraints latches a one-shot [min,max] box consumed by the next
    begin_child: it bounds the resolved width / height, so an auto-sized (h <= 0) box grows with
    its content only up to max_h and then scrolls, never collapses below min_h, and a resize drag
    cannot leave that range.  A 0 (or non-positive) bound is "unconstrained" on that side.
----------------------------------------------------------------------------------------------*/

/* Smallest a resizeable child may be dragged to: a couple of rows wide, one row plus border tall. */
#define CHILD_MIN_W ( WIDGET_H * 3.0f )
#define CHILD_MIN_H ( WIDGET_H + WIN_BORDER )

/* Next-child size constraints (the Dear ImGui SetNextWindowSizeConstraints analogue): a one-shot
   [min,max] box consumed by the next begin_child.  Set by imgui_set_next_window_size_constraints,
   cleared on consume so it targets exactly one child.  A bound <= 0 means unconstrained on that
   side; the absolute CHILD_MIN floors still apply to a resize drag. */
static struct
{
    bool has;
    f32  min_w, min_h, max_w, max_h;

} s_next_child_con;

void
imgui_set_next_window_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h )
{
    s_next_child_con.has   = true;
    s_next_child_con.min_w = min_w;
    s_next_child_con.min_h = min_h;
    s_next_child_con.max_w = max_w;
    s_next_child_con.max_h = max_h;
}

/* Clamp v into [mn, mx], treating a non-positive bound as unconstrained on that side. */
static f32
child_con_clamp( f32 v, f32 mn, f32 mx )
{
    if ( mn > 0.0f && v < mn ) v = mn;
    if ( mx > 0.0f && v > mx ) v = mx;
    return v;
}

bool
imgui_begin_child( const char* id_str, f32 w, f32 h, imgui_win_flags_t flags )
{
    layout_frame_t* parent = lf();

    /* Combine against the active id scope (the parent region, plus any push_id) so the same child
       label nests safely under different parents and never collides with a window id. */
    imgui_id_t id = id_combine( id_seed(), id_hash( id_str ) );

    /* Persistent state (scroll offset, last-measured content extent, user-resized size), keyed by
       id.  Fetched up front: an auto-sized (h <= 0) child reads content_h, a resizeable one reads
       the user size, to fix the box. */
    imgui_region_t* rg = region_get( id );

    /* Resize is a flow-child affordance: a grid cell sizes its own child, so the flags are inert
       there.  resize_id is the active_id this child holds while its border is being dragged. */
    bool       resize_x  = ( flags & IMGUI_WIN_CHILD_RESIZE_X ) && parent->lay_nrows == 0;
    bool       resize_y  = ( flags & IMGUI_WIN_CHILD_RESIZE_Y ) && parent->lay_nrows == 0;
    imgui_id_t resize_id = id_combine( id, IMGUI_RESIZE_SALT );

    /* Consume any next-child size constraints up front (cleared so they bind only this child).  A
       grid cell sizes its own child, so the bounds, like the resize flags, are inert there; in flow
       they clamp the resolved size below and the resize-drag apply that follows. */
    f32 con_min_w = 0.0f, con_min_h = 0.0f, con_max_w = 0.0f, con_max_h = 0.0f;
    if ( s_next_child_con.has )
    {
        con_min_w = s_next_child_con.min_w;  con_min_h = s_next_child_con.min_h;
        con_max_w = s_next_child_con.max_w;  con_max_h = s_next_child_con.max_h;
        s_next_child_con.has = false;
    }

    /* Where the child box lands: in a grid parent it takes the next cell (w / h ignored -- the
       cell sizes it, the natural way to drop a scroll region into a split pane); in flow it sits
       at the pen, on its own line.  The parent pen / grid cursor is advanced by layout_pop_region
       for flow, but the grid cursor must step here since pop does not touch (col,row). */
    imgui_rect_t box;
    if ( parent->lay_nrows > 0 )
    {
        box = grid_next_rect( parent );   /* the next matrix cell; advances the matrix cursor */
    }
    else
    {
        layout_row_break( parent );       /* a flow child starts on its own line */
        if ( w <= 0.0f ) w = parent->content_w;   /* default: fill the remaining content width */

        /* A resizeable axis takes its size from the persisted user value, seeded once from the
           incoming w/h (a sensible 8-row default when h <= 0 -- RESIZE_Y supersedes auto-size). */
        if ( resize_x )
        {
            if ( rg->user_w <= 0.0f ) rg->user_w = w;
            w = rg->user_w;
        }
        if ( resize_y )
        {
            if ( rg->user_h <= 0.0f ) rg->user_h = ( h > 0.0f ) ? h : WIDGET_H * 8.0f;
            h = rg->user_h;
        }
        /* h <= 0 (and not RESIZE_Y) auto-sizes the height to the content measured last frame (the
           AutoResizeY case): the box hugs its widgets, like an ALWAYS_AUTOSIZE window on the
           vertical axis.  Before any content is measured (first frame) it opens one widget-row
           tall and settles next frame.  An auto-sized child has nothing to scroll. */
        else if ( h <= 0.0f )
            h = ( rg->content_h > 0.0f ) ? rg->content_h + WIDGET_GAP + WIN_BORDER : WIDGET_H;

        /* Bound the resolved size by any next-child constraints: an auto-sized box hugs content up
           to max_h then the default vertical scrollbar takes over, and never shrinks below min_h. */
        w = child_con_clamp( w, con_min_w, con_max_w );
        h = child_con_clamp( h, con_min_h, con_max_h );

        box = ( imgui_rect_t ){ parent->content_x, parent->cursor_y, w, h };
    }

    /* Edge-resize interaction, resolved here -- before the body widgets -- so a press on the grip
       band pre-empts a body widget under it, mirroring how begin_window resolves the window edge
       first.  Gated on the owning window being front-most and a free or self-owned active_id.
       Apply an in-flight drag to the persisted size, then re-derive the box so the frame painted
       below tracks the cursor this frame (the right/bottom edges only -- the origin stays put). */
    u8 resize_hot = 0;
    if ( ( resize_x || resize_y ) && s_ctx.win_id == s_ctx.hover_win
         && ( s_ctx.active_id == IMGUI_ID_NONE || s_ctx.active_id == resize_id ) )
    {
        if ( s_ctx.active_id == resize_id )
        {
            /* Shared raw edge-drag (R / B only -- the child's top-left is pinned); the child then
               layers its own policy: clamp to the next-child constraints and the CHILD_MIN floor,
               persist into the region record, and feed the result back into the box drawn below. */
            imgui_rect_t rr = box;
            resize_apply_edges( &rr, (u8)( s_resize_edges & ( IMGUI_RESIZE_R | IMGUI_RESIZE_B ) ) );

            if ( s_resize_edges & IMGUI_RESIZE_R )
            {
                rg->user_w = child_con_clamp( rr.w, con_min_w, con_max_w );
                if ( rg->user_w < CHILD_MIN_W ) rg->user_w = CHILD_MIN_W;
                box.w = rg->user_w;
            }
            if ( s_resize_edges & IMGUI_RESIZE_B )
            {
                rg->user_h = child_con_clamp( rr.h, con_min_h, con_max_h );
                if ( rg->user_h < CHILD_MIN_H ) rg->user_h = CHILD_MIN_H;
                box.h = rg->user_h;
            }
        }

        /* Hot edges under the cursor, narrowed to this child's resizeable axes -- and only the
           grow-from-origin pair (right + bottom), since the child's top-left is pinned. */
        u8 allow   = (u8)( ( resize_x ? IMGUI_RESIZE_R : 0u ) | ( resize_y ? IMGUI_RESIZE_B : 0u ) );
        resize_hot = (u8)( window_resize_hit( box, false ) & allow );

        /* Grab on press: the shared resize_grab claims the resize active_id and records the offset
           that keeps the grabbed edge under the cursor (so the size does not jump by the band width
           at grab time).  resize_hot is only ever R / B here, so its far-edge pins go unused. */
        if ( resize_hot && s_ctx.active_id == IMGUI_ID_NONE && s_io.mouse_pressed[ 0 ] )
            resize_grab( id, box, resize_hot );
    }

    /* The child box is chrome, not an item: paint its frame opaque even if a disabled widget
       precedes the begin_child call. */
    item_flags_chrome_reset();

    /* Child body fill, drawn under the parent clip before the region clips in.  The border is
       deferred to end_child (after the scrollbars) so the bar tracks cannot overdraw it -- the
       same deferral end_window uses for the window frame. */
    draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0,0,1,1, 0, COL_CHILD_BG );

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags,
                        &rg->scroll_x, &rg->scroll_y, &rg->content_w, &rg->content_h,
                        /* own_clip */ true );   /* the child's own scissor -- second clip in the window */

    /* Stamp the child's resize bookkeeping on its just-pushed frame, and suppress body-widget hover
       under a hot/armed edge for the child's duration (the edges stay armed mid-drag even if the
       cursor drifts off).  end_child restores the saved hot, so siblings below are unaffected. */
    layout_frame_t* f         = lf();
    f->child_resize_edge      = ( s_ctx.active_id == resize_id ) ? s_resize_edges : resize_hot;
    f->child_resize_saved_hot = s_ctx.win_resize_hot;
    if ( f->child_resize_edge ) s_ctx.win_resize_hot = f->child_resize_edge;

    /* No collapse concept for a child: always returns true, always pair with end_child. */
    return true;
}

void
imgui_end_child( void )
{
    /* Capture the box + resize state before layout_pop_region unwinds the frame, then draw the
       border after it has painted the scrollbars.  The bar tracks are inset by WIN_BORDER and butt
       against the frame, so drawing the outline last keeps it solid where a track meets the box
       edge.  The region clip is already popped, so this paints under the parent clip. */
    layout_frame_t* f     = lf();
    imgui_rect_t    box   = f->outer;
    u8              edges = f->child_resize_edge;
    u8              saved = f->child_resize_saved_hot;

    layout_pop_region();

    s_ctx.win_resize_hot = saved;   /* lift the body-widget suppression this child raised */

    draw_push_rect_outline( box.x, box.y, box.w, box.h, WIN_BORDER, 0, COL_BORDER );

    /* Resize affordance: bold the hot/armed edge so the border reads as draggable. */
    if ( edges )
        window_draw_resize_highlight( box, edges );
}

/*----------------------------------------------------------------------------------------------
    push_layout / pop_layout -- a sub-layout that fills one cell.

    Consumes the next cell of the active template exactly as a widget would -- so the parent
    advances (column, row wrap, same_line anchor) the instant push_layout is called, and on pop it
    resumes at the following cell -- then opens a transient layout frame whose content area *is*
    that cell.  Inside, shape it with the normal verbs (row / row_cols / grid / widgets); a fresh
    sub-layout opens undeclared, so name its mode inside (stack / columns / ...).  This is the recursive completion of the
    cell model: a cell can host a layout, the way a window or child does, but with none of the
    weight -- no scroll, no clip, no persistent state, no frame.

    A sub-layout obeys the same sizing rules as any widget: it gets one standard-height cell unless
    the row height was declared larger up front (row( calc_row(...) ) / a fixed row_h).  It does not
    grow the parent row to fit its contents -- fitting them inside the cell is the caller's job, and
    overflow is not clipped.  Always pair with pop_layout, like push_id / pop_id.

    Id scope is left unchanged, so a widget inside the sub-layout shares the parent region's ids;
    use push_id / "##" to disambiguate repeats, exactly as anywhere else.
----------------------------------------------------------------------------------------------*/

/* Sink for a sub-layout's unused scroll / content-measure fields -- it never scrolls and its extent
   feeds nothing back, so these only ever hold zero / discard.  Shared by every push_layout frame. */
static f32 s_sublayout_sink[ 4 ];

void
imgui_push_layout( void )
{
    /* Take the next cell on the parent template -- this advances the parent like any widget emit. */
    imgui_rect_t cell = widget_next_rect( WIDGET_H );

    /* Cap the write slot at the top of the stack (mirroring layout_push_region) so an over-deep
       nesting aliases the deepest frame rather than writing past the array; sp still counts true. */
    u32 slot = s_layout_sp < IMGUI_LAYOUT_DEPTH ? s_layout_sp : IMGUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    /* Transient frame: no scroll, no clip, no own id scope.  The unused region fields point at the
       shared sink, and parent_clip / id_restore are saved only so pop is symmetric. */
    f->region_id   = IMGUI_ID_NONE;
    f->outer       = cell;
    f->flags       = IMGUI_WIN_NOSCROLL;
    f->parent_clip = s_ctx.clip_rect;
    f->pushed_clip = false;
    f->id_restore  = s_id_sp;

    s_sublayout_sink[ 0 ] = s_sublayout_sink[ 1 ] = 0.0f;
    f->scroll_x   = &s_sublayout_sink[ 0 ];
    f->scroll_y   = &s_sublayout_sink[ 1 ];
    f->pcontent_w = &s_sublayout_sink[ 2 ];
    f->pcontent_h = &s_sublayout_sink[ 3 ];

    f->sb_w = f->sb_h = 0.0f;
    f->show_v = f->show_h = false;
    f->view_w = cell.w;
    f->view_h = cell.h;

    /* Content area = the cell.  content_y_max is the cell bottom, so a grid sub-layout fills it. */
    f->origin_x      = cell.x;
    f->origin_y      = cell.y;
    f->content_x     = cell.x;
    f->content_w     = cell.w;
    f->cursor_x      = cell.x;
    f->cursor_y      = cell.y;
    f->content_max_x = cell.x;
    f->content_y_max = cell.y + cell.h;

    layout_clear( f );                       /* sub-layout opens undeclared -- declare a mode inside */
}

void
imgui_pop_layout( void )
{
    layout_frame_t* f = lf();
    s_id_sp         = f->id_restore;         /* unwind any push_id the body left open */
    s_ctx.clip_rect = f->parent_clip;        /* unchanged, but symmetric with push */
    if ( s_layout_sp ) --s_layout_sp;        /* parent already advanced at push -- nothing more */
}

/*----------------------------------------------------------------------------------------------
    Public layout API -- shape the active region's repeating row template.

    These set the template on the current region; it persists and repeats for every subsequent
    widget until set again (or the region ends).  No push/pop: a region opens with the default
    (one flex column, auto height), and each call simply replaces it.  See imgui_layout_t for
    the column unit rule.  imgui_pad sets the region padding -- the inset between the region box
    and where the layout starts -- distinct from the item padding carried in the template.
----------------------------------------------------------------------------------------------*/

/* Full flow template: columns, row height, gaps, and alignment in one call.  (rows[] is for grid
   mode -- use imgui_grid; a zero-initialized descriptor is flow, so this ignores rows.) */
void
imgui_layout( imgui_layout_t desc )
{
    layout_set( desc.cols, desc.row_h, desc.gap_x, desc.gap_y );
    lf()->lay_align = (u8)desc.align;   /* full template carries the content alignment too */
}

/* stack -- the explicit header for a single full-width flex column, rows accumulating + scrolling
   (the everyday vertical list).  This is the canonical name for what a region used to do silently
   by default; it must now be declared.  Keeps the orthogonal modifiers (align, field split) as
   they stand -- use layout_default() for the full reset. */
void
imgui_stack( void )
{
    layout_set( NULL, 0.0f, 0.0f, 0.0f );
    lf()->mode = IMGUI_MODE_STACK;   /* a single full-width column is a stack, not columns */
}

/* Single full-width column of height row_h (0 = auto) -- a stack with an explicit row height. */
void
imgui_row( f32 row_h )
{
    layout_set( NULL, row_h, 0.0f, 0.0f );
    lf()->mode = IMGUI_MODE_STACK;
}

/* Reset the active region's layout to the state it opened with: one flex column of auto height,
   no field split, default gaps.  Finishes any open row first and leaves the region padding intact
   (use pad() to re-inset).  The single "clear everything" verb -- row( 0 ) only resets the columns
   and field_label_left( 0 ) only the field split, so this is the way back to the plain stack when
   both a template and a field split are in play. */
void
imgui_layout_default( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );      /* finish any partially-filled row before clearing */
    layout_set_default( f );    /* single flex column, no field split, default gaps */
}

/* n equal flex columns of height row_h (0 = auto, one standard line). */
void
imgui_row_cols( f32 row_h, u32 n )
{
    if ( n == 0 )                 n = 1;
    if ( n > IMGUI_LAYOUT_COLS )  n = IMGUI_LAYOUT_COLS;

    f32 cols[ IMGUI_LAYOUT_COLS + 1 ];
    for ( u32 i = 0; i < n; ++i ) cols[ i ] = 1.0f;   /* all fill -> equal split */
    cols[ n ] = IMGUI_END;
    layout_set( cols, row_h, 0.0f, 0.0f );
}

/* Explicit per-column widths (IMGUI_END-terminated, overloaded units) of height row_h. */
void
imgui_row_track( f32 row_h, const f32* cols )
{
    layout_set( cols, row_h, 0.0f, 0.0f );
}

/* columns -- the explicit header for N pre-divided column tracks (IMGUI_END-terminated, overloaded
   units), auto height, rows accumulating + scrolling.  The canonical name for the multi-column flow
   template; row_track is the same with an explicit row height. */
void
imgui_columns( const f32* tracks )
{
    layout_set( tracks, 0.0f, 0.0f, 0.0f );
}

/* cols_n -- N equal flex columns, auto height: the everyday uniform split (a wrapper over row_cols). */
void
imgui_cols_n( u32 n )
{
    imgui_row_cols( 0.0f, n );
}

/* Fixed-arity weighted rows -- the everyday 2/3/4-column split without a track array or its
   IMGUI_END terminator.  Each width takes the overloaded unit (>1 px, 1 fill, (0,1) fraction, 0
   natural), so row2( 0.3f, 0.7f ) is a 30/70 split and row2( 120, 1 ) is a 120px column plus a fill.
   Auto height (the common case); reach for row_track / layout when a fixed height or >4 columns
   is needed. */

void imgui_row2( f32 a, f32 b )                { f32 c[ 3 ] = { a, b, IMGUI_END };       layout_set( c, 0.0f, 0.0f, 0.0f ); }
void imgui_row3( f32 a, f32 b, f32 c )         { f32 t[ 4 ] = { a, b, c, IMGUI_END };    layout_set( t, 0.0f, 0.0f, 0.0f ); }
void imgui_row4( f32 a, f32 b, f32 c, f32 d )  { f32 t[ 5 ] = { a, b, c, d, IMGUI_END }; layout_set( t, 0.0f, 0.0f, 0.0f ); }

/* same_line -- keep the next widget on the line of the one just emitted, instead of breaking to a
   new row.  It is placed just past the previous item: `spacing` is the pixel gap (0 = flush; < 0 =
   the theme's default widget gap).  The widget takes its natural width (a button to its label, text
   to its glyphs); a widget with no natural width fills to the content's right edge.  The next plain
   widget after it resumes a fresh row below the line.  No-op before any widget has emitted in the
   region.  Mirrors ImGui::SameLine; built entirely on the cell engine's prev_item anchor. */
void
imgui_same_line( f32 spacing )
{
    layout_frame_t* f = lf();
    if ( f->prev_item.w <= 0.0f && f->prev_item.h <= 0.0f ) return;   /* nothing to continue from */

    f32 gap   = ( spacing >= 0.0f ) ? spacing : WIDGET_GAP;
    f->cont_x = f->prev_item.x + f->prev_item.w + gap;
    f->cont_line = true;
}

/* stack_sameline -- the mode-prefixed name for same_line; identical behavior.  The stack_ spelling
   groups the "keep the next widget on this line" verb with the stack() header. */
void
imgui_stack_sameline( f32 spacing )
{
    imgui_same_line( spacing );
}

/* Field split -- the labeled value widgets (input_text, slider_float, checkbox) split their cell
   into a label track + a control track and lay out as an aligned "Label  [control]" form from a
   single call.  `side` places the label on the left or right; `label` / `control` are two sizes in
   the same overloaded unit as columns (>1 px, 1 fill, (0,1) fraction, 0 natural), so field_split(
   LEFT, 0.4f, 0.6f ) is a 40/60 split and field_split( LEFT, 120, 1 ) is a 120px label + fill control.
   Pass IMGUI_LABEL_NONE to turn it off (back to the trailing natural-width label).  Set once on a
   region; it persists like the row template until changed, and is resolved against whatever cell
   each widget is handed -- a full row or a single column. */
void
imgui_field_split( imgui_label_side_t side, f32 label, f32 control )
{
    layout_frame_t* f   = lf();
    f->lay_field_side    = (u8)side;
    f->lay_field_label   = label;
    f->lay_field_control = control;
}

/* field_split sugar -- a fixed-width label column with a flex control filling the rest, on the
   left or the right.  width <= 0 turns the field split off (restores the trailing label). */
void imgui_field_label_left ( f32 width ) { imgui_field_split( width > 0.0f ? IMGUI_LABEL_LEFT  : IMGUI_LABEL_NONE, width, 1.0f ); }
void imgui_field_label_right( f32 width ) { imgui_field_split( width > 0.0f ? IMGUI_LABEL_RIGHT : IMGUI_LABEL_NONE, width, 1.0f ); }

/* form_split -- the mode-prefixed name for field_split; identical behavior.  The form_* spelling
   groups the label/control split with the form() header. */
void
imgui_form_split( imgui_label_side_t side, f32 label, f32 control )
{
    imgui_field_split( side, label, control );
}

/* form -- a stack of aligned "Label  [control]" rows: a single flex column (stack) with a field
   split installed in one call.  label_w is the fixed label-track width on `side`, the control
   flex-fills the rest; label_w <= 0 turns the split off (a plain stack).  The reflection-tweaker /
   settings-panel header. */
void
imgui_form( imgui_label_side_t side, f32 label_w )
{
    imgui_stack();
    imgui_field_split( label_w > 0.0f ? side : IMGUI_LABEL_NONE, label_w, 1.0f );   /* label px + fill control */
}

/* Content alignment -- where each widget's natural-sized content sits inside its cell (a label, an
   image, a text run; a frame-filling widget like button / input still fills the cell and only its
   label follows).  Set once on a region; it persists like the row template and the field split
   until changed, and is independent of the columns -- row() / row_cols() leave it untouched, while
   layout_default() clears it back to LEFT | TOP.  Orthogonal to field_split, which positions a
   label *track*; align positions content *within* whatever cell a widget is handed. */
void
imgui_align( imgui_align_t a )
{
    lf()->lay_align = (u8)a;
}

/* Grid mode: partition the band from the pen to the region bottom into desc.cols x desc.rows
   (both IMGUI_END-terminated, overloaded units).  Uses cols, rows, gaps, and align; row_h is
   flow-only and ignored.  Widgets then fill cells row-major; nothing scrolls. */
void
imgui_grid( imgui_layout_t desc )
{
    layout_set_grid( desc.cols, desc.rows, desc.gap_x, desc.gap_y );
    lf()->lay_align = (u8)desc.align;   /* full template carries the content alignment too */
}

/* nc x nr equal flex cells filling the band -- the uniform grid (image grids, dashboards). */
void
imgui_grid_cells( u32 nc, u32 nr )
{
    if ( nc == 0 )                nc = 1;
    if ( nr == 0 )                nr = 1;
    if ( nc > IMGUI_LAYOUT_COLS ) nc = IMGUI_LAYOUT_COLS;
    if ( nr > IMGUI_LAYOUT_COLS ) nr = IMGUI_LAYOUT_COLS;

    f32 cols[ IMGUI_LAYOUT_COLS + 1 ];
    f32 rows[ IMGUI_LAYOUT_COLS + 1 ];
    for ( u32 i = 0; i < nc; ++i ) cols[ i ] = 1.0f;   /* all fill -> equal columns */
    for ( u32 i = 0; i < nr; ++i ) rows[ i ] = 1.0f;   /* all fill -> equal rows    */
    cols[ nc ] = IMGUI_END;
    rows[ nr ] = IMGUI_END;
    layout_set_grid( cols, rows, 0.0f, 0.0f );
}

/*----------------------------------------------------------------------------------------------
    Pack mode -- the print run: place items one after another along an axis at their natural size.

    pack( dir ) opens a run; bar() is the horizontal pack (a toolbar), strip() the vertical one.
    Each widget takes its natural main-axis size unless pack_size() overrides the next one, resolved
    against the space left on the line (0 natural, 1 fill the rest, (0,1) a fraction, >1 px).  A
    widget with no natural width (slider / input / selectable) fills the remainder of the line by
    default.  pack_nextline() breaks to a fresh line.  Mode persists like any other until re-set.
----------------------------------------------------------------------------------------------*/

/* pack -- open a print run along `dir`.  Finishes any flow row above it, then seeds the pack pen
   at the current layout position: the main axis runs along dir from there, the cross axis from the
   content edge.  Fill it with widgets (bar / strip are the sugar). */
void
imgui_pack( imgui_pack_dir_t dir )
{
    layout_frame_t* f = lf();
    layout_row_break( f );            /* finish any flow row above the run */

    f->mode           = IMGUI_MODE_PACK;
    f->pack_dir       = (u8)dir;
    f->pack_size_next = -1.0f;        /* next item is natural until pack_size() */
    f->lay_ncols      = 1;            /* non-zero: pack bypasses the column walk */
    f->lay_nrows      = 0;
    f->col            = 0;
    f->row            = 0;
    f->pack_line      = 0.0f;
    f->prev_item      = ( imgui_rect_t ){ 0 };
    f->cont_line      = false;

    if ( dir == IMGUI_PACK_HORIZONTAL )
    {
        f->pack_main  = f->content_x;     /* x pen runs along the line     */
        f->pack_cross = f->cursor_y;      /* y top of the current line     */
    }
    else
    {
        f->pack_main  = f->cursor_y;      /* y pen runs down the column    */
        f->pack_cross = f->content_x;     /* x left of the current column  */
    }
    f->pack_origin_main = f->pack_main;
}

/* bar -- horizontal pack: items left to right (the toolbar). */
void imgui_bar( void ) { imgui_pack( IMGUI_PACK_HORIZONTAL ); }

/* strip -- vertical pack: items top to bottom at their natural height. */
void imgui_strip( void ) { imgui_pack( IMGUI_PACK_VERTICAL ); }

/* pack_size -- set the next packed item's main-axis measure (overloaded unit, resolved against the
   space remaining on the current line); cleared back to natural after that one item. */
void imgui_pack_size( f32 unit ) { lf()->pack_size_next = unit; }

/* pack_nextline -- break to a fresh line: reset the main pen to the line start and step the cross
   axis past the line just laid.  No-op outside pack mode. */
void
imgui_pack_nextline( void )
{
    layout_frame_t* f = lf();
    if ( f->mode != IMGUI_MODE_PACK ) return;

    if ( f->pack_dir == IMGUI_PACK_HORIZONTAL )
    {
        f->pack_cross += f->pack_line + f->lay_gap_y;   /* drop below the line */
        f->pack_main   = f->content_x;                  /* back to the left    */
        f->cursor_y    = f->pack_cross;
    }
    else
    {
        f->pack_cross += f->pack_line + f->lay_gap_x;   /* move past the column */
        f->pack_main   = f->pack_origin_main;           /* back to the top      */
    }
    f->pack_line = 0.0f;
    f->prev_item = ( imgui_rect_t ){ 0 };
}

/* Region padding: re-inset the current region's content area and clear the template back to
   undeclared at the padded top-left.  Call right after opening a region; declare a mode header
   (stack / columns / grid / ...) afterward, since pad() leaves the region with no template. */
void
imgui_pad( imgui_pad_t p )
{
    layout_frame_t* f = lf();
    f->content_x     = f->outer.x + p.l - *f->scroll_x;
    f->content_w     = f->outer.w - p.l - p.r - f->sb_w;
    f->origin_x      = f->outer.x + p.l;
    f->origin_y      = f->outer.y + p.t;
    f->cursor_x      = f->content_x;
    f->cursor_y      = f->outer.y + p.t - *f->scroll_y;
    f->content_max_x = f->content_x;
    f->content_y_max = f->outer.y + f->outer.h - p.b - f->sb_h;   /* grid band end, new bottom pad */

    layout_clear( f );   /* re-inset clears the template -- declare a mode header again after pad() */
}

/*----------------------------------------------------------------------------------------------
    Layout metrics -- theme-derived sizes for pre-computing fixed row / column dimensions.

    line_h / text_w are the raw font metrics a caller cannot compute itself.  h_min / w_min are
    the standard margin a row / cell puts around its content -- the "size without content".
    calc_row / calc_col add that margin to a content pixel size, giving a fixed dimension that
    fits the content plus breathing room:

        imgui()->row( imgui()->calc_row( 128 ) );          // a row sized for a 128px image
        f32 w = imgui()->calc_col( imgui()->text_w("X") ); // a column sized to a label
----------------------------------------------------------------------------------------------*/

/* Height of one line of text in the active font. */
f32 imgui_line_h( void ) { return font_line_h(); }

/* Pixel width of a string in the active font (whole string, no "##" handling). */
f32 imgui_text_w( const char* s ) { return font_text_w( s ); }

/* Standard vertical margin a row adds around its content (so calc_row( char_h ) == one row). */
f32 imgui_h_min( void ) { f32 m = WIDGET_H - font_char_h(); return m > 0.0f ? m : 0.0f; }

/* Standard horizontal margin a cell adds around its content (a left + right content inset). */
f32 imgui_w_min( void ) { return 2.0f * WIDGET_PAD; }

/* Fixed row height / column width that fits content_* pixels plus the standard margin. */
f32 imgui_calc_row( f32 content_h ) { return content_h + imgui_h_min(); }
f32 imgui_calc_col( f32 content_w ) { return content_w + imgui_w_min(); }

/* Remaining free space in the current region from the layout pen -- the GetContentRegionAvail
   analogue.  Width is what a flex widget would fill (the content column from the pen to its right
   edge); height is the room left before the region bottom (the grid band end / view bottom).  Use
   it to size a begin_child to the leftover space, or to lay widgets out by hand.  Measured from the
   pen, so call it where the next widget would land; the height is most meaningful before scrolling. */
imgui_vec2_t
imgui_content_avail( void )
{
    layout_frame_t* f = lf();
    f32 w = ( f->content_x + f->content_w ) - f->cursor_x;
    f32 h = f->content_y_max - f->cursor_y;
    if ( w < 0.0f ) w = 0.0f;
    if ( h < 0.0f ) h = 0.0f;
    return ( imgui_vec2_t ){ w, h };
}

/* Screen position where the next item would be emitted -- the GetCursorScreenPos analogue.  Anchor
   custom draw_* geometry to the layout pen without reserving a cell first; pair with content_avail()
   for the space ahead.  (Read it where the next widget would land -- it advances as items emit.) */
imgui_vec2_t
imgui_cursor_screen_pos( void )
{
    layout_frame_t* f = lf();
    return ( imgui_vec2_t ){ f->cursor_x, f->cursor_y };
}

/* Reserve a w x h block in the layout and return its screen rect, advancing the pen like any widget
   (the Dummy analogue) -- blank space, or a slot to fill with custom draw_* geometry / make clickable
   with invisible_button.  `w` is the main-axis size: honored in a pack run or on a same_line, while
   column / grid flow sizes the width to the track as for every widget.  The returned rect is always
   the actual reserved space, so draw into it rather than assuming w x h. */
imgui_rect_t
imgui_dummy( f32 w, f32 h )
{
    return widget_next_rect_w( w, h );
}

/*----------------------------------------------------------------------------------------------
    indent / unindent -- shift the active region's content column right (or back), so subsequent
    rows lay out inset.  The single mechanism behind tree_node's nesting, but usable on its own to
    inset any block of widgets.  w <= 0 uses the standard step (one row height, so a tree child
    lines up under its parent's label, past the fold arrow).  Finishes any open row first, moves
    the pen to the new column edge, and re-resolves the flow template against the narrowed width;
    always balance an indent with an unindent of the same width.  Flow layouts (stack / columns)
    only -- a grid / pack carries its own resolved geometry and ignores the reflow.
----------------------------------------------------------------------------------------------*/

void
imgui_indent( f32 w )
{
    layout_frame_t* f = lf();
    if ( w <= 0.0f ) w = WIDGET_H;       /* default step: one row height (aligns under the arrow) */

    layout_row_break( f );               /* close the current row before shifting the column */
    f->content_x += w;
    f->content_w -= w;
    if ( f->content_w < 0.0f ) f->content_w = 0.0f;
    f->cursor_x   = f->content_x;
    layout_reflow( f );
}

void
imgui_unindent( f32 w )
{
    layout_frame_t* f = lf();
    if ( w <= 0.0f ) w = WIDGET_H;

    layout_row_break( f );
    f->content_x -= w;
    f->content_w += w;
    f->cursor_x   = f->content_x;
    layout_reflow( f );
}

/*----------------------------------------------------------------------------------------------
    push_id / pop_id -- add a temporary id-scope level for repeated widgets within one region.

    Widget ids are already region-seeded, so this is only needed to separate widgets that share a
    label in the same region (e.g. list rows keyed by index).  push_id combines its key onto the
    current scope; pop_id removes one level.  Always balance them -- a region pop restores the
    scope depth anyway, so a stray push cannot escape its region, but balancing keeps ids stable.
----------------------------------------------------------------------------------------------*/

void imgui_push_id    ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ) ) ); }
void imgui_push_id_int( i32 i )           { id_push( id_combine( id_seed(), (u32)i ) ); }
void imgui_pop_id     ( void )            { id_pop(); }

/*----------------------------------------------------------------------------------------------
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    push/pop affect every widget until popped (and nest); next_item_flag is a one-shot override the
    very next widget consumes, no pop needed.  The merged value is resolved once per widget at emit
    time and read by widget_behavior / the widget, so a new flag never touches a call site or the
    vtable layout consumers see.  See imgui_item_flags_t in imgui.h for the model and the flags.

        imgui()->push_item_flag( IMGUI_ITEM_DISABLED, true );
        imgui()->button( "Off A" );  imgui()->button( "Off B" );   // both disabled
        imgui()->pop_item_flag();

        imgui()->next_item_flag( IMGUI_ITEM_DISABLED, true );
        imgui()->button( "Only this one is disabled" );
----------------------------------------------------------------------------------------------*/

void imgui_push_item_flag( imgui_item_flags_t flag, bool enable ) { item_flag_push( flag, enable ); }
void imgui_pop_item_flag ( void )                                 { item_flag_pop(); }
void imgui_next_item_flag( imgui_item_flags_t flag, bool enable ) { item_flag_next( flag, enable ); }

/*----------------------------------------------------------------------------------------------
    push_style_color / push_style_var (+ pop / next) -- the push-model theme override.

    push overrides a slot for every widget until the matching pop; pop takes a count, so two pushes
    are undone with one pop_style_*( 2 ), mirroring ImGui.  next_style_* overrides a slot for just
    the next widget, no pop needed.  Colors are abgr (IMGUI_COLOR); vars are f32 pixels.  The slots
    are imgui_col_t / imgui_style_var_t.  See imgui_style.c for the resolution model.

        imgui()->push_style_color( IMGUI_COL_WIDGET_BG,  IMGUI_COLOR( 0xFF,0,0,0xFF ) );  // red
        imgui()->push_style_color( IMGUI_COL_WIDGET_HOT, IMGUI_COLOR( 0xFF,0x40,0x40,0xFF ) );
        imgui()->button( "Red Button" );
        imgui()->pop_style_color( 2 );                                                    // both

        imgui()->push_style_var( IMGUI_VAR_WIDGET_PAD, 20.0f );
        imgui()->button( "Roomy" );
        imgui()->pop_style_var( 1 );
----------------------------------------------------------------------------------------------*/

void imgui_push_style_color( imgui_col_t slot, u32 abgr )       { style_push_color( slot, abgr ); }
void imgui_pop_style_color ( u32 count )                        { style_pop_color( count ); }
void imgui_next_style_color( imgui_col_t slot, u32 abgr )       { style_next_color( slot, abgr ); }

void imgui_push_style_var( imgui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void imgui_pop_style_var ( u32 count )                          { style_pop_var( count ); }
void imgui_next_style_var( imgui_style_var_t var, f32 value )   { style_next_var( var, value ); }

// clang-format on
/*============================================================================================*/
