/*==============================================================================================

    runtime_service/imgui/imgui_widget_core.c -- Shared widget primitives + theme.

    The foundation the rest of the widget layer is built on: the layout-derived size
    macros, the color palette, the per-widget interaction state machine, and the layout
    cursor helpers.  Both the leaf widgets (imgui_widget.c) and the window chrome
    (imgui_widget_window.c) draw and interact through these, so they live here, ahead of
    both in the unity build.

    Interaction uses the classic hover/active/focused model:
        hover   : the cursor is over the widget this frame
        active  : the primary button is held with this widget as the target
        focused : this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_window.c so s_ctx, s_io, s_layout, rect_hit, and the
    draw helpers are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

#define WIDGET_H      ( (f32)s_layout.line_size     )
#define WIDGET_GAP    ( (f32)s_layout.widget_gap    )
#define WIDGET_PAD    ( (f32)s_layout.widget_pad    )
#define WIN_TITLE_H   ( (f32)s_layout.win_title_h   )
#define WIN_BORDER    ( (f32)s_layout.win_border    )
#define CHECKBOX_SZ   ( (f32)s_layout.checkbox_sz   )
#define SLIDER_KNOB_W ( (f32)s_layout.slider_knob_w )

/* Default region padding (the inset every window body / child opens with): pad columns by
   WIDGET_PAD left and right, offset the first row by WIDGET_GAP, no bottom reserve. */
#define REGION_PAD_DEFAULT ( ( imgui_pad_t ){ WIDGET_PAD, WIDGET_PAD, WIDGET_GAP, 0.0f } )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

#define COL_WIN_BG       IMGUI_COLOR( 0x24, 0x24, 0x24, 0xE8 )
#define COL_CHILD_BG     IMGUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF )   /* child region body, inset darker than the window */
#define COL_TITLE_BG     IMGUI_COLOR( 0x10, 0x60, 0xA0, 0xFF )
#define COL_BORDER       IMGUI_COLOR( 0x80, 0x80, 0x80, 0xFF )
#define COL_TEXT         IMGUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF )
#define COL_TEXT_DIM     IMGUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF )
#define COL_WIDGET_BG    IMGUI_COLOR( 0x40, 0x40, 0x40, 0xFF )
#define COL_WIDGET_HOT   IMGUI_COLOR( 0x50, 0x80, 0xB0, 0xFF )
#define COL_WIDGET_ACT   IMGUI_COLOR( 0x30, 0x60, 0x90, 0xFF )
#define COL_WIDGET_FG    IMGUI_COLOR( 0x20, 0x90, 0xD0, 0xFF )
#define COL_CHECK_MARK   IMGUI_COLOR( 0x20, 0xC0, 0x60, 0xFF )
#define COL_SLIDER_TRACK IMGUI_COLOR( 0x30, 0x30, 0x30, 0xFF )
#define COL_RESIZE_HOT   IMGUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF )   /* bold edge line when a resize border is hot */
#define COL_INPUT_BG     IMGUI_COLOR( 0x38, 0x38, 0x38, 0xFF )
#define COL_INPUT_FOCUS  IMGUI_COLOR( 0x20, 0x50, 0x70, 0xFF )
#define COL_CURSOR       IMGUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF )

/*----------------------------------------------------------------------------------------------
    Layout cursor helpers
----------------------------------------------------------------------------------------------*/

static f32 widget_right( void ) { return lf()->content_x + lf()->content_w; }

/* Grow the active region's measured horizontal extent to include a widget that draws out to
   right_x (in screen coords).  layout_pop_region cancels the scroll bias and compares the
   total against the view to decide the horizontal scrollbar, mirroring how cursor_y travel
   measures content height.  Most widgets fill content_w, but text draws its natural width
   and can reach past the view -- that overflow is what the horizontal bar scrolls. */
static void
widget_track_width( f32 right_x )
{
    if ( right_x > lf()->content_max_x )
        lf()->content_max_x = right_x;
}

/*----------------------------------------------------------------------------------------------
    Layout engine -- carve a region's content area into cells from a repeating row template.

    One resolver does both axes (here, only columns are wired); the row template lives on the
    layout frame and persists until changed, so widgets emit cell-by-cell while staying wholly
    agnostic to the layout shape.  See imgui_layout_t (imgui.h) for the overloaded unit rule.
----------------------------------------------------------------------------------------------*/

/* Resolve a track list into pixel [pos,size] pairs along one axis.  `n` tracks (>= 1) are laid
   from `origin` across `extent`, with `gap` between each.  Units: >1 fixed px, (0,1] fraction of
   the gap-adjusted extent, 0 flex (equal share of the leftover).  Gaps are removed before the
   split, so a fraction is of usable space and cells tile exactly. */
static void
layout_resolve_tracks( const f32* tracks, u32 n, f32 origin, f32 extent, f32 gap,
                       f32* out_pos, f32* out_size )
{
    f32 avail = extent - gap * (f32)( n - 1 );
    if ( avail < 0.0f ) avail = 0.0f;

    /* Pass 1: fixed + fractional consume space; flex tracks share what's left. */
    f32 used = 0.0f;
    u32 flex = 0;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 t = tracks[ i ];
        if      ( t == 0.0f ) ++flex;
        else if ( t <= 1.0f ) used += t * avail;
        else                  used += t;
    }
    f32 leftover  = avail - used;
    if ( leftover < 0.0f ) leftover = 0.0f;
    f32 flex_each = flex ? leftover / (f32)flex : 0.0f;

    /* Pass 2: place left-to-right, gap between cells (not before the first / after the last). */
    f32 pos = origin;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 t  = tracks[ i ];
        f32 sz = ( t == 0.0f ) ? flex_each : ( t <= 1.0f ? t * avail : t );
        out_pos [ i ] = pos;
        out_size[ i ] = sz;
        pos += sz + gap;
    }
}

/* Finish a partially-filled row: advance the pen past it and return to column 0.  No-op at a
   row start.  Called before the template changes or a child region opens, so the next thing
   lands on a fresh line rather than overlapping the open row. */
static void
layout_row_break( layout_frame_t* f )
{
    if ( f->col == 0 ) return;
    f->cursor_y = f->row_y + f->row_h_cur + f->lay_gap_y;
    f->col      = 0;
}

/* Install the region's default template: one flex column, auto height -- the classic stack.
   Called when a region opens (no row in progress yet, so no break needed). */
static void
layout_set_default( layout_frame_t* f )
{
    f->lay_cols[ 0 ] = 0.0f;
    f->lay_ncols     = 1;
    f->lay_row_h     = 0.0f;
    f->lay_item_pad  = ( imgui_pad_t ){ 0 };
    f->lay_gap_x     = WIDGET_GAP;
    f->lay_gap_y     = WIDGET_GAP;
    f->col           = 0;
}

/* Replace the active row template on the current frame.  Finishes any open row first, copies the
   IMGUI_END-terminated columns (empty / NULL => one flex column), and resolves gaps (0 => theme
   default).  The next widget starts a fresh row of the new shape; it repeats until set again. */
static void
layout_set( const f32* cols, f32 row_h, imgui_pad_t item_pad, f32 gap_x, f32 gap_y )
{
    layout_frame_t* f = lf();
    layout_row_break( f );

    u32 n = 0;
    if ( cols )
        while ( n < IMGUI_LAYOUT_COLS && cols[ n ] >= 0.0f ) { f->lay_cols[ n ] = cols[ n ]; ++n; }
    if ( n == 0 ) { f->lay_cols[ 0 ] = 0.0f; n = 1; }    /* default to a single flex column */

    f->lay_ncols    = n;
    f->lay_row_h    = row_h;
    f->lay_item_pad = item_pad;
    f->lay_gap_x    = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y    = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;
    f->col          = 0;
}

/* Baseline y to vertically center one line of glyphs in a row of height h starting at y.
   Used by every labeled widget and the window title so the centering math lives in one place. */
static f32 text_center_y( f32 y, f32 h ) { return y + ( h - font_char_h() ) * 0.5f; }

/*----------------------------------------------------------------------------------------------
    Widget label grammar  (Dear ImGui style)

        "Text"        -> display "Text",  id = hash("Text")
        "Text##key"   -> display "Text",  id = hash("Text##key")   distinct ids, same visible text
        "pre###key"   -> display "pre",   id = hash("###key")      id ignores a dynamic prefix

    The visible span ends at the first "##".  A "###" additionally re-roots the id hash at that
    "###", so a label whose visible part changes every frame (a counter, a name) keeps one stable
    id.  Every labeled widget routes its display through label_width / draw_label and its id
    through widget_id, so the grammar is honored uniformly in one place.
----------------------------------------------------------------------------------------------*/

/* Visible byte count: up to the first "##" marker, or the whole string. */
static u32
label_vis_len( const char* s )
{
    u32 i = 0;
    while ( s[ i ] )
    {
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' )    /* s[i+1] is at worst the NUL: safe */
            break;
        ++i;
    }
    return i;
}

/* The substring hashed for the id: the whole label, unless a "###" tail re-roots it there. */
static const char*
label_id_str( const char* s )
{
    for ( u32 i = 0; s[ i ]; ++i )
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' && s[ i + 2 ] == '#' )    /* reads stop at NUL */
            return s + i;
    return s;
}

/* The id for a labeled widget: the active scope seed combined with the label's id key. */
static imgui_id_t widget_id( const char* label ) { return id_combine( id_seed(), id_hash( label_id_str( label ) ) ); }

/* Width / draw of a label's visible span (markers stripped). */
static f32  label_width( const char* s )                        { return font_text_w_n( s, label_vis_len( s ) ); }
static void draw_label ( f32 x, f32 y, u32 c, const char* s )    { draw_push_text_n( x, y, c, s, label_vis_len( s ) ); }

/* Hand the next cell to a widget.  `h` is the widget's natural height, honored only when the row
   is auto-height and single-column (the classic stack); a fixed row_h or a multi-column row sets
   the height instead.  The row resolves once at column 0, then each call returns one cell --
   inset by item_pad -- and advances, wrapping to a fresh row when the columns run out.  The
   widget just fills the rect; it never sees columns, gaps, or padding. */
static imgui_rect_t
widget_next_rect( f32 h )
{
    layout_frame_t* f = lf();
    if ( f->lay_ncols == 0 ) layout_set_default( f );   /* repair a stray-emit (empty) frame */

    /* Resolve the row on its first cell: cell rects, top, and height for the whole row. */
    if ( f->col == 0 )
    {
        layout_resolve_tracks( f->lay_cols, f->lay_ncols, f->content_x, f->content_w,
                               f->lay_gap_x, f->cellx, f->cellw );
        f->row_y     = f->cursor_y;
        f32 base     = ( f->lay_row_h > 0.0f ) ? f->lay_row_h
                     : ( f->lay_ncols == 1 ? h : WIDGET_H );   /* multi-col auto = one line  */
        f->row_h_cur = base + f->lay_item_pad.t + f->lay_item_pad.b;
    }

    u32         c = f->col;
    imgui_pad_t p = f->lay_item_pad;
    imgui_rect_t r = {
        .x = f->cellx[ c ] + p.l,
        .y = f->row_y      + p.t,
        .w = f->cellw[ c ] - p.l - p.r,
        .h = f->row_h_cur  - p.t - p.b,
    };

    widget_track_width( f->cellx[ c ] + f->cellw[ c ] );   /* this cell's right edge -> hscroll */

    /* Advance; wrap to a fresh row when the template's columns are exhausted. */
    if ( ++f->col >= f->lay_ncols )
    {
        f->cursor_y = f->row_y + f->row_h_cur + f->lay_gap_y;
        f->col      = 0;
    }
    return r;
}

/* Split a labeled widget row into a left-hand control rect and a right-hand label.  The label
   keeps its natural width pinned at the row's right-of-control edge; the control takes the rest
   of the row, never shrinking below min_control_w so it stays usable when the label is long
   (the control then overruns under the label, matching the prior per-widget behavior).  Draws
   the label here, vertically centered in the given color, and returns the control rect for the
   caller to interact with and fill.  The single seam every "control + trailing label" widget
   (slider_float, input_text, future combo / drag / color widgets) routes through, so row
   proportions can be retuned in one place. */
static imgui_rect_t
widget_split_label( imgui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    f32 label_w   = label_width( label );
    f32 control_w = row.w - label_w - WIDGET_PAD;
    if ( control_w < min_control_w ) control_w = min_control_w;

    imgui_rect_t control = { row.x, row.y, control_w, row.h };

    draw_label( control.x + control.w + WIDGET_PAD, text_center_y( row.y, row.h ),
                label_color, label );
    return control;
}

/*----------------------------------------------------------------------------------------------
    Interaction state machine
----------------------------------------------------------------------------------------------*/

/* Interaction class for a widget, selected at the call site.  Only the press-time
   behavior differs between widgets; everything else (hover/active/click) is uniform. */
typedef enum
{
    WIDGET_KIND_BUTTON    = 0,   /* press captures active; reports clicked   */
    WIDGET_KIND_DRAG      = 1,   /* press captures active; held for dragging */
    WIDGET_KIND_FOCUSABLE = 2,   /* press also claims keyboard focus         */

} widget_kind_t;

/* Result of one frame of interaction for a widget.  Every widget drives its
   visuals and value changes from these flags instead of touching s_ctx directly. */
typedef struct
{
    bool hover;       /* cursor is over the widget this frame                  */
    bool active;    /* primary button held with this widget as the target    */
    bool pressed;   /* primary button went down on the widget this frame     */
    bool clicked;   /* press + release completed with the cursor still over  */
    bool focused;   /* widget owns keyboard input (focusable widgets)        */

} widget_state_t;

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( imgui_id_t id, imgui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget.

       Modal-while-dragging: once any item owns active_id (a slider, scrollbar, or window
       drag is in flight) every other item is frozen -- only the active item may hover.
       The active item keeps interacting through st.active below, which reads active_id
       directly, so a drag stays live while the cursor sweeps over inert neighbours. */

    bool can_hover = ( s_ctx.active_id == IMGUI_ID_NONE || s_ctx.active_id == id );
    bool win_hover = ( s_ctx.win_id == s_ctx.hover_win );
    bool eligible  = can_hover && win_hover && !s_ctx.win_resize_hot;
    if ( eligible && rect_hit( s_ctx.clip_rect ) && rect_hit( r ) )
         s_ctx.hover_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_ctx.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_ctx.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_ctx.focused_id = id;
    }

    st.hover   = ( s_ctx.hover_id == id );
    st.active  = ( s_ctx.active_id == id );
    st.focused = ( s_ctx.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_ctx.hover_id == id && s_ctx.active_id == id;

    /* Debug overlay: every interactive widget passes through here, so this one site captures
       the hit rects -- tinted by hover/active so the live interaction is visible.  Capture the
       *visible* rect (the widget clipped to the active region clip): a row scrolled fully
       outside its child box has an empty intersection and is not hit-testable, so it is dropped
       from the overlay too, rather than drawing an interaction rect outside the clip box. */
#ifdef IMGUI_DEBUG_OVERLAY
    {
        if ( eligible ) {
            imgui_rect_t vis = rect_intersect( r, s_ctx.clip_rect );
            if ( vis.w > 0.0f && vis.h > 0.0f )
                 DBG_WIDGET( id, vis, st.hover, st.active );
        }
    }
#endif

    return st;
}

/* Background color for a pushbutton / knob style widget, from its interaction state. */
static u32
widget_bg_color( widget_state_t st )
{
    if ( st.active ) return COL_WIDGET_ACT;
    if ( st.hover  ) return COL_WIDGET_HOT;
    return COL_WIDGET_BG;
}

// clang-format on
/*============================================================================================*/
