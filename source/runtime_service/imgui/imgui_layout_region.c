/*==============================================================================================

    runtime_service/imgui/imgui_layout_region.c -- Scroll region engine.

    Implements the shared scrollable-region mechanism used by both window bodies and
    begin_child boxes:

        imgui_region_t      persistent scroll + content-size state, keyed by id
        scroll_clamp        pin a scroll offset into [0, content - view]
        region_scrollbar    one axis-generic scrollbar track + knob widget
        layout_push_region  open a region: reserve gutters, clamp scroll, seed a layout frame
        layout_pop_region   close a region: measure content, draw bars, claim the wheel

    The persistent state for begin_child boxes is kept in the shared keyed pool
    (imgui_state_get); window bodies pass pointers to their own imgui_window_t fields.

    Included by imgui.c after imgui_layout_core.c (provides widget_next_rect, widget_behavior,
    layout_frame_t, lf, layout_clear, layout_set_default, item_flags_chrome_reset) and before
    imgui_layout_child.c and imgui_layout.c which call layout_push/pop_region.

==============================================================================================*/
#include "runtime_service/imgui/imgui_internal.h"   /* imgui_region_t, layout_frame_t, imgui_window_t */
// clang-format off

/*----------------------------------------------------------------------------------------------
    Scrollbar ids -- distinct salts so a region's vertical and horizontal bars never share an
    id, nor collide with a label-hashed widget in the same window.  Applied to a region id.
----------------------------------------------------------------------------------------------*/

#define IMGUI_SCROLLBAR_SALT  0x5C011B01u
#define IMGUI_HSCROLLBAR_SALT 0x5C011B02u

/* Grab offset within the knob at the moment of press.  Single-slot: only one scrollbar can be
   active (own active_id) at a time, so this covers every bar on every region. */
static f32 s_sb_grab_off = 0.0f;

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

/* imgui_region_t (persistent begin_child scroll + content-size state) is defined in imgui_internal.h. */

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

    /* Knob length is the visible fraction of the track, clamped to a grabbable minimum
       and never longer than the track itself (content <= view => full-length knob).  The
       min-then-cap order matters: a track shorter than the minimum collapses to track_len,
       so it is not folded into one clampf (whose bounds would invert). */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = SLIDER_KNOB_W;
    if ( knob_len < min_len )   knob_len = min_len;
    if ( knob_len > track_len ) knob_len = track_len;
    f32 travel = track_len - knob_len;

    /* Derive the current knob position before any interaction this frame -- used for
       press-hit-detection and (after possible update) for drawing. */
    f32 t_cur    = ( max_scroll > 0.0f ) ? *scroll / max_scroll : 0.0f;
    f32 knob_off = track_org + t_cur * travel;

    widget_state_t st = widget_behavior( id, track, WIDGET_KIND_DRAG );

    /* On the press frame, decide whether the cursor landed on the knob (drag from the grabbed
       point) or in the gutter (jump: center the knob under the cursor).  s_sb_grab_off is the
       offset from the knob's leading edge to the cursor and stays fixed for the whole drag. */
    if ( st.pressed )
    {
        if ( mouse_along >= knob_off && mouse_along <= knob_off + knob_len )
            s_sb_grab_off = mouse_along - knob_off;   /* preserve the grab point within handle */
        else
            s_sb_grab_off = knob_len * 0.5f;          /* gutter click: center knob on cursor  */
    }

    /* Drag maps the cursor back into the scroll offset via the grab offset. */
    if ( st.active && travel > 0.0f )
    {
        f32 t = saturate( ( mouse_along - track_org - s_sb_grab_off ) / travel );
        *scroll = t * max_scroll;
        t_cur    = t;
        knob_off = track_org + t_cur * travel;
    }

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
    f->parent_clip = s_build.clip_rect;

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
        s_build.clip_rect = rect_intersect( f->parent_clip, clip );   /* hit-test clip = the box */
    }
    else
    {
        f->pushed_clip  = false;
        /* s_build.clip_rect stays the enclosing clip -- the window body inherits it. */
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
    s_build.clip_rect = f->parent_clip;

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
    if ( !s_build.wheel_used
         && !( f->flags & IMGUI_WIN_NOMOUSESCROLL )
         && s_build.win_id == s_interaction.hover_win
         && s_interaction.active_id == IMGUI_ID_NONE
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

        s_build.wheel_used = true;
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

// clang-format on
/*============================================================================================*/
