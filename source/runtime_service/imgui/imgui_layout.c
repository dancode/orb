/*==============================================================================================

    runtime_service/imgui/imgui_layout.c -- Public layout API verbs.

    Defines the per-region template-shaping calls (imgui_layout, imgui_stack, imgui_row,
    imgui_columns, imgui_grid, imgui_pack, imgui_bar, imgui_strip, imgui_field_split,
    imgui_form, imgui_indent/unindent, imgui_content_avail, etc.).

    The scrollable region engine (layout_push/pop_region, region_scrollbar, imgui_region_t,
    scroll_clamp) is in imgui_layout_region.c, included just before this file.

    Child box and sub-layout lifecycle (imgui_begin/child_end, imgui_push/pop_layout,
    imgui_window_set_next_size_constraints) is in imgui_layout_child.c, also included before
    this file.

    Included by imgui.c after imgui_layout_child.c.

==============================================================================================*/
// clang-format off

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
   it to size a child_begin to the leftover space, or to lay widgets out by hand.  Measured from the
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

// clang-format on
/*============================================================================================*/
