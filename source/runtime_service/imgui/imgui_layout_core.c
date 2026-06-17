/*==============================================================================================

    runtime_service/imgui/imgui_layout_core.c -- Layout mechanism: track resolver + cell emitters.

    The engine the public layout API (imgui_layout.c) drives.  It carves a region's content area
    into cells from a repeating row / column template (or a fixed grid, or a pack run) and hands
    the next cell to each widget, hiding the layout shape from the widgets entirely.  Two halves:

        - track resolver + template installers (layout_set / _grid / _reflow / _clear and the
          overloaded-unit track math) -- the "what shape is this region" mechanism;
        - cell emitters (widget_next_rect_w, grid_next_rect, pack_next_rect, field_split_resolve,
          widget_split_label) -- the per-item "hand out the next rect" mechanism.

    Lifted out of imgui_widget_core.c so the layout engine sits adjacent to the API that drives it,
    the way imgui_window.c (state) precedes imgui_widget_window.c (chrome).

    Included by imgui.c after imgui_widget_core.c (so the size / color macros, rect_align, the
    label grammar, text_center_y, and item_flags_resolve are in scope) and before imgui_layout.c
    (which calls layout_set / widget_next_rect / the region helpers).

==============================================================================================*/
// clang-format off

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

// clang-format on
/*============================================================================================*/
