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
    f32 min_len  = (f32)s_layout.slider_knob_w;
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

    /* Open with the default template: a single flex column of auto height (the classic stack).
       imgui_layout / the row sugar replace it; a fresh region always starts from the default. */
    layout_set_default( f );

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
----------------------------------------------------------------------------------------------*/

bool
imgui_begin_child( const char* id_str, f32 w, f32 h, imgui_win_flags_t flags )
{
    layout_frame_t* parent = lf();

    /* Combine against the active id scope (the parent region, plus any push_id) so the same child
       label nests safely under different parents and never collides with a window id. */
    imgui_id_t id = id_combine( id_seed(), id_hash( id_str ) );

    /* Where the child box lands: in a grid parent it takes the next cell (w / h ignored -- the
       cell sizes it, the natural way to drop a scroll region into a split pane); in flow it sits
       at the pen, on its own line.  The parent pen / grid cursor is advanced by layout_pop_region
       for flow, but the grid cursor must step here since pop does not touch (col,row). */
    imgui_rect_t box;
    if ( parent->lay_nrows > 0 )
    {
        box = grid_next_rect( parent );   /* item_pad-inset cell; advances the matrix cursor */
    }
    else
    {
        layout_row_break( parent );       /* a flow child starts on its own line */
        if ( w <= 0.0f ) w = parent->content_w;   /* default: fill the remaining content width */
        box = ( imgui_rect_t ){ parent->content_x, parent->cursor_y, w, h };
    }

    imgui_region_t* rg = region_get( id );

    /* Child frame: body fill + border, drawn under the parent clip before the region clips in. */
    draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0,0,1,1, 0, COL_CHILD_BG );
    draw_push_rect_outline( box.x, box.y, box.w, box.h, WIN_BORDER, 0, COL_BORDER );

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags,
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

/*----------------------------------------------------------------------------------------------
    Public layout API -- shape the active region's repeating row template.

    These set the template on the current region; it persists and repeats for every subsequent
    widget until set again (or the region ends).  No push/pop: a region opens with the default
    (one flex column, auto height), and each call simply replaces it.  See imgui_layout_t for
    the column unit rule.  imgui_pad sets the region padding -- the inset between the region box
    and where the layout starts -- distinct from the item padding carried in the template.
----------------------------------------------------------------------------------------------*/

/* Full flow template: columns, row height, item padding, and gaps in one call.  (rows[] is for
   grid mode -- use imgui_grid; a zero-initialized descriptor is flow, so this ignores rows.) */
void
imgui_layout( imgui_layout_t desc )
{
    layout_set( desc.cols, desc.row_h, desc.item_pad, desc.gap_x, desc.gap_y );
}

/* Single full-width column of height row_h (0 = auto) -- back to the classic vertical stack. */
void
imgui_row( f32 row_h )
{
    layout_set( NULL, row_h, ( imgui_pad_t ){ 0 }, 0.0f, 0.0f );
}

/* n equal flex columns of height row_h (0 = auto, one standard line). */
void
imgui_row_cols( f32 row_h, u32 n )
{
    if ( n == 0 )                 n = 1;
    if ( n > IMGUI_LAYOUT_COLS )  n = IMGUI_LAYOUT_COLS;

    f32 cols[ IMGUI_LAYOUT_COLS + 1 ];
    for ( u32 i = 0; i < n; ++i ) cols[ i ] = 0.0f;   /* all flex -> equal split */
    cols[ n ] = IMGUI_END;
    layout_set( cols, row_h, ( imgui_pad_t ){ 0 }, 0.0f, 0.0f );
}

/* Explicit per-column widths (IMGUI_END-terminated, overloaded units) of height row_h. */
void
imgui_row_track( f32 row_h, const f32* cols )
{
    layout_set( cols, row_h, ( imgui_pad_t ){ 0 }, 0.0f, 0.0f );
}

/* Grid mode: partition the band from the pen to the region bottom into desc.cols x desc.rows
   (both IMGUI_END-terminated, overloaded units).  Uses cols, rows, item_pad, and gaps; row_h is
   flow-only and ignored.  Widgets then fill cells row-major; nothing scrolls. */
void
imgui_grid( imgui_layout_t desc )
{
    layout_set_grid( desc.cols, desc.rows, desc.item_pad, desc.gap_x, desc.gap_y );
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
    for ( u32 i = 0; i < nc; ++i ) cols[ i ] = 0.0f;   /* all flex -> equal columns */
    for ( u32 i = 0; i < nr; ++i ) rows[ i ] = 0.0f;   /* all flex -> equal rows    */
    cols[ nc ] = IMGUI_END;
    rows[ nr ] = IMGUI_END;
    layout_set_grid( cols, rows, ( imgui_pad_t ){ 0 }, 0.0f, 0.0f );
}

/* Region padding: re-inset the current region's content column and reset the pen to the padded
   top-left.  Call right after opening a region (l/r/t take effect this frame). */
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
    f->col           = 0;
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

// clang-format on
/*============================================================================================*/
