/*==============================================================================================

    runtime_service/gui/gui_layout.c -- Public layout API verbs.

    Defines the per-region template-shaping calls (gui_layout, gui_stack, gui_row,
    gui_cols, gui_grid, gui_pack, gui_bar, gui_strip, gui_field_split,
    gui_form, gui_indent/unindent, gui_content_avail, etc.).

    The scrollable region engine (layout_push/pop_region, region_scrollbar, gui_region_t,
    scroll_clamp) is in gui_layout_region.c, included just before this file.

    Child box and sub-layout lifecycle (gui_begin/child_end, gui_push/pop_layout,
    gui_window_set_next_size_constraints) is in gui_layout_child.c, also included before
    this file.

    Included by gui.c after gui_layout_child.c.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Public layout API -- shape the active region's repeating row template.

    These set the template on the current region; it persists and repeats for every subsequent
    widget until set again (or the region ends).  No push/pop: a region opens with the default
    (one flex column, auto height), and each call simply replaces it.  See gui_layout_t for
    the column unit rule.  gui_pad sets the region padding -- the inset between the region box
    and where the layout starts -- distinct from the item padding carried in the template.
----------------------------------------------------------------------------------------------*/

/* Full flow template: columns, row height, gaps, and alignment in one call.  (rows[] is for grid
   mode -- use gui_grid; a zero-initialized descriptor is flow, so this ignores rows.) */
void
gui_layout( gui_layout_t desc )
{
    layout_set( desc.cols, desc.row_h, desc.gap_x, desc.gap_y );
    lf()->lay_align = (u8)desc.align;   /* full template carries the content alignment too */
}

/* stack -- the explicit header for a single full-width flex column, rows accumulating + scrolling
   (the everyday vertical list).  This is the canonical name for what a region used to do silently
   by default; it must now be declared.  Keeps the orthogonal modifiers (align, field split) as
   they stand -- use layout_default() for the full reset. */
void
gui_stack( void )
{
    layout_set( NULL, 0.0f, 0.0f, 0.0f );
    lf()->mode = GUI_MODE_STACK;   /* a single full-width column is a stack, not columns */
}

/* Single full-width column of height row_h (0 = auto) -- a stack with an explicit row height. */
void
gui_row( f32 row_h )
{
    layout_set( NULL, row_h, 0.0f, 0.0f );
    lf()->mode = GUI_MODE_STACK;
}

/* Reset the active region's layout to the state it opened with: one flex column of auto height,
   no field split, default gaps.  Finishes any open row first and leaves the region padding intact
   (use pad() to re-inset).  The single "clear everything" verb -- row( 0 ) only resets the columns
   and field_label_left( 0 ) only the field split, so this is the way back to the plain stack when
   both a template and a field split are in play. */
void
gui_layout_default( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );      /* finish any partially-filled row before clearing */
    layout_set_default( f );    /* single flex column, no field split, default gaps */
}

/* row_cols_n -- n equal flex columns of height row_h (0 = auto, one standard line).  The fixed-height
   twin of cols_n; row_cols (below) is the explicit-tracks twin. */
void
gui_row_cols_n( f32 row_h, u32 n )
{
    if ( n == 0 )                 n = 1;
    if ( n > GUI_LAYOUT_COLS )  n = GUI_LAYOUT_COLS;

    f32 cols[ GUI_LAYOUT_COLS + 1 ];
    for ( u32 i = 0; i < n; ++i ) cols[ i ] = 1.0f;   /* all fill -> equal split */
    cols[ n ] = GUI_END;
    layout_set( cols, row_h, 0.0f, 0.0f );
}

/* row_cols -- explicit per-column tracks (GUI_END-terminated, overloaded units) of height row_h.
   The fixed-height twin of cols. */
void
gui_row_cols( f32 row_h, const f32* tracks )
{
    layout_set( tracks, row_h, 0.0f, 0.0f );
}

/* cols -- the explicit header for N pre-divided column tracks (GUI_END-terminated, overloaded
   units), auto height, rows accumulating + scrolling.  The canonical name for the multi-column flow
   template; row_cols is the same with an explicit row height. */
void
gui_cols( const f32* tracks )
{
    layout_set( tracks, 0.0f, 0.0f, 0.0f );
}

/* cols_n -- N equal flex columns, auto height: the everyday uniform split (a wrapper over row_cols_n). */
void
gui_cols_n( u32 n )
{
    gui_row_cols_n( 0.0f, n );
}

/* Fixed-arity weighted rows -- the everyday 2/3/4-column split without a track array or its
   GUI_END terminator.  Each width takes the overloaded unit (>1 px, 1 fill, (0,1) fraction, 0
   natural), so row2( 0.3f, 0.7f ) is a 30/70 split and row2( 120, 1 ) is a 120px column plus a fill.
   Auto height (the common case); reach for row_cols / layout when a fixed height or >4 columns
   is needed. */

void gui_row2( f32 a, f32 b )                { f32 c[ 3 ] = { a, b, GUI_END };       layout_set( c, 0.0f, 0.0f, 0.0f ); }
void gui_row3( f32 a, f32 b, f32 c )         { f32 t[ 4 ] = { a, b, c, GUI_END };    layout_set( t, 0.0f, 0.0f, 0.0f ); }
void gui_row4( f32 a, f32 b, f32 c, f32 d )  { f32 t[ 5 ] = { a, b, c, d, GUI_END }; layout_set( t, 0.0f, 0.0f, 0.0f ); }

/* same_line -- keep the next widget on the line of the one just emitted, instead of breaking to a
   new row.  It is placed just past the previous item: `spacing` is the pixel gap (0 = flush; < 0 =
   the theme's default widget gap).  The widget takes its natural width (a button to its label, text
   to its glyphs); a widget with no natural width fills to the content's right edge.  The next plain
   widget after it resumes a fresh row below the line.  No-op before any widget has emitted in the
   region.  Mirrors ImGui::SameLine; built entirely on the cell engine's prev_item anchor. */
void
gui_same_line( f32 spacing )
{
    layout_frame_t* f = lf();
    if ( f->prev_item.w <= 0.0f && f->prev_item.h <= 0.0f ) return;   /* nothing to continue from */

    f32 gap   = ( spacing >= 0.0f ) ? spacing : WIDGET_GAP;
    f->cont_x = f->prev_item.x + f->prev_item.w + gap;
    f->cont_line = true;
}

/* stack_same_line -- the mode-prefixed name for same_line; identical behavior.  The stack_ spelling
   groups the "keep the next widget on this line" verb with the stack() header. */
void
gui_stack_same_line( f32 spacing )
{
    gui_same_line( spacing );
}

/* Field split -- the labeled value widgets (input_text, slider_float, checkbox) split their cell
   into a label track + a control track and lay out as an aligned "Label  [control]" form from a
   single call.  `side` places the label on the left or right; `label` / `control` are two sizes in
   the same overloaded unit as columns (>1 px, 1 fill, (0,1) fraction, 0 natural), so field_split(
   LEFT, 0.4f, 0.6f ) is a 40/60 split and field_split( LEFT, 120, 1 ) is a 120px label + fill control.
   Pass GUI_LABEL_NONE to turn it off (back to the trailing natural-width label).  Set once on a
   region; it persists like the row template until changed, and is resolved against whatever cell
   each widget is handed -- a full row or a single column. */
void
gui_field_split( gui_label_side_t side, f32 label, f32 control )
{
    layout_frame_t* f   = lf();
    f->lay_field_side    = (u8)side;
    f->lay_field_label   = label;
    f->lay_field_control = control;
}

/* field_split sugar -- a fixed-width label column with a flex control filling the rest, on the
   left or the right.  width <= 0 turns the field split off (restores the trailing label). */
void gui_field_label_left ( f32 width ) { gui_field_split( width > 0.0f ? GUI_LABEL_LEFT  : GUI_LABEL_NONE, width, 1.0f ); }
void gui_field_label_right( f32 width ) { gui_field_split( width > 0.0f ? GUI_LABEL_RIGHT : GUI_LABEL_NONE, width, 1.0f ); }

/* form -- a stack of aligned "Label  [control]" rows: a single flex column (stack) with a field
   split installed in one call.  label_w is the fixed label-track width on `side`, the control
   flex-fills the rest; label_w <= 0 turns the split off (a plain stack).  The reflection-tweaker /
   settings-panel header. */
void
gui_form( gui_label_side_t side, f32 label_w )
{
    gui_stack();
    gui_field_split( label_w > 0.0f ? side : GUI_LABEL_NONE, label_w, 1.0f );   /* label px + fill control */
}

/* Content alignment -- where each widget's natural-sized content sits inside its cell (a label, an
   image, a text run; a frame-filling widget like button / input still fills the cell and only its
   label follows).  Set once on a region; it persists like the row template and the field split
   until changed, and is independent of the columns -- row() / row_cols() leave it untouched, while
   layout_default() clears it back to LEFT | TOP.  Orthogonal to field_split, which positions a
   label *track*; align positions content *within* whatever cell a widget is handed. */
void
gui_align( gui_align_t a )
{
    lf()->lay_align = (u8)a;
}

/* Grid mode: partition the band from the pen to the region bottom into desc.cols x desc.rows
   (both GUI_END-terminated, overloaded units).  Uses cols, rows, gaps, and align; row_h is
   flow-only and ignored.  Widgets then fill cells row-major; nothing scrolls. */
void
gui_grid( gui_layout_t desc )
{
    layout_set_grid( desc.cols, desc.rows, desc.gap_x, desc.gap_y );
    lf()->lay_align = (u8)desc.align;   /* full template carries the content alignment too */
}

/* nc x nr equal flex cells filling the band -- the uniform grid (image grids, dashboards). */
void
gui_grid_cells( u32 nc, u32 nr )
{
    if ( nc == 0 )                nc = 1;
    if ( nr == 0 )                nr = 1;
    if ( nc > GUI_LAYOUT_COLS ) nc = GUI_LAYOUT_COLS;
    if ( nr > GUI_LAYOUT_COLS ) nr = GUI_LAYOUT_COLS;

    f32 cols[ GUI_LAYOUT_COLS + 1 ];
    f32 rows[ GUI_LAYOUT_COLS + 1 ];
    for ( u32 i = 0; i < nc; ++i ) cols[ i ] = 1.0f;   /* all fill -> equal columns */
    for ( u32 i = 0; i < nr; ++i ) rows[ i ] = 1.0f;   /* all fill -> equal rows    */
    cols[ nc ] = GUI_END;
    rows[ nr ] = GUI_END;
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
gui_pack( gui_pack_dir_t dir )
{
    layout_frame_t* f = lf();
    layout_row_break( f );            /* finish any flow row above the run */

    f->mode           = GUI_MODE_PACK;
    f->pack_dir       = (u8)dir;
    f->pack_size_next = -1.0f;        /* next item is natural until pack_size() */
    f->lay_ncols      = 1;            /* non-zero: pack bypasses the column walk */
    f->lay_nrows      = 0;
    f->col            = 0;
    f->row            = 0;
    f->pack_line      = 0.0f;
    f->prev_item      = ( gui_rect_t ){ 0 };
    f->cont_line      = false;

    if ( dir == GUI_PACK_HORIZONTAL )
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
void gui_bar( void ) { gui_pack( GUI_PACK_HORIZONTAL ); }

/* strip -- vertical pack: items top to bottom at their natural height. */
void gui_strip( void ) { gui_pack( GUI_PACK_VERTICAL ); }

/* pack_size -- set the next packed item's main-axis measure (overloaded unit, resolved against the
   space remaining on the current line); cleared back to natural after that one item. */
void gui_pack_size( f32 unit ) { lf()->pack_size_next = unit; }

/* pack_nextline -- break to a fresh line: reset the main pen to the line start and step the cross
   axis past the line just laid.  No-op outside pack mode. */
void
gui_pack_nextline( void )
{
    layout_frame_t* f = lf();
    if ( f->mode != GUI_MODE_PACK ) return;

    if ( f->pack_dir == GUI_PACK_HORIZONTAL )
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
    f->prev_item = ( gui_rect_t ){ 0 };
}

/* Region padding: re-inset the current region's content area and clear the template back to
   undeclared at the padded top-left.  Call right after opening a region; declare a mode header
   (stack / columns / grid / ...) afterward, since pad() leaves the region with no template. */
void
gui_pad( gui_pad_t p )
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

        gui()->row( gui()->calc_row( 128 ) );          // a row sized for a 128px image
        f32 w = gui()->calc_col( gui()->text_w("X") ); // a column sized to a label
----------------------------------------------------------------------------------------------*/

/* Height of one line of text in the active font. */
f32 gui_line_h( void ) { return font_line_h(); }

/* Pixel width of a string in the active font (whole string, no "##" handling). */
f32 gui_text_w( const char* s ) { return font_text_w( s ); }

/* Standard vertical margin a row adds around its content (so calc_row( char_h ) == one row). */
f32 gui_h_min( void ) { f32 m = WIDGET_H - font_char_h(); return m > 0.0f ? m : 0.0f; }

/* Standard horizontal margin a cell adds around its content (a left + right content inset). */
f32 gui_w_min( void ) { return 2.0f * WIDGET_PAD; }

/* Fixed row height / column width that fits content_* pixels plus the standard margin. */
f32 gui_calc_row( f32 content_h ) { return content_h + gui_h_min(); }
f32 gui_calc_col( f32 content_w ) { return content_w + gui_w_min(); }

/* Remaining free space in the current region from the layout pen -- the GetContentRegionAvail
   analogue.  Width is what a flex widget would fill (the content column from the pen to its right
   edge); height is the room left before the region bottom (the grid band end / view bottom).  Use
   it to size a child_begin to the leftover space, or to lay widgets out by hand.  Measured from the
   pen, so call it where the next widget would land; the height is most meaningful before scrolling. */
gui_vec2_t
gui_content_avail( void )
{
    layout_frame_t* f = lf();
    f32 w = ( f->content_x + f->content_w ) - f->cursor_x;
    f32 h = f->content_y_max - f->cursor_y;
    if ( w < 0.0f ) w = 0.0f;
    if ( h < 0.0f ) h = 0.0f;
    return ( gui_vec2_t ){ w, h };
}

/* Screen position where the next item would be emitted -- the GetCursorScreenPos analogue.  Anchor
   custom draw_* geometry to the layout pen without reserving a cell first; pair with content_avail()
   for the space ahead.  (Read it where the next widget would land -- it advances as items emit.) */
gui_vec2_t
gui_cursor_screen_pos( void )
{
    layout_frame_t* f = lf();
    return ( gui_vec2_t ){ f->cursor_x, f->cursor_y };
}

/* The current region's available area as a screen rect: the layout pen (top-left) joined with the
   room ahead (content_avail).  The rect to hand gui()->split, or to carve with the rect_cut_*
   helpers, when laying a band out by hand.  Like content_avail, read it where the next item lands. */
gui_rect_t
gui_content_rect( void )
{
    gui_vec2_t p = gui_cursor_screen_pos();
    gui_vec2_t a = gui_content_avail();
    return ( gui_rect_t ){ p.x, p.y, a.x, a.y };
}

/* split -- carve `area` into panels along `axis` using the overloaded column unit ( >1 px, ==1 fill,
   (0,1) fraction; the exact rule cols() uses ), writing each panel's screen rect into out[].  Returns
   the panel count ( <= GUI_LAYOUT_COLS, so size out[] to that ).  Pure rect math -- no state, no
   cached sizes, nothing emitted: pair each rect with push_layout_overlay to fill it, and RECURSE by
   splitting a returned rect again ( e.g. a vertical header/body/footer inside the content column ).
   `sizes` is GUI_END-terminated; gap <= 0 uses the theme widget gap.  The cross axis spans the whole
   `area` extent, so the panels tile one band -- nest splits for a grid.  This is the single-pass,
   known-size companion to the layout templates: it never measures content, so a panel is exactly the
   size asked for (use a fixed/fraction/fill size, not a content-driven one). */
u32
gui_split( gui_rect_t area, gui_axis_t axis, const f32* sizes, f32 gap, gui_rect_t* out )
{
    f32 g = ( gap > 0.0f ) ? gap : WIDGET_GAP;

    f32 tracks[ GUI_LAYOUT_COLS ];
    u32 n = layout_copy_tracks( sizes, tracks );   /* GUI_END-terminated -> count; NULL -> one fill */

    bool horiz  = ( axis == GUI_AXIS_X );
    f32  origin = horiz ? area.x : area.y;
    f32  extent = horiz ? area.w : area.h;

    /* The same resolver the column / field tracks use, so the unit rule is identical everywhere. */
    f32 pos[ GUI_LAYOUT_COLS ], size[ GUI_LAYOUT_COLS ];
    layout_resolve_tracks( tracks, n, origin, extent, g, pos, size );

    for ( u32 i = 0; i < n; ++i )
        out[ i ] = horiz ? ( gui_rect_t ){ pos[ i ], area.y, size[ i ], area.h }
                         : ( gui_rect_t ){ area.x, pos[ i ], area.w, size[ i ] };
    return n;
}

/*----------------------------------------------------------------------------------------------
    carve -- a whole nested partition from one flat f32 form.

    The recursive completion of split(): split() resolves one track list; carve resolves a list
    where any track may itself be a track list on the other axis.  The form is a single
    GUI_END-terminated f32 array in the same overloaded unit as cols (>1 px, ==1 fill, (0,1)
    fraction), with two control sentinels (GUI_CUT_X / GUI_CUT_Y) that turn a flat list into a
    tree: a size followed by a CUT is a container of that size subdivided on the named axis; a
    size followed by anything else is a leaf.  The form opens with a leading CUT that fills the
    whole area.  Resolution is a stack walk -- one layout_resolve_tracks per container (the same
    engine cols uses), leaf rects streamed to out[] in reading order -- with no per-leaf storage.
----------------------------------------------------------------------------------------------*/

/* p points at a GUI_CUT_* sentinel; return the pointer just past the GUI_END that closes the list
   it opens (depth-counted, so nested cuts are skipped whole).  Used to step over a container's
   sub-tree when gathering its parent's sibling tracks. */
static const f32*
carve_skip( const f32* p )
{
    int depth = 0;
    for ( ;; )
    {
        f32 t = *p++;
        if      ( t == GUI_CUT_X || t == GUI_CUT_Y ) ++depth;       /* open a nested list */
        else if ( t == GUI_END && --depth == 0 )     return p;      /* closed our own list */
        /* size tokens are ignored while skipping */
    }
}

/* Resolve one list into rects.  `p` points at the first item token (just past the opening CUT);
   `axis` is the list's cut axis; `area` is the box divided.  Leaf rects stream into out[] (capped
   at `max`) in reading order; containers recurse with the flipped axis.  Returns the pointer just
   past this list's closing GUI_END. */
static const f32*
carve_list( const f32* p, gui_axis_t axis, gui_rect_t area, f32 gap,
            gui_rect_t* out, u32* n, u32 max )
{
    /* Phase 1: gather this level's item extents; mark which items recurse (a CUT follows). */
    f32        sizes[ GUI_LAYOUT_COLS ];
    const f32* sub  [ GUI_LAYOUT_COLS ];   /* sub[i] -> the CUT token, or NULL for a leaf */
    u32        c = 0;
    const f32* q = p;

    while ( *q != GUI_END && c < GUI_LAYOUT_COLS )
    {
        sizes[ c ] = *q++;                                     /* the item's extent in this axis */
        if ( *q == GUI_CUT_X || *q == GUI_CUT_Y ) { sub[ c ] = q; q = carve_skip( q ); }
        else                                        sub[ c ] = NULL;
        ++c;
    }
    /* If the column cap stopped the gather early, walk on to this list's own GUI_END. */
    while ( *q != GUI_END ) q = ( *q == GUI_CUT_X || *q == GUI_CUT_Y ) ? carve_skip( q ) : q + 1;

    /* Phase 2: one resolve -- the same track engine cols() drives. */
    bool h = ( axis == GUI_AXIS_X );
    f32  pos[ GUI_LAYOUT_COLS ], size[ GUI_LAYOUT_COLS ];
    layout_resolve_tracks( sizes, c, h ? area.x : area.y, h ? area.w : area.h, gap, pos, size );

    /* Phase 3: emit leaves in reading order; recurse containers with the flipped axis. */
    for ( u32 i = 0; i < c; ++i )
    {
        gui_rect_t r = h ? ( gui_rect_t ){ pos[ i ], area.y, size[ i ], area.h }
                         : ( gui_rect_t ){ area.x, pos[ i ], area.w, size[ i ] };
        if ( sub[ i ] )
            carve_list( sub[ i ] + 1, ( *sub[ i ] == GUI_CUT_X ) ? GUI_AXIS_X : GUI_AXIS_Y,
                        r, gap, out, n, max );
        else if ( *n < max )
            out[ ( *n )++ ] = r;
    }
    return q + 1;   /* past our GUI_END */
}

u32
gui_carve( const f32* form, gui_rect_t area, f32 gap, gui_rect_t* out, u32 max )
{
    if ( !form || !out || max == 0 ) return 0;

    /* The form opens with a root CUT naming the axis; it fills the whole area. */
    gui_axis_t axis;
    if      ( *form == GUI_CUT_X ) axis = GUI_AXIS_X;
    else if ( *form == GUI_CUT_Y ) axis = GUI_AXIS_Y;
    else                            return 0;   /* malformed: a form must open with a CUT */

    f32 g = ( gap > 0.0f ) ? gap : WIDGET_GAP;
    u32 n = 0;
    carve_list( form + 1, axis, area, g, out, &n, max );
    return n;
}

/* Resolve one axis of a gui_anchor_t against the parent span [org, org+ext].  lo / hi are the
   normalized anchor fractions; when they are equal the child is point-anchored (size px hung off the
   line by `pivot`, shifted by `off_lo`); when they differ it stretch-anchors between the two
   fractions with off_lo / off_hi as edge insets.  Writes the child origin and size on this axis. */
static void
anchor_axis( f32 org, f32 ext, f32 lo, f32 hi, f32 pivot, f32 size, f32 off_lo, f32 off_hi,
             f32* out_pos, f32* out_size )
{
    f32 a = org + ext * lo;                      /* the near anchor line in screen space */
    if ( lo == hi )                              /* point-anchored: fixed size hung off one line */
    {
        *out_size = size;
        *out_pos  = a - pivot * size + off_lo;
    }
    else                                         /* stretch-anchored: edges track two fractions */
    {
        f32 b     = org + ext * hi;
        *out_pos  = a + off_lo;
        *out_size = ( b - off_hi ) - ( a + off_lo );
    }
}

/* anchor -- place a child rect inside `parent` from a normalized anchor frame (the UE4 Slate model).
   The general free-placement primitive behind gui_rect_align / gui_anchor_box: an axis with min ==
   max point-pins a fixed-size child, an axis with min < max stretches it between two parent
   fractions.  Pure rect math, nothing emitted -- pair with push_layout_overlay to fill the result, or
   draw into it directly.  See gui_anchor_t (gui.h) for the field meanings. */
gui_rect_t
gui_anchor( gui_rect_t parent, gui_anchor_t a )
{
    gui_rect_t r;
    anchor_axis( parent.x, parent.w, a.min.x, a.max.x, a.pivot.x, a.size.x, a.off.l, a.off.r,
                 &r.x, &r.w );
    anchor_axis( parent.y, parent.h, a.min.y, a.max.y, a.pivot.y, a.size.y, a.off.t, a.off.b,
                 &r.y, &r.h );
    return r;
}

/* Reserve a w x h block in the layout and return its screen rect, advancing the pen like any widget
   (the Dummy analogue) -- blank space, or a slot to fill with custom draw_* geometry / make clickable
   with invisible_button.  `w` is the main-axis size: honored in a pack run or on a same_line, while
   column / grid flow sizes the width to the track as for every widget.  The returned rect is always
   the actual reserved space, so draw into it rather than assuming w x h. */
gui_rect_t
gui_dummy( f32 w, f32 h )
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
gui_indent( f32 w )
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
gui_unindent( f32 w )
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
