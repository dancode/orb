/*==============================================================================================

    runtime_service/gui/core/gui_layout_core.c -- Layout mechanism: track resolver + cell emitters.

    The engine the public layout API (gui_layout.c) drives.  It carves a region's content area
    into cells from a repeating row / column template (or a fixed grid, or a pack run) and hands
    the next cell to each widget, hiding the layout shape from the widgets entirely.  Two halves:

        - track resolver + template installers (the overloaded-unit math in unit_resolve /
          layout_resolve_tracks, the layout_template_reset / layout_modifiers_reset seams,
          layout_seed_content, and layout_set / _grid / _reflow / _clear) -- the "what shape
          is this region" mechanism;
        - cell emitters (widget_next_rect_w, grid_next_rect, pack_next_rect, field_split_resolve,
          widget_split_label) -- the per-item "hand out the next rect" mechanism.

    Lifted out of gui_widget_core.c so the layout engine sits adjacent to the API that drives it,
    the way gui_window.c (state) precedes gui_widget_window.c (chrome).

    Included by gui.c after gui_widget_core.c (so the size / color macros, rect_align, the
    label grammar, text_center_y, and item_flags_resolve are in scope) and before gui_layout.c
    (which calls layout_set / widget_next_rect / the region helpers).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout cursor helpers
----------------------------------------------------------------------------------------------*/

static f32 widget_right( void ) { return lf()->content_x + lf()->content_w; }

/* Grow the region's highwater (content_max_x, content_max_y) to include a content corner (x, y) in
   screen coords: the monotonic bounding-box max layout_pop_region cancels the scroll bias out of and
   compares against each view to decide a scrollbar.  The highwater only ever climbs, so a running
   max over every item's far corner reconstructs a line's full extent -- one call per placement, on
   either axis, in place of the per-axis, per-mode inline updates the emitters used to do.  Does not
   touch the pen; content_reach moves both, widget_track_width grows the x highwater alone. */
static void
extent_track( layout_frame_t* f, f32 x, f32 y )
{
    if ( x > f->content_max_x ) f->content_max_x = x;
    if ( y > f->content_max_y ) f->content_max_y = y;
}

/* The x-only face of extent_track, for a leaf widget reporting it drew out to right_x -- wider than
   the cell it was handed (text to its glyphs, a label past the row edge).  The widget knows only its
   horizontal overflow; its vertical extent is the emitter's business.  Grows the x highwater alone,
   so the horizontal bar sees content the cell did not bound.  Every leaf-widget overflow site here. */
static void
widget_track_width( f32 right_x )
{
    if ( right_x > lf()->content_max_x ) lf()->content_max_x = right_x;
}

/* A forward flow step: content now reaches corner (x, y), so drop the pen to it and lift the
   highwater with it.  The shared advance behind every placement and block emit (a cell, a packed
   item, a popped child box).  The pen and highwater move together here -- only a pen reposition
   (layout_pen_jump) parts them.  content_y only ever climbs through this seam, so max == the drop. */
static void
content_reach( layout_frame_t* f, f32 x, f32 y )
{
    if ( y > f->content_y ) f->content_y = y;   /* pen drops to the content end */
    extent_track( f, x, y );                    /* highwater climbs with it     */
}

/*----------------------------------------------------------------------------------------------
    Layout engine -- carve a region's content area into cells from a repeating row template.

    One resolver does both axes (here, only columns are wired); the row template lives on the
    layout frame and persists until changed, so widgets emit cell-by-cell while staying wholly
    agnostic to the layout shape.  See gui_layout_t (gui.h) for the overloaded unit rule.
----------------------------------------------------------------------------------------------*/

/* Resolve a track list into pixel [pos,size] pairs along one axis.  `n` tracks (>= 1) are laid
   from `origin` across `extent`, with `gap` between each.  Units (see gui_layout_t): >1 fixed px,
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
        bool is_flex_or_fraction_track = ( t == 1.0f ) || ( t > 0.0f && t < 1.0f );
        if ( is_flex_or_fraction_track && sz < min_w )
            sz = min_w;

        out_pos [ i ] = pos;
        out_size[ i ] = sz;
        pos += sz + gap;
    }
}

/* Resolve one overloaded size unit against a single span -- the scalar form of the rule
   layout_resolve_tracks applies to a whole list (see gui_layout_t): > 1 fixed px, == 1 fill the
   available extent, (0,1) a fraction of it, == 0 the natural measure.  < 0 (unset) is natural
   with a fill fallback for an item that has no natural measure -- the pack-mode default.  The
   single-value sites (pack_size_next, the one-shot cell fit_next) resolve through here, so the
   unit rule lives in exactly two functions: this scalar and the track-list resolver. */
static f32
unit_resolve( f32 u, f32 natural, f32 avail )
{
    if ( u <  0.0f ) return ( natural > 0.0f ) ? natural : avail;   /* unset: natural, else fill */
    if ( u == 0.0f ) return natural;                                /* explicit natural          */
    if ( u == 1.0f ) return avail;                                  /* fill the available extent */
    if ( u <  1.0f ) return u * avail;                              /* fraction of it            */
    return u;                                                       /* fixed px                  */
}

/* The size-animate seam: turn a remembered extent's `target` into the extent to use this frame,
   optionally easing toward it through the animation pool.  Every panel / child / split routes its
   size here after picking its own target (a user-resized override, or the content it measured), so
   animated resize is a matter of handing this an anim_id.  GUI_ID_NONE (which every caller currently
   passes) is the exact-size fast path -- no pool touch, the target verbatim.  SEAM: the animation hook
   stays inert until a panel opts in with id_combine( panel_id, tag ).  Speed is the gui_anim_f32
   Hz-like rate (10 ~ 250ms). */
static f32
size_animate( f32 target, gui_id_t anim_id, f32 speed )
{
    return ( anim_id != GUI_ID_NONE ) ? gui_anim_f32( anim_id, target, speed ) : target;
}

/* Close the open line and return the column walk to a row start: the next line owes a gap before it
   (gap_pending) rather than one appended after, so content_y stays the exact content end.  The one
   commit behind flow rows, pack lines, and same_line continuations.  The line's extent is already in
   the watermark -- every item grew it from its far corner via extent_track as it was placed -- so
   there is nothing to fold here.  An empty line (nothing emitted) owes nothing.  No-op when closed. */
static void
line_commit( layout_frame_t* f )
{
    f->col = 0;
    if ( !f->line_open ) return;
    f->line_open = false;
    if ( f->line_ext <= 0.0f ) return;   /* nothing emitted -- owe nothing */
    f->gap_pending = true;
}

/* True for a line that gui_pack just opened and nothing has been placed on yet (line_ext, the
   cross extent, only grows once an item lands).  Its own position -- not the cursor plus a gap --
   is the next placement point, since the gap was already applied when the line opened. */
static bool
line_just_opened( const layout_frame_t* f )
{
    return f->line_open && f->line_ext <= 0.0f;
}

/* Where the next line -- or block placed at the pen: a child box, a split band, a grid band --
   opens on the cross axis: the content end plus the gap owed by the content above it.  An open
   line owes one too (content_y already carries its live extent); a fresh, still-empty pack line
   is its own next position (see line_just_opened). */
static f32
layout_next_y( layout_frame_t* f )
{
    if ( line_just_opened( f ) )
    {
        bool vert = ( f->mode == GUI_MODE_PACK && f->pack_dir == GUI_PACK_VERTICAL );
        return vert ? f->line_main : f->line_cross;
    }
    if ( f->line_open || f->gap_pending )
        return f->content_y + f->lay_gap_y;
    return f->content_y;
}

/* Reposition the pen to an explicit y -- an imperative host taking authority over the flow (a table
   stepping row to row, a menu bar restoring the pen it borrowed).  The pen is authoritative: no line
   is open and no gap is owed, so the next line opens exactly here.  This moves the PEN alone: the
   highwater is lifted up-only, so a forward jump (a table row extending the body) is still measured,
   while a backward restore (a menu bar handing the pen back) does not rewind the content the region
   already reached. */
static void
layout_pen_jump( layout_frame_t* f, f32 y )
{
    f->content_y   = y;
    if ( y > f->content_max_y ) f->content_max_y = y;   /* highwater climbs, never rewinds */
    f->col         = 0;
    f->line_open   = false;
    f->gap_pending = false;
}

/* Finish the active template's open geometry before it is replaced or a block is placed at the
   pen: commit the open line -- or, leaving a grid, surrender the band.  A grid owns everything
   from its top to the region's content bottom, so once any cell is emitted the pen lands at the
   band bottom (content_y_max); an untouched grid gives the band back.  Safe in any mode. */
static void
layout_row_break( layout_frame_t* f )
{
    if ( f->mode == GUI_MODE_GRID )
    {
        if ( f->col > 0 || f->row > 0 )   /* any cell emitted -> the band is consumed */
        {
            content_reach( f, f->content_x, f->content_y_max );   /* pen + highwater to the band bottom */
            f->gap_pending = true;
        }
        f->col = 0;
        f->row = 0;
        return;
    }
    line_commit( f );
}

/* Count an GUI_END-terminated track list into out[] (capped), substituting a single flex track
   for an empty / NULL list.  Returns the count.  The source list is never stored -- callers
   resolve it straight into cell geometry, so the template arrays do not live on the frame. */
static u32
layout_copy_tracks( const f32* src, f32* out )
{
    u32 n = 0;
    if ( src )
        while ( n < GUI_LAYOUT_COLS && src[ n ] >= 0.0f ) { out[ n ] = src[ n ]; ++n; }
    if ( n == 0 ) { out[ 0 ] = 1.0f; n = 1; }   /* default to a single fill track (full extent) */
    return n;
}

/* Reset the per-template iteration state -- everything a new template must not inherit from the
   shape it replaces: the column/row walk, the flow-mode row rows counter, the same_line anchor,
   and the pack pen.  Every installer (layout_set / _grid / _default, gui_pack) runs this after
   breaking the open row, so "what a fresh template starts from" has exactly one answer.  Gaps,
   field split, and align are modifiers: they persist across installs and only the full clears
   (layout_clear / layout_set_default) reset them via layout_modifiers_reset below. */
static void
layout_template_reset( layout_frame_t* f )
{
    f->lay_nrows      = 0;                      /* flow until a grid installs rows */
    f->col            = 0;
    f->row            = 0;
    f->prev_item      = ( gui_rect_t ){ 0 };    /* no same_line anchor in a fresh template */
    f->cont_pending   = false;
    f->line_open      = false;                  /* line_cross/ext/main re-seed when a line opens */
    f->line_ext       = 0.0f;
    f->pack_dir       = 0;                      /* pack line is re-seeded by pack() */
    f->pack_size_next = -1.0f;                  /* unset -> next packed item is natural */
    f->fit_next       = -1.0f;                  /* unset -> next cell item uses its own natural_w */
    /* gap_pending is NOT reset: content committed above still owes its gap to the next line. */
}

/* Reset the orthogonal modifiers: gaps back to the theme, field split off, align to LEFT | TOP. */
static void
layout_modifiers_reset( layout_frame_t* f )
{
    f->lay_gap_x         = WIDGET_GAP;
    f->lay_gap_y         = WIDGET_GAP;
    f->lay_field_side    = 0;             /* trailing label until field_split / field_label_* */
    f->lay_field_label   = 0.0f;
    f->lay_field_control = 0.0f;
    f->lay_align         = 0;             /* LEFT | TOP until align() / layout.align sets it */
}

/* Open a region UNDECLARED: zero the template state and leave the mode NONE so the first layout
   header (stack / columns / grid / ...) installs real geometry.  A widget emitted before any
   header trips the guard in widget_next_rect_w.  Called when a region or sub-layout opens (the
   old silent single-column default is gone) and by gui_pad after re-insetting -- the modifiers
   and iteration state start from a known state every region. */
static void
layout_clear( layout_frame_t* f )
{
    f->mode         = GUI_MODE_NONE;
    f->lay_ncols    = 0;            /* no template -- first header resolves one */
    f->lay_row_h    = 0.0f;
    f->gap_pending  = false;        /* fresh region: the first line opens flush at the pen */
    f->nav_line_pin = false;        /* fresh content: nav lines dispense normally again */
    layout_modifiers_reset( f );
    layout_template_reset( f );
}

/* Install the region's default template: one flex column, auto height -- the classic stack, mode
   STACK.  Resolves immediately (content_x/content_w must already be set), so the single column
   fills the content width with no gap.  This is the full reset (clears field split + align too):
   it backs gui_layout_default and the emit-before-header guard's release fallback.  The plain
   stack() header keeps modifiers and routes through layout_set instead. */
static void
layout_set_default( layout_frame_t* f )
{
    f->mode          = GUI_MODE_STACK;
    f->lay_ncols     = 1;
    f->lay_row_h     = 0.0f;
    f->lay_cols[ 0 ] = 1.0f;             /* single flex track, kept so indent can re-resolve */
    f->cellx[ 0 ]    = f->content_x;     /* one flex column == the whole content width */
    f->cellw[ 0 ]    = f->content_w;
    layout_modifiers_reset( f );
    layout_template_reset( f );
}

/* Seed the frame's content column + pen from its outer box and a pad inset, leaving the template
   UNDECLARED.  The one derivation of the content geometry: layout_push_region (region open),
   sublayout_open (transient cell frame), and gui_pad (re-inset) all route through here.  Requires
   outer, scroll, and the gutter reservation (sb_w / sb_h) already set on the frame.  The live pen
   is biased by -scroll so widgets slide under the clip, while origin_* stays unscrolled so the
   content extent measures cleanly at pop; content_y_max is the grid band end / view bottom. */
static void
layout_seed_content( layout_frame_t* f, gui_pad_t pad )
{
    f->pad           = pad;   /* kept: the pads join the measured canvas at pop */
    f->origin_x      = f->outer.x + pad.l;
    f->origin_y      = f->outer.y + pad.t;
    f->content_x     = f->origin_x - f->scroll->scroll_x;
    f->content_w     = f->outer.w - pad.l - pad.r - f->sb_w;
    f->content_y     = f->origin_y - f->scroll->scroll_y;
    f->content_max_x = f->content_x;   /* seed the highwater at the origin corner -> an empty */
    f->content_max_y = f->content_y;   /* body measures 0 on both axes (premeasure sentinel)  */
    f->content_y_max = f->outer.y + f->outer.h - pad.b - f->sb_h;

    /* Fresh nav coordinate: this content column is one container to the keyboard (a window body,
       a child box, a re-inset pad).  The first line dispenses when the first line opens. */
    f->nav_region = ++s_build.nav_region_seq;
    f->nav_line   = 0;

    layout_clear( f );   /* content re-seeded -> the template opens undeclared; declare a header */
}

/* layout_push_scoped / layout_pop_scoped -- a minimal layout-frame push/pop at an explicit
   (x, y, w), used only by the volatile-widget replay scope, now live in widgets/gui_volatile.c
   (gui_replay_scope_enter/_exit) alongside the rest of that feature. Both call
   layout_set_default (below), which is why they must be textually included after this file. */

/* Replace the active flow template on the current frame.  Finishes any open row first, then
   resolves the columns into cell geometry once (they are constant for every row of the template).
   The next widget starts a fresh row of the new shape; it repeats until set again. */
static void
layout_set( const f32* cols, f32 row_h, f32 gap_x, f32 gap_y )
{
    layout_frame_t* f = lf();
    layout_row_break( f );
    layout_template_reset( f );

    f->mode      = GUI_MODE_COLUMNS;   /* a flow template; stack()/row() override to STACK */
    f->lay_row_h = row_h;
    f->lay_gap_x = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;

    f32 tracks[ GUI_LAYOUT_COLS ];
    f->lay_ncols = layout_copy_tracks( cols, tracks );
    for ( u32 i = 0; i < f->lay_ncols; ++i ) f->lay_cols[ i ] = tracks[ i ];   /* kept for indent reflow */
    layout_resolve_tracks( tracks, f->lay_ncols, f->content_x, f->content_w, f->lay_gap_x,
                           f->cellx, f->cellw );
}

/* Re-resolve a flow template's cells from the current content column -- used after indent /
   unindent shifts content_x / content_w so subsequent rows land at the new inset.  Flow only
   (STACK / COLUMNS); a grid carries a pre-resolved matrix and a pack its own pen, neither of which
   is re-indented mid-iteration, so they are left untouched. */
static void
layout_reflow( layout_frame_t* f )
{
    if ( f->mode == GUI_MODE_STACK || f->mode == GUI_MODE_COLUMNS )
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
    layout_template_reset( f );

    f->mode      = GUI_MODE_GRID;
    f->lay_gap_x = ( gap_x > 0.0f ) ? gap_x : WIDGET_GAP;
    f->lay_gap_y = ( gap_y > 0.0f ) ? gap_y : WIDGET_GAP;

    /* Resolve columns across the content column and rows across the band from the pen to the
       content bottom.  An empty band (content already overflowed) clamps to zero. */
    f32 tracks[ GUI_LAYOUT_COLS ];
    f->lay_ncols = layout_copy_tracks( cols, tracks );
    layout_resolve_tracks( tracks, f->lay_ncols, f->content_x, f->content_w, f->lay_gap_x,
                           f->cellx, f->cellw );

    f->lay_nrows = layout_copy_tracks( rows, tracks );
    f32 grid_top = layout_next_y( f );   /* gap-before: the band opens below prior content */
    f32 grid_h   = f->content_y_max - grid_top;
    if ( grid_h < 0.0f ) grid_h = 0.0f;
    layout_resolve_tracks( tracks, f->lay_nrows, grid_top, grid_h, f->lay_gap_y,
                           f->rowy, f->rowh );

    f->nav_line = ++s_build.nav_line_seq;   /* grid row 0 opens as a fresh nav line */
}

/* Resolve one cell's horizontal box -- the fit-then-align sequence every cell placement (flow or
   grid) shares: decide how big before align_x decides where.  A one-shot fit_next (overloaded
   unit -- see gui_layout_t) always wins when set, taken as authored intent even past the cell
   edge, exactly like a fixed track px is never floored (layout_resolve_tracks).  Unset (-1, the
   common case) falls back to the widget's own natural_w signal: a natural width smaller than the
   cell shrinks to it (a button hugs its label), anything else fills the cell verbatim -- the
   per-widget-type default every emit already carries, now consulted for every mode, not just
   STACK.  A shrunk box is seated in the leftover space by lay_align; a filled one starts flush at
   the cell's left edge (align has nothing to do once the box owns the whole cell). */
static gui_rect_t
cell_fit_resolve( layout_frame_t* f, f32 cell_x, f32 cell_w, f32 natural_w, f32 y, f32 h )
{
    f32 fit     = f->fit_next;
    f->fit_next = -1.0f;   /* one-shot: consumed whichever branch below reads it */

    f32 w = ( fit >= 0.0f )
          ? unit_resolve( fit, ( natural_w > 0.0f ) ? natural_w : cell_w, cell_w )
          : ( ( natural_w > 0.0f && natural_w < cell_w ) ? natural_w : cell_w );

    f32 x = ( w < cell_w ) ? align_x( cell_x, cell_w, w, f->lay_align ) : cell_x;
    return ( gui_rect_t ){ x, y, w, h };
}

/* Cell a grid hands to a widget: a fixed (col,row) slot of the pre-resolved matrix, then advance
   row-major.  Past the last cell the cursor clamps to it, so overflow widgets stack harmlessly in
   the final slot rather than reading out of bounds.  Row height is the matrix's, not the item's --
   grid has no vertical natural-size concept, only horizontal fit within the column. */
static gui_rect_t
grid_next_rect( layout_frame_t* f, f32 natural_w )
{
    if ( f->row >= f->lay_nrows ) f->row = f->lay_nrows - 1;   /* clamp overflow to the last row */

    u32 c = f->col, rr = f->row;

    /* A fresh row's nav line is dispensed HERE, on its first cell, not at the row advance below:
       widget_next_rect_w latches the stamp after this returns, so an advance-time dispense would
       hand the LAST cell of each row the next row's line -- walling Left/Right one short of the
       edge.  Row 0's line comes from layout_set_grid at install. */
    if ( c == 0 && rr > 0 )
        f->nav_line = ++s_build.nav_line_seq;

    gui_rect_t r  = cell_fit_resolve( f, f->cellx[ c ], f->cellw[ c ], natural_w, f->rowy[ rr ], f->rowh[ rr ] );

    if ( ++f->col >= f->lay_ncols )   /* next slot, row-major */
    {
        f->col = 0;
        ++f->row;
    }
    return r;
}

/* Place one item at the running pen on the open line -- the shared print-run placement behind
   pack mode (bar / strip) and a same_line continuation in flow.  The widget's natural size feeds
   the main axis (width along a bar or a continued row, height down a strip); the cross axis takes
   its natural extent, or fills the column when it has none.  A pending pack_size overrides the
   main extent, resolved by unit_resolve against the space left on the line, and is consumed
   (back to natural) after one item.  line_ext grows by running max, and content_y is carried live
   at the content end so queries and the commit both see the true extent. */
static gui_rect_t
line_place_pen( layout_frame_t* f, f32 natural_w, f32 h )
{
    bool horiz = ( f->mode != GUI_MODE_PACK ) || ( f->pack_dir == GUI_PACK_HORIZONTAL );

    /* Nav line: a placement onto a closed line opens a new one; a strip (vertical pack) runs its
       items down the cross axis, so to the keyboard each item is its own line -- Up/Down step
       them -- while a bar / same_line continuation keeps every item on the one it reopened.
       A pinned line (a table cell: the host stamped the whole row's) is never re-dispensed. */
    if ( !f->nav_line_pin && ( !f->line_open || !horiz ) )
        f->nav_line = ++s_build.nav_line_seq;

    f->line_open = true;   /* self-heal: a pen placement always continues the current line */

    /* Natural extents per axis from the widget's preferred size.  A fill widget (no natural width,
       natural_w <= 0) has no main extent of its own: it defaults to filling the rest of the line. */
    f32 nat_main  = horiz ? ( natural_w > 0.0f ? natural_w : 0.0f ) : h;
    f32 cross_ext = horiz ? h : ( natural_w > 0.0f ? natural_w : f->content_w );

    f32 main_avail = ( horiz ? ( f->content_x + f->content_w ) : f->content_y_max ) - f->line_main;
    if ( main_avail < 0.0f ) main_avail = 0.0f;

    f32 main_ext      = unit_resolve( f->pack_size_next, nat_main, main_avail );
    f->pack_size_next = -1.0f;   /* consume -> next item is natural */

    /* Assemble the rect by mapping (main, cross) onto (x, y): a bar runs main along x, a strip along
       y.  The pen advance, the line's cross-extent max, and the watermark grow are axis-agnostic
       once the rect is placed -- content_reach drops the pen and grows the highwater from its corner. */
    gui_rect_t r = horiz ? ( gui_rect_t ){ f->line_main, f->line_cross, main_ext, cross_ext }
                         : ( gui_rect_t ){ f->line_cross, f->line_main, cross_ext, main_ext };

    f->line_main += main_ext + ( horiz ? f->lay_gap_x : f->lay_gap_y );
    if ( cross_ext > f->line_ext ) f->line_ext = cross_ext;

    content_reach( f, r.x + r.w, r.y + r.h );
    f->prev_item = r;
    return r;
}

/* Place one item in the next template cell -- the flow placement.  At a row start (col 0) the
   line opens at the pen (gap-before): an auto-height row (row_h == 0) takes the *first* item's h
   as the whole row's height and every cell conforms -- a running max would retroactively misalign
   cells already handed out -- while a fixed row_h overrides it.  Fit-then-align (cell_fit_resolve)
   decides the cell box the same way in STACK and COLUMNS alike: a widget with a natural width
   (button, checkbox, text) shrinks and seats by lay_align, one with none (slider, input) fills the
   track -- the mode only ever chose the track's *width*, never whether an item fills it.  Wrapping
   past the last column commits the line.  line_main is kept at the pen past each cell, so a
   same_line continuation starts exactly where this cell ended. */
static gui_rect_t
line_place_cell( layout_frame_t* f, f32 natural_w, f32 h )
{
    if ( f->col == 0 )
    {
        line_commit( f );                       /* close a reopened same_line row, if any */
        f->line_cross = layout_next_y( f );     /* the gap owed above is applied here */
        f->line_ext   = ( f->lay_row_h > 0.0f ) ? f->lay_row_h : h;
        f->line_open  = true;
        if ( !f->nav_line_pin )                     /* a fresh flow row is a fresh nav line -- */
            f->nav_line = ++s_build.nav_line_seq;   /* unless a table pinned the row's        */
    }

    u32        c = f->col;
    gui_rect_t r = cell_fit_resolve( f, f->cellx[ c ], f->cellw[ c ], natural_w, f->line_cross, f->line_ext );

    f->line_main = r.x + r.w + f->lay_gap_x;    /* pen past the cell -- the same_line handoff */
    content_reach( f, r.x + r.w, r.y + r.h );   /* pen + highwater to the cell's far corner */

    if ( ++f->col >= f->lay_ncols )
        line_commit( f );                        /* row full -> fold it; col back to 0 */

    f->prev_item = r;
    return r;
}

/* Width-aware form.  `natural_w` is the widget's preferred width.  In stack mode a widget that
   carries one (natural_w > 0) shrinks to it instead of filling the cell -- matching Dear ImGui's
   behavior where buttons size to their label while field widgets (slider, input) fill the row.
   In columns / grid mode the track cell always wins.  Every emit records f->prev_item so
   same_line() can anchor the next widget to this one's line.  This is the per-mode dispatch over
   the line machinery above; the widget just fills the rect it is handed. */
static gui_rect_t
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
    if ( f->mode == GUI_MODE_NONE )
    {
        ORB_ASSERT( f->mode != GUI_MODE_NONE );   /* declare a mode (stack/columns/grid/...) first */
        layout_set_default( f );                    /* release fallback: behave as a plain stack */
    }

    gui_rect_t r;
    if ( f->mode == GUI_MODE_PACK )
    {
        r = line_place_pen( f, natural_w, h );     /* print run along pack_dir */
    }
    else if ( f->lay_nrows > 0 )
    {
        r = grid_next_rect( f, natural_w );         /* fixed matrix walk */
        f->prev_item = r;
    }
    else if ( f->cont_pending )
    {
        /* same_line continuation: one pen placement on the reopened line; the next plain widget
           starts a fresh row below it (cell placement at col 0 commits the line first). */
        f->cont_pending = false;
        r = line_place_pen( f, natural_w, h );
        f->col = 0;
    }
    else
    {
        r = line_place_cell( f, natural_w, h );    /* the next template cell */
    }

    /* Latch the item's structural nav coordinate for widget_behavior: which region and line the
       cell belongs to.  Latched (not one-shot) on purpose -- a widget that interacts in several
       parts from one cell (a numeric's sub-fields) lists each part as a same-line sibling.
       item_flags_chrome_reset drops it at the chrome seams. */
    s_build.nav_item_region = f->nav_region;
    s_build.nav_item_line   = f->nav_line;
    s_build.nav_item_placed = true;

    DBG_LAYOUT( r );
    return r;
}

/* The common case: fill the track cell (natural_w < 0 => no same_line preference). */
static gui_rect_t widget_next_rect( f32 h ) { return widget_next_rect_w( -1.0f, h ); }

/* Resolve a labeled widget's cell into a label position + a control rect when a field split is
   active (field_split / field_label_*).  The label and control are two tracks laid across the cell
   by the same resolver columns use, so a field split obeys the overloaded unit rule and adapts to
   whatever width the widget is handed -- a full row or a single column cell.  `side` flips which
   track sits left.  Draws nothing; the caller places its label + control from the outputs.  The
   control is floored at min_control_w so it stays usable (overrunning under the label, as before).
   Returns false when no field split is set, leaving the caller on its default layout. */
static bool
field_split_resolve( gui_rect_t cell, f32 min_control_w, f32* out_label_x, f32* out_label_w,
                     gui_rect_t* out_control )
{
    layout_frame_t* f = lf();
    if ( f->lay_field_side == 0 ) return false;

    /* Order the two tracks by side so the resolver lays them left-to-right correctly. */
    f32 tracks[ 2 ];
    u32 lab_i, ctl_i;
    if ( f->lay_field_side == GUI_LABEL_LEFT )
    {
        tracks[ 0 ] = f->lay_field_label;   tracks[ 1 ] = f->lay_field_control;   lab_i = 0; ctl_i = 1;
    }
    else /* GUI_LABEL_RIGHT */
    {
        tracks[ 0 ] = f->lay_field_control; tracks[ 1 ] = f->lay_field_label;     ctl_i = 0; lab_i = 1;
    }

    f32 pos[ 2 ], size[ 2 ];
    layout_resolve_tracks( tracks, 2, cell.x, cell.w, WIDGET_PAD, pos, size );

    f32 control_w = size[ ctl_i ];
    if ( control_w < min_control_w ) control_w = min_control_w;

    *out_label_x = pos[ lab_i ];
    *out_label_w = size[ lab_i ];
    *out_control = ( gui_rect_t ){ pos[ ctl_i ], cell.y, control_w, cell.h };
    return true;
}

/* Split a labeled widget row into a control rect and its label.  In the default (trailing-label)
   mode the label keeps its natural width pinned at the row's right edge and the control takes the
   rest, never shrinking below min_control_w so it stays usable when the label is long (the control
   then overruns under the label, matching the prior per-widget behavior); in field-split mode
   (field_split_resolve) the label and control are two resolved tracks.  Draws the label here,
   vertically centered in the given color, and returns the control rect for the caller to interact
   with and fill.  The single seam every "control + trailing label" widget (slider_float,
   input_text, combo, drag_float, color_edit) routes through, so row proportions retune in one place. */
static gui_rect_t
widget_split_label( gui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    /* Field split mode: the label sits in its track at full strength (the trailing-label dim hint,
       label_color, does not apply -- a field label reads as primary); the control fills the rest.
       The label is fitted to its track width so a narrow (fraction-shrunk) track ellipsizes it. */
    f32          label_x, label_w;
    gui_rect_t control;
    if ( field_split_resolve( row, min_control_w, &label_x, &label_w, &control ) )
    {
        draw_label_fit( label_x, text_center_y( row.y, row.h ), COL_TEXT, label, label_w );
        return control;
    }

    /* Default: control on the left, the label trailing at its natural width on the right.  When the
       control floors at min_control_w the label space narrows; fit it so it ellipsizes there
       instead of bleeding under the row's right edge.  No visible label ("##key") => full row. */
    label_w = label_width( label );
    if ( label_w == 0.0f ) return row;
    f32 control_w = row.w - label_w - WIDGET_PAD;
    if ( control_w < min_control_w ) control_w = min_control_w;

    control = ( gui_rect_t ){ row.x, row.y, control_w, row.h };

    f32 trail_x = control.x + control.w + WIDGET_PAD;
    draw_label_fit( trail_x, text_center_y( row.y, row.h ), label_color, label,
                    ( row.x + row.w ) - trail_x );
    return control;
}

// clang-format on
/*============================================================================================*/
