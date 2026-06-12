/*==============================================================================================

    runtime_service/imgui/imgui_layout.c -- Scrollable layout regions + the layout stack engine.

    A "region" is a rectangular area with its own layout pen, content column, clip, and
    scrollbars.  Both the window body and a begin_child box are regions: begin_window and
    begin_child resolve a box, then call layout_push_region; their matching end calls
    layout_pop_region.  Regions nest on the layout-frame stack (imgui_ctx.c), so a list box
    with its own scrollbar can live inside a window and the parent layout resumes below it.

    The scroll mechanics here -- two-pass gutter reservation, wheel handling, clamping, content
    measurement, and the scrollbar widget -- were lifted out of the window so windows and child
    regions share one engine.  window_scrollbar() is axis-generic and id-parameterized; it is
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
    allocates here.  Entries are stamped with the frame they were last seen and the
    least-recently-seen slot is recycled, so transient ids do not leak the pool.
----------------------------------------------------------------------------------------------*/

#define IMGUI_MAX_REGIONS 128

typedef struct
{
    imgui_id_t id;                  /* 0 = free slot                                   */
    f32        scroll_x, scroll_y;  /* persisted scroll offset                         */
    f32        content_w, content_h;/* content extent measured last frame              */
    u32        seen_frame;          /* last frame touched -- drives recycling          */

} imgui_region_t;

static imgui_region_t s_regions[ IMGUI_MAX_REGIONS ];

/* Find the region for this id, or claim a slot for it (free first, else least-recently-seen).
   A freshly claimed slot starts zeroed, so a new child opens at the top with no measured size. */
static imgui_region_t*
region_get( imgui_id_t id )
{
    imgui_region_t* freeslot = NULL;
    imgui_region_t* oldest   = &s_regions[ 0 ];

    for ( u32 i = 0; i < IMGUI_MAX_REGIONS; ++i )
    {
        imgui_region_t* r = &s_regions[ i ];
        if ( r->id == id )
        {
            r->seen_frame = s_frame_counter;
            return r;
        }
        if ( r->id == 0 && !freeslot )            freeslot = r;
        if ( r->seen_frame < oldest->seen_frame ) oldest   = r;
    }

    imgui_region_t* r = freeslot ? freeslot : oldest;
    *r = ( imgui_region_t ){ .id = id, .seen_frame = s_frame_counter };
    return r;
}

/*----------------------------------------------------------------------------------------------
    window_scrollbar -- one scrollbar track + knob along an axis; folds a knob drag into *scroll.

    `vertical` picks the axis; `track` is the full track rect, `content`/`view` the measured and
    visible extents along that axis, and `mouse_along` the live cursor coordinate on it.  The
    knob length tracks the visible fraction (min-clamped so it stays grabbable) and the drag
    maps the cursor back into scroll, mirroring slider_float.  Shared by every region's bars.
----------------------------------------------------------------------------------------------*/

static void
window_scrollbar( imgui_id_t id, imgui_rect_t track, bool vertical,
                  f32 content, f32 view, f32 mouse_along, f32* scroll )
{
    f32 max_scroll = content - view;
    if ( max_scroll < 0.0f ) max_scroll = 0.0f;

    f32 track_len = vertical ? track.h : track.w;
    f32 track_org = vertical ? track.y : track.x;

    widget_state_t st = widget_behavior( id, track, WIDGET_KIND_DRAG );

    /* Knob length is the visible fraction of the track, clamped to a grabbable minimum
       and never longer than the track itself (content <= view => full-length knob). */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = (f32)s_layout.slider_knob_w;
    if ( knob_len < min_len )   knob_len = min_len;
    if ( knob_len > track_len ) knob_len = track_len;
    f32 travel = track_len - knob_len;

    /* Drag maps the cursor (knob centre) back into the scroll offset. */
    if ( st.active && travel > 0.0f )
    {
        f32 t = ( mouse_along - track_org - knob_len * 0.5f ) / travel;
        t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
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
layout_push_region( imgui_id_t id, imgui_rect_t outer, f32 pad, imgui_win_flags_t flags,
                    f32* scroll_x, f32* scroll_y, f32* content_w, f32* content_h, bool own_clip )
{
    layout_frame_t* f = &s_layout_stack[ s_layout_sp++ ];

    f->region_id  = id;
    f->outer      = outer;
    f->pad        = pad;
    f->flags      = flags;
    f->scroll_x   = scroll_x;
    f->scroll_y   = scroll_y;
    f->pcontent_w = content_w;
    f->pcontent_h = content_h;
    f->parent_clip = s_ctx.clip_rect;

    const f32 knob = (f32)s_layout.slider_knob_w;

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
    f32 max_y = last_h - view_h;  if ( max_y < 0.0f ) max_y = 0.0f;
    f32 max_x = last_w - view_w;  if ( max_x < 0.0f ) max_x = 0.0f;
    if ( *scroll_y < 0.0f )   *scroll_y = 0.0f;
    if ( *scroll_y > max_y )  *scroll_y = max_y;
    if ( *scroll_x < 0.0f )   *scroll_x = 0.0f;
    if ( *scroll_x > max_x )  *scroll_x = max_x;

    /* Content column + pen.  origin_* is the unscrolled top-left used to measure extent at pop;
       the live pen is biased by -scroll so widgets slide under the clip. */
    f->origin_x      = outer.x + pad;
    f->origin_y      = outer.y + WIDGET_GAP;
    f->content_x     = outer.x + pad - *scroll_x;
    f->content_w     = outer.w - 2.0f * pad - f->sb_w;
    f->cursor_x      = f->content_x;
    f->cursor_y      = outer.y + WIDGET_GAP - *scroll_y;
    f->content_max_x = f->content_x;   /* seed extent at the origin -> an empty body measures 0 */

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
        window_scrollbar( f->region_id ^ IMGUI_SCROLLBAR_SALT, track, true,
                          content_h, f->view_h, s_io.mouse_y, f->scroll_y );
    }
    if ( f->show_h )
    {
        imgui_rect_t track = { f->outer.x + WIN_BORDER,
                               f->outer.y + f->outer.h - WIN_BORDER - f->sb_h, f->view_w, f->sb_h };
        window_scrollbar( f->region_id ^ IMGUI_HSCROLLBAR_SALT, track, false,
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

        f32 max_y = content_h - f->view_h;  if ( max_y < 0.0f ) max_y = 0.0f;
        f32 max_x = content_w - f->view_w;  if ( max_x < 0.0f ) max_x = 0.0f;
        if ( *f->scroll_y < 0.0f )  *f->scroll_y = 0.0f;
        if ( *f->scroll_y > max_y ) *f->scroll_y = max_y;
        if ( *f->scroll_x < 0.0f )  *f->scroll_x = 0.0f;
        if ( *f->scroll_x > max_x ) *f->scroll_x = max_x;

        s_ctx.wheel_used = true;
    }

    /* Pop the frame and advance the parent pen past the region box, so the parent's next
       widget lands directly below it.  The root region (a window body) has no parent frame. */
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
----------------------------------------------------------------------------------------------*/

bool
imgui_begin_child( const char* id_str, f32 w, f32 h, imgui_win_flags_t flags )
{
    layout_frame_t* parent = lf();

    /* Combine the parent region id so the same child label nests safely under different
       parents (and never collides with a window id, which is its own region id). */
    imgui_id_t id = id_hash( id_str ) ^ ( parent->region_id * 0x9E3779B1u + 0x85EBCA77u );

    if ( w <= 0.0f ) w = parent->content_w;   /* default: fill the remaining content width */

    /* Box reserved at the current pen.  The parent pen is advanced by layout_pop_region, not
       here, so the advance accounts for the whole box exactly once. */
    imgui_rect_t box = { parent->content_x, parent->cursor_y, w, h };

    imgui_region_t* rg = region_get( id );

    /* Child frame: body fill + border, drawn under the parent clip before the region clips in. */
    draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0,0,1,1, 0, COL_CHILD_BG );
    draw_push_rect_outline( box.x, box.y, box.w, box.h, WIN_BORDER, 0, COL_BORDER );

    layout_push_region( id, box, WIDGET_PAD, flags,
                        &rg->scroll_x, &rg->scroll_y, &rg->content_w, &rg->content_h,
                        /* own_clip */ true );   /* the child's own scissor -- second clip in the window */

    /* No collapse concept for a child: always returns true, always pair with end_child. */
    return true;
}

void
imgui_end_child( void )
{
    layout_pop_region();
}

// clang-format on
/*============================================================================================*/
