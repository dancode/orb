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
#define WIDGET_MIN_W  ( (f32)s_layout.min_cell_w    )

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
   from `origin` across `extent`, with `gap` between each.  Units (see imgui_layout_t): >1 fixed px,
   ==1 fill (equal share of the leftover -- several fills split it), (0,1) fraction of the gap-
   adjusted extent, ==0 natural.  This is a pre-divide (up-front) resolve with no content in hand,
   so a 0/natural track collapses to zero width here -- natural only has a measure in pack mode.
   Gaps are removed before the split, so a fraction is of usable space and cells tile exactly.

   Minimum width: a fill / fraction track that the available space squeezes below WIDGET_MIN_W is
   floored there and the row simply overflows -- it stops shrinking at a still-usable size and the
   surplus cells push out past the content edge, where the region clip cuts them (no auto-scroll;
   the scroll flags stay the author's choice).  Fixed-px tracks are never floored: an explicit
   pixel width is taken as intent, even when small.  All fills share one floor and hit it together,
   so a flat post-clamp matches a freeze-and-redistribute here; per-track minimums would need the
   iterative form. */
static void
layout_resolve_tracks( const f32* tracks, u32 n, f32 origin, f32 extent, f32 gap,
                       f32* out_pos, f32* out_size )
{
    const f32 min_w = WIDGET_MIN_W;

    f32 avail = extent - gap * (f32)( n - 1 );
    if ( avail < 0.0f ) avail = 0.0f;

    /* Pass 1: fixed px + fractions consume space; fill (==1) tracks share what's left; natural
       (==0) contributes nothing in a pre-divide. */
    f32 used = 0.0f;
    u32 fill = 0;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 t = tracks[ i ];
        if      ( t == 1.0f ) ++fill;            /* fill -- equal share of the leftover */
        else if ( t >  1.0f ) used += t;         /* fixed px                            */
        else if ( t >  0.0f ) used += t * avail; /* fraction (0,1)                      */
        /* t == 0.0f : natural -- no content to measure here, contributes 0             */
    }
    f32 leftover  = avail - used;
    if ( leftover < 0.0f ) leftover = 0.0f;
    f32 fill_each = fill ? leftover / (f32)fill : 0.0f;

    /* Pass 2: place left-to-right, gap between cells (not before the first / after the last). */
    f32 pos = origin;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 t  = tracks[ i ];
        f32 sz = ( t == 1.0f ) ? fill_each
               : ( t >  1.0f ) ? t
               : ( t >  0.0f ) ? t * avail
               :                 0.0f;           /* natural -> zero-width track in pre-divide */

        /* Floor a shrinking flex / fraction track at the usable minimum; let the row overflow
           rather than crush the cell.  Fixed-px ( t > 1 ) and natural ( t == 0 ) are left as-is. */
        if ( ( t == 1.0f || ( t > 0.0f && t < 1.0f ) ) && sz < min_w )
            sz = min_w;

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
    if ( n == 0 ) { out[ 0 ] = 1.0f; n = 1; }   /* default to a single fill track (full extent) */
    return n;
}

/* Open a region UNDECLARED: zero the template state and leave the mode NONE so the first layout
   header (stack / columns / grid / ...) installs real geometry.  A widget emitted before any
   header trips the guard in widget_next_rect_w.  Called when a region or sub-layout opens (the
   old silent single-column default is gone) and by imgui_pad after re-insetting -- the modifiers
   (gaps reset to the theme, field split + align cleared) start from a known state every region. */
static void
layout_clear( layout_frame_t* f )
{
    f->mode              = IMGUI_MODE_NONE;
    f->lay_ncols         = 0;             /* no template -- first header resolves one */
    f->lay_nrows         = 0;
    f->lay_row_h         = 0.0f;
    f->lay_gap_x         = WIDGET_GAP;
    f->lay_gap_y         = WIDGET_GAP;
    f->lay_field_side    = 0;             /* trailing label until field_split / field_label_* */
    f->lay_field_label   = 0.0f;
    f->lay_field_control = 0.0f;
    f->lay_align         = 0;             /* LEFT | TOP until align() / layout.align sets it */
    f->col               = 0;
    f->row               = 0;
    f->prev_item         = ( imgui_rect_t ){ 0 };   /* no same_line anchor in a fresh region */
    f->cont_line         = false;
    f->pack_dir          = 0;             /* pack pen is seeded by pack(); only the pending size */
    f->pack_size_next    = -1.0f;         /* matters before then -- unset means natural          */
}

/* Install the region's default template: one flex column, auto height -- the classic stack, mode
   STACK.  Resolves immediately (content_x/content_w must already be set), so the single column
   fills the content width with no gap.  This is the full reset (clears field split + align too):
   it backs imgui_layout_default and the emit-before-header guard's release fallback.  The plain
   stack() header keeps modifiers and routes through layout_set instead. */
static void
layout_set_default( layout_frame_t* f )
{
    f->mode            = IMGUI_MODE_STACK;
    f->lay_ncols       = 1;
    f->lay_nrows       = 0;               /* flow mode */
    f->lay_row_h       = 0.0f;
    f->lay_gap_x       = WIDGET_GAP;
    f->lay_gap_y       = WIDGET_GAP;
    f->lay_field_side    = 0;             /* trailing label until field_split / field_label_* */
    f->lay_field_label   = 0.0f;
    f->lay_field_control = 0.0f;
    f->lay_align         = 0;             /* LEFT | TOP until align() / layout.align sets it */
    f->lay_cols[ 0 ] = 1.0f;              /* single flex track, kept so indent can re-resolve */
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

    f->mode         = IMGUI_MODE_COLUMNS;   /* a flow template; stack()/row() override to STACK */
    f->lay_row_h    = row_h;
    f->lay_gap_x    = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y    = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;
    f->lay_nrows    = 0;            /* flow mode */

    f32 tracks[ IMGUI_LAYOUT_COLS ];
    f->lay_ncols = layout_copy_tracks( cols, tracks );
    for ( u32 i = 0; i < f->lay_ncols; ++i ) f->lay_cols[ i ] = tracks[ i ];   /* kept for indent reflow */
    layout_resolve_tracks( tracks, f->lay_ncols, f->content_x, f->content_w, f->lay_gap_x,
                           f->cellx, f->cellw );
    f->col = 0;
    f->row = 0;
}

/* Re-resolve a flow template's cells from the current content column -- used after indent /
   unindent shifts content_x / content_w so subsequent rows land at the new inset.  Flow only
   (STACK / COLUMNS); a grid carries a pre-resolved matrix and a pack its own pen, neither of which
   is re-indented mid-iteration, so they are left untouched. */
static void
layout_reflow( layout_frame_t* f )
{
    if ( f->mode == IMGUI_MODE_STACK || f->mode == IMGUI_MODE_COLUMNS )
        layout_resolve_tracks( f->lay_cols, f->lay_ncols, f->content_x, f->content_w,
                               f->lay_gap_x, f->cellx, f->cellw );
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

    f->mode         = IMGUI_MODE_GRID;
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

/* Draw at most `len` bytes of s left-anchored at x, fitted into max_w: when the run is wider than
   max_w, truncate on a glyph boundary and mark the cut with an ellipsis ("...") so a compressed
   widget reads as deliberately clipped rather than bleeding mid-glyph under the region clip.  When
   not even the ellipsis fits, the leading glyphs that do are drawn and the rest dropped -- never
   worse than a hard clip.  max_w <= 0 draws nothing.  Cheap: one width walk, no extra clip / draw
   command (so draw batching is untouched).  draw_label_fit is the label-grammar wrapper; callers
   with a raw string (the window title) pass the whole length through here directly. */
static void
draw_text_fit_n( f32 x, f32 y, u32 c, const char* s, u32 len, f32 max_w )
{
    if ( max_w <= 0.0f ) return;

    /* Fits whole -- the common path: draw the span as-is. */
    if ( font_text_w_n( s, len ) <= max_w )
    {
        draw_push_text_n( x, y, c, s, len );
        return;
    }

    /* Too wide: reserve the ellipsis (dropped if even it will not fit), then take the longest
       glyph prefix that fits in the remaining budget. */
    f32  ell    = font_char_advance( (u8)'.' ) * 3.0f;
    bool dots   = ( ell <= max_w );
    f32  budget = dots ? max_w - ell : max_w;

    f32 w = 0.0f;
    u32 n = 0;
    while ( n < len && s[ n ] )
    {
        f32 adv = font_char_advance( (u8)s[ n ] );
        if ( w + adv > budget ) break;
        w += adv;
        ++n;
    }

    draw_push_text_n( x, y, c, s, n );
    if ( dots )
        draw_push_text_n( x + w, y, c, "...", 3u );
}

/* Clean-shrink companion to draw_label: fit a label's visible span (markers stripped) into max_w,
   ellipsizing when a cell squeezes it below its natural width.  Used by the labeled widgets. */
static void
draw_label_fit( f32 x, f32 y, u32 c, const char* s, f32 max_w )
{
    draw_text_fit_n( x, y, c, s, label_vis_len( s ), max_w );
}

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

/* Place one item in pack mode (bar / strip): the print run.  The widget's natural size feeds the
   main axis (width for a horizontal bar, height for a vertical strip); the cross axis takes its
   natural extent on that axis, or fills the column when it has none.  A pending pack_size overrides
   the main extent, resolved against the space left on the current line (0 = natural, 1 = fill the
   rest, (0,1) a fraction of the remainder, >1 px); it is consumed (back to natural) after one item.
   pack_main advances along the axis; cursor_y is kept at the content bottom so the region measures
   its height correctly without a trailing pack_nextline. */
static imgui_rect_t
pack_next_rect( layout_frame_t* f, f32 natural_w, f32 h )
{
    bool horiz = ( f->pack_dir == IMGUI_PACK_HORIZONTAL );

    /* Natural extents per axis from the widget's preferred size.  A fill widget (no natural width,
       natural_w <= 0) has no main extent of its own: it defaults to filling the rest of the line. */
    f32 nat_main  = horiz ? ( natural_w > 0.0f ? natural_w : 0.0f ) : h;
    f32 cross_ext = horiz ? h : ( natural_w > 0.0f ? natural_w : f->content_w );

    f32 main_avail = ( horiz ? ( f->content_x + f->content_w ) : f->content_y_max ) - f->pack_main;
    if ( main_avail < 0.0f ) main_avail = 0.0f;

    f32 u = f->pack_size_next;
    f32 main_ext;
    if      ( u <  0.0f ) main_ext = ( nat_main > 0.0f ) ? nat_main : main_avail; /* unset: natural, or fill if none */
    else if ( u == 0.0f ) main_ext = nat_main;                                    /* explicit natural               */
    else if ( u == 1.0f ) main_ext = main_avail;                                  /* fill the rest of the line      */
    else if ( u <  1.0f ) main_ext = u * main_avail;                              /* fraction of the remainder      */
    else                  main_ext = u;                                           /* fixed px                       */
    f->pack_size_next = -1.0f;                                                    /* consume -> next item is natural */

    imgui_rect_t r;
    if ( horiz )
    {
        r            = ( imgui_rect_t ){ f->pack_main, f->pack_cross, main_ext, cross_ext };
        f->pack_main += main_ext + f->lay_gap_x;
        if ( cross_ext > f->pack_line ) f->pack_line = cross_ext;
        f->cursor_y   = f->pack_cross + f->pack_line;   /* content bottom = current line's bottom */
    }
    else
    {
        r            = ( imgui_rect_t ){ f->pack_cross, f->pack_main, cross_ext, main_ext };
        f->pack_main += main_ext + f->lay_gap_y;
        if ( cross_ext > f->pack_line ) f->pack_line = cross_ext;
        f->cursor_y   = f->pack_main;                   /* content bottom = the running y pen */
    }

    widget_track_width( r.x + r.w );
    f->prev_item = r;
    return r;
}

/* Width-aware form.  `natural_w` is the widget's preferred width, used only when a same_line is
   pending (the widget then sits at the running x sized to natural_w, or fills to the content edge
   when natural_w <= 0); in normal column flow / grid it is ignored and the track cell width wins.
   Every emit records f->prev_item so same_line() can anchor the next widget to this one's line. */
static imgui_rect_t
widget_next_rect_w( f32 natural_w, f32 h )
{
    layout_frame_t* f = lf();

    /* An item is being emitted: resolve its flags (stack + the one-shot next-item override) and
       latch them for widget_behavior / the widget to read.  This is the single per-item seam every
       widget passes through, so the push-model needs no plumbing at the individual call sites. */
    item_flags_resolve();

    /* Emit-before-header guard: a region opens UNDECLARED (mode NONE), and the first layout header
       names the mode.  A widget emitted before any header is a usage error -- assert in debug so it
       is caught at the call site, and fall back to a stack in release so a shipped build degrades
       rather than faults (mirrors how the layout / id stacks clamp instead of crashing). */
    if ( f->mode == IMGUI_MODE_NONE )
    {
        ORB_ASSERT( f->mode != IMGUI_MODE_NONE );   /* declare a mode (stack/columns/grid/...) first */
        layout_set_default( f );                    /* release fallback: behave as a plain stack */
    }

    /* Pack mode (bar / strip): the print run places items along its axis, ignoring same_line and the
       column walk -- pack_nextline is its line break. */
    if ( f->mode == IMGUI_MODE_PACK )
        return pack_next_rect( f, natural_w, h );

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
field_split_resolve( imgui_rect_t cell, f32 min_control_w, f32* out_label_x, f32* out_label_w,
                     imgui_rect_t* out_control )
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
    *out_label_w = size[ lab_i ];
    *out_control = ( imgui_rect_t ){ pos[ ctl_i ], cell.y, control_w, cell.h };
    return true;
}

static imgui_rect_t
widget_split_label( imgui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    /* Field split mode: the label sits in its track at full strength (the trailing-label dim hint,
       label_color, does not apply -- a field label reads as primary); the control fills the rest.
       The label is fitted to its track width so a narrow (fraction-shrunk) track ellipsizes it. */
    f32          label_x, label_w;
    imgui_rect_t control;
    if ( field_split_resolve( row, min_control_w, &label_x, &label_w, &control ) )
    {
        draw_label_fit( label_x, text_center_y( row.y, row.h ), COL_TEXT, label, label_w );
        return control;
    }

    /* Default: control on the left, the label trailing at its natural width on the right.  When the
       control floors at min_control_w the label space narrows; fit it so it ellipsizes there
       instead of bleeding under the row's right edge. */
    label_w       = label_width( label );
    f32 control_w = row.w - label_w - WIDGET_PAD;
    if ( control_w < min_control_w ) control_w = min_control_w;

    control = ( imgui_rect_t ){ row.x, row.y, control_w, row.h };

    f32 trail_x = control.x + control.w + WIDGET_PAD;
    draw_label_fit( trail_x, text_center_y( row.y, row.h ), label_color, label,
                    ( row.x + row.w ) - trail_x );
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

    /* Disabled item: inert this frame -- no hover, active, focus, or click.  Returning the zeroed
       state here is the one place that suppresses interaction for every widget, the behavioral half
       of IMGUI_ITEM_DISABLED (the visual dim is the draw list's global alpha, set at resolve).  The
       flags were latched by widget_next_rect_w just before this call. */
    if ( s_ctx.cur_item_flags & IMGUI_ITEM_DISABLED )
        return st;

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
    bool eligible  = can_hover && win_hover && !s_ctx.win_resize_hot && !s_ctx.win_grip_hot;
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

/*----------------------------------------------------------------------------------------------
    Text edit state -- persisted per-id across frames by input_field_edit.

    cursor and anchor together describe the selection: the highlighted range is
    [min(cursor,anchor), max(cursor,anchor)), and cursor == anchor means no selection
    (bare insertion point).  scroll_x is the horizontal pixel bias that keeps the caret
    inside the visible portion of a narrow box.  blink_t accumulates s_io.dt and drives
    the 50/50 on-off cursor blink; it is reset to 0 on any keypress or click so the caret
    is always immediately visible after user activity.

    - Left/Right arrows with cursor-collapse on Shift-less movement over a selection
    - Home/End
    - Shift+any of the above to extend selection
    - Ctrl+A to select all
    - Ctrl+C / Ctrl+X copy / cut to the OS clipboard; paste applied from s_io.paste (the
      platform posts APP_EV_CLIPBOARD on the paste gesture; no Ctrl+V check needed here)
    - Backspace and Delete both at caret and over selection
    - Character insertion at caret, or selection replacement on first char
    - Blinking caret (reset to visible on any keypress or click)
    - Click-to-caret positioning (also fires on the focus-gaining click)
    - Click-drag to select, double-click to select the word under the cursor
    - Horizontal scroll that keeps the caret visible whenever it moves
    - Draw clip so scrolled text never bleeds past the box border

----------------------------------------------------------------------------------------------*/

typedef struct
{
    u32  cursor;    /* byte offset of the caret (insertion / deletion point) */
    u32  anchor;    /* passive end of the selection; cursor == anchor -> none */
    f32  scroll_x;  /* horizontal scroll offset in pixels                     */
    f32  blink_t;   /* seconds since last caret-visibility reset              */

} imgui_edit_state_t;

/* Both return flags from input_field_edit.  changed fires on any buffer modification;
   enter fires when the user submits the field with Enter (and focus is dropped).  They
   are independent: a paste that happens to contain a newline could set both in one frame,
   though the current implementation does not process newlines as submit signals. */
typedef struct { bool changed; bool enter; } input_field_result_t;

/* Pixel x-offset of the insertion point at byte index `off` in `buf`, measured from the
   left edge of the first glyph (scroll is not applied here; the caller adjusts).  Stops
   safely at a NUL so off > len is handled without bounds checks. */
static f32
text_x_at( const char* buf, u32 off )
{
    f32 x = 0.0f;
    for ( u32 i = 0; i < off && buf[ i ]; ++i )
        x += font_char_advance( (u8)buf[ i ] );
    return x;
}

/* Byte offset in buf[0..len) nearest to pixel position `px` measured from the text origin.
   Snaps to the midpoint of each glyph so a click in the left half of a glyph lands before
   it and in the right half lands after it, matching standard click-to-caret behaviour. */
static u32
text_offset_at( const char* buf, u32 len, f32 px )
{
    f32 x = 0.0f;
    for ( u32 i = 0; i < len; ++i )
    {
        f32 adv = font_char_advance( (u8)buf[ i ] );
        if ( px < x + adv * 0.5f ) return i;
        x += adv;
    }
    return len;
}

/* Character class for double-click word selection: a click extends over the maximal run of
   one class.  0 = whitespace, 1 = word (alphanumeric or underscore), 2 = punctuation/other --
   the classic "select word, or select a run of symbols" split. */
static int
char_class( u8 c )
{
    if ( c == ' ' || c == '\t' )                                  return 0;
    if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
         ( c >= '0' && c <= '9' ) || c == '_' )                   return 1;
    return 2;
}

/* Word bounds [*lo,*hi) around byte `off`: the run of same-class characters containing it.
   Used by double-click to snap the selection to a whole word.  A click past the end (off ==
   len) collapses to an empty range at len so a double-click in empty space selects nothing. */
static void
word_bounds( const char* buf, u32 len, u32 off, u32* lo, u32* hi )
{
    if ( off >= len ) { *lo = *hi = len; return; }
    int cls = char_class( (u8)buf[ off ] );
    u32 a = off, b = off;
    while ( a > 0   && char_class( (u8)buf[ a - 1u ] ) == cls ) --a;
    while ( b < len && char_class( (u8)buf[ b ]      ) == cls ) ++b;
    *lo = a; *hi = b;
}

/*----------------------------------------------------------------------------------------------
    input_field_edit -- generic single-line text editing inside a caller-supplied rect.

    Handles: cursor movement (Left / Right / Home / End), selection (Shift variants of the
    above, Ctrl+A), deletion (Backspace / Delete at the caret or over a selection), character
    insertion or selection replacement, horizontal scroll to keep the caret in view, and
    rendering (text content, selection highlight, blinking caret) -- all clipped to the box.

    The caller is responsible for:
        - carving `box` from the layout (widget_next_rect has already been called),
        - drawing the box background and border (so the visual treatment is widget-specific),
        - obtaining `st` from widget_behavior with WIDGET_KIND_FOCUSABLE.
    On Enter or Escape the function drops focus by clearing s_ctx.focused_id.

    Mouse capture: widget_behavior already claims active_id on the press (for every widget
    kind), and that single mechanism is the engine's general-purpose mouse grab -- while a
    widget owns active_id every other widget is frozen (can_hover is false for them) and no
    hover fires elsewhere, so a drag stays bound to this field until the button is released.
    This function leans on exactly that: it tracks the selection drag from st.active and never
    re-tests whether the cursor is still inside the box, so the drag survives the cursor
    leaving the field -- identical in spirit to how the scrollbar knob drags.

    id   -- widget id; keys the persisted imgui_edit_state_t (cursor, anchor, scroll, blink).
    box  -- pixel rect the text renders into; text is inset by WIDGET_PAD on left / right.
    st   -- interaction state from widget_behavior: focused gates keyboard input, pressed marks
            the grab frame, active is held true for the life of the mouse-capture drag.
    buf  -- caller-owned NUL-terminated buffer, modified in-place by keyboard input.
    bufsz-- total byte capacity of buf, including the NUL terminator.

    Returns { .changed = true } on any buffer modification this frame, { .enter = true } when
    Enter is pressed.
----------------------------------------------------------------------------------------------*/

static input_field_result_t
input_field_edit( imgui_id_t id, imgui_rect_t box, widget_state_t st, char* buf, u32 bufsz )
{
    imgui_edit_state_t*  es      = IMGUI_STATE( imgui_edit_state_t, id );
    input_field_result_t res     = { false, false };
    bool                 focused = st.focused;

    u32 len = 0;
    while ( len < bufsz - 1u && buf[ len ] ) ++len;

    /* Clamp cursor and anchor to the current length -- a programmatic buffer change between
       frames may have shortened the string under the old positions. */
    if ( es->cursor > len ) es->cursor = len;
    if ( es->anchor > len ) es->anchor = len;

    u32  sel_lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
    u32  sel_hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
    bool has_sel = ( sel_lo != sel_hi );

    if ( focused )
    {
        bool shift = s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ];
        bool ctrl  = s_io.keys_down[ APP_KEY_LCTRL  ] || s_io.keys_down[ APP_KEY_RCTRL  ];
        bool blink_reset = false;

        /* Clipboard.  Copy / cut are key-driven (only this field knows the selection) and push
           to the OS clipboard via imgui_clipboard_set.  Paste is event-driven: the platform
           already read the OS clipboard on the paste gesture and delivered it in s_io.paste, so
           there is no Ctrl+V key check here -- a non-empty s_io.paste IS the paste.  Resolved
           first so it acts on the selection as the user sees it, before navigation moves it. */

        if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_C ] )
        {
            imgui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
            blink_reset = true;
        }

        if ( ctrl && has_sel && s_io.keys_pressed[ APP_KEY_X ] )
        {
            imgui_clipboard_set( buf + sel_lo, sel_hi - sel_lo );
            memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
            len -= ( sel_hi - sel_lo );
            es->cursor = es->anchor = sel_lo;
            has_sel = false; sel_lo = sel_hi = es->cursor;
            res.changed = true;
            blink_reset = true;
        }

        if ( s_io.paste[ 0 ] )
        {
            /* Drop the selection first so the paste lands where it was. */
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                has_sel = false; sel_lo = sel_hi = es->cursor;
            }
            /* Insert each pasted byte at the advancing caret, skipping control characters
               (a single-line field rejects newlines / tabs) and stopping at capacity. */
            for ( const char* c = s_io.paste; *c && len + 1u < bufsz; ++c )
            {
                if ( (u8)*c < 0x20u || (u8)*c == 0x7Fu ) continue;
                memmove( buf + es->cursor + 1u, buf + es->cursor, len - es->cursor + 1u );
                buf[ es->cursor ] = *c;
                ++len; ++es->cursor;
            }
            es->anchor  = es->cursor;
            res.changed = true;
            blink_reset = true;
        }

        /* Navigation: Left / Right collapse or extend the selection; Home / End jump. */

        if ( s_io.keys_pressed[ APP_KEY_LEFT ] )
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_lo; }
            else if ( es->cursor > 0 ) { --es->cursor; if ( !shift ) es->anchor = es->cursor; }
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_RIGHT ] )
        {
            if ( !shift && has_sel ) { es->cursor = es->anchor = sel_hi; }
            else if ( es->cursor < len ) { ++es->cursor; if ( !shift ) es->anchor = es->cursor; }
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_HOME ] )
        {
            es->cursor = 0; if ( !shift ) es->anchor = 0;
            blink_reset = true;
        }

        if ( s_io.keys_pressed[ APP_KEY_END ] )
        {
            es->cursor = len; if ( !shift ) es->anchor = len;
            blink_reset = true;
        }

        /* Ctrl+A: select the entire buffer. */
        if ( ctrl && s_io.keys_pressed[ APP_KEY_A ] )
        {
            es->anchor = 0; es->cursor = len;
            blink_reset = true;
        }

        /* Backspace: delete the selection, or the character before the caret. */
        if ( s_io.keys_pressed[ APP_KEY_BACKSPACE ] )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                res.changed = true;
            }
            else if ( es->cursor > 0 )
            {
                --es->cursor;
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                es->anchor = es->cursor;
                res.changed = true;
            }
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink_reset = true;
        }

        /* Delete: delete the selection, or the character after the caret. */
        if ( s_io.keys_pressed[ APP_KEY_DELETE ] )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo, buf + sel_hi, len - sel_hi + 1u );
                len -= ( sel_hi - sel_lo );
                es->cursor = es->anchor = sel_lo;
                res.changed = true;
            }
            else if ( es->cursor < len )
            {
                memmove( buf + es->cursor, buf + es->cursor + 1u, len - es->cursor );
                --len; buf[ len ] = '\0';
                res.changed = true;
            }
            has_sel = false; sel_lo = sel_hi = es->cursor;
            blink_reset = true;
        }

        /* Character input: replace the selection with the first incoming char, then insert
           any remaining chars at the advancing caret.  Selection is cleared after the first
           replacement so subsequent chars in the same frame insert normally.  Skipped while
           Ctrl is held so shortcut combos (Ctrl+C/V/X/A) never leak a stray glyph -- the OS
           filters their control codes too, but this keeps the contract explicit. */
        for ( const char* ch = ctrl ? "" : s_io.text; *ch; ++ch )
        {
            if ( has_sel )
            {
                memmove( buf + sel_lo + 1u, buf + sel_hi, len - sel_hi + 1u );
                buf[ sel_lo ] = *ch;
                len          -= ( sel_hi - sel_lo ) - 1u;
                es->cursor    = es->anchor = sel_lo + 1u;
                has_sel       = false; sel_lo = sel_hi = es->cursor;
            }
            else if ( len + 1u < bufsz )
            {
                memmove( buf + es->cursor + 1u, buf + es->cursor, len - es->cursor + 1u );
                buf[ es->cursor ] = *ch;
                ++len; ++es->cursor;
                es->anchor = es->cursor;
            }
            res.changed = true;
            blink_reset = true;
        }

        /* Enter submits; Escape cancels; both drop focus. */
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] )
        {
            s_ctx.focused_id = IMGUI_ID_NONE;
            res.enter = true;
        }
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
            s_ctx.focused_id = IMGUI_ID_NONE;

        /* Mouse.  st.pressed is the grab frame (also the focus-gaining click, since
           widget_behavior set focused_id = id by now); st.active stays true for the whole
           capture, so the drag below keeps extending the selection even after the cursor
           leaves the box.  text_offset_at clamps a cursor past either edge to 0 / len, so a
           drag past the ends selects to the start / end naturally. */
        {
            f32 px  = s_io.mouse_x - ( box.x + WIDGET_PAD ) + es->scroll_x;
            u32 off = text_offset_at( buf, len, px );

            if ( st.pressed && s_io.mouse_double[ 0 ] )
            {
                /* Double-click: snap the selection to the word under the cursor. */
                word_bounds( buf, len, off, &es->anchor, &es->cursor );
                blink_reset = true;
            }
            else if ( st.pressed )
            {
                /* Single press: caret to the click; Shift keeps the anchor to extend. */
                es->cursor = off;
                if ( !shift ) es->anchor = off;
                blink_reset = true;
            }
            else if ( st.active )
            {
                /* Drag: move the caret, leaving the anchor put, so the selection grows. */
                es->cursor  = off;
                blink_reset = true;
            }
        }

        /* Recompute selection bounds after all edits this frame. */
        sel_lo  = es->cursor < es->anchor ? es->cursor : es->anchor;
        sel_hi  = es->cursor > es->anchor ? es->cursor : es->anchor;
        has_sel = ( sel_lo != sel_hi );

        if ( blink_reset ) es->blink_t = 0.0f;
        else               es->blink_t += s_io.dt;
    }

    /* Scroll to keep the caret inside the visible width on every frame, not just when
       focused, so a programmatic cursor move from outside is also honoured. */
    {
        f32 cx    = text_x_at( buf, es->cursor );
        f32 vis_w = box.w - 2.0f * WIDGET_PAD;
        if ( vis_w < 0.0f ) vis_w = 0.0f;
        if ( cx - es->scroll_x < 0.0f )  es->scroll_x = cx;
        if ( cx - es->scroll_x > vis_w ) es->scroll_x = cx - vis_w;
        if ( es->scroll_x < 0.0f )       es->scroll_x = 0.0f;
    }

    /* Clip text, selection, and caret to the box interior so scrolled content does not
       bleed past the border.  Balanced with draw_pop_clip_rect below. */
    draw_push_clip_rect( box.x + WIN_BORDER, box.y + WIN_BORDER,
                         box.w - 2.0f * WIN_BORDER, box.h - 2.0f * WIN_BORDER );

    f32 text_x = box.x + WIDGET_PAD - es->scroll_x;
    f32 text_y = text_center_y( box.y, box.h );

    /* Selection highlight behind the text. */
    if ( focused && has_sel )
    {
        f32 sx0 = text_x + text_x_at( buf, sel_lo );
        f32 sx1 = text_x + text_x_at( buf, sel_hi );
        draw_push_rect_filled( sx0, box.y + 1.0f, sx1 - sx0, box.h - 2.0f,
                               0, 0, 1, 1, 0, COL_WIDGET_ACT );
    }

    draw_push_text( text_x, text_y, COL_TEXT, buf );

    /* Blinking caret: visible for the first 0.5 s of each 1 s cycle. */
    if ( focused )
    {
        bool caret_vis = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
        if ( caret_vis )
        {
            f32 cx = text_x + text_x_at( buf, es->cursor );
            draw_push_rect_filled( cx, box.y + (f32)s_layout.cursor_inset,
                                   (f32)s_layout.cursor_w,
                                   box.h - 2.0f * (f32)s_layout.cursor_inset,
                                   0, 0, 1, 1, 0, COL_CURSOR );
        }
    }

    draw_pop_clip_rect();

    return res;
}

// clang-format on
/*============================================================================================*/
