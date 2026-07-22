/*==============================================================================================

    runtime_service/gui/flow/gui_layout_core.c -- Layout mechanism: track resolver + cell emitters.

    The engine the public layout API (gui_layout.c) drives.  It carves a region's content area
    into cells from a repeating row / column template (or a fixed grid, or a pack run) and hands
    the next cell to each widget, hiding the layout shape from the widgets entirely.  Two halves:

        - track resolver + template installers (the overloaded-unit math in unit_resolve /
          layout_resolve_tracks, the layout_template_reset / layout_modifiers_reset seams,
          layout_seed_content, and layout_set / _grid / _reflow / _clear) -- the "what shape
          is this region" mechanism;
        - cell emitters (cell_next_w, grid_next_rect, pack_next_rect,
          cell_split_field) -- the per-item "hand out the next rect" mechanism.

    This tier composes and never paints: cell_split_field hands out geometry, and its painting
    companion draw_field_label lives with the rest of the label grammar in
    element/gui_adornment.c.  The METRICS vocabulary (WIDGET_H / WIDGET_PAD / ...) resolves
    in style/gui_style_core.c.

    Part of the flow unit (gui_flow.c); rect_align and item_flags_resolve resolve cross-unit (in
    scope) and before gui_layout.c (which calls layout_set / cell_next / the region
    helpers).

==============================================================================================*/
// clang-format off

/* REGION_PAD_DEFAULT (the inset every window body / child opens with) lives in flow/gui_flow.h:
   window chrome opens its body region with it. */

/*==============================================================================================
    Layout-frame stack (here -- the frames are composition's records)

    Every scrollable region (a window body or a child_begin box) pushes one frame; the top frame
    owns the layout pen and the content column leaf widgets emit into.  The rest of the frame is the
    resolve context layout_pop_region needs to measure content and draw the region's scrollbars.
    Storage is just the fixed array, so a deep nesting costs nothing beyond these slots.
==============================================================================================*/

layout_frame_t s_layout_stack[ GUI_LAYOUT_DEPTH ];
u32            s_layout_sp;   // active frame count; top = s_layout_sp - 1

/* Top layout frame.  Valid between a window_begin/child_begin and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 is returned instead of indexing out of bounds -- the stray widget draws into whatever
   the last frame's root region left there rather than crashing.  The read index is also clamped
   to the top slot so an over-deep nesting (capped in layout_push_region) never reads past the
   array. */

layout_frame_t*
lf( void )
{
    u32 i = s_layout_sp ? s_layout_sp - 1 : 0;
    if ( i >= GUI_LAYOUT_DEPTH ) i = GUI_LAYOUT_DEPTH - 1;
    return &s_layout_stack[ i ];
}

/*==============================================================================================

    Layout cursor helpers

==============================================================================================*/

/* mod.gap_x / mod.gap_y store the caller's raw request (0 = theme default), not a resolved
   number -- resolved live here, at the moment a gap is actually consumed, the same way tmpl.row_h
   (0 = auto) resolves live against WIDGET_H per row.  This is what lets a scale_push placed after
   a region has opened (e.g. inside a combo dropdown body, which combo_begin opens internally)
   still land: an eager resolve at layout_set() time would freeze WIDGET_GAP before the caller ever
   gets a chance to push a different scale. */

static f32 mod_gap_x( const layout_frame_t* f ) { return ( f->mod.gap_x > 0.0f ) ? f->mod.gap_x : WIDGET_GAP; }
static f32 mod_gap_y( const layout_frame_t* f ) { return ( f->mod.gap_y > 0.0f ) ? f->mod.gap_y : WIDGET_GAP; }

/* Grow the region's highwater (high_x, high_y) to include a content corner (x, y) in
   screen coords: the monotonic bounding-box max layout_pop_region cancels the scroll bias out of and
   compares against each view to decide a scrollbar.  The highwater only ever climbs, so a running
   max over every item's far corner reconstructs a line's full extent -- one call per placement, on
   either axis, in place of the per-axis, per-mode inline updates the emitters used to do.  Does not
   touch the pen; content_reach moves both, cell_reach grows the x highwater alone. */

void
extent_track( layout_frame_t* f, f32 x, f32 y )
{
    if ( x > f->high_x ) f->high_x = x;
    if ( y > f->high_y ) f->high_y = y;
}

/* The x-only face of extent_track, for a leaf widget reporting it drew out to right_x -- wider than
   the cell it was handed (text to its glyphs, a label past the row edge).  The widget knows only its
   horizontal overflow; its vertical extent is the emitter's business.  Grows the x highwater alone,
   so the horizontal bar sees content the cell did not bound.  Every leaf-widget overflow site here. */

void
cell_reach( f32 right_x )
{
    if ( right_x > lf()->high_x ) lf()->high_x = right_x;
}

/* A forward flow step: content now reaches corner (x, y), so drop the pen to it and lift the
   highwater with it.  The shared advance behind every placement and block emit (a cell, a packed
   item, a popped child box).  The pen and highwater move together here -- only a pen reposition
   (layout_pen_jump) parts them.  pen_y only ever climbs through this seam, so max == the drop. */

static void
content_reach( layout_frame_t* f, f32 x, f32 y )
{
    if ( y > f->pen_y ) f->pen_y = y;   /* pen drops to the content end */
    extent_track( f, x, y );                    /* highwater climbs with it     */
}

/*==============================================================================================

    Grid lattice -- the theme's grid_quantum (gui_style_t; 0/1 = off) snaps CONTENT-DRIVEN sizes
    so resolved flex tracks and natural widths land on the same px lattice the theme metrics
    were already quantized to (layout_compute).  Authored sizes are never snapped: a fixed-px
    track, an explicit row_h, a line.fit_next / pack_size are taken verbatim -- unit-first authoring
    goes through gui_sz_u(), which is on-lattice by construction.  Content sizes round UP (text is
    never clipped); divided space rounds DOWN (tracks never overflow their extent).

    The snapping arithmetic itself lives in the lat_* primitives (gui_theme.c, behind the
    GUI_GRID_LATTICE compile switch).  The wrappers below just bind the pitch to the live
    grid_quantum so call sites stay terse (quant_floor_min( v ) not lat_floor( v, q )).

==============================================================================================*/

/* Content size floored onto the live style's lattice, never below one quantum so a live size
   never collapses.  Thin wrapper over the lat_* primitives (gui_theme.c) that binds the pitch to
   the active grid_quantum -- the lattice arithmetic and the GUI_GRID_LATTICE gate live there. */
static f32
quant_floor_min( f32 v )
{
    return lat_floor_min( v, s_style.grid_quantum );
}

/* Largest lattice multiple <= v (0 allowed -- no one-quantum floor).  For snapping a CUMULATIVE
   track edge onto the grid: sizes are taken as the difference of consecutive snapped edges, so the
   per-edge value must be free to be exactly the running total's lattice point (including 0 for the
   first edge) or the differences drift.  quant_floor_min's collapse guard belongs on a standalone size,
   not on an edge. */
static f32
quant_floor( f32 v )
{
    return lat_floor( v, s_style.grid_quantum );
}

/* Smallest lattice multiple >= v. */
static f32
quant_ceil( f32 v )
{
    return lat_ceil( v, s_style.grid_quantum );
}

/*==============================================================================================

    Natural (== 0) column tracks -- resolved from MEASURED FEEDBACK.

    The engine is single-pass, so a pre-divided track has no content in hand at resolve time.
    Instead each natural column remembers the widest natural_w placed into it, and the NEXT
    frame's install resolves the track to that measure -- the same one-frame lag the region
    content extent and autosize windows already run on.  Slots live in the keyed state pool,
    keyed per (region, template install ordinal, column): re-installing the same shape later in
    the region measures independently, and a vanished template's slots age out on their own.
    The resolve floors at WIDGET_MIN_W so a first appearance (nothing measured yet) opens
    usable instead of collapsed.  Flow columns and grid COLUMNS feed back; grid ROWS have no
    vertical natural signal and still collapse (see gui_layout_t).

==============================================================================================*/

typedef struct
{
    f32 stable;   // last completed frame's max -- what an install resolves against
    f32 accum;    // running max of this frame's placements
    u32 frame;    // retained-frame stamp of accum (rolls the pair once per frame)

} nat_track_t;

/* The column's slot, rolled to the current frame (stable <- last frame's accum, once).  A
   transient sub-layout has no region id; its frame-order nav_region stands in -- stable while
   the UI is stable, and a mis-key after a structural change just re-measures for a frame. */
static nat_track_t*
nat_track_touch( layout_frame_t* f, u32 col )
{
    gui_id_t base = ( f->region_id != GUI_ID_NONE ) ? f->region_id
                                                    : id_combine( 0x4E415455u, f->nav_region );
    nat_track_t* t = GUI_STATE( nat_track_t, id_combine( base, ( f->tmpl.seq << 3 ) | col ) );

    u32 now = gui_frame_index();
    if ( t->frame != now ) { t->stable = t->accum; t->accum = 0.0f; t->frame = now; }
    return t;
}

/* Fold one placed item's natural width into its column's running measure (flow + grid cells).
   A fill widget (no natural width) contributes nothing -- the column sizes to the items that
   do carry one. */
static void
nat_track_note( layout_frame_t* f, u32 col, f32 natural_w )
{
    if ( !( ( f->tmpl.nat_mask >> col ) & 1u ) || natural_w <= 0.0f ) return;
    nat_track_t* t = nat_track_touch( f, col );
    f32 w = quant_ceil( natural_w );
    if ( w > t->accum ) t->accum = w;
}

/* Substitute every natural (== 0) track in `tracks` with last frame's measure (floored at the
   minimum cell width) and record which columns feed the measure back.  Runs at install, before
   the resolve, so layout_resolve_tracks and indent's reflow only ever see px for these tracks. */
static void
nat_tracks_substitute( layout_frame_t* f, f32* tracks, u32 n )
{
    f->tmpl.seq      = ++f->tmpl_seq;
    f->tmpl.nat_mask = 0;
    for ( u32 i = 0; i < n; ++i )
    {
        if ( tracks[ i ] != 0.0f ) continue;
        f->tmpl.nat_mask |= (u8)( 1u << i );
        f32 m  = nat_track_touch( f, i )->stable;
        f32 mn = WIDGET_MIN_W;
        tracks[ i ] = ( m > mn ) ? m : mn;
    }
}

/*==============================================================================================

    Layout engine -- carve a region's content area into cells from a repeating row template.

    One resolver does both axes (here, only columns are wired); the row template lives on the
    layout frame and persists until changed, so widgets emit cell-by-cell while staying wholly
    agnostic to the layout shape.  See gui_layout_t (gui.h) for the overloaded unit rule.

==============================================================================================*/

/* Resolve a track list into pixel [pos,size] pairs along one axis.  `n` tracks (>= 1) are laid
   from `origin` across `extent`, with `gap` between each.  Units (see gui_layout_t): >1 fixed px,
   ==1 fill (equal share of the leftover -- several fills split it), (0,1) fraction of the gap-
   adjusted extent, ==0 natural.  This is a pre-divide (up-front) resolve with no content in hand,
   so a 0/natural track collapses to zero width HERE -- the column installers substitute measured
   px for natural tracks before resolving (nat_tracks_substitute above), so a 0 only reaches this
   function from the callers with no measure (field_split, split, carve), where it collapses.
   Gaps are removed before the split, so a fraction is of usable space and cells tile exactly.

   Minimum width: a fill / fraction track that the available space squeezes below WIDGET_MIN_W is
   floored there and the row simply overflows -- it stops shrinking at a still-usable size and the
   surplus cells push out past the content edge, where the region clip cuts them (no auto-scroll;
   the scroll flags stay the author's choice).  Fixed-px tracks are never floored: an explicit
   pixel width is taken as intent, even when small.  All fills share one floor and hit it together,
   so a flat post-clamp matches a freeze-and-redistribute here; per-track minimums would need the
   iterative form. */

void
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

    /* Pass 2: place left-to-right, gap between cells (not before the first / after the last).

       Grid snapping is done on CUMULATIVE EDGES, not per-track sizes.  Each flex / fraction track's
       far edge (its running position along avail) is floored onto the lattice, and the track's size
       is the difference from the previous snapped edge.  This is why row track-counts stay
       consistent: the last flex edge is quant_floor( its ideal cumulative ) regardless of how many
       tracks preceded it, so a two-column 0.5/0.5 row reaches the SAME right edge as a single full
       -width flex over the same avail -- the rounding loss is one sub-quantum remainder at the row
       end, never one-per-track.  (Per-size flooring, the old form, floored each track independently:
       0.5/0.5 could lose up to two quanta and the loss jumped around as a drag moved 0.5*avail
       across a lattice line, so the halves snapped at different widths than full rows above them.)

       ideal_cum tracks the unsnapped running edge; snap_cum the snapped one.  A fixed-px / natural
       track advances both by its exact size (authored intent, never snapped); a flex / fraction
       track advances ideal_cum by its share, then snaps the edge and takes the delta. */

    f32 pos      = origin;
    f32 ideal_cum = 0.0f;   /* unsnapped running edge along avail (content only, gaps excluded) */
    f32 snap_cum  = 0.0f;   /* lattice-snapped running edge */

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

        if ( is_flex_or_fraction_track )
        {
            ideal_cum += sz;
            f32 edge   = quant_floor( ideal_cum );   /* snap the cumulative edge, not the size */
            sz         = edge - snap_cum;
            if ( sz < 0.0f ) sz = 0.0f;
            snap_cum   = edge;
        }
        else
        {
            ideal_cum += sz;   /* fixed / natural: exact, and it carries the running edge forward */
            snap_cum  += sz;
        }

        out_pos [ i ] = pos;
        out_size[ i ] = sz;
        pos += sz + gap;
    }
}

/*============================================================================================*/
/* Resolve one overloaded size unit against a single span -- the scalar form of the rule
   layout_resolve_tracks applies to a whole list (see gui_layout_t): > 1 fixed px, == 1 fill the
   available extent, (0,1) a fraction of it, == 0 the natural measure.  < 0 (unset) is natural
   with a fill fallback for an item that has no natural measure -- the pack-mode default.  The
   single-value sites (line.pack_size_next, the one-shot cell line.fit_next) resolve through here, so the
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

/*============================================================================================*/
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

/*============================================================================================*/
/* Close the open line and return the column walk to a row start: the next line owes a gap before it
   (gap_pending) rather than one appended after, so pen_y stays the exact content end.  The one
   commit behind flow rows, pack lines, and same_line continuations.  The line's extent is already in
   the watermark -- every item grew it from its far corner via extent_track as it was placed -- so
   there is nothing to fold here.  An empty line (nothing emitted) owes nothing.  No-op when closed. */

static void
line_commit( layout_frame_t* f )
{
    f->line.col = 0;
    if ( !f->line.open ) return;
    f->line.open = false;
    if ( f->line.ext <= 0.0f ) return;   /* nothing emitted -- owe nothing */
    f->gap_pending = true;
}

/*============================================================================================*/
/* True for a line that gui_pack just opened and nothing has been placed on yet (line.ext, the
   cross extent, only grows once an item lands).  Its own position -- not the cursor plus a gap --
   is the next placement point, since the gap was already applied when the line opened. */

static bool
line_just_opened( const layout_frame_t* f )
{
    return f->line.open && f->line.ext <= 0.0f;
}

/*============================================================================================*/
/* Where the next line -- or block placed at the pen: a child box, a split band, a grid band --
   opens on the cross axis: the content end plus the gap owed by the content above it.  An open
   line owes one too (pen_y already carries its live extent); a fresh, still-empty pack line
   is its own next position (see line_just_opened). */

f32
layout_next_y( layout_frame_t* f )
{
    if ( line_just_opened( f ) )
    {
        bool vert = ( f->mode == GUI_MODE_PACK && f->line.pack_dir == GUI_PACK_VERTICAL );
        return vert ? f->line.main : f->line.cross;
    }
    if ( f->line.open || f->gap_pending )
        return f->pen_y + mod_gap_y( f );
    return f->pen_y;
}

/*============================================================================================*/
/* Break a pack run to a fresh line: commit the one just laid and open the next past it, with the
   main pen back at the line start.  An empty line still advances by one gap (a deliberate blank
   line).  The shared body of gui_pack_nextline (gui_layout.c) and the opt-in auto-wrap in
   line_place_pen below. */

static void
pack_line_break( layout_frame_t* f )
{
    bool horiz     = ( f->line.pack_dir == GUI_PACK_HORIZONTAL );
    f32  gap       = horiz ? mod_gap_y( f ) : mod_gap_x( f );
    f32  new_cross = f->line.cross + f->line.ext + gap;   /* past the line just laid */

    line_commit( f );              /* close the line -- its extent is already in the highwater */

    f->line.cross = new_cross;
    f->line.main  = f->line.origin;
    f->line.ext   = 0.0f;
    f->line.open  = true;
    f->nav_line   = ++s_build.nav_line_seq;   /* the broken-to line is a fresh nav line */
    f->line.prev_item = ( gui_rect_t ){ 0 };
}

/*============================================================================================*/
/* Reposition the pen to an explicit y -- an imperative host taking authority over the flow (a table
   stepping row to row, a menu bar restoring the pen it borrowed).  The pen is authoritative: no line
   is open and no gap is owed, so the next line opens exactly here.  This moves the PEN alone: the
   highwater is lifted up-only, so a forward jump (a table row extending the body) is still measured,
   while a backward restore (a menu bar handing the pen back) does not rewind the content the region
   already reached. */

void
layout_pen_jump( layout_frame_t* f, f32 y )
{
    f->pen_y   = y;
    if ( y > f->high_y ) f->high_y = y;   /* highwater climbs, never rewinds */
    f->line.col         = 0;
    f->line.open   = false;
    f->gap_pending = false;
}

/*============================================================================================*/
/* Finish the active template's open geometry before it is replaced or a block is placed at the
   pen: commit the open line -- or, leaving a grid, surrender the band.  A grid owns everything
   from its top to the region's content bottom, so once any cell is emitted the pen lands at the
   band bottom (band_bottom); an untouched grid gives the band back.  Safe in any mode. */

void
layout_row_break( layout_frame_t* f )
{
    if ( f->mode == GUI_MODE_GRID )
    {
        if ( f->line.col > 0 || f->line.row > 0 )   /* any cell emitted -> the band is consumed */
        {
            content_reach( f, f->content_x, f->band_bottom );   /* pen + highwater to the band bottom */
            f->gap_pending = true;
        }
        f->line.col = 0;
        f->line.row = 0;
        return;
    }
    line_commit( f );
}

/*============================================================================================*/
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

/*============================================================================================*/
/* Reset the per-template iteration state -- everything a new template must not inherit from the
   shape it replaces: the whole line record (column/row walk, same_line anchor, pack pen, the
   one-shot overrides back to unset).  Every installer (layout_set / _grid / _default, gui_pack)
   runs this after breaking the open row, so "what a fresh template starts from" has exactly one
   answer.  Gaps, field split, and align are modifiers: they persist across installs and only the
   full clears (layout_clear / layout_set_default) reset them via layout_modifiers_reset below. */

static void
layout_template_reset( layout_frame_t* f )
{
    f->tmpl.nrows = 0;   /* flow until a grid installs rows */
    f->line       = ( layout_line_t ){ .pack_size_next = -1.0f,    /* unset -> natural */
                                       .fit_next       = -1.0f,    /* unset -> own natural_w */
                                       .h_next         = -1.0f };  /* unset -> caller's h */
    /* gap_pending is NOT reset: content committed above still owes its gap to the next line. */
}

/*============================================================================================*/
/* push_layout_state / pop_layout_state -- save and restore the region's declared shape (mode +
   template + modifiers) around a scoped header change, so a helper that switches the active
   region into bar() / grid() / whatever for its own widgets can hand the caller's shape back
   verbatim on the way out, instead of requiring the caller to remember and re-declare it (or the
   helper to guess it -- there is no way to guess a caller's columns back).  Small fixed depth
   like the font / id stacks -- these are coarse scope brackets, not deeply nested.

       gui()->push_layout_state();
           gui()->bar();
           gui()->button( "Save" );  gui()->button( "Open" );
       gui()->pop_layout_state();       // caller's stack() / grid() / cols() ... is back

   Line iteration (layout_line_t) is NOT part of the snapshot: layout_row_break folds the open
   line into pen_y before the save (so a partial row the caller had open is committed, not lost)
   and again before the restore (so whatever the scoped shape emitted is committed too); the
   restored shape then starts a fresh line at the pen, same as any header install. */

#define GUI_LAYOUT_STATE_STACK_MAX 8

typedef struct
{
    gui_layout_mode_t  mode;
    layout_tmpl_t      tmpl;
    layout_mod_t       mod;

} layout_state_t;

static layout_state_t s_layout_state_stack[ GUI_LAYOUT_STATE_STACK_MAX ];
static u32             s_layout_state_stack_depth = 0;

void
gui_push_layout_state( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );
    if ( s_layout_state_stack_depth < GUI_LAYOUT_STATE_STACK_MAX ) {
         s_layout_state_stack[ s_layout_state_stack_depth++ ] = 
            ( layout_state_t ){ f->mode, f->tmpl, f->mod };
    }
}

void
gui_pop_layout_state( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );
    if ( s_layout_state_stack_depth == 0 )
        return;
    layout_state_t s = s_layout_state_stack[ --s_layout_state_stack_depth ];
    f->mode = s.mode;
    f->tmpl = s.tmpl;
    f->mod  = s.mod;
    layout_template_reset( f );   /* fresh iteration cursor for the restored shape */
}

/*============================================================================================*/
/* Reset the orthogonal modifiers: gaps back to the theme, align to LEFT | TOP, and the field split
   back to the style default -- FIELD_LABEL_W > 0 seeds a fixed left label column for every region so
   forms align window-wide with no per-block call; 0 leaves it off (each label trails at its own
   width).  A local field_split / field_label_* still overrides within its region. */

static void
layout_modifiers_reset( layout_frame_t* f )
{
    f32 lw = FIELD_LABEL_W;
    f->mod = ( layout_mod_t ){ .gap_x = WIDGET_GAP, .gap_y = WIDGET_GAP,
                               .field_side    = ( lw > 0.0f ) ? (u8)GUI_LABEL_LEFT : 0u,
                               .field_label   = lw,
                               .field_control = 1.0f };
}

/*============================================================================================*/
/* Open a region UNDECLARED: zero the template state and leave the mode NONE so the first layout
   header (stack / columns / grid / ...) installs real geometry.  A widget emitted before any
   header trips the guard in cell_next_w.  Called when a region or sub-layout opens (the
   old silent single-column default is gone) and by gui_pad after re-insetting -- the modifiers
   and iteration state start from a known state every region. */

static void
layout_clear( layout_frame_t* f )
{
    f->mode         = GUI_MODE_NONE;
    f->tmpl.ncols   = 0;            /* no template -- first header resolves one */
    f->tmpl.row_h   = 0.0f;
    f->gap_pending  = false;        /* fresh region: the first line opens flush at the pen */
    f->nav_line_pin = false;        /* fresh content: nav lines dispense normally again */
    layout_modifiers_reset( f );
    layout_template_reset( f );
}

/*============================================================================================*/
/* Install the region's default template: one flex column, auto height -- the classic stack, mode
   STACK.  Resolves immediately (content_x/content_w must already be set), so the single column
   fills the content width with no gap.  This is the full reset (clears field split + align too):
   it backs gui_layout_default and the emit-before-header guard's release fallback.  The plain
   stack() header keeps modifiers and routes through layout_set instead. */

void
layout_set_default( layout_frame_t* f )
{
    f->mode            = GUI_MODE_STACK;
    f->tmpl.ncols      = 1;
    f->tmpl.row_h      = 0.0f;
    f->tmpl.nat_mask   = 0;               /* one flex track -- nothing measures back */
    f->tmpl.cols[ 0 ]  = 1.0f;            /* single flex track, kept so indent can re-resolve */
    f->tmpl.cellx[ 0 ] = f->content_x;    /* one flex column == the whole content width */
    f->tmpl.cellw[ 0 ] = f->content_w;
    layout_modifiers_reset( f );
    layout_template_reset( f );
}

/*============================================================================================*/
/* Seed the frame's content column + pen from its outer box and a pad inset, leaving the template
   UNDECLARED.  The one derivation of the content geometry: layout_push_region (region open),
   sublayout_open (transient cell frame), and gui_pad (re-inset) all route through here.  Requires
   outer, scroll, and the resolved view rect (f->view) already set on the frame.  The live pen
   is biased by -scroll so widgets slide under the clip, while origin_* stays unscrolled so the
   content extent measures cleanly at pop; band_bottom is the grid band end / view bottom. */
static void
layout_seed_content( layout_frame_t* f, gui_pad_t pad )
{
    /* The content band's far edges derive from f->view (set before this runs), never from the
       raw outer rect: the view already excludes the border and the reserved gutters, so a
       widget that fills the remaining space (canvas(0), a fill grid / pack) stops exactly at
       the true visible edge.  Deriving from outer here while the bars size against the view
       is how content used to measure a hairline past the view every frame -- a permanent
       phantom scroll fragment with nothing to scroll to. */

    f->pad           = pad;   /* kept: the pads join the measured canvas at pop */
    f->origin_x      = f->outer.x + pad.l;
    f->origin_y      = f->outer.y + pad.t;
    f->content_x     = f->origin_x - f->scroll->scroll_x;
    f->pen_y     = f->origin_y - f->scroll->scroll_y;

    /* Bottom anchor (GUI_WIN_ANCHOR_BOTTOM): when last frame's content underfills the view, drop the
       whole block by the slack so the last (newest) row hugs the view bottom and the empty space
       falls at the TOP -- the flush-at-bottom, overflow-at-top flow a console wants, with no per-row
       pen math.  Overflow (bias <= 0) leaves the block top-anchored; the scroll offset (pinned to the
       tail in layout_push_region) shows the bottom instead.  The bias is a PRESENTATION shift only --
       layout_pop_region subtracts anchor_bias back out of the measure so content_h stays the true
       content height and the scroll range / autosize read honestly.  Uses last frame's content_h, the
       same one-frame lag every measured-feedback path here already runs on. */
    f->anchor_bias = 0.0f;
    if ( f->flags & GUI_WIN_ANCHOR_BOTTOM )
    {
        f32 interior_h = f->view.h - pad.t - pad.b;
        f32 items_h    = ( f->scroll->content_h > 0.0f ) ? f->scroll->content_h - pad.t - pad.b : 0.0f;
        f32 bias       = interior_h - items_h;
        if ( bias > 0.0f ) { f->pen_y += bias; f->anchor_bias = bias; }
    }

    /* Track width: the view width, widened to last frame's measured content when that content ran
       wider.  scroll_clamp (layout_push_region) already lets scroll_x range across that extra width
       regardless of the HSCROLL flags -- a wheel or bar can reach it -- so a track pinned to the view
       would leave every fill-type widget (separator, slider, input) stopping short of ground the
       region can actually scroll to, and force any text longer than the view into a permanent,
       unreachable ellipsis no scroll offset ever uncovers.  Only a genuine last-frame overflow widens
       it, so a region that fits exactly still measures flush to the view (no phantom bar). */

    f32 view_content_w = f->view.w - pad.l - pad.r;
    f32 content_w       = view_content_w;
    f32 last_items_w    = ( f->scroll->content_w > 0.0f ) ? f->scroll->content_w - pad.l - pad.r : 0.0f;
    if ( last_items_w > content_w ) content_w = last_items_w;
    f->content_w = content_w;
    f->high_x = f->content_x;   /* seed the highwater at the origin corner -> an empty */
    f->high_y = f->pen_y;   /* body measures 0 on both axes (premeasure sentinel)  */
    f->band_bottom = f->view.y + f->view.h - pad.b;

    /* Fresh nav coordinate: this content column is one container to the keyboard (a window body,
       a child box, a re-inset pad).  The first line dispenses when the first line opens. */
    f->nav_region = ++s_build.nav_region_seq;
    f->nav_line   = 0;
    f->tmpl_seq   = 0;   /* install ordinals restart with the region (natural-track keys) */

    layout_clear( f );   /* content re-seeded -> the template opens undeclared; declare a header */
}

/*============================================================================================*/
/* volatile_layout_push / volatile_layout_pop -- a minimal layout-frame push/pop at an explicit
   (x, y, w), used only by the volatile-widget replay scope, now live in chrome/widgets/gui_volatile.c
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
    f->tmpl.row_h = row_h;
    f->mod.gap_x = gap_x;   /* raw request; 0 = live theme default, resolved by mod_gap_x/_y */
    f->mod.gap_y = gap_y;

    f32 tracks[ GUI_LAYOUT_COLS ];
    f->tmpl.ncols = layout_copy_tracks( cols, tracks );
    nat_tracks_substitute( f, tracks, f->tmpl.ncols );   /* natural (0) -> last frame's measure */
    for ( u32 i = 0; i < f->tmpl.ncols; ++i ) f->tmpl.cols[ i ] = tracks[ i ];   /* kept for indent reflow */
    layout_resolve_tracks( tracks, f->tmpl.ncols, f->content_x, f->content_w, mod_gap_x( f ),
                           f->tmpl.cellx, f->tmpl.cellw );
}

/*============================================================================================*/
/* Re-resolve a flow template's cells from the current content column -- used after indent /
   unindent shifts content_x / content_w so subsequent rows land at the new inset.  Flow only
   (STACK / COLUMNS); a grid carries a pre-resolved matrix and a pack its own pen, neither of which
   is re-indented mid-iteration, so they are left untouched. */
static void
layout_reflow( layout_frame_t* f )
{
    if ( f->mode == GUI_MODE_STACK || f->mode == GUI_MODE_COLUMNS )
        layout_resolve_tracks( f->tmpl.cols, f->tmpl.ncols, f->content_x, f->content_w,
                               mod_gap_x( f ), f->tmpl.cellx, f->tmpl.cellw );
}

/*============================================================================================*/
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
    f->mod.gap_x = gap_x;   /* raw request; 0 = live theme default, resolved by mod_gap_x/_y */
    f->mod.gap_y = gap_y;

    /* Resolve columns across the content column and rows across the band from the pen to the
       content bottom.  An empty band (content already overflowed) clamps to zero. */
    f32 tracks[ GUI_LAYOUT_COLS ];
    f->tmpl.ncols = layout_copy_tracks( cols, tracks );
    nat_tracks_substitute( f, tracks, f->tmpl.ncols );   /* columns measure back; rows cannot */
    layout_resolve_tracks( tracks, f->tmpl.ncols, f->content_x, f->content_w, mod_gap_x( f ),
                           f->tmpl.cellx, f->tmpl.cellw );

    f->tmpl.nrows = layout_copy_tracks( rows, tracks );
    f32 grid_top = layout_next_y( f );   /* gap-before: the band opens below prior content */
    f32 grid_h   = f->band_bottom - grid_top;
    if ( grid_h < 0.0f ) grid_h = 0.0f;
    layout_resolve_tracks( tracks, f->tmpl.nrows, grid_top, grid_h, mod_gap_y( f ),
                           f->tmpl.rowy, f->tmpl.rowh );

    f->nav_line = ++s_build.nav_line_seq;   /* grid row 0 opens as a fresh nav line */
}

/*============================================================================================*/
/* Resolve one cell's horizontal box -- the fit-then-align sequence every cell placement (flow or
   grid) shares: decide how big before align_x decides where.  A one-shot line.fit_next (overloaded
   unit -- see gui_layout_t) always wins when set, taken as authored intent even past the cell
   edge, exactly like a fixed track px is never floored (layout_resolve_tracks).  Unset (-1, the
   common case) falls back to the widget's own natural_w signal: a natural width smaller than the
   cell shrinks to it (a button hugs its label), anything else fills the cell verbatim -- the
   per-widget-type default every emit already carries, now consulted for every mode, not just
   STACK.  A shrunk box is seated in the leftover space by mod.align; a filled one starts flush at
   the cell's left edge (align has nothing to do once the box owns the whole cell). */
static gui_rect_t
cell_fit_resolve( layout_frame_t* f, f32 cell_x, f32 cell_w, f32 natural_w, f32 y, f32 h )
{
    natural_w = quant_ceil( natural_w );   /* grid: content-driven widths round up onto the lattice */

    f32 fit     = f->line.fit_next;
    f->line.fit_next = -1.0f;   /* one-shot: consumed whichever branch below reads it */

    f32 w = ( fit >= 0.0f )
          ? unit_resolve( fit, ( natural_w > 0.0f ) ? natural_w : cell_w, cell_w )
          : ( ( natural_w > 0.0f && natural_w < cell_w ) ? natural_w : cell_w );

    f32 x = ( w < cell_w ) ? align_x( cell_x, cell_w, w, f->mod.align ) : cell_x;
    return ( gui_rect_t ){ x, y, w, h };
}

/*============================================================================================*/
/* Cell a grid hands to a widget: a fixed (col,row) slot of the pre-resolved matrix, then advance
   row-major.  Past the last cell the cursor clamps to it, so overflow widgets stack harmlessly in
   the final slot rather than reading out of bounds.  Row height is the matrix's, not the item's --
   grid has no vertical natural-size concept, only horizontal fit within the column. */
static gui_rect_t
grid_next_rect( layout_frame_t* f, f32 natural_w )
{
    if ( f->line.row >= f->tmpl.nrows ) f->line.row = f->tmpl.nrows - 1;   /* clamp overflow to the last row */

    u32 c = f->line.col, rr = f->line.row;

    /* A fresh row's nav line is dispensed HERE, on its first cell, not at the row advance below:
       cell_next_w latches the stamp after this returns, so an advance-time dispense would
       hand the LAST cell of each row the next row's line -- walling Left/Right one short of the
       edge.  Row 0's line comes from layout_set_grid at install. */
    if ( c == 0 && rr > 0 )
        f->nav_line = ++s_build.nav_line_seq;

    nat_track_note( f, c, natural_w );   /* a natural column measures its widest item */

    gui_rect_t r  = cell_fit_resolve( f, f->tmpl.cellx[ c ], f->tmpl.cellw[ c ], natural_w, f->tmpl.rowy[ rr ], f->tmpl.rowh[ rr ] );

    if ( ++f->line.col >= f->tmpl.ncols )   /* next slot, row-major */
    {
        f->line.col = 0;
        ++f->line.row;
    }
    return r;
}

/*============================================================================================*/
/* Place one item at the running pen on the open line -- the shared print-run placement behind
   pack mode (bar / strip) and a same_line continuation in flow.  The widget's natural size feeds
   the main axis (width along a bar or a continued row, height down a strip); the cross axis takes
   its natural extent, or fills the column when it has none.  A pending pack_size overrides the
   main extent, resolved by unit_resolve against the space left on the line, and is consumed
   (back to natural) after one item.  line.ext grows by running max, and pen_y is carried live
   at the content end so queries and the commit both see the true extent. */
static gui_rect_t
line_place_pen( layout_frame_t* f, f32 natural_w, f32 h )
{
    natural_w = quant_ceil( natural_w );   /* grid: content-driven extents round up onto the */
    h         = quant_ceil( h );           /* lattice, so pack pens advance in quantum steps */

    bool horiz = ( f->mode != GUI_MODE_PACK ) || ( f->line.pack_dir == GUI_PACK_HORIZONTAL );

    /* Nav line: a placement onto a closed line opens a new one; a strip (vertical pack) runs its
       items down the cross axis, so to the keyboard each item is its own line -- Up/Down step
       them -- while a bar / same_line continuation keeps every item on the one it reopened.
       A pinned line (a table cell: the host stamped the whole row's) is never re-dispensed. */
    if ( !f->nav_line_pin && ( !f->line.open || !horiz ) )
        f->nav_line = ++s_build.nav_line_seq;

    f->line.open = true;   /* self-heal: a pen placement always continues the current line */

    /* Natural extents per axis from the widget's preferred size.  A fill widget (no natural width,
       natural_w <= 0) has no main extent of its own: it defaults to filling the rest of the line. */
    f32 nat_main  = horiz ? ( natural_w > 0.0f ? natural_w : 0.0f ) : h;
    f32 cross_ext = horiz ? h : ( natural_w > 0.0f ? natural_w : f->content_w );

    f32 unit = f->line.pack_size_next;
    f->line.pack_size_next = -1.0f;   /* consume -> next item is natural */

    f32 main_edge  = horiz ? ( f->content_x + f->content_w ) : f->band_bottom;
    f32 main_avail = main_edge - f->line.main;
    if ( main_avail < 0.0f ) main_avail = 0.0f;

    f32 main_ext = unit_resolve( unit, nat_main, main_avail );

    /* Opt-in auto-wrap (pack_wrap): a natural / fixed item that overruns the line breaks to a
       fresh one first -- never on a still-empty line (an oversized item places rather than
       looping), and a fill / fraction resolves to the space left so it always fits.  The size
       unit re-resolves against the fresh line, so a pending fraction is of the full run.

       The wrap edge is the VISIBLE track (view width from the line origin), not main_edge:
       content_w inherits last frame's overflow so fill widgets can span scrollable ground, but
       wrapping against that widened edge means one unwrapped long line re-justifies its own
       width forever and the run never wraps again once the window shrinks back.  The view width
       is scroll-free and honest, and it keeps every line of the run the same length.  A strip's
       band_bottom is already view-derived. */
    f32 wrap_edge = horiz ? f->content_x + ( f->view.w - f->pad.l - f->pad.r ) : f->band_bottom;
    if ( f->line.wrap && f->mode == GUI_MODE_PACK && f->line.ext > 0.0f
         && f->line.main + main_ext > wrap_edge )
    {
        pack_line_break( f );
        main_avail = main_edge - f->line.main;
        if ( main_avail < 0.0f ) main_avail = 0.0f;
        main_ext   = unit_resolve( unit, nat_main, main_avail );
    }

    /* Assemble the rect by mapping (main, cross) onto (x, y): a bar runs main along x, a strip along
       y.  The pen advance, the line's cross-extent max, and the watermark grow are axis-agnostic
       once the rect is placed -- content_reach drops the pen and grows the highwater from its corner. */
    gui_rect_t r = horiz ? ( gui_rect_t ){ f->line.main, f->line.cross, main_ext, cross_ext }
                         : ( gui_rect_t ){ f->line.cross, f->line.main, cross_ext, main_ext };

    f->line.main += main_ext + ( horiz ? mod_gap_x( f ) : mod_gap_y( f ) );
    if ( cross_ext > f->line.ext ) f->line.ext = cross_ext;

    content_reach( f, r.x + r.w, r.y + r.h );
    f->line.prev_item = r;
    return r;
}

/*============================================================================================*/
/* Place one item in the next template cell -- the flow placement.  At a row start (col 0) the
   line opens at the pen (gap-before): an auto-height row (row_h == 0) takes the *first* item's h
   as the whole row's height and every cell conforms -- a running max would retroactively misalign
   cells already handed out -- while a fixed row_h overrides it.  Fit-then-align (cell_fit_resolve)
   decides the cell box the same way in STACK and COLUMNS alike: a widget with a natural width
   (button, checkbox, text) shrinks and seats by mod.align, one with none (slider, input) fills the
   track -- the mode only ever chose the track's *width*, never whether an item fills it.  Wrapping
   past the last column commits the line.  line.main is kept at the pen past each cell, so a
   same_line continuation starts exactly where this cell ended. */
static gui_rect_t
line_place_cell( layout_frame_t* f, f32 natural_w, f32 h )
{
    if ( f->line.col == 0 )
    {
        line_commit( f );                       /* close a reopened same_line row, if any */
        f->line.cross = layout_next_y( f );     /* the gap owed above is applied here */

        /* Auto row height (row_h == 0) takes the first item's h ceiled onto the lattice, so the
           row pitch of content-sized rows (a text line) is a quantum step like the metric rows
           already are.  An explicit row_h is authored intent and stays verbatim. */
        f->line.ext   = ( f->tmpl.row_h > 0.0f ) ? f->tmpl.row_h : quant_ceil( h );
        f->line.open  = true;
        if ( !f->nav_line_pin )                     /* a fresh flow row is a fresh nav line -- */
            f->nav_line = ++s_build.nav_line_seq;   /* unless a table pinned the row's        */
    }

    u32        c = f->line.col;
    nat_track_note( f, c, natural_w );   /* a natural column measures its widest item */
    gui_rect_t r = cell_fit_resolve( f, f->tmpl.cellx[ c ], f->tmpl.cellw[ c ], natural_w, f->line.cross, f->line.ext );

    f->line.main = r.x + r.w + mod_gap_x( f );    /* pen past the cell -- the same_line handoff */

    /* A cell that FILLED to the track width (r.w >= the track, e.g. a separator / slider / input
       with no natural width of its own) reports no real content: its width is just whatever the
       track happened to be, which is itself derived from the region's OWN view width.  Folding
       that into the x highwater makes content_w chase view_w -- and the two are decided a frame
       apart (view_w this frame vs. content_w read back next frame), so a vertical scrollbar
       toggling on/off (changing the track by its gutter width) reads as a bogus one-frame swing
       in horizontal content, flickering a horizontal bar that has nothing real to scroll to.  Only
       a cell that actually SHRANK to its natural width is real content; grow the x highwater from
       that, but a filled cell contributes no more x than its own left edge (already covered by the
       region's seed / earlier columns) -- content_reach still runs for the y advance either way.

       A shrunk cell claims its WIDTH from the track ORIGIN (cellx + r.w), never its seated
       position (r.x + r.w): an align( RIGHT / HCENTER ) seat derives from the track width, which
       itself derives from last frame's measured content -- folding the seated corner in makes the
       widened canvas re-justify itself forever (content_w can grow but never shrink back, and a
       right-aligned item parks off-view for good).  Width from origin is position-independent, so
       the measure collapses the frame the wide content disappears.  Left-aligned the two spellings
       are identical. */
    f32 x_reach = ( r.w < f->tmpl.cellw[ c ] ) ? f->tmpl.cellx[ c ] + r.w : f->tmpl.cellx[ c ];
    content_reach( f, x_reach, r.y + r.h );

    if ( ++f->line.col >= f->tmpl.ncols )
        line_commit( f );                        /* row full -> fold it; col back to 0 */

    f->line.prev_item = r;
    return r;
}

/*============================================================================================*/
/* Width-aware form.  `natural_w` is the widget's preferred width.  In stack mode a widget that
   carries one (natural_w > 0) shrinks to it instead of filling the cell -- matching Dear ImGui's
   behavior where buttons size to their label while field widgets (slider, input) fill the row.
   In columns / grid mode the track cell always wins.  Every emit records f->line.prev_item so
   same_line() can anchor the next widget to this one's line.  This is the per-mode dispatch over
   the line machinery above; the widget just fills the rect it is handed. */

gui_rect_t
cell_next_w( f32 natural_w, f32 h )
{
    layout_frame_t* f = lf();

    /* Restore a one-shot next_item_align: the armed item keeps its override through its own
       draw (widgets read mod.align at paint time), so the base align comes back HERE, at the
       following emit, before this item resolves anything. */
    if ( f->line.align_swap )
    {
        if ( f->line.align_armed ) f->line.align_armed = false;   /* this is the override's item */
        else { f->mod.align = f->line.align_restore; f->line.align_swap = false; }
    }

    /* An item is being emitted: resolve its flags (stack + the one-shot next-item override) and
       latch them for item_state / the widget to read.  This is the single per-item seam every
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

    /* One-shot next_item_h: resolve the caller's h against the room left below the pen, at the
       shared seam so every mode sees the final h (a grid cell carries the matrix height and
       ignores h by construction). */
    if ( f->line.h_next >= 0.0f )
    {
        f32 avail_h = f->band_bottom - layout_next_y( f );
        if ( avail_h < 0.0f ) avail_h = 0.0f;
        h = unit_resolve( f->line.h_next, h, avail_h );
        f->line.h_next = -1.0f;
    }

    gui_rect_t r;
    if ( f->mode == GUI_MODE_PACK )
    {
        r = line_place_pen( f, natural_w, h );     /* print run along line.pack_dir */
    }
    else if ( f->tmpl.nrows > 0 )
    {
        r = grid_next_rect( f, natural_w );         /* fixed matrix walk */
        f->line.prev_item = r;
    }
    else if ( f->line.cont_pending )
    {
        /* same_line continuation: one pen placement on the reopened line; the next plain widget
           starts a fresh row below it (cell placement at col 0 commits the line first). */
        f->line.cont_pending = false;
        r = line_place_pen( f, natural_w, h );
        f->line.col = 0;
    }
    else
    {
        r = line_place_cell( f, natural_w, h );    /* the next template cell */
    }

    /* Latch the item's structural nav coordinate for item_state: which region and line the
       cell belongs to.  Latched (not one-shot) on purpose -- a widget that interacts in several
       parts from one cell (a numeric's sub-fields) lists each part as a same-line sibling.
       item_flags_chrome_reset drops it at the chrome seams. */
    s_scope.nav.region = f->nav_region;
    s_scope.nav.line   = f->nav_line;
    s_scope.nav.placed = true;

    DBG_LAYOUT( r );
    return r;
}

/*============================================================================================*/
/* The common case: fill the track cell (natural_w < 0 => no same_line preference). */
gui_rect_t cell_next( f32 h ) { return cell_next_w( -1.0f, h ); }

/*============================================================================================*/
/* Resolve a labeled widget's cell into a label position + a control rect when a field split is
   active (field_split / field_label_*).  The label and control are two tracks laid across the cell
   by the same resolver columns use, so a field split obeys the overloaded unit rule and adapts to
   whatever width the widget is handed -- a full row or a single column cell.  `side` flips which
   track sits left.  Draws nothing; the caller places its label + control from the outputs.  The
   control is floored at min_control_w so it stays usable (overrunning under the label, as before).
   Returns false when no field split is set, leaving the caller on its default layout. */
bool
cell_split_field( gui_rect_t cell, f32 min_control_w, f32* out_label_x, f32* out_label_w,
                     gui_rect_t* out_control )
{
    layout_frame_t* f = lf();
    if ( f->mod.field_side == 0 ) return false;

    /* Order the two tracks by side so the resolver lays them left-to-right correctly. */
    f32 tracks[ 2 ];
    u32 lab_i, ctl_i;
    if ( f->mod.field_side == GUI_LABEL_LEFT )
    {
        tracks[ 0 ] = f->mod.field_label;   tracks[ 1 ] = f->mod.field_control;   lab_i = 0; ctl_i = 1;
    }
    else /* GUI_LABEL_RIGHT */
    {
        tracks[ 0 ] = f->mod.field_control; tracks[ 1 ] = f->mod.field_label;     ctl_i = 0; lab_i = 1;
    }

    f32 pos[ 2 ], size[ 2 ];
    layout_resolve_tracks( tracks, 2, cell.x, cell.w, WIDGET_PAD, pos, size );

    f32 control_w = size[ ctl_i ];
    if ( control_w < min_control_w ) control_w = min_control_w;

    f32 label_x = pos[ lab_i ];

    /* When the control floors at min_control_w it grows past its resolved track.  In LABEL_RIGHT
       the trailing label was anchored to the resolved (shrinking) control edge, so a squeezed row
       slid the label leftward under the control.  Re-anchor it to the FLOORED control's right edge:
       the label stops moving left once the control hits its minimum, and the row overflows into the
       scroll boundary instead of the label crawling beneath the field. */
    if ( f->mod.field_side == GUI_LABEL_RIGHT )
    {
        f32 min_label_x = pos[ ctl_i ] + control_w + WIDGET_PAD;
        if ( label_x < min_label_x ) label_x = min_label_x;
    }

    *out_label_x = label_x;
    *out_label_w = size[ lab_i ];
    *out_control = ( gui_rect_t ){ pos[ ctl_i ], cell.y, control_w, cell.h };
    return true;
}

// clang-format on
/*============================================================================================*/
