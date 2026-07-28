/*==============================================================================================

    runtime_service/gui/flow/gui_layout.c -- The public layout vocabulary.

    Every layout verb a caller speaks, over the engine in gui_layout_core.c.  Nothing here holds
    machinery of its own beyond the rect algebra at the foot -- these are the names, the argument
    conventions, and the one-shot latches; the resolving happens below.

    File order, grouped by what the caller is doing:

        template headers   stack / row / cols_n / cols / row2-4, layout_default
        same_line          the line-continuation pair
        field + form       the ambient labeled-row split
        align + next_item  region alignment, and the one-shot per-item overrides
        grid               grid / grid_cells -- the fixed matrix over the band
        pack mode          bar / strip, and the run modifiers (size / nextline / wrap)
        indent             shift the content column, re-resolving the template against it
        sizing (sz_)       intent -> pixels; the one family that produces a dimension
        queries            cursor_screen_pos / content_avail / view_avail / content_rect
        region verbs       empty / rows_clip / scroll_by -- act on the region that is open
        rect algebra       split / carve / anchor -- pure math over a caller rect, nothing
                           emitted and no region required

    The region ENGINE the headers shape (layout_push/pop_region, scroll_clamp, nav_scroll_chase)
    is gui_scroll.c's, and the child / sub-layout lifecycles are gui_layout_child.c's and
    gui_sublayout.c's -- all included before this file.

    Included by gui_flow.c after those, second to last.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Public layout API -- shape the active region's repeating row template.

    These set the template on the current region; it persists and repeats for every subsequent
    widget until set again (or the region ends).  No push/pop: a region opens UNDECLARED, the
    first header names its mode, and each later call simply replaces the template.  Track sizes
    are THE OVERLOADED UNIT (gui.h) throughout.
==============================================================================================*/

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
   region.  Mirrors ImGui::SameLine; built entirely on the cell engine's line.prev_item anchor. */
void
gui_same_line( f32 spacing )
{
    layout_frame_t* f = lf();
    if ( f->line.prev_item.w <= 0.0f && f->line.prev_item.h <= 0.0f ) return;   /* nothing to continue from */

    f32 gap         = ( spacing >= 0.0f ) ? spacing : WIDGET_GAP;
    f->line.main    = f->line.prev_item.x + f->line.prev_item.w + gap;
    f->line.open    = true;   /* reopen the last line -- line.cross / line.ext are still current */
    f->line.cont_pending = true;   /* one-shot: the next emit is a pen placement on that line */
}

/* stack_same_line -- the mode-prefixed name for same_line; identical behavior.  The stack_ spelling
   groups the "keep the next widget on this line" verb with the stack() header. */
void
gui_stack_same_line( f32 spacing )
{
    gui_same_line( spacing );
}

/* The two PAINTLESS spacers: pure cell consumption, so they live with the composer rather than with
   the rules that draw (separator / separator_text are chrome's, over the same cell_next).  Each
   takes the next cell from the active template exactly like a real widget, so both compose with
   rows and grids with no special case -- which is the point of the cell model.

   skip     -- leave one blank slot of a standard row: the natural way to step over a grid cell.
   new_line -- break to a fresh line of height h and draw nothing, undoing a same_line and inserting
               a blank line between runs (ImGui NewLine, generalized).  A line has no body to
               measure, so its own "natural" is literally zero: h == 0 is a true zero-height break,
               not a fallback, and h < 0 defers to the theme's line height -- the vertical mirror of
               same_line( -1 ). */

void gui_skip    ( void )  { cell_next( WIDGET_H ); }
void gui_new_line( f32 h ) { cell_next( h >= 0.0f ? h : font_char_h() ); }

/* Field split -- the labeled value widgets (input_text, slider_float, checkbox) split their cell
   into a label track + a control track and lay out as an aligned "Label  [control]" form from a
   single call.  `side` places the label on the left or right; `label` / `control` are two sizes in
   the same overloaded unit as columns (>1 px, 1 fill, (0,1) fraction, 0 natural), so field_split(
   LEFT, 0.4f, 0.6f ) is a 40/60 split and field_split( LEFT, 120, 1 ) is a 120px label + fill control.
   Pass GUI_LABEL_NONE to turn it off (back to the trailing natural-width label).  Sugar over the
   ambient gui_field_t (field_set): it writes the one field authority, so -- like a style -- it
   persists until changed and is not reset per region (hide / align are left untouched).  Resolved
   against whatever cell each widget is handed, a full row or a single column. */
void
gui_field_split( gui_label_side_t side, f32 label, f32 control )
{
    gui_field_t* f = gui_field_get();
    f->side    = (u8)side;
    f->label   = label;
    f->control = control;
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
    layout_frame_t* f = lf();
    f->mod.align = (u8)a;
    f->line.align_swap = f->line.align_armed = false;   /* explicit set: nothing left to restore */
}

/* next_item_align -- one-shot alignment for the very next item only (flexbox's align-self), over
   the region's persistent align().  The override rides mod.align through the item's own draw
   (widgets seat their content by it at paint time) and the base alignment is restored at the
   following emit -- so call it immediately before the item; a template install in between drops
   the pending restore.

       gui()->next_item_align( GUI_ALIGN_RIGHT );  gui()->button( "Apply" );   // this one right */
void
gui_next_item_align( gui_align_t a )
{
    layout_frame_t* f = lf();
    if ( !f->line.align_swap ) f->line.align_restore = f->mod.align;   /* first set saves the base */
    f->mod.align        = (u8)a;
    f->line.align_swap  = true;
    f->line.align_armed = true;
}

/* next_item_fit -- override how big the very next cell item is (STACK / COLUMNS / GRID; pack has
   its own pack_size), instead of the widget's own implicit natural_w signal.  Overloaded unit, same
   rule as a column track: >1 px, (0,1) fraction of the cell, 1 fill it, 0 explicit natural (the
   widget's natural_w verbatim, even past the cell edge -- authored intent is never clamped).
   One-shot: consumed by the next item regardless of which branch resolves it.

       gui()->next_item_fit( 1.0f ); gui()->button( "Save" );   // stretch a button across its column
       gui()->next_item_fit( 0.0f ); gui()->slider_float(...);  // shrink a field to its own width */
void gui_next_item_fit( f32 unit ) { lf()->line.fit_next = unit; }

/* next_item_h -- one-shot override of the very next item's HEIGHT, the vertical twin of
   next_item_fit.  Overloaded unit resolved against the room left below the pen (from the next
   line down to the region bottom): >1 px, 1 fill the rest of the region, (0,1) a fraction of it,
   0 the widget's own h.  Flow: it lands when the item OPENS its row (an auto-height row takes the
   first item's h) and is ignored mid-row (the open row's height is already fixed).  Pack: a bar
   takes it as the item's cross extent, a strip as its main advance.  A grid cell carries the
   matrix height and ignores it.

       gui()->next_item_h( 1.0f );              gui()->button( "Fill" );  // rest of the region
       gui()->next_item_h( gui()->sz_u( 16 ) ); gui()->button( "Tall" );  // a 16-quantum row */
void gui_next_item_h( f32 unit ) { lf()->line.h_next = unit; }

/* next_item_rect -- one-shot: the next widget's cell IS this exact screen rect, whoever produced it
   (a carve/split/anchor leaf, a hand-cut band, a manual box).  The rect-first door onto the whole
   widget set: any gui_* widget takes its rect from here instead of the flow template, so one call
   site works under manual, carved, or flow layout.  Pure placement -- no pen advance, no highwater,
   no declared mode required (see cell_next_w); reserve with empty() if a region must size around it.

       gui_rect_t leaf[4]; gui()->carve( FORM, area, 6, leaf, 4 );
       gui()->next_item_rect( leaf[0] ); gui()->button( "Save" );        // carved cell, flow widget
       gui()->next_item_rect( leaf[1] ); gui()->checkbox( "On", &on ); */
void gui_next_item_rect( gui_rect_t r ) { lf()->line.rect_next = r; lf()->line.rect_next_set = true; }

/* Grid mode: partition the band from the pen to the region bottom into desc.cols x desc.rows
   (both GUI_END-terminated, overloaded units).  Widgets then fill cells row-major; nothing
   scrolls.  gui_grid_cells is the uniform nc x nr case and needs no descriptor. */
void
gui_grid( gui_grid_t desc )
{
    layout_set_grid( desc.cols, desc.rows, desc.gap_x, desc.gap_y );
    lf()->mod.align = (u8)desc.align;   /* full template carries the content alignment too */
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

/*==============================================================================================
    Pack mode -- the print run: place items one after another along an axis at their natural size.

    bar() opens the horizontal run (a toolbar), strip() the vertical one.
    Each widget takes its natural main-axis size unless pack_size() overrides the next one, resolved
    against the space left on the line (0 natural, 1 fill the rest, (0,1) a fraction, >1 px).  A
    widget with no natural width (slider / input / selectable) fills the remainder of the line by
    default.  pack_nextline() breaks to a fresh line.  Mode persists like any other until re-set.
==============================================================================================*/

/* pack -- open a print run along `dir`.  Finishes any flow row above it, then seeds the pack pen
   at the current layout position: the main axis runs along dir from there, the cross axis from the
   content edge.  Internal: bar / strip are the public names -- one per axis, no dir parameter. */
static void
gui_pack( gui_pack_dir_t dir )
{
    layout_frame_t* f = lf();
    layout_row_break( f );            /* finish any flow row above the run */
    layout_template_reset( f );       /* fresh iteration state; the line opens below */

    f->mode      = GUI_MODE_PACK;
    f->line.pack_dir  = (u8)dir;
    f->tmpl.ncols = 1;                 /* non-zero: pack bypasses the column walk */
    f->tmpl.nat_mask = 0;              /* pack has no columns -- nothing measures back */

    /* Open the first line at the pen: the main pen runs along dir from the line origin, the
       cross axis sits at the gap-before position below prior content / the content edge. */
    if ( dir == GUI_PACK_HORIZONTAL )
    {
        f->line.origin = f->content_x;        /* x start of every line       */
        f->line.cross  = layout_next_y( f );  /* y top of the first line     */
    }
    else
    {
        f->line.origin = layout_next_y( f );  /* y top of every column       */
        f->line.cross  = f->content_x;        /* x left of the first column  */
    }
    f->line.main = f->line.origin;
    f->line.ext  = 0.0f;
    f->line.open = true;
    f->nav_line  = ++s_build.nav_line_seq;   /* the run's first line is a fresh nav line */
}

/* bar -- horizontal pack: items left to right (the toolbar). */
void gui_bar( void ) { gui_pack( GUI_PACK_HORIZONTAL ); }

/* strip -- vertical pack: items top to bottom at their natural height. */
void gui_strip( void ) { gui_pack( GUI_PACK_VERTICAL ); }

/* pack_size -- set the next packed item's main-axis measure (overloaded unit, resolved against the
   space remaining on the current line); cleared back to natural after that one item. */
void gui_pack_size( f32 unit ) { lf()->line.pack_size_next = unit; }

/* pack_nextline -- break to a fresh line: commit the line just laid (folding it into the content
   measure) and open the next one past it, with the main pen back at the line start.  An empty
   line still advances by one gap (a deliberate blank line).  No-op outside pack mode. */
void
gui_pack_nextline( void )
{
    layout_frame_t* f = lf();
    if ( f->mode != GUI_MODE_PACK ) return;
    pack_line_break( f );   /* the shared break (gui_layout_core.c) -- auto-wrap uses it too */
}

/* pack_wrap -- opt the current pack run into auto-wrap: an item whose natural / fixed measure
   overruns the line breaks to a fresh one first (the flex-wrap toolbar / tag-row behavior).  A
   fill / fraction item resolves to the space left on its line, so it always fits and never
   wraps; an item wider than a whole empty line places anyway (no loop).  Call after bar() /
   strip(); cleared when another template is installed.  No-op outside pack mode. */
void
gui_pack_wrap( void )
{
    layout_frame_t* f = lf();
    if ( f->mode == GUI_MODE_PACK ) f->line.wrap = true;
}

/*==============================================================================================
    indent / unindent -- shift the active region's content column right (or back), so subsequent
    rows lay out inset.  The single mechanism behind tree_node's nesting, but usable on its own to
    inset any block of widgets.  w <= 0 uses the standard step (one row height, so a tree child
    lines up under its parent's label, past the fold arrow).  Finishes any open row first, moves
    the pen to the new column edge, and re-resolves the flow template against the narrowed width;
    always balance an indent with an unindent of the same width.  Flow layouts (stack / columns)
    only -- a grid / pack carries its own resolved geometry and ignores the reflow.
==============================================================================================*/

/* Narrow the content column by `left` on one side and `right` on the other, closing the open row
   first and re-resolving the template against what is left.  The mechanism indent has always
   been -- indent is the one-sided case -- exported because a DECORATOR insets both sides at once
   (chrome/widgets/gui_box.c) and re-deriving that here would be the same three lines twice.
   Negative values widen, which is how a scope undoes itself.  Flow layouts only. */
void
layout_inset( f32 left, f32 right )
{
    layout_frame_t* f = lf();
    if ( f->mode == GUI_MODE_GRID ) return;   /* flow / pack only -- a grid carries a fixed matrix */

    layout_row_break( f );               /* close the current row before shifting the column */
    f->content_x += left;
    f->content_w -= left + right;
    if ( f->content_w < 0.0f ) f->content_w = 0.0f;
    layout_reflow( f );
}

void
gui_indent( f32 w )
{
    if ( w <= 0.0f ) w = WIDGET_H;       /* default step: one row height (aligns under the arrow) */
    layout_inset( w, 0.0f );
}

void
gui_unindent( f32 w )
{
    if ( w <= 0.0f ) w = WIDGET_H;
    layout_inset( -w, 0.0f );
}

/*==============================================================================================
    Sizing (sz_) -- the one public family that turns intent into a pixel dimension.  Everything
    a caller feeds to row / cols / child_begin / window_set_next_size that is not a fraction or
    a fill comes from here; layout verbs consume sizes, sz_ produces them.

    Grid-first vocabulary (the standard rungs, in order of preference):

        sz_u( n )         -- authored geometry, in grid quanta
        sz_rows_h( n )    -- box height from a row count (scale-aware via the style stack)
        sz_row_gap()      -- the flow gap those boxes owe
        sz_scale_row( s ) -- one row height at a named ramp step, without pushing the scope

    Content-fit escape hatches (rare -- prefer letting the layout measure via natural sizing):

        sz_fit_row( content_h ) / sz_fit_col( content_w ) -- content px plus the standard row /
        cell margin; fit( 0 ) is the bare margin (the "size without content").
        sz_line_h() -- the raw font line advance, for text-shaped custom-draw rects.

    Text measurement lives with the draw family (text_size), not here.
==============================================================================================*/

/* sz_u -- n grid quanta in pixels (grid_quantum, the theme's px lattice; 4 by default).  The
   unit-first way to author any px size -- tracks, row heights, child / window sizes, pack_size --
   so authored geometry sits on the same lattice the theme metrics and the resolved tracks do,
   and retunes when the theme's quantum changes:

       gui()->cols( (f32[]){ gui()->sz_u( 12 ), 1.0f, GUI_END } );   // a 12-quantum column + a fill
       gui()->row( gui()->sz_u( 8 ) );                                // a 32px row at q=4

   q <= 1 degenerates to raw pixels.  Convention: gaps are one quantum, blocky panel measures
   are multiples of four quanta (the coarse "cell" -- 16px at q=4). */
f32
gui_sz_u( f32 n )
{
    u32 q = GRID_Q;
    return n * (f32)( q > 1 ? q : 1 );
}

/* Vertical gap the flow places between consecutive rows in a region -- also the top/bottom pad a
   window body / child opens with (REGION_PAD_DEFAULT).  A caller stacking N flow rows to
   precompute a fixed box height (a child_begin sized to hold an exact row count, say) owes this
   once above the first row and once below the last, plus once between every pair of rows. */
f32 gui_sz_row_gap( void ) { return WIDGET_GAP; }

/* Fixed box height for n uniform WIDGET_H rows stacked in a region with the default pad/gap
   (REGION_PAD_DEFAULT top/bottom, sz_row_gap() between) -- the everyday case (a fixed list of
   buttons/fields, a popup sized to its item count).  Reads through the style stack, so inside a
   scale_push scope it speaks that step's metrics. */
f32 gui_sz_rows_h( u32 n ) { return ( n == 0 ) ? 0.0f : (f32)n * WIDGET_H + ( (f32)n + 1.0f ) * WIDGET_GAP; }

/* Outer height to pass to child_begin (or a bare NODECORATION window) so its INTERIOR holds
   exactly n uniform WIDGET_H rows.  sz_rows_h( n ) is the region interior -- its own top/bottom
   pad and inter-row gaps included -- but a container carves its border (WIN_BORDER) off the box
   you hand it, so a child sized to sz_rows_h( n ) clips its last row by that border.  Add it back.
   This is the sizing rung an external caller needs to fit content to a row count without knowing
   the theme's border metric; n == 0 is the bare chrome, so a body-composer can read WIN_BORDER
   itself as sz_child_rows_h( 0 ). */
f32 gui_sz_child_rows_h( u32 n ) { return gui_sz_rows_h( n ) + WIN_BORDER; }

/* Height of one line of text in the active font (the raw line advance) -- a font metric, for
   text-shaped custom-draw rects; it knows nothing about row margins or gaps. */
f32 gui_sz_line_h( void ) { return font_line_h(); }

/* Width of n characters in the active font -- n times the advance of a representative glyph ('0',
   a solid non-proportional reference), for sizing a field to a fixed character count without
   measuring a placeholder string.  Raw content width (no cell margin); wrap in sz_fit_col to add
   the standard inset.  gui()->col( gui()->sz_chars( 8 ) )  // a column ~8 digits wide. */
f32 gui_sz_chars( f32 n ) { return n * font_text_w( "0" ); }

/* Fixed row height / column width that fits content_* pixels plus the standard margin a row /
   cell puts around its content (fit( 0 ) is the bare margin -- the "size without content"):

       gui()->row( gui()->sz_fit_row( 128 ) );                  // a row sized for a 128px image
       f32 w = gui()->sz_fit_col( gui()->text_size("X").x );    // a column sized to a label */
f32 gui_sz_fit_row( f32 content_h ) { f32 m = WIDGET_H - font_char_h(); return content_h + ( m > 0.0f ? m : 0.0f ); }
f32 gui_sz_fit_col( f32 content_w ) { return content_w + 2.0f * WIDGET_PAD; }

/*==============================================================================================
    Layout queries -- read where the next item would land, and how much room is left.

    All four answer from the layout pen, so read them WHERE the next widget would land; they
    advance as items emit.  cursor_screen_pos is the primitive the other three are built on.
==============================================================================================*/

/* Screen position where the next item would be emitted -- the GetCursorScreenPos analogue.  Anchor
   custom draw_* geometry to the layout pen without reserving a cell first; pair with content_avail()
   for the space ahead.  Mode-aware: a pack run (or an armed same_line) reports the running line
   pen, a mid-row flow reports the next open cell, and a fresh row reports the gap-before line
   origin. */
gui_vec2_t
gui_cursor_screen_pos( void )
{
    layout_frame_t* f = lf();

    if ( f->line.open && ( f->mode == GUI_MODE_PACK || f->line.cont_pending ) )
    {
        if ( f->mode == GUI_MODE_PACK && f->line.pack_dir == GUI_PACK_VERTICAL )
            return ( gui_vec2_t ){ f->line.cross, f->line.main };   /* strip: pen runs down     */
        return ( gui_vec2_t ){ f->line.main, f->line.cross };       /* bar / continuation: right */
    }
    if ( f->line.open && f->line.col > 0 )
        return ( gui_vec2_t ){ f->tmpl.cellx[ f->line.col ], f->line.cross }; /* next cell on the open row */

    return ( gui_vec2_t ){ f->content_x, layout_next_y( f ) };      /* a fresh line at the pen   */
}

/* Remaining free space in the current region from the layout pen -- the GetContentRegionAvail
   analogue.  Width is what a flex widget would fill (the content column from the pen to its right
   edge); height is the room left before the region bottom (the grid band end / view bottom).  Use
   it to size a child_begin to the leftover space, or to lay widgets out by hand.  The height is
   most meaningful before scrolling. */
gui_vec2_t
gui_content_avail( void )
{
    layout_frame_t* f = lf();
    gui_vec2_t      p = gui_cursor_screen_pos();
    f32 w = ( f->content_x + f->content_w ) - p.x;
    f32 h = f->band_bottom - p.y;
    if ( w < 0.0f ) w = 0.0f;
    if ( h < 0.0f ) h = 0.0f;
    return ( gui_vec2_t ){ w, h };
}

/* content_avail clamped to the VISIBLE view.  The content column can run wider than the view
   (an overflowing sibling -- a long unwrapped text run -- widens it so scroll can reach it);
   content_avail reports that full column, which is right for passive rows but wrong for sizing
   an opaque interactive surface (a child box, a text editor): sized to the column, it seats
   itself under the scrollbar gutter and border.  This query never exceeds the visible track.
   The pen offset is content-space (pen and column share the scroll bias), so the width carries
   no scroll term -- a box sized by it keeps a constant width while the region scrolls.  Height
   is content_avail's own (band_bottom already stops at the view). */
gui_vec2_t
gui_view_avail( void )
{
    layout_frame_t* f = lf();
    gui_vec2_t      a = gui_content_avail();

    f32 off   = gui_cursor_screen_pos().x - f->content_x;
    f32 vis_w = ( f->view.w - f->pad.l - f->pad.r ) - off;
    if ( vis_w < 0.0f ) vis_w = 0.0f;
    if ( a.x > vis_w )  a.x = vis_w;
    return a;
}

/* The current region's available area as a screen rect: the layout pen (top-left) joined with the
   room ahead (content_avail).  The rect to hand gui()->split, or to carve with the rect_cut_*
   helpers, when laying a band out by hand. */
gui_rect_t
gui_content_rect( void )
{
    gui_vec2_t p = gui_cursor_screen_pos();
    gui_vec2_t a = gui_content_avail();
    return ( gui_rect_t ){ p.x, p.y, a.x, a.y };
}

/*==============================================================================================
    Region verbs -- they act on the region that is currently open (unlike the rect algebra below,
    which is pure math over a caller rect).
==============================================================================================*/

/* Reserve a w x h block in the layout and return its screen rect, advancing the pen like any widget
   (the ImGui Dummy analogue) -- blank space, or a slot to fill with custom draw_* geometry / make
   clickable with invisible_button.  `w` is the main-axis size: honored in a pack run or on a
   same_line, while column / grid flow sizes the width to the track as for every widget.  The
   returned rect is always the actual reserved space, so draw into it rather than assuming w x h. */
gui_rect_t
gui_empty( f32 w, f32 h )
{
    return cell_next_w( w, h );
}

/*============================================================================================*/
/* Fixed-pitch row clipper -- the ListClipper analogue.  Emitting thousands of rows costs the full
   per-item pipeline (cell resolve, id/state, nav, text measure) whether or not the row is visible;
   the draw floor's clip cull only drops the geometry, never that work.  This skips the offscreen
   rows entirely: reserve `count` rows of extent, jump the pen past the culled head, and return the
   visible [first, last) for the caller to emit.

       gui_span_t s = gui()->rows_clip( count, row_h );
       for ( i32 i = s.first; i < s.last; ++i ) { ...emit row i... }
       gui()->rows_clip_end();   // only when content follows the run in the same region

   The rows must be FIXED PITCH: row_h + the region's row gap, every row.  row_h 0 defaults to the
   template's fixed row_h when one is set (row_cols), else WIDGET_H -- an auto-height template with
   rows of another height (bare text is font_char_h) needs the true height passed or the culled and
   emitted pitches drift apart and rows swim under scroll.  The head jump goes through
   layout_pen_jump and the tail is one up-only highwater touch, so the region measures all `count`
   rows exactly as if they were emitted: scrollbar range, clamp, and the one-frame extent lag are
   unchanged.  Nav only sees the emitted rows (keyboard can't walk into the culled range). */

static f32 s_rows_run_end = 0.0f;   // reserved run bottom for rows_clip_end (0 = no open run)

gui_span_t
gui_rows_clip( i32 count, f32 row_h )
{
    layout_frame_t* f = lf();
    if ( count <= 0 ) return ( gui_span_t ){ 0, 0 };

    f32 h = ( row_h > 0.0f )       ? row_h
          : ( f->tmpl.row_h > 0.0f ) ? f->tmpl.row_h
          :                            (f32)WIDGET_H;
    f32 pitch = h + mod_gap_y( f );
    f32 top   = layout_next_y( f );   /* where row 0 opens -- the gap owed above applied once */

    /* Reserve the whole run's extent up front (last row's bottom, no trailing gap): the highwater
       only climbs, so this one touch is all the scroll/extent machinery ever needed from the rows. */
    f32 run_end = top + (f32)count * pitch - mod_gap_y( f );
    extent_track( f, f->content_x, run_end );
    s_rows_run_end = run_end;

    /* Visible band test in screen space: the pen (and top) already carry the scroll bias and
       f->view is THE screen-space visible rect, so the row range is a straight division. */
    i32 first = (i32)floorf( ( f->view.y - top ) / pitch );
    i32 last  = (i32)ceilf ( ( f->view.y + f->view.h - top ) / pitch );
    if ( first < 0 )     first = 0;
    if ( first > count ) first = count;
    if ( last  > count ) last  = count;
    if ( last  < first ) last  = first;

    /* Skip the culled head: row `first` opens exactly at its true scrolled position.  The jump
       closes any open line and owes no gap, matching where the unclipped flow would place it. */
    layout_pen_jump( f, top + (f32)first * pitch );

    return ( gui_span_t ){ first, last };
}

/* Close a rows_clip run: jump the pen past the reserved tail so content emitted after the run
   lands below all `count` rows instead of overlapping the culled ones.  Only needed when content
   follows the run in the same region -- a run that ends the region needs no call (the extent was
   already reserved).  No-op without an open run. */
void
gui_rows_clip_end( void )
{
    if ( s_rows_run_end <= 0.0f ) return;
    layout_frame_t* f = lf();
    layout_pen_jump( f, s_rows_run_end );
    f->gap_pending = true;   /* the run's last row owes the usual gap to what follows */
    s_rows_run_end = 0.0f;
}

/* scroll_by -- nudge the CURRENTLY OPEN region's scroll offset by (dx, dy) in px, applied THIS frame.
   scroll_y stays a normal 0=top value, so a large delta drives to an edge (clamped here against last
   frame's measured content): +BIG jumps to the bottom / tail, -BIG to the top.  Unlike a wheel notch
   (claimed at pop, so it only reaches the screen next frame), this re-bases the live pen by the same
   delta the moment it is called, so items emitted after it land at the new offset with no one-frame
   lag -- call it right after opening the region, before its content.  The measure stays honest: the
   pen and highwater move together (as a region scroll does), so pop reads the true content extent.
   No-op in a region without a scroll link (a sub-layout, or outside any region). */
void
gui_scroll_by( f32 dx, f32 dy )
{
    layout_frame_t* f = lf();
    if ( !f->scroll ) return;

    f32 old_x = f->scroll->scroll_x;
    f32 old_y = f->scroll->scroll_y;
    f->scroll->scroll_x += dx;
    f->scroll->scroll_y += dy;

    /* Clamp to the same range the next push would (last frame's content vs. this frame's view), so a
       jump-to-edge delta settles exactly on the edge instead of overshooting into a blank frame. */
    scroll_clamp( &f->scroll->scroll_y, f->scroll->content_h, f->view.h );
    scroll_clamp( &f->scroll->scroll_x, f->scroll->content_w, f->view.w );

    /* Re-base the live canvas by the delta actually applied: pen_y = origin_y - scroll_y, so a larger
       scroll_y slides the content up.  pen and highwater shift together, keeping the seed invariant
       (an empty region still measures 0) and the pop measure true. */
    f32 applied_x = f->scroll->scroll_x - old_x;
    f32 applied_y = f->scroll->scroll_y - old_y;
    f->content_x -= applied_x;
    f->high_x    -= applied_x;
    f->pen_y     -= applied_y;
    f->high_y    -= applied_y;

    redraw_request();
}

/*==============================================================================================
    Rect algebra -- split / carve / anchor.

    Pure math over a caller-supplied rect: no state, no cached sizes, nothing emitted, and no open
    region required.  Each returns rects the caller then fills (push_layout_overlay / flow_begin,
    or next_item_rect for a single widget), and each composes with itself -- a returned rect can be
    split, carved, or anchored again.  These are the single-pass, known-size companion to the
    measuring layout templates above.
==============================================================================================*/

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
    layout_tracks_resolve( tracks, n, origin, extent, g, pos, size );

    for ( u32 i = 0; i < n; ++i )
        out[ i ] = horiz ? ( gui_rect_t ){ pos[ i ], area.y, size[ i ], area.h }
                         : ( gui_rect_t ){ area.x, pos[ i ], area.w, size[ i ] };
    return n;
}

/*==============================================================================================
    carve -- a whole nested partition from one flat f32 form.

    The recursive completion of split(): split() resolves one track list; carve resolves a list
    where any track may itself be a track list on the other axis.  The form is a single
    GUI_END-terminated f32 array in the same overloaded unit as cols (>1 px, ==1 fill, (0,1)
    fraction), with two control sentinels (GUI_CUT_X / GUI_CUT_Y) that turn a flat list into a
    tree: a size followed by a CUT is a container of that size subdivided on the named axis; a
    size followed by anything else is a leaf.  The form opens with a leading CUT that fills the
    whole area.  Resolution is a stack walk -- one layout_tracks_resolve per container (the same
    engine cols uses), leaf rects streamed to out[] in reading order -- with no per-leaf storage.
==============================================================================================*/

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
    layout_tracks_resolve( sizes, c, h ? area.x : area.y, h ? area.w : area.h, gap, pos, size );

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

// clang-format on
/*============================================================================================*/
