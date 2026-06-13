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

/* Count an IMGUI_END-terminated track list into out[] (capped), substituting a single flex track
   for an empty / NULL list.  Returns the count.  The source list is never stored -- callers
   resolve it straight into cell geometry, so the template arrays do not live on the frame. */
static u32
layout_copy_tracks( const f32* src, f32* out )
{
    u32 n = 0;
    if ( src )
        while ( n < IMGUI_LAYOUT_COLS && src[ n ] >= 0.0f ) { out[ n ] = src[ n ]; ++n; }
    if ( n == 0 ) { out[ 0 ] = 0.0f; n = 1; }   /* default to a single flex track */
    return n;
}

/* Install the region's default template: one flex column, auto height -- the classic stack.
   Resolves immediately (content_x/content_w must already be set), so the single column fills the
   content width with no gap.  Called when a region opens and by imgui_pad after re-insetting. */
static void
layout_set_default( layout_frame_t* f )
{
    f->lay_ncols       = 1;
    f->lay_nrows       = 0;               /* flow mode */
    f->lay_row_h       = 0.0f;
    f->lay_gap_x       = WIDGET_GAP;
    f->lay_gap_y       = WIDGET_GAP;
    f->lay_field_side    = 0;             /* trailing label until field_split / field_label_* */
    f->lay_field_label   = 0.0f;
    f->lay_field_control = 0.0f;
    f->lay_align         = 0;             /* LEFT | TOP until align() / layout.align sets it */
    f->cellx[ 0 ]   = f->content_x;       /* one flex column == the whole content width */
    f->cellw[ 0 ]   = f->content_w;
    f->col          = 0;
    f->row          = 0;
}

/* Replace the active flow template on the current frame.  Finishes any open row first, then
   resolves the columns into cell geometry once (they are constant for every row of the template).
   The next widget starts a fresh row of the new shape; it repeats until set again. */
static void
layout_set( const f32* cols, f32 row_h, f32 gap_x, f32 gap_y )
{
    layout_frame_t* f = lf();
    layout_row_break( f );

    f->lay_row_h    = row_h;
    f->lay_gap_x    = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y    = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;
    f->lay_nrows    = 0;            /* flow mode */

    f32 tracks[ IMGUI_LAYOUT_COLS ];
    f->lay_ncols = layout_copy_tracks( cols, tracks );
    layout_resolve_tracks( tracks, f->lay_ncols, f->content_x, f->content_w, f->lay_gap_x,
                           f->cellx, f->cellw );
    f->col = 0;
    f->row = 0;
}

/* Install a grid template on the current frame.  cols x rows partition a bounded box -- from the
   current pen down to the region's content bottom -- into a fixed matrix, both axes resolved up
   front (the defining difference from flow, where the row height resolves lazily per row).
   Widgets then fill cells row-major; nothing scrolls.  Empty / NULL on either axis => one flex
   track.  Persists until another template is set, exactly like the flow row. */
static void
layout_set_grid( const f32* cols, const f32* rows, f32 gap_x, f32 gap_y )
{
    layout_frame_t* f = lf();
    layout_row_break( f );          /* finish any flow row above the grid band */

    f->lay_gap_x    = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y    = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;

    /* Resolve columns across the content column and rows across the band from the pen to the
       content bottom.  An empty band (content already overflowed) clamps to zero. */
    f32 tracks[ IMGUI_LAYOUT_COLS ];
    f->lay_ncols = layout_copy_tracks( cols, tracks );
    layout_resolve_tracks( tracks, f->lay_ncols, f->content_x, f->content_w, f->lay_gap_x,
                           f->cellx, f->cellw );

    f->lay_nrows = layout_copy_tracks( rows, tracks );
    f32 grid_top = f->cursor_y;
    f32 grid_h   = f->content_y_max - grid_top;
    if ( grid_h < 0.0f ) grid_h = 0.0f;
    layout_resolve_tracks( tracks, f->lay_nrows, grid_top, grid_h, f->lay_gap_y,
                           f->rowy, f->rowh );

    f->col = 0;
    f->row = 0;
}

/* Baseline y to vertically center one line of glyphs in a row of height h starting at y.
   Used by every labeled widget and the window title so the centering math lives in one place.
   (The text_center_y( y, h ) form is the VCENTER case of rect_align below, kept as a scalar
   convenience because most labeled widgets only need the y and already own their x.) */
static f32 text_center_y( f32 y, f32 h ) { return y + ( h - font_char_h() ) * 0.5f; }

/* Place a natural nat_w x nat_h box inside `cell` per the alignment flags (imgui_align_t).  The
   single seam for positioning sub-cell content -- a button's label, a checkbox box, an aligned
   text run, a separator line -- so every widget edges / centers content the same way and a
   region's align setting flows through one place.  Returns the placed rect (w/h are nat_*). */
static imgui_rect_t
rect_align( imgui_rect_t cell, f32 nat_w, f32 nat_h, u32 align )
{
    f32 x = cell.x;                                                            /* LEFT (default) */
    if      ( align & IMGUI_ALIGN_HCENTER ) x = cell.x + ( cell.w - nat_w ) * 0.5f;
    else if ( align & IMGUI_ALIGN_RIGHT   ) x = cell.x +   cell.w - nat_w;

    f32 y = cell.y;                                                            /* TOP (default)  */
    if      ( align & IMGUI_ALIGN_VCENTER ) y = cell.y + ( cell.h - nat_h ) * 0.5f;
    else if ( align & IMGUI_ALIGN_BOTTOM  ) y = cell.y +   cell.h - nat_h;

    return ( imgui_rect_t ){ x, y, nat_w, nat_h };
}

/* Collapse toggle glyph: a small triangle centered in a square box.  Points down when expanded,
   right when collapsed (the following label reads as the thing being toggled).  Shared by the
   window title bar and collapsing_header, so the arrow looks identical wherever a region folds. */
static void
draw_collapse_arrow( imgui_rect_t box, bool collapsed, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( box.h * 0.22f );   /* triangle half-extent */

    if ( collapsed )
        /* pointing right:  |>  */
        draw_push_triangle( cx - s, cy - s, cx - s, cy + s, cx + s, cy, 0, color );
    else
        /* pointing down:   \/  */
        draw_push_triangle( cx - s, cy - s, cx + s, cy - s, cx, cy + s, 0, color );
}

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
static f32  label_width( const char* s )                         { return font_text_w_n( s, label_vis_len( s ) ); }
static void draw_label ( f32 x, f32 y, u32 c, const char* s )    { draw_push_text_n( x, y, c, s, label_vis_len( s ) ); }

/* Cell a grid hands to a widget: a fixed (col,row) slot of the pre-resolved matrix, then advance
   row-major.  Past the last cell the cursor clamps to it, so overflow widgets stack harmlessly in
   the final slot rather than reading out of bounds. */
static imgui_rect_t
grid_next_rect( layout_frame_t* f )
{
    if ( f->row >= f->lay_nrows ) f->row = f->lay_nrows - 1;   /* clamp overflow to the last row */

    u32          c = f->col, rr = f->row;
    imgui_rect_t r = { f->cellx[ c ], f->rowy[ rr ], f->cellw[ c ], f->rowh[ rr ] };

    if ( ++f->col >= f->lay_ncols ) { f->col = 0; ++f->row; }   /* next slot, row-major */
    return r;
}

/* Hand the next cell to a widget.  `h` is the widget's natural height; in an auto-height flow row
   (row_h == 0) the *first* widget's h sets the height for the whole row, and the rest of the
   columns conform.  A fixed row_h overrides it.  The row resolves once at column 0, then each
   call returns one cell and advances, wrapping to a fresh row when the columns run out.  In grid
   mode the matrix is already resolved, so it just walks (see above).  The widget just fills the
   rect; it never sees columns or gaps. */

/* Width-aware form.  `natural_w` is the widget's preferred width, used only when a same_line is
   pending (the widget then sits at the running x sized to natural_w, or fills to the content edge
   when natural_w <= 0); in normal column flow / grid it is ignored and the track cell width wins.
   Every emit records f->prev_item so same_line() can anchor the next widget to this one's line. */
static imgui_rect_t
widget_next_rect_w( f32 natural_w, f32 h )
{
    layout_frame_t* f = lf();
    if ( f->lay_ncols == 0 ) layout_set_default( f );   /* repair a stray-emit (empty) frame */

    /* same_line: place on the previous item's line at the running x, sized to natural_w (or the
       remaining content width).  Bypasses the column walk; the column cursor restarts below the
       line, and cursor_y is pushed past the tallest item so following rows clear it. */
    if ( f->cont_line )
    {
        f->cont_line = false;
        f32 right    = f->content_x + f->content_w;
        f32 x        = f->cont_x;
        f32 w        = ( natural_w > 0.0f ) ? natural_w : ( right - x );
        if ( w < 0.0f ) w = 0.0f;
        imgui_rect_t r = { x, f->prev_item.y, w, h };

        f32 line_h   = ( h > f->prev_item.h ) ? h : f->prev_item.h;
        f32 bottom   = f->prev_item.y + line_h + f->lay_gap_y;
        if ( bottom > f->cursor_y ) f->cursor_y = bottom;
        f->col       = 0;                                   /* next normal widget starts a row */

        widget_track_width( x + w );
        f->prev_item = r;
        return r;
    }

    imgui_rect_t r;
    if ( f->lay_nrows > 0 )
    {
        r = grid_next_rect( f );                            /* grid: fixed matrix, both axes set */
    }
    else
    {
        /* Cells (cellx/cellw) were resolved at install and are constant for every row; only the row
           top and height are per-row, set here on the first cell.  Auto height takes the first
           widget's h; a fixed row_h overrides it. */
        if ( f->col == 0 )
        {
            f->row_y     = f->cursor_y;
            f->row_h_cur = ( f->lay_row_h > 0.0f ) ? f->lay_row_h : h;
        }

        u32 c = f->col;
        r = ( imgui_rect_t ){ f->cellx[ c ], f->row_y, f->cellw[ c ], f->row_h_cur };
        widget_track_width( f->cellx[ c ] + f->cellw[ c ] );   /* cell right edge -> hscroll */

        /* Advance; wrap to a fresh row when the template's columns are exhausted. */
        if ( ++f->col >= f->lay_ncols )
        {
            f->cursor_y = f->row_y + f->row_h_cur + f->lay_gap_y;
            f->col      = 0;
        }
    }

    f->prev_item = r;
    return r;
}

/* The common case: fill the track cell (natural_w < 0 => no same_line preference). */
static imgui_rect_t widget_next_rect( f32 h ) { return widget_next_rect_w( -1.0f, h ); }

/* Split a labeled widget row into a left-hand control rect and a right-hand label.  The label
   keeps its natural width pinned at the row's right-of-control edge; the control takes the rest
   of the row, never shrinking below min_control_w so it stays usable when the label is long
   (the control then overruns under the label, matching the prior per-widget behavior).  Draws
   the label here, vertically centered in the given color, and returns the control rect for the
   caller to interact with and fill.  The single seam every "control + trailing label" widget
   (slider_float, input_text, future combo / drag / color widgets) routes through, so row
   proportions can be retuned in one place. */

/* Resolve a labeled widget's cell into a label position + a control rect when a field split is
   active (field_split / field_label_*).  The label and control are two tracks laid across the cell
   by the same resolver columns use, so a field split obeys the overloaded unit rule and adapts to
   whatever width the widget is handed -- a full row or a single column cell.  `side` flips which
   track sits left.  Draws nothing; the caller places its label + control from the outputs.  The
   control is floored at min_control_w so it stays usable (overrunning under the label, as before).
   Returns false when no field split is set, leaving the caller on its default layout. */
static bool
field_split_resolve( imgui_rect_t cell, f32 min_control_w, f32* out_label_x, imgui_rect_t* out_control )
{
    layout_frame_t* f = lf();
    if ( f->lay_field_side == 0 ) return false;

    /* Order the two tracks by side so the resolver lays them left-to-right correctly. */
    f32 tracks[ 2 ];
    u32 lab_i, ctl_i;
    if ( f->lay_field_side == IMGUI_LABEL_LEFT )
    {
        tracks[ 0 ] = f->lay_field_label;   tracks[ 1 ] = f->lay_field_control;   lab_i = 0; ctl_i = 1;
    }
    else /* IMGUI_LABEL_RIGHT */
    {
        tracks[ 0 ] = f->lay_field_control; tracks[ 1 ] = f->lay_field_label;     ctl_i = 0; lab_i = 1;
    }

    f32 pos[ 2 ], size[ 2 ];
    layout_resolve_tracks( tracks, 2, cell.x, cell.w, WIDGET_PAD, pos, size );

    f32 control_w = size[ ctl_i ];
    if ( control_w < min_control_w ) control_w = min_control_w;

    *out_label_x = pos[ lab_i ];
    *out_control = ( imgui_rect_t ){ pos[ ctl_i ], cell.y, control_w, cell.h };
    return true;
}

static imgui_rect_t
widget_split_label( imgui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    /* Field split mode: the label sits in its track at full strength (the trailing-label dim hint,
       label_color, does not apply -- a field label reads as primary); the control fills the rest. */
    f32          label_x;
    imgui_rect_t control;
    if ( field_split_resolve( row, min_control_w, &label_x, &control ) )
    {
        draw_label( label_x, text_center_y( row.y, row.h ), COL_TEXT, label );
        return control;
    }

    /* Default: control on the left, the label trailing at its natural width on the right. */
    f32 label_w   = label_width( label );
    f32 control_w = row.w - label_w - WIDGET_PAD;
    if ( control_w < min_control_w ) control_w = min_control_w;

    control = ( imgui_rect_t ){ row.x, row.y, control_w, row.h };

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
