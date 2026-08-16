/*==============================================================================================

    runtime_service/gui/chrome/table/gui_table.c -- Table layout: multi-column rows over the columns engine.

    A table is a region whose content is laid out in a column grid -- columns resolved once per
    table_begin, rows accumulated per table_next_row.  table_next_column just points the layout pen
    at the column's cell (tmpl.cellx / tmpl.cellw); content stays inside the column because widgets self-fit
    to that width (text ellipsizes, labeled widgets shrink), exactly like the layout engine's
    columns mode.  There is therefore NO per-cell clip.

    ONE clip, like a window: the table pushes a single clip around the whole table box (header
    included) and runs the body scroll region inside it with own_clip=false -- the same pattern a
    window body uses.  The header is then drawn LAST (as chrome, like a title bar) so it overpaints
    any rows that scrolled up under it.  So the entire table -- header, rows, scrollbar -- lives in
    that one clip, and the body needs no clip of its own.

    The feature set, in layers:
      - Column layout: next_row / next_column, auto-height rows.
      - headers_row: header strip with sort click and sort indicator (drawn as chrome).
      - Decoration: row stripes, H/V dividers + outer frame, row/cell background overrides,
        hovered-column highlight.
      - Column management: drag-to-resize, double-click / menu size-to-fit, drag-to-reorder,
        hide / show, and the built-in right-click header menu that drives all three.
      - Scrolling body (GUI_TABLE_SCROLL_Y / _X): rows scroll under the one table clip and the chrome
        header covers the top, so the header stays frozen with no extra clip.  Columns resolve inside
        the region (after the scrollbar gutter is reserved) so header and body cells stay aligned.

    Logical vs display columns:
      Every public verb speaks LOGICAL column indices -- setup order, stable for the table's life.
      The persist slot holds a permutation (persist->disp) that maps display position to logical
      column, plus a visibility mask; each frame table_columns_build flattens those two into this
      frame's display list (t->disp / t->ndisp), and ALL geometry (col_x / col_w) is indexed by
      display position from there.  Reorder swaps entries in the permutation, hiding drops a column
      out of the flattened list, and the rest of the widget never has to think about either.

    Deferred open model:
      table_begin records outer_rect and exits.  table_open_body() (called lazily from
      table_headers_row or the first table_next_row) pushes the one table clip, opens the body
      region (own_clip=false), and resolves columns.  table_headers_row runs the header sort
      interaction up front but defers the header DRAW to table_end so it lands on top as chrome.

    State model:
      - s_tab_stack (gui_table_t): per-frame active tables, one entry per open nesting level;
                                module-static frame scratch, like s_build (contexts build
                                sequentially on one thread).
      - gui_table_persist_t  : persistent per-table state (col widths, fit measures, display
                                order, visibility, sort, scroll), a big-class tenant of the keyed
                                state pool (GUI_STATE) -- per-context like all keyed state, so two
                                bound contexts never share a same-titled table's state.  The type
                                and the machinery over it (track resolve, pair-resize drag, sort
                                cycle + order sort, row span) are the table ENGINE:
                                flow/gui_table_engine.c.

    Include order (unity build): included by the chrome unit after the flow seam so the engine
    verbs (table_tracks_resolve, table_resize_drag, table_sort_click, table_order_sort,
    table_rows_span), layout_push_region, layout_pop_region, layout_set_default,
    layout_row_break, item_state, lf, s_build, s_interaction, and the draw + style macros are
    all in scope.  The built-in context menu calls the PUBLIC popup / menu vocabulary (declared
    in gui_host.h), which the chrome unit defines further down -- the same dogfooding the stock
    widgets do.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Types
==============================================================================================*/

/* Nesting depth: a table cell may host another whole table (one more frame-scratch slot each). */
#define GUI_TABLE_DEPTH 4

/* Sort indicator triangle: its size, and the horizontal room a header label gives up for it.
   The label placement and the header's own contribution to the fit measure both read these, so
   the arrow can never overlap the text that was measured around it. */
#define GUI_TABLE_ARROW_W       6.0f
#define GUI_TABLE_ARROW_H       4.0f
#define GUI_TABLE_ARROW_RESERVE ( GUI_TABLE_ARROW_W + (f32)WIDGET_PAD )

/* Per-column setup data filled by table_setup_column before the first row. */
typedef struct
{
    char                    label[ 32 ];   /* display name drawn by table_headers_row     */
    gui_table_col_flags_t   flags;         /* FIXED / STRETCH / NO_RESIZE / etc.          */
    f32                     init_w;        /* 0 = stretch (==1 fill); >1 = fixed pixels   */

} gui_table_col_t;

/* The persistent per-table state (gui_table_persist_t: column widths, fit measures, display
   order, visibility, sort, scroll) and the widget-agnostic machinery over it -- track
   resolution, the boundary pair-resize drag, the sort state machine + order sort, row
   virtualization -- live in the table ENGINE at the service tier (flow/gui_table_engine.c, seam
   decls in flow/gui_flow.h).  This widget is the engine's chrome face: geometry, paint, and the
   region policy. */

/* Per-frame active table context, one per open nesting level (see s_tab_stack). */
typedef struct
{
    gui_id_t                id;
    gui_table_flags_t       flags;
    i32                     ncols;
    gui_table_col_t         cols[ GUI_TABLE_COLS_MAX ];
    i32                     col_setup_n;   /* number of table_setup_column calls so far */

    /* This frame's display list: disp[ slot ] = logical column drawn at that slot (hidden ones
       are absent), disp_src[ slot ] = where that slot sits in the persisted permutation, so a
       reorder swap can write back through the hidden columns without disturbing them. */
    i32                     disp    [ GUI_TABLE_COLS_MAX ];
    i32                     disp_src[ GUI_TABLE_COLS_MAX ];
    i32                     slot_of [ GUI_TABLE_COLS_MAX ];   /* logical column -> slot, -1 hidden */
    i32                     ndisp;

    /* Resolved column geometry (screen space), BY DISPLAY SLOT, set in table_open_body. */
    f32                     col_x[ GUI_TABLE_COLS_MAX ];
    f32                     col_w[ GUI_TABLE_COLS_MAX ];

    /* Fit measure (see table_cell_close): the widest content any cell of a LOGICAL column asked
       for this frame, folded into the persist slot at table_end for next frame's size-to-fit. */
    f32                     fit_w[ GUI_TABLE_COLS_MAX ];
    f32                     cell_x0;       // open cell's content left edge (canvas)
    f32                     cell_w0;       // its content width (the fill-widget fallback measure)
    f32                     cell_high_x;   // region highwater saved at cell open
    bool                    cell_open;

    /* Iteration state. */
    i32                     cur_col;       // LOGICAL column of the open cell (-1 before the first)
    i32                     cur_disp;      // its display slot (-1 before the first next_column)
    i32                     cur_row;       // -1 before first table_next_row
    f32                     row_top;       // screen-space top of the current row's BAND
    f32                     row_h;         // content height inside the band
    f32                     row_band;      // painted band height = row_h + the row gap (see next_row)
    f32                     row_pad;       // content inset inside the band (half the gap)
    u32                     nav_row_line;  // nav line the whole row shares (keyboard row = table row)

    gui_rect_t              outer_rect;    // full table box in screen space
    gui_rect_t              body_rect;     // content area inside the opened region
    f32                     header_h;      // header strip height; 0 if no header
    u8                      align_base;    // region's ambient content align (column align overlays it)

    /* Set true once the body region has been pushed (either by table_headers_row or
       the first table_next_row).  Guards layout_pop_region in table_end. */
    bool                    header_done;

    /* The header is drawn last (as chrome, like a window title bar) so it overpaints rows that
       scrolled under it.  table_headers_row only does the sort interaction up front and records
       what the deferred draw needs: whether a header exists and which column is hot / active. */

    bool                    want_header;   // table_headers_row was called this frame
    i8                      hdr_hot;       // display slot under the cursor (-1 none)
    i8                      hdr_act;       // display slot being pressed    (-1 none)

    /* Column under the cursor anywhere in the table box: the GUI_TABLE_HIGHLIGHT_COL tint and
       table_get_hovered_column.  hover_col is LOGICAL, hover_slot its display slot. */
    i8                      hover_col;
    i8                      hover_slot;

    /* Column-resize feedback: index of the interior boundary (between display slot i and i+1)
       that is hot or being dragged, drawn as a highlight line in table_end.  -1 = none. */
    i8                      resize_hot;

    /* Set true in table_headers_row when the user clicks a sort-active column header (and on the
       frame a DEFAULT_SORT column seeds the persist slot).  Cleared by table_get_sort_specs /
       table_sort_order.  Automatically false each new frame (the slot is memset at begin). */
    bool                    sort_dirty;

    /* s_scope.clip on entry, restored when the one table clip is popped in table_end. */
    gui_rect_t              saved_clip;

    gui_table_persist_t*    persist;       // keyed state slot; never NULL (GUI_STATE contract)

} gui_table_t;

/*==============================================================================================
    Module-static state
==============================================================================================*/

static gui_table_t          s_tab_stack[ GUI_TABLE_DEPTH ];
static i32                  s_tab_sp;     /* open tables; the top one is the active context */

/* Reorder drag: the header id that has actually MOVED during the current press, and the last
   frame that press was seen.  A live reorder keeps the dragged column under the cursor, so the
   release would otherwise read as a plain click and sort the column the user was only
   repositioning.  The frame stamp is refreshed while the header is held and bounds the
   suppression to that press -- a stale id can never swallow a later, genuine click. */
static gui_id_t s_tab_reorder_id;
static u32      s_tab_reorder_frame;

/* Reorder drag tracking for the held header: the id being dragged, its cursor x last frame (the
   direction gate -- a swap only fires while the cursor is actually MOVING that way, or the
   dragged column and its neighbour trade places forever), and the settle latch (see
   table_reorder_interact). */
static gui_id_t s_tab_drag_id;
static f32      s_tab_drag_mx;
static bool     s_tab_drag_lock;

/* Which table's built-in header menu is open, and the column it was opened over (the menu
   outlives the frame that opened it, so neither can live in the frame slot). */
static gui_id_t s_tab_ctx_id;
static i8       s_tab_ctx_col;

/* The active table (top of the stack).  Never called with an empty stack -- every public verb
   tests table_is_open() first. */
static gui_table_t*
tab( void )
{
    return &s_tab_stack[ ( s_tab_sp > 0 ? s_tab_sp : 1 ) - 1 ];
}

static bool
table_is_open( void )
{
    return s_tab_sp > 0;
}

/*==============================================================================================
    Internal helpers -- the vocabulary every layer below is written in
==============================================================================================*/

/* Tables are square: a rounded fill under the rectangular table scissor leaves a gap at each
   rounded corner that whatever sits behind the table shows through.  So every piece of table
   chrome paints inside a zero-radius scope -- push it, paint, restore the ambient radius (cell
   widgets keep their own rounding, which is why it is a scope and not a global). */
static f32
table_square_push( void )
{
    f32 save = draw_rounding();
    draw_set_rounding( 0.0f );
    return save;
}

static void
table_square_pop( f32 save )
{
    draw_set_rounding( save );
}

/* One square fill -- the whole paint vocabulary of the table's decoration layers. */
static void
table_fill( gui_rect_t r, u32 abgr )
{
    f32 save = table_square_push();
    draw_fill( r, abgr );
    table_square_pop( save );
}

/* Fill the current row's BAND (its full pitch, so consecutive rows tile with no unpainted seam --
   see table_row_metrics) across the horizontal span [ x, x + w ). */
static void
table_band_fill( const gui_table_t* t, f32 x, f32 w, u32 abgr )
{
    table_fill( ( gui_rect_t ){ x, t->row_top, w, t->row_band }, abgr );
}

/* The table box as a hit rect: the outer box under the clip the caller had on entry. */
static gui_rect_t
table_box( const gui_table_t* t )
{
    return rect_intersect( t->saved_clip, t->outer_rect );
}

/* Chrome above the body (the header strip, the full-height resize bands) sits outside the body
   hit clip table_open_body left in place, so its queries would be rejected.  Widen the hit clip
   to the whole table box for them; the returned clip goes back through table_hit_narrow before
   any row content is emitted. */
static gui_rect_t
table_hit_widen( const gui_table_t* t )
{
    gui_rect_t body_hit = s_scope.clip;
    s_scope.clip        = table_box( t );
    return body_hit;
}

static void
table_hit_narrow( gui_rect_t body_hit )
{
    s_scope.clip = body_hit;
}

/* Bound text to [ x, x + w ) clamped to the table viewport -- the ambient glyph-clip window both
   cells and header labels draw under, so a column scrolled partway off the edge cuts its glyphs
   cleanly at the table border instead of bleeding past it.  Cleared in table_end. */
static void
table_text_clip( const gui_table_t* t, f32 x, f32 w )
{
    f32 vx0 = t->outer_rect.x;
    f32 vx1 = t->outer_rect.x + t->outer_rect.w;
    f32 x1  = x + w;
    draw_set_text_clip_x( ( x > vx0 ) ? x : vx0, ( x1 < vx1 ) ? x1 : vx1 );
}

/* Interaction id for a column's header.  Keyed on the LOGICAL column: a reorder drag moves a
   column between display slots mid-drag, and a slot-keyed id would hand the live press to
   whatever column slid under the cursor. */
static gui_id_t
table_header_id( const gui_table_t* t, i32 col )
{
    return id_combine( t->id, (gui_id_t)( col + 1 ) );
}

/* Point the layout template at one horizontal span: the content column the pen flows in AND the
   STACK-mode cell widgets measure themselves against (cell_next_w reads tmpl.cellx / cellw[ 0 ]). */
static void
table_span_set( layout_frame_t* f, f32 x, f32 w )
{
    f->content_x       = x;
    f->content_w       = w;
    f->tmpl.cellx[ 0 ] = x;
    f->tmpl.cellw[ 0 ] = w;
}

/* A scrolling table frames a fixed viewport and takes the wheel; a plain one frames the rows it
   laid out and declines it (see table_open_body). */
static bool
table_scrolls( const gui_table_t* t )
{
    return ( t->flags & ( GUI_TABLE_SCROLL_Y | GUI_TABLE_SCROLL_X ) ) != 0;
}

/* Does the built-in header menu have anything to offer?  Both the open and the emit gate on this
   -- an opened-but-never-begun popup would sit in the stack until the stale-close drops it. */
static bool
table_has_menu( const gui_table_t* t )
{
    if ( t->flags & GUI_TABLE_NO_CONTEXT_MENU ) return false;
    return ( t->flags & ( GUI_TABLE_RESIZABLE | GUI_TABLE_REORDERABLE | GUI_TABLE_HIDEABLE ) ) != 0;
}

static bool
table_col_hidden( const gui_table_t* t, i32 c )
{
    return ( t->persist->hidden & ( 1u << c ) ) != 0;
}

/* A column's header label, or `fallback` when it was never set up (or set up blank). */
static const char*
table_col_label( const gui_table_t* t, i32 c, const char* fallback )
{
    return ( c < t->col_setup_n && t->cols[ c ].label[ 0 ] ) ? t->cols[ c ].label : fallback;
}

/* The sorted column as a 0-based LOGICAL index, -1 when unsorted (the persist stores it 1-based
   so a zeroed slot reads as unsorted). */
static i32
table_sort_col( const gui_table_t* t )
{
    return (i32)t->persist->sort_col - 1;
}

/* Setup flags for a logical column (columns past the setup count carry none). */
static gui_table_col_flags_t
table_col_flags( const gui_table_t* t, i32 c )
{
    return ( c >= 0 && c < t->col_setup_n ) ? t->cols[ c ].flags : GUI_TABLE_COL_NONE;
}

/* Width to fit a column's measured content, in the same overloaded unit the tracks take
   (a fixed pixel size, so always > 1). */
static f32
table_fit_width( const gui_table_t* t, i32 c )
{
    f32 w = (f32)t->persist->fit_w[ c ] + 2.0f * (f32)WIDGET_PAD;
    return ( w < (f32)WIDGET_MIN_W ) ? (f32)WIDGET_MIN_W : w;
}

/* Gather the setup widths into the flat array the engine takes (the engine never sees
   gui_table_col_t -- labels and flags are chrome business).  A GUI_TABLE_COL_WIDTH_AUTO column
   substitutes last frame's fit measure, which is what makes it size to its content; a user
   resize writes persist->col_w and outranks it from then on, as it does for any column. */
static i32
table_init_widths( gui_table_t* t, f32* out )
{
    for ( i32 i = 0; i < t->col_setup_n; ++i )
        out[ i ] = ( t->cols[ i ].flags & GUI_TABLE_COL_WIDTH_AUTO )
                 ? table_fit_width( t, i )
                 : t->cols[ i ].init_w;
    return t->col_setup_n;
}

/* Stamp the setup's own layout choices onto the persist slot: identity display order plus the
   DEFAULT_HIDE mask.  Shared by the first-use seed and the explicit reset, which differ only in
   what else each one does (clear the widths / seed the default sort). */
static void
table_layout_defaults( gui_table_t* t )
{
    gui_table_persist_t* p = t->persist;

    for ( i32 i = 0; i < GUI_TABLE_COLS_MAX; ++i )
        p->disp[ i ] = (i8)i;

    p->hidden = 0;
    for ( i32 i = 0; i < t->col_setup_n; ++i )
        if ( t->cols[ i ].flags & GUI_TABLE_COL_DEFAULT_HIDE )
            p->hidden |= (u16)( 1u << i );
}

/* Seed a fresh persist slot from the setup flags.  The keyed pool zeroes new slots and the zero
   of every field is its default, but "identity display order" and the DEFAULT_HIDE / DEFAULT_SORT
   choices are only knowable here (chrome owns the column flags), so they are stamped once and
   `seeded` marks the slot as carrying user state from then on. */
static void
table_seed_persist( gui_table_t* t )
{
    gui_table_persist_t* p = t->persist;
    if ( p->seeded ) return;
    p->seeded = 1;

    table_layout_defaults( t );

    for ( i32 i = 0; i < t->col_setup_n; ++i )
    {
        gui_table_col_flags_t cf = t->cols[ i ].flags;
        if ( !( cf & GUI_TABLE_COL_DEFAULT_SORT ) ) continue;

        p->sort_col = (i8)( i + 1 );
        p->sort_dir = (i8)( ( cf & GUI_TABLE_COL_PREFER_DESC ) ? 1 : 0 );

        /* Report the seeded sort as a change: table_sort_order only reorders on a dirty frame,
           so without this the opening sort would wait for a first click. */
        t->sort_dirty = true;
    }
}

/* Flatten the persisted permutation + visibility mask into this frame's display list.
   Canonicalizing first (the ncols real columns fill slots 0..ncols-1 in their remembered order,
   anything stale or duplicated is dropped, anything missing is appended) is what makes the same
   table id survive being reopened with a different column count -- and it is idempotent, so a
   settled table rewrites the same permutation it read. */
static void
table_columns_build( gui_table_t* t )
{
    gui_table_persist_t* p = t->persist;

    i32 order[ GUI_TABLE_COLS_MAX ];
    i32 n    = 0;
    u32 seen = 0;

    for ( i32 s = 0; s < GUI_TABLE_COLS_MAX; ++s )
    {
        i32  c     = (i32)p->disp[ s ];
        bool stale = ( c < 0 || c >= t->ncols );
        bool dup   = !stale && ( seen & ( 1u << c ) ) != 0;
        if ( stale || dup ) continue;
        seen |= 1u << c;
        order[ n++ ] = c;
    }
    for ( i32 c = 0; c < t->ncols; ++c )          /* never mentioned: a grown ncols */
        if ( !( seen & ( 1u << c ) ) )
            order[ n++ ] = c;

    t->ndisp = 0;
    for ( i32 c = 0; c < GUI_TABLE_COLS_MAX; ++c )
        t->slot_of[ c ] = -1;

    for ( i32 i = 0; i < n; ++i )
    {
        i32 c = order[ i ];
        p->disp[ i ] = (i8)c;

        if ( table_col_hidden( t, c ) ) continue;  /* hidden: keeps its place, skips the list */

        t->slot_of [ c        ] = t->ndisp;
        t->disp    [ t->ndisp ] = c;
        t->disp_src[ t->ndisp ] = i;
        ++t->ndisp;
    }
    for ( i32 i = n; i < GUI_TABLE_COLS_MAX; ++i )
        p->disp[ i ] = (i8)i;
}

/* Resolve column positions and widths through the engine (persist override > setup / fit >
   stretch).  The strip is always the OPEN REGION's content column -- header and body share that
   one authoritative width, which is why every re-resolve (drag, reorder, fit, reset) reads it
   back from the live layout frame rather than carrying a remembered box around. */
static void
table_columns_resolve( gui_table_t* t )
{
    f32 init_w[ GUI_TABLE_COLS_MAX ];
    i32 init_n = table_init_widths( t, init_w );
    table_tracks_resolve( t->persist, init_w, init_n, t->disp, t->ndisp,
                          lf()->content_x, lf()->content_w, t->col_x, t->col_w );
}

/* Size a logical column to the content measured last frame; col < 0 fits every visible column.
   Writes a fixed-px persist width, exactly as a manual drag does. */
static void
table_fit_apply( gui_table_t* t, i32 col )
{
    if ( col >= 0 )
    {
        if ( col < t->ncols ) t->persist->col_w[ col ] = table_fit_width( t, col );
        return;
    }

    for ( i32 s = 0; s < t->ndisp; ++s )
        t->persist->col_w[ t->disp[ s ] ] = table_fit_width( t, t->disp[ s ] );
}

/* Number of columns currently shown -- the floor the hide paths respect (a table with no visible
   column has nowhere to put its content, and no way back through its own menu). */
static i32
table_visible_count( const gui_table_t* t )
{
    i32 n = 0;
    for ( i32 c = 0; c < t->ncols; ++c )
        if ( !table_col_hidden( t, c ) ) ++n;
    return n;
}

/* Show / hide a logical column.  Hiding is refused for a NO_HIDE column and for the last visible
   one; showing is always allowed (it can only widen the table's way back). */
static void
table_show_column( gui_table_t* t, i32 col, bool show )
{
    if ( col < 0 || col >= t->ncols ) return;

    if ( show )
    {
        t->persist->hidden &= (u16)~( 1u << col );
        return;
    }

    bool pinned = ( table_col_flags( t, col ) & GUI_TABLE_COL_NO_HIDE ) != 0;
    bool last   = ( table_visible_count( t ) <= 1 );
    if ( pinned || last ) return;

    t->persist->hidden |= (u16)( 1u << col );
}

/* Drag an interior column boundary to resize (GUI_TABLE_RESIZABLE) -- the engine's pair-resize
   mechanism (table_resize_drag) over this table's geometry.  Run from table_open_body right
   after columns resolve, so a live drag updates the persist widths and re-resolves for
   same-frame feedback.  The grab bands span the FULL table height (header included), so widen
   the hit clip to the whole table box for the engine's queries -- the same trick
   table_header_interact uses -- then restore it for the body rows.  Gated on this table's
   window being front-most, mirroring the child edge-resize. */
static void
table_resize_interact( gui_table_t* t )
{
    if ( !( t->flags & GUI_TABLE_RESIZABLE ) ) return;

    gui_rect_t body_hit = table_hit_widen( t );

    /* A column flagged NO_RESIZE pins the boundary on its right edge (by display slot, which is
       where the boundaries actually are). */
    u32 pin_mask = 0;
    for ( i32 s = 0; s < t->ndisp - 1; ++s )
        if ( table_col_flags( t, t->disp[ s ] ) & GUI_TABLE_COL_NO_RESIZE )
            pin_mask |= 1u << s;

    f32 init_w[ GUI_TABLE_COLS_MAX ];
    i32 init_n = table_init_widths( t, init_w );

    bool front = ( s_build.win.id == s_interaction.hover_win );
    i32  dbl   = -1;
    i32  hot   = table_resize_drag( t->id, t->persist, init_w, init_n, t->disp, t->ndisp, pin_mask,
                                    t->outer_rect, front, lf()->content_x, lf()->content_w,
                                    t->col_x, t->col_w, &dbl );
    if ( hot >= 0 )
        t->resize_hot = (i8)hot;

    /* Double-click on a boundary sizes the column on its left to fit, then re-resolves so the
       new width lands this frame. */
    if ( dbl >= 0 && dbl < t->ndisp )
    {
        table_fit_apply( t, t->disp[ dbl ] );
        table_columns_resolve( t );
    }

    table_hit_narrow( body_hit );
}

/* Resolve which column the cursor is over (LOGICAL + slot), for the hovered-column highlight and
   table_get_hovered_column.  Cheap point tests, no interaction claim: hovering a column is not
   an item, and must not compete with the widgets inside the cells. */
static void
table_hover_resolve( gui_table_t* t )
{
    t->hover_col  = -1;
    t->hover_slot = -1;

    if ( s_build.win.id != s_interaction.hover_win ) return;
    if ( !gui_rect_contains( table_box( t ), s_io.mouse_x, s_io.mouse_y ) ) return;

    for ( i32 s = 0; s < t->ndisp; ++s )
    {
        bool over = ( s_io.mouse_x >= t->col_x[ s ] )
                 && ( s_io.mouse_x <  t->col_x[ s ] + t->col_w[ s ] );
        if ( over )
        {
            t->hover_col  = (i8)t->disp[ s ];
            t->hover_slot = (i8)s;
            return;
        }
    }
}

/* Close the open cell: fold its content reach into the column's fit measure and hand the region
   highwater back.  The measure is the ImGui trick -- the cell resets high_x to its own left edge
   on open, so whatever the content pushed it to is that cell's natural width (widgets report
   their NATURAL extent through cell_reach even when they self-fit narrower, which is exactly the
   number size-to-fit wants).  Restoring the max keeps the region's own extent measurement, and
   therefore its scrollbars, untouched by the borrow. */
static void
table_cell_close( gui_table_t* t )
{
    if ( !t->cell_open ) return;
    t->cell_open = false;

    layout_frame_t* f   = lf();
    f32             raw = f->high_x;   /* what the region measured: natural widths only */

    /* Fit measure only: a widget that FILLED its cell reports no natural width to the highwater
       (by design -- see cell_next_w, where folding a filled width in makes content_w chase
       view_w), so for the fit fall back to where the last item in this cell was actually seated.
       The bounds test keeps a previous cell's item out of the answer when this one emitted
       nothing (logical emit order is not display order once columns are reordered).  The value
       stays out of `raw`: the region's own measurement must not learn about filled widths. */
    f32  seat      = f->line.prev_item.x + f->line.prev_item.w;
    bool seat_mine = ( f->line.prev_item.x >= t->cell_x0 - 0.5f )
                  && ( f->line.prev_item.x <= t->cell_x0 + t->cell_w0 );

    f32 fit = ( seat_mine && seat > raw ) ? seat : raw;
    f32 wid = fit - t->cell_x0;

    bool widest = ( t->cur_col >= 0 ) && ( wid > t->fit_w[ t->cur_col ] );
    if ( widest )
        t->fit_w[ t->cur_col ] = wid;

    f->high_x = ( t->cell_high_x > raw ) ? t->cell_high_x : raw;
}

/* Set the row pitch the whole table steps and paints by.  The BAND is the full pitch -- content
   height plus the inter-row gap -- and every painted layer (stripe, hovered column, bg override,
   divider) covers the band, so consecutive rows tile with no unpainted seam between them.
   Painting only the content height instead leaves the gap bare, which reads as alternating row
   heights: a striped row looks like a box with its text jammed against the top, the unstriped row
   between looks taller with its text floating in the middle.  The content sits inset by half the
   gap (row_pad), so a standard-height widget is centered in its band and the air above the first
   row matches the air below the last (see table_end).  Shared by table_next_row and the row
   clipper, which must describe rows exactly the same way or the pen lands off-band. */
static void
table_row_metrics( gui_table_t* t, f32 min_h )
{
    f32 gap = (f32)WIDGET_GAP;

    t->row_h    = ( min_h > 0.0f ) ? min_h : (f32)WIDGET_H;
    t->row_band = t->row_h + gap;
    t->row_pad  = floorf( gap * 0.5f );
}

/* Advance the layout cursor past the current row.  The table is an imperative host: it owns the
   row pen (next row_top = this row's bottom plus the inter-row gap, where the divider draws), so
   it places the pen explicitly rather than flowing -- no line stays open, no gap is owed. */
static void
table_end_row( gui_table_t* t )
{
    table_cell_close( t );

    if ( t->cur_row >= 0 )
        layout_pen_jump( lf(), t->row_top + t->row_band );   /* next band starts where this ends */
}

/* Open the body region below the (optional) header strip and resolve columns inside it.
   Called from table_headers_row and from table_next_row (as an auto-open when the caller skips
   the header). */
static void
table_open_body( gui_table_t* t )
{
    /* The ONE clip: the whole table box, header included.  Like a window body, the scroll region
       below runs own_clip=false inside this single clip, and the header is drawn last (as chrome)
       to overpaint rows that scroll up under it -- so the entire table needs just this one clip. */
    t->saved_clip = s_scope.clip;
    draw_push_clip_rect( t->outer_rect.x, t->outer_rect.y, t->outer_rect.w, t->outer_rect.h );
    s_scope.clip = table_box( t );

    gui_rect_t body = { t->outer_rect.x,
                        t->outer_rect.y + t->header_h,
                        t->outer_rect.w,
                        t->outer_rect.h - t->header_h };
    t->body_rect = body;

    /* Scrolling body: the persist slot supplies the scroll + content storage (the region biases
       the pen by *scroll and writes *content back at pop).  SCROLL_Y leaves the vertical bar
       dynamic (shown only when content overflows); SCROLL_X adds the horizontal bar.

       The link is ALWAYS this table's own slot, scrolling or not.  A shared one is a trap: a
       non-scrolling region still measures its content into the link and still claims the wheel,
       so two such tables would trade scroll offsets and content heights through it every frame --
       one table's rows visibly shuttling between two positions while the other one, whose content
       clamps the offset back to zero, sits perfectly still.

       A non-scrolling table also declines the wheel (NOMOUSESCROLL): with no bars and no scroll
       there is nothing for it to do with a notch, so it belongs to the window underneath, which
       is where the user is aiming. */
    gui_win_flags_t rflags = GUI_WIN_NOSCROLL | GUI_WIN_NOMOUSESCROLL;
    if ( table_scrolls( t ) )
        rflags = ( t->flags & GUI_TABLE_SCROLL_X ) ? GUI_WIN_HSCROLL : GUI_WIN_NONE;

    /* own_clip=false: the region does NOT push its own clip -- it reuses the table clip for drawing
       (so rows scroll under where the header will be) and only narrows the hit-test clip to the body
       box.  Exactly the window-body-with-chrome pattern. */
    layout_push_region( t->id, body, ( gui_pad_t ){ 0, 0, 0, 0 }, rflags,
                        &t->persist->scroll, /* own_clip */ false );

    /* STACK mode so cell_next_w uses tmpl.cellx[0] / tmpl.cellw[0], overridden per column. */
    layout_set_default( lf() );
    t->align_base = lf()->mod.align;

    /* Persisted column state -> this frame's display list -> geometry.  Resolve from the newly-
       opened region's content geometry -- content_w already excludes the vertical scrollbar
       gutter, so header and body columns share one authoritative width. */
    table_seed_persist( t );
    table_columns_build( t );
    table_columns_resolve( t );
    t->body_rect.x = lf()->content_x;
    t->body_rect.w = lf()->content_w;

    /* Resolve column-boundary drags now (after geometry, before any cell content) so a live drag
       re-resolves the columns this frame and a grab pre-empts the header sort under the same pixel. */
    table_resize_interact( t );
    table_hover_resolve( t );

    t->header_done = true;
}

/* Draw column dividers and the outer frame.  Called from table_end AFTER layout_pop_region so the
   lines render in the parent clip (covering header + body) and sit on top of the cell content.
   content_bottom is the screen-space bottom of the last drawn row (or the body top if no rows). */
static void
table_draw_borders( gui_table_t* t, f32 content_bottom )
{
    const f32 x0 = t->outer_rect.x;
    const f32 y0 = t->outer_rect.y;
    const f32 w  = t->outer_rect.w;
    const f32 h  = content_bottom - y0;   /* used height: header strip + drawn rows */
    if ( h <= 0.0f ) return;

    /* Square, so the outer frame's corners meet flush with the rectangular table scissor. */
    f32 save_round = table_square_push();

    /* Vertical dividers between columns, full used height (run through the header strip too). */
    if ( t->flags & GUI_TABLE_BORDERS_V )
    {
        for ( i32 s = 1; s < t->ndisp; ++s )
            draw_fill( ( gui_rect_t ){ t->col_x[ s ], y0, 1.0f, h }, COL_BORDER_IDLE );
    }

    /* Outer frame around the used table box. */
    if ( t->flags & GUI_TABLE_BORDERS_OUTER )
        draw_outline( ( gui_rect_t ){ x0, y0, w, h }, 1.0f, COL_BORDER_IDLE );

    /* Column-resize feedback: recolor the hot / dragged boundary in COL_BORDER_HOT, drawn LAST so
       it wins over the BORDERS_V divider that sits at the same x (and over the outer frame).  Drawn
       here -- in the parent clip after the one table clip is popped -- for the same reason the
       dividers are: so it is not half-clipped by the table box edge. */
    bool boundary_lit = ( t->resize_hot >= 0 ) && ( t->resize_hot < t->ndisp - 1 );
    if ( boundary_lit )
        draw_fill( ( gui_rect_t ){ t->col_x[ t->resize_hot + 1 ], y0, 1.0f, h }, COL_BORDER_HOT );

    table_square_pop( save_round );
}

/* Defined below; table_end draws the header (as chrome) and emits the context menu, and the
   header interaction raises that menu, before their definitions appear. */
static void table_draw_header  ( gui_table_t* t );
static void table_context_menu ( gui_table_t* t );
static void table_menu_open    ( gui_table_t* t, i32 col );

/* Drop every user column choice -- widths, display order, visibility -- back to the setup.  The
   caller re-derives geometry (the menu path is outside the region; the public verb is inside). */
static void
table_reset_apply( gui_table_t* t )
{
    for ( i32 i = 0; i < GUI_TABLE_COLS_MAX; ++i )
        t->persist->col_w[ i ] = 0.0f;

    table_layout_defaults( t );
}

/*==============================================================================================
    Public API
==============================================================================================*/

bool
gui_table_begin( const char* id_str, i32 ncols, gui_table_flags_t flags, f32 height )
{
    /* Nesting is one more frame-scratch slot (a cell may host a whole table); past the depth the
       begin fails and, like any failed begin, its table_end is a no-op. */
    if ( s_tab_sp >= GUI_TABLE_DEPTH ) return false;
    if ( ncols < 1 ) ncols = 1;
    if ( ncols > GUI_TABLE_COLS_MAX ) ncols = GUI_TABLE_COLS_MAX;

    layout_frame_t* parent = lf();
    layout_row_break( parent );   /* finish any partial row in the parent template */

    gui_id_t id = id_combine( id_seed(), id_hash( id_str ) );
    DBG_NAME( id, id_str );

    /* Box width: the content column, clamped to the visible track.  A table is an opaque
       interactive surface (headers, resize dividers, its own bars): sized to a column a sibling
       overflowed, it would seat itself under the parent's scrollbar gutter (invariant 5,
       GUI_ARCHITECTURE.md) -- the same clamp child_begin's w <= 0 default applies. */
    f32 w   = parent->content_w;
    f32 vis = parent->view.w - parent->pad.l - parent->pad.r;
    if ( w > vis ) w = vis;
    f32 h = ( height > 0.0f ) ? height : ( (f32)WIDGET_H + (f32)WIDGET_GAP ) * 8.0f;

    gui_table_t* t = &s_tab_stack[ s_tab_sp ];
    memset( t, 0, sizeof( gui_table_t ) );
    t->id         = id;
    t->flags      = flags;
    t->ncols      = ncols;
    t->cur_col    = -1;
    t->cur_disp   = -1;
    t->cur_row    = -1;
    t->hdr_hot    = -1;
    t->hdr_act    = -1;
    t->hover_col  = -1;
    t->hover_slot = -1;
    t->resize_hot = -1;
    t->outer_rect = ( gui_rect_t ){ parent->content_x, layout_next_y( parent ), w, h };
    t->persist    = GUI_STATE( gui_table_persist_t, id );

    /* The body region is opened lazily by table_open_body (called from table_headers_row or
       the first table_next_row).  Column geometry is resolved there. */
    ++s_tab_sp;
    return true;
}

void
gui_table_end( void )
{
    if ( !table_is_open() ) return;

    gui_table_t* t = tab();

    /* Guard: open body briefly so layout_pop_region can advance the parent cursor correctly
       even when called immediately after table_begin with no rows. */
    if ( !t->header_done )
        table_open_body( t );

    table_end_row( t );

    /* Fold this frame's content measures into the persist slot -- next frame's size-to-fit and
       WIDTH_AUTO tracks read them.  A column that emitted no cell keeps its previous measure
       (a virtualized table only measures the rows it drew). */
    for ( i32 c = 0; c < t->ncols; ++c )
    {
        if ( t->fit_w[ c ] <= 0.0f ) continue;
        f32 m = ( t->fit_w[ c ] > 65535.0f ) ? 65535.0f : t->fit_w[ c ];
        t->persist->fit_w[ c ] = (u16)( m + 0.5f );
    }

    /* Bottom edge the borders frame to.  A scrolling table frames its fixed viewport box (rows
       scroll inside it); a non-scrolling table frames exactly the content it laid out -- the last
       row's bottom edge, or the body top when no rows were emitted.  Captured before pop. */
    bool any_rows       = ( t->cur_row >= 0 );
    f32  content_bottom = table_scrolls( t )
                        ? ( t->outer_rect.y + t->outer_rect.h )
                        : ( any_rows ? ( t->row_top + t->row_band ) : t->body_rect.y );

    /* Restore the full-width content column before pop so layout_pop_region measures the correct
       horizontal extent (high_x tracks the rightmost draw edge for hscroll decisions). */
    layout_frame_t* f = lf();
    table_span_set( f, t->body_rect.x, t->body_rect.w );
    f->mod.align = t->align_base;

    /* Chrome is painted bottom-to-top while the one table clip is still on the draw stack, so every
       layer is bounded by the table box:
         1. header   -- covers rows that scrolled up under the top strip;
         2. borders  -- column dividers run through the header strip, so they must sit ON TOP of the
                        header background (hence after it); the outer frame is inside-aligned
                        (tess_rect_outline draws within the rect) so the table scissor does not
                        half-clip it -- no need to defer past the clip pop;
         3. scrollbar (drawn by layout_pop_region) -- the right / bottom gutter bars draw LAST so
                        they sit over the divider + outer-frame lines instead of being overpainted by
                        them when the window shrinks and a divider lands in the scrollbar gutter. */
    if ( t->want_header )
        table_draw_header( t );

    /* Header labels done -- drop the per-cell / per-header glyph-clip window before the bars. */
    draw_clear_text_clip();

    table_draw_borders( t, content_bottom );

    /* own_clip=false: layout_pop_region pops no draw clip; it restores the hit clip to the table
       box, measures content, and draws the scrollbar -- on top of the borders just laid down. */
    layout_pop_region();

    /* Done with the one table clip: pop it and restore the caller's clip. */
    draw_pop_clip_rect();
    s_scope.clip = t->saved_clip;

    /* The built-in header menu is an overlay: emitted last, outside the table's region and clip
       (the popup detaches from the parent context anyway, but this keeps the table's own paint
       and measurement complete before anything else builds). */
    table_context_menu( t );

    --s_tab_sp;
}

void
gui_table_setup_column( const char* label, gui_table_col_flags_t flags, f32 width )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();
    if ( t->col_setup_n >= t->ncols ) return;

    gui_table_col_t* col = &t->cols[ t->col_setup_n ];

    if ( label )
    {
        const i32 cap = (i32)sizeof( col->label ) - 1;   /* truncates; the header self-fits anyway */
        i32       i   = 0;
        while ( i < cap && label[ i ] ) { col->label[ i ] = label[ i ]; ++i; }
        col->label[ i ] = '\0';
    }

    col->flags  = flags;
    col->init_w = width;
    ++t->col_setup_n;
    /* Column geometry is resolved lazily in table_open_body. */
}

/* Live column reorder (GUI_TABLE_REORDERABLE): while a header is held, dragging out past a
   neighbour's edge swaps the two in the persisted permutation, so the grabbed column travels with
   the cursor and the rest of the table re-flows around it -- the dock tab-strip model.  Runs
   before the header's own interaction pass, from the previous frame's active id, and re-resolves
   the geometry the rest of this frame uses.

   Two gates keep the swap from ping-ponging, which it otherwise does whenever the neighbour is
   much wider (drag a 64px column past a 300px one and the swap parks it clear of the cursor,
   whose position instantly satisfies the reverse test -- the table then flickers between both
   orders until the button is released):
     - DIRECTION: swap only while the cursor is actually moving that way this frame.  A stationary
       cursor can never trigger the reverse swap, which is the flicker's engine.
     - SETTLE: after a swap, no further swap until the cursor is back inside the dragged column.
       The user has to reach the column again to push it further, so one gesture moves it one
       place, however lopsided the two widths are. */
static void
table_reorder_interact( gui_table_t* t )
{
    if ( !( t->flags & GUI_TABLE_REORDERABLE ) ) return;

    for ( i32 s = 0; s < t->ndisp; ++s )
    {
        i32      c   = t->disp[ s ];
        gui_id_t hid = table_header_id( t, c );

        if ( s_interaction.active_id != hid ) continue;

        /* Keep an established suppression alive for as long as this press lasts (the user may
           park the column for a while before letting go). */
        if ( s_tab_reorder_id == hid ) s_tab_reorder_frame = gui_frame_index();

        /* Cursor motion since the last frame of THIS press (a fresh press starts at zero). */
        bool fresh_press = ( s_tab_drag_id != hid );
        f32  dx          = fresh_press ? 0.0f : ( s_io.mouse_x - s_tab_drag_mx );
        if ( fresh_press )
        {
            s_tab_drag_id   = hid;
            s_tab_drag_lock = false;
        }
        s_tab_drag_mx = s_io.mouse_x;

        const f32 cl = t->col_x[ s ];
        const f32 cr = t->col_x[ s ] + t->col_w[ s ];

        bool inside = ( s_io.mouse_x >= cl ) && ( s_io.mouse_x < cr );
        if ( inside ) s_tab_drag_lock = false;   /* the cursor caught up: swaps are armed again */

        bool settling = s_tab_drag_lock;
        bool pinned   = ( table_col_flags( t, c ) & GUI_TABLE_COL_NO_REORDER ) != 0;
        if ( settling || pinned ) break;

        /* Which way did the cursor leave the column, and is it still going that way? */
        bool push_left  = ( dx < 0.0f ) && ( s_io.mouse_x < cl );
        bool push_right = ( dx > 0.0f ) && ( s_io.mouse_x > cr );

        i32 other = -1;
        if ( push_left && s > 0 )                   other = s - 1;
        else if ( push_right && s < t->ndisp - 1 )  other = s + 1;

        if ( other < 0 ) break;
        if ( table_col_flags( t, t->disp[ other ] ) & GUI_TABLE_COL_NO_REORDER ) break;

        /* Swap through the PERSISTED permutation (disp_src), so hidden columns parked between the
           two keep their own places. */
        i8* pd = t->persist->disp;
        i8  tmp                    = pd[ t->disp_src[ s ] ];
        pd[ t->disp_src[ s ] ]     = pd[ t->disp_src[ other ] ];
        pd[ t->disp_src[ other ] ] = tmp;

        /* The release must not read as a sort click on the column the user was repositioning. */
        s_tab_reorder_id    = hid;
        s_tab_reorder_frame = gui_frame_index();
        s_tab_drag_lock     = true;

        table_columns_build( t );
        table_columns_resolve( t );
        table_hover_resolve( t );   /* geometry moved: the column highlight follows it this frame */
        break;
    }

    /* Drag over: forget the press so the next one starts from a clean delta / lock. */
    if ( s_tab_drag_id != GUI_ID_NONE && s_interaction.active_id != s_tab_drag_id )
    {
        s_tab_drag_id   = GUI_ID_NONE;
        s_tab_drag_lock = false;
    }
}

/* Header sort interaction, run up front in table_headers_row.  The header strip sits above the body
   box, so the body hit clip set by table_open_body would reject clicks on it -- widen the hit clip to
   the whole table box for the queries, then restore it for the rows.  Records which column is hot /
   active so the deferred draw (table_draw_header, called as chrome in table_end) can tint it. */
static void
table_header_interact( gui_table_t* t )
{
    gui_rect_t body_hit = table_hit_widen( t );

    table_reorder_interact( t );

    const f32 hy = t->outer_rect.y;
    const f32 hh = t->header_h;

    bool sortable = ( t->flags & GUI_TABLE_SORTABLE ) != 0;
    bool tristate = ( t->flags & GUI_TABLE_SORT_TRISTATE ) != 0;
    t->hdr_hot = -1;
    t->hdr_act = -1;

    for ( i32 s = 0; s < t->ndisp; ++s )
    {
        i32                   c        = t->disp[ s ];
        gui_table_col_flags_t cf       = table_col_flags( t, c );
        bool                  can_sort = sortable && !( cf & GUI_TABLE_COL_NO_SORT );

        gui_id_t   hid = table_header_id( t, c );
        gui_rect_t cr  = { t->col_x[ s ], hy, t->col_w[ s ], hh };

        /* Headers are chrome, not keyboard targets -- the same opt-out scrollbars and tab chips
           take.  Without it a header click adopts the nav cursor, and because the strip sits
           ABOVE the body region's view, the nav scroll-chase then yanks the body's scroll trying
           to bring an item that is not in it into view: the table appears to jump / reset when a
           header is clicked.  Sort and reorder are mouse gestures; nothing is lost. */
        s_scope.nav.skip = true;

        gui_item_state_t st = item_state( hid, cr, ITEM_BUTTON );

        if ( st.hover )  t->hdr_hot = (i8)s;
        if ( st.active ) t->hdr_act = (i8)s;

        /* The header label is content too: size-to-fit must not cut the column's own title.  Bare
           text width plus the sort triangle's reserve -- the cell pad is added once, by
           table_fit_width, for every contributor (cell measures are already pad-relative). */
        if ( c < t->col_setup_n )
        {
            f32 lw = font_text_w( t->cols[ c ].label );
            if ( can_sort ) lw += GUI_TABLE_ARROW_RESERVE;
            if ( lw > t->fit_w[ c ] ) t->fit_w[ c ] = lw;
        }

        /* Right-click anywhere in the strip opens the built-in column menu, over this column. */
        if ( st.hover && s_io.mouse_pressed[ 1 ] )
            table_menu_open( t, c );

        /* Sort click: the engine cycles the persist's column / direction.  A press that ended a
           reorder drag is consumed here instead -- the column moved, the user did not ask to sort. */
        bool ended_reorder = ( hid == s_tab_reorder_id )
                          && ( gui_frame_index() <= s_tab_reorder_frame + 1u );

        if ( st.clicked )
        {
            if ( ended_reorder )
            {
                s_tab_reorder_id = 0;   /* the press repositioned the column; swallow it */
            }
            else if ( can_sort )
            {
                table_sort_click( t->persist, c, tristate, ( cf & GUI_TABLE_COL_PREFER_DESC ) != 0 );
                t->sort_dirty = true;
            }
        }
    }

    table_hit_narrow( body_hit );
}

/* Draw the header strip.  Called LAST (from table_end) within the one table clip so it overpaints
   any rows that scrolled up under it -- the same "chrome drawn last" trick a window uses for its
   title bar.  Visual only: the sort interaction already ran in table_header_interact. */
static void
table_draw_header( gui_table_t* t )
{
    const f32 hy = t->outer_rect.y;
    const f32 hh = t->header_h;

    /* Square header fills (see table_square_push).  Text and the sort triangle below are
       unaffected by the ambient radius, so holding it at zero for the whole strip is safe. */
    f32 save_round = table_square_push();

    /* Full-width opaque header background (also the cover for rows scrolled under the header). */
    draw_face( ( gui_rect_t ){ t->outer_rect.x, hy, t->outer_rect.w, hh }, GUI_ROLE_TITLE, GUI_PHASE_IDLE );

    const i32 sorted_col = table_sort_col( t );

    for ( i32 s = 0; s < t->ndisp; ++s )
    {
        i32 c  = t->disp[ s ];
        f32 cx = t->col_x[ s ];
        f32 cw = t->col_w[ s ];

        gui_table_col_flags_t cf     = table_col_flags( t, c );
        bool                  sorted = ( sorted_col == c );

        /* Hover / active tint (state captured in table_header_interact); a hovered column tints
           its header too when the whole-column highlight is on. */
        bool pressed = ( s == (i32)t->hdr_act );
        bool hovered = ( s == (i32)t->hdr_hot );
        bool col_lit = ( t->flags & GUI_TABLE_HIGHLIGHT_COL ) && ( s == (i32)t->hover_slot );

        if ( pressed || hovered || col_lit )
            draw_fill( ( gui_rect_t ){ cx, hy, cw, hh }, pressed ? COL_BG_ACTIVE : COL_BG_HOT );

        /* Column label, self-fitted to the column (reserving the right pad, plus the sort triangle
           on the sorted column) so a long label ellipsizes instead of bleeding into the next column
           -- no per-column clip.  Placed by the column's own alignment, like its cells. */
        const char* lbl   = table_col_label( t, c, "" );
        f32         arrow = sorted ? GUI_TABLE_ARROW_RESERVE : 0.0f;
        f32         lblx  = cx + (f32)WIDGET_PAD;
        f32         lblw  = ( cx + cw - (f32)WIDGET_PAD - arrow ) - lblx;
        if ( lblw < 0.0f ) lblw = 0.0f;

        /* Alignment only has somewhere to move the label when it fits its slot.  Header labels
           wear the SMALL type role: the measure, the centering and the fitted draw all read the
           small font inside the bracket. */
        gui_type_push( GUI_TYPE_SMALL );
        f32 tw = font_text_w( lbl );
        if ( tw < lblw )
        {
            if ( cf & GUI_TABLE_COL_ALIGN_RIGHT )       lblx += lblw - tw;
            else if ( cf & GUI_TABLE_COL_ALIGN_CENTER ) lblx += ( lblw - tw ) * 0.5f;
        }

        /* Same glyph-clip window as the body cells: clamp the label to its column, intersected with
           the table viewport, so a header at the scroll edge cuts cleanly at the border too.
           Vertically centered in the strip (a fixed gap offset drifts whenever the metric ramp
           changes the header height / glyph size ratio). */
        table_text_clip( t, lblx, lblw );
        draw_text_fit_n( lblx, hy + ( hh - font_char_h() ) * 0.5f, COL_TEXT_PRIMARY_IDLE, lbl, 0xFFFFFFFFu, lblw );
        gui_type_pop();

        /* Sort indicator triangle on the active sort column: tip up for ascending, down for
           descending, seated in the reserve the label just gave up. */
        if ( sorted )
        {
            const f32 aw = GUI_TABLE_ARROW_W, ah = GUI_TABLE_ARROW_H;
            f32       tx = cx + cw - (f32)WIDGET_PAD - aw * 0.5f;  /* right edge inset by WIDGET_PAD */
            f32       ty = hy + ( hh - ah ) * 0.5f;                /* vertically centered */

            if ( t->persist->sort_dir == 0 )
                draw_push_triangle( tx - aw * 0.5f, ty + ah, tx, ty, tx + aw * 0.5f, ty + ah,
                                    COL_TEXT_PRIMARY_IDLE );
            else
                draw_push_triangle( tx - aw * 0.5f, ty, tx, ty + ah, tx + aw * 0.5f, ty,
                                    COL_TEXT_PRIMARY_IDLE );
        }
    }

    table_square_pop( save_round );
}

/* Reserve the header strip and run its sort interaction up front; the strip itself is drawn last
   (as chrome) in table_end.  Must be called after all table_setup_column calls, before the first
   table_next_row. */
void
gui_table_headers_row( void )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();

    t->header_h    = (f32)WIDGET_H;
    t->want_header = true;
    table_open_body( t );        /* opens body below the header, resolves columns */
    table_header_interact( t );  /* sort / reorder / menu now; the draw is deferred to table_end */
}

void
gui_table_next_row( f32 min_h )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();

    /* Auto-open the body if table_headers_row was not called. */
    if ( !t->header_done )
        table_open_body( t );

    table_end_row( t );   /* advance past the previous row if any */

    t->cur_row++;
    t->cur_col  = -1;
    t->cur_disp = -1;
    t->row_top  = lf()->pen_y;
    table_row_metrics( t, min_h );

    /* Reserve the whole band: the scroll range must reach the band's bottom edge, not just the
       last widget in it, or the final row ends flush against the view with its padding cut off. */
    extent_track( lf(), lf()->content_x, t->row_top + t->row_band );

    /* One nav line for the whole row (table_next_column pins it over its per-column pen jumps),
       so the keyboard sees the table as rows of cells: Up/Down step rows, Left/Right the cells. */
    t->nav_row_line = ++s_build.nav_line_seq;

    /* The row's decoration layers, painted here so cell content (emitted after next_column) sits
       on top of all of them -- a tint over the content would wash out its text.  All auto-clipped
       to the body region by the active draw clip. */
    bool striped = ( t->flags & GUI_TABLE_ROW_STRIPES ) && ( t->cur_row & 1 );
    bool col_lit = ( t->flags & GUI_TABLE_HIGHLIGHT_COL ) && ( t->hover_slot >= 0 );
    bool ruled   = ( t->flags & GUI_TABLE_BORDERS_H ) && ( t->cur_row > 0 );

    if ( striped )
        table_band_fill( t, t->body_rect.x, t->body_rect.w, GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x12 ) );

    if ( col_lit )
        table_band_fill( t, t->col_x[ t->hover_slot ], t->col_w[ t->hover_slot ],
                         GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x10 ) );

    /* Horizontal divider in the gap above this row (between the previous row and this one). */
    if ( ruled )
        table_fill( ( gui_rect_t ){ t->body_rect.x, t->row_top, t->body_rect.w, 1.0f },
                    COL_BORDER_IDLE );
}

/* Fixed-pitch row clipper for tables -- the table face of rows_clip (see gui_layout.c for the
   full contract).  Call after the header, before the row loop, with the SAME min_h the rows pass
   to table_next_row (0 = WIDGET_H); the loop then emits only the returned [first, last):

       gui_span_t s = gui()->table_rows_clip( count, 0.0f );
       for ( i32 i = s.first; i < s.last; ++i ) { gui()->table_next_row( 0.0f ); ... }

   The table is an imperative host (rows step by pen jump, pitch = row_h + WIDGET_GAP), so the
   skip is one pen jump plus one highwater touch for the tail; cur_row is seeded to the absolute
   index so row stripes and dividers keep phase across the culled head.  No end call: table_end
   already measures the reserved extent.  Meant for a SCROLL_Y table -- a non-scrolling table
   frames only the rows it drew, so clipping one just truncates it. */
gui_span_t
gui_table_rows_clip( i32 count, f32 min_h )
{
    if ( !table_is_open() || count <= 0 ) return ( gui_span_t ){ 0, 0 };
    gui_table_t* t = tab();

    if ( !t->header_done )
        table_open_body( t );

    table_end_row( t );   /* close any row already emitted; the pen is at its bottom + gap */

    table_row_metrics( t, min_h );   /* same pitch the emitted rows will use */
    f32 top = lf()->pen_y;           /* imperative host: the pen is authoritative, no gap owed */

    /* The engine reserves the full extent, computes the visible span, and jumps the pen past
       the culled head. */
    gui_span_t s = table_rows_span( count, t->row_h, top );

    /* Seed the culled head as the "previous row": absolute cur_row keeps stripe/divider phase,
       and row_top must describe the last culled row -- the next table_next_row re-steps the pen
       from it via table_end_row (row_top + row_band), and left at table_begin's zeros that step
       would yank the pen to the screen top and strand the whole run above the view. */
    t->cur_row = s.first - 1;
    t->row_top = top + (f32)( s.first - 1 ) * t->row_band;

    return s;
}

/* Advance to the next LOGICAL column and open its cell.  Stepping is logical -- a hidden column
   still consumes one next_column call and returns false -- so the caller's "one call per column,
   in setup order" loop keeps its data on the right column no matter what the user has hidden or
   reordered.  The cell is then placed at that column's DISPLAY position, which is what reordering
   moves.  False past the last column, or for a hidden one: emit nothing for that cell. */
bool
gui_table_next_column( void )
{
    if ( !table_is_open() ) return false;
    gui_table_t* t = tab();

    table_cell_close( t );

    t->cur_col++;

    bool past_last = ( t->cur_col >= t->ncols );
    if ( past_last ) { t->cur_col = t->ncols; t->cur_disp = -1; return false; }

    t->cur_disp = t->slot_of[ t->cur_col ];

    bool hidden = ( t->cur_disp < 0 );
    if ( hidden ) return false;   /* consumed the call, emits nothing */

    /* Set the layout pen and column geometry so cell_next_w returns the correct cell.
       Content stays inside the column because widgets self-fit to tmpl.cellw (text ellipsizes, labeled
       widgets shrink) -- the same contract as the layout engine's columns mode, so no per-cell clip
       is pushed.  The one exterior clip (the body region) bounds the table as a whole. */
    layout_frame_t* f = lf();
    f32 cx = t->col_x[ t->cur_disp ];
    f32 cw = t->col_w[ t->cur_disp ];

    /* Inset cell content by WIDGET_PAD on each side (the Dear ImGui CellPadding analogue).  This
       keeps content off the column edge so it lines up under the header labels (which inset by the
       same pad) and, at the table corners, stays clear of the rounding arc of the containing window
       / child -- flush content would otherwise bleed past the rounded corner under the rect scissor.
       Only the content pen is inset; col_x / col_w stay full width for borders, stripes, and the
       header strip. */
    f32 pad = (f32)WIDGET_PAD;
    f32 ix  = cx + pad;
    f32 iw  = cw - 2.0f * pad;
    if ( iw < 0.0f ) iw = 0.0f;

    /* Pen to the row's content top for each column -- the band's top plus its inset, so the
       content is centered in the band (see table_row_metrics).  No gap is owed either way. */
    layout_pen_jump( f, t->row_top + t->row_pad );
    table_span_set( f, ix, iw );

    /* Per-column content alignment (GUI_TABLE_COL_ALIGN_*): the horizontal bits come from the
       column, the vertical bits stay whatever the region was set to -- so a numeric column can be
       right-aligned without giving up the caller's vertical centering. */
    gui_table_col_flags_t cf = table_col_flags( t, t->cur_col );
    u8 al = (u8)( t->align_base & (u8)~( GUI_ALIGN_HCENTER | GUI_ALIGN_RIGHT ) );
    if ( cf & GUI_TABLE_COL_ALIGN_RIGHT )       al |= GUI_ALIGN_RIGHT;
    else if ( cf & GUI_TABLE_COL_ALIGN_CENTER ) al |= GUI_ALIGN_HCENTER;
    f->mod.align = al;

    /* Pin the row's nav line over the per-column pen jumps: left to itself each cell's placement
       would open (and dispense) a fresh line, splitting one visual row into many keyboard rows. */
    f->nav_line_pin = true;
    f->nav_line     = t->nav_row_line;

    /* Open the cell's fit measure: high_x is borrowed as this cell's content reach (restored, at
       its true maximum, by table_cell_close). */
    t->cell_x0     = ix;
    t->cell_w0     = iw;
    t->cell_high_x = f->high_x;
    t->cell_open   = true;
    f->high_x      = ix;

    /* Bound text drawn into this cell to its visible window: the cell's inset rect, clamped to the
       table viewport box.  A column scrolled partway off the edge then cuts its glyphs cleanly at
       the border instead of bleeding under the row-selection highlight (which stops at the
       viewport) -- the worst-case overlap that reads as unpolished.  draw_push_text picks this up
       ambiently, so selectable labels and plain cell text self-terminate at the cell / viewport
       edge with no per-cell scissor. */
    table_text_clip( t, ix, iw );

    return true;
}

/* Jump to a LOGICAL column rather than advancing.  Returns false for a hidden (or out of range)
   column -- the caller's cue to skip that cell's content, exactly like a next_column that ran
   out of columns. */
bool
gui_table_set_column_index( i32 col )
{
    if ( !table_is_open() ) return false;
    gui_table_t* t = tab();

    if ( col < 0 || col >= t->ncols ) return false;

    t->cur_col = col - 1;       /* table_next_column will step onto col */
    return gui_table_next_column();
}

i32
gui_table_get_column_count( void )
{
    return table_is_open() ? tab()->ncols : 0;
}

i32
gui_table_get_column_index( void )
{
    return table_is_open() ? tab()->cur_col : -1;
}

i32
gui_table_get_row_index( void )
{
    return table_is_open() ? tab()->cur_row : -1;
}

/* The LOGICAL column under the cursor anywhere in the table box (header included), or -1. */
i32
gui_table_get_hovered_column( void )
{
    return table_is_open() ? (i32)tab()->hover_col : -1;
}

bool
gui_table_is_column_visible( i32 col )
{
    if ( !table_is_open() ) return false;
    gui_table_t* t = tab();
    if ( col < 0 || col >= t->ncols ) return false;
    return !table_col_hidden( t, col );
}

/* Show / hide a logical column.  Refused for a NO_HIDE column, and for the last visible one
   (a table with no columns has no way back). */
void
gui_table_set_column_visible( i32 col, bool visible )
{
    if ( !table_is_open() ) return;
    table_show_column( tab(), col, visible );
}

/* Size a column to the widest content measured for it (col < 0 = every visible column).  Uses the
   PREVIOUS frame's measure, so calling it while building this frame's table takes effect now for
   a column whose cells have not been emitted yet, and next frame otherwise. */
void
gui_table_fit_column( i32 col )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();
    table_fit_apply( t, col );
    if ( t->header_done )
        table_columns_resolve( t );
}

/* Drop every user column choice -- widths, display order, and visibility -- back to what the
   table_setup_column calls asked for. */
void
gui_table_reset_columns( void )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();

    table_reset_apply( t );

    if ( t->header_done )
    {
        table_columns_build( t );
        table_columns_resolve( t );
    }
}

/* Return true on the frame the sort changed; out is filled either way (col -1 = unsorted), so a
   caller that re-sorts on its own schedule can read the live state at any time. */
bool
gui_table_get_sort_specs( gui_table_sort_specs_t* out )
{
    if ( !table_is_open() ) return false;
    gui_table_t* t = tab();

    if ( out )
    {
        out->col        = table_sort_col( t );
        out->descending = ( t->persist->sort_dir != 0 );
    }

    bool changed = t->sort_dirty && ( table_sort_col( t ) >= 0 );
    if ( !changed ) return false;

    t->sort_dirty = false;
    return true;
}

/* Reorder a user-owned display-order index array to match the table's active sort.  order holds
   the user data indices in display order; count is its length.  Sorts ONLY on the frame the sort
   changed (consuming the same dirty flag table_get_sort_specs reads -- including the first frame,
   where a GUI_TABLE_COL_DEFAULT_SORT column seeds one), so it is cheap to call unconditionally
   every frame and the order is preserved across frames.  Pass val_fn for the built-in
   alphabetical / numeric sort (direction handled by the engine), or cmp_fn for a full-control
   comparator (cmp_fn wins if both are given).  The sort itself is the engine's stable order sort
   (table_order_sort).  Returns true when it reordered the array. */
bool
gui_table_sort_order( i32* order, i32 count, gui_table_sort_value_fn val_fn,
                        gui_table_sort_cmp_fn cmp_fn, void* user )
{
    if ( !table_is_open() || !order || count < 2 ) return false;
    gui_table_t* t = tab();

    i32  col     = table_sort_col( t );
    bool changed = t->sort_dirty && ( col >= 0 );
    if ( !changed ) return false;

    /* Consume the dirty flag regardless: with no comparator there is nothing to do, but we should
       not keep re-reporting the same click on later frames. */
    t->sort_dirty = false;

    return table_order_sort( order, count, col, t->persist->sort_dir != 0, val_fn, cmp_fn, user );
}

/* Tint the current row or cell.  Call after table_next_row (for ROW) or after table_next_column
   (for CELL) and before emitting the cell's content, so the fill lands under that content. */
void
gui_table_set_bg_color( gui_table_bg_target_t target, u32 abgr )
{
    if ( !table_is_open() ) return;
    gui_table_t* t = tab();
    if ( t->cur_row < 0 ) return;

    /* Both fills cover the row's BAND (the full pitch), so a tinted row or cell tiles flush with
       its neighbours exactly like the stripes -- see table_row_metrics.  The one table clip keeps
       them in bounds; there is no per-cell clip. */
    bool cell_open = ( target == GUI_TABLE_BG_CELL ) && ( t->cur_disp >= 0 );

    if ( target == GUI_TABLE_BG_ROW )
        table_band_fill( t, t->body_rect.x, t->body_rect.w, abgr );
    else if ( cell_open )
        table_band_fill( t, t->col_x[ t->cur_disp ], t->col_w[ t->cur_disp ], abgr );
}

/*==============================================================================================
    Built-in header context menu

    Right-clicking the header opens the table's own column menu: size-to-fit, reset, and one
    toggle per hideable column -- the standard grid affordances, so no caller has to hand-build
    them to reach the features the flags already advertise.  Emitted from table_end through the
    PUBLIC popup / menu vocabulary (the popup detaches from the parent context, which is what
    makes it legal to raise one from inside a table's chrome).

    Suppressed by GUI_TABLE_NO_CONTEXT_MENU for a table that wants the right button for itself.
==============================================================================================*/

/* The menu's popup key.  Salted with the table id so two tables in one window (or one scope) get
   their own menus; both the open and the begin mint it the same way. */
static void
table_menu_key( const gui_table_t* t, char* buf, u32 cap )
{
    snprintf( buf, cap, "##tblmenu%08X", (unsigned)t->id );
}

/* Record the column the menu was raised over (it must outlive this frame) and open it. */
static void
table_menu_open( gui_table_t* t, i32 col )
{
    if ( !table_has_menu( t ) ) return;

    char key[ 32 ];
    table_menu_key( t, key, sizeof( key ) );

    s_tab_ctx_id  = t->id;
    s_tab_ctx_col = (i8)col;
    gui_popup_open( key );
}

static void
table_context_menu( gui_table_t* t )
{
    if ( !table_has_menu( t ) ) return;

    char key[ 32 ];
    table_menu_key( t, key, sizeof( key ) );

    if ( !gui_popup_begin( key, GUI_WIN_NONE ) ) return;

    /* The column the menu was raised over, if this table is the one that raised it. */
    i32 ctx_col = ( s_tab_ctx_id == t->id ) ? (i32)s_tab_ctx_col : -1;
    if ( ctx_col >= t->ncols ) ctx_col = -1;

    if ( t->flags & GUI_TABLE_RESIZABLE )
    {
        if ( ctx_col >= 0 && gui_menu_item( "Size column to fit", NULL, NULL ) )
            table_fit_apply( t, ctx_col );
        if ( gui_menu_item( "Size all columns to fit", NULL, NULL ) )
            table_fit_apply( t, -1 );
    }

    if ( gui_menu_item( "Reset columns", NULL, NULL ) )
        table_reset_apply( t );

    /* Visibility toggles: checkboxes rather than menu items, so toggling several columns does not
       dismiss the menu between each one.  Ids are salted by column index -- two columns may
       legitimately carry the same label. */
    if ( t->flags & GUI_TABLE_HIDEABLE )
    {
        gui_separator();

        for ( i32 c = 0; c < t->ncols; ++c )
        {
            if ( table_col_flags( t, c ) & GUI_TABLE_COL_NO_HIDE ) continue;

            const char* lbl = table_col_label( t, c, "(column)" );
            bool        on  = !table_col_hidden( t, c );

            gui_push_id_int( c );
            if ( gui_checkbox( lbl, &on ) )
                table_show_column( t, c, on );
            gui_pop_id();
        }
    }

    gui_popup_end();
}

// clang-format on
/*============================================================================================*/
