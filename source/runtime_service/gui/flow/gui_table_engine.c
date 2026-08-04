/*==============================================================================================

    runtime_service/gui/flow/gui_table_engine.c -- the table ENGINE: the widget-agnostic
    machinery behind chrome's table_* verbs, at the service tier so a second kit builds its
    own table look without chrome.

    Four mechanisms over ( id, gui_table_persist_t ), no paint and no frame scratch:
      - column tracks : persist-override > setup fixed-px > stretch, through the one track
                        resolver (table_tracks_resolve);
      - pair-resize   : the grab-band boundary drag that mutates the persisted widths and
                        re-resolves for same-frame feedback (table_resize_drag);
      - sort state    : the header-click column / direction cycle on the persist slot
                        (table_sort_click) and the stable display-order sort (table_order_sort);
      - virtualization: the fixed-pitch visible span over the open region, reserving the full
                        extent and jumping the pen past the culled head (table_rows_span).

    The chrome widget (chrome/table/gui_table.c) keeps everything visual and regional: the
    one-clip model, the deferred body open, header paint + stripes + dividers, and the
    scroll-region policy.  Seam decls: flow/gui_flow.h.  Included by the gui_flow.c unit root.

==============================================================================================*/
// clang-format off

void
table_tracks_resolve( const gui_table_persist_t* p, const f32* init_w, i32 init_n,
                      const i32* disp, i32 ndisp, f32 x, f32 w, f32* out_x, f32* out_w )
{
    f32 tracks[ GUI_TABLE_COLS_MAX ];

    for ( i32 s = 0; s < ndisp; ++s )
    {
        i32 c = disp[ s ];   /* logical column shown at display slot s */

        /* Priority: user-resized / fitted persist width > setup fixed width > default stretch. */
        f32 track = 1.0f;    /* stretch / fill by default */

        if ( p->col_w[ c ] > 0.0f )
            track = p->col_w[ c ];
        else if ( c < init_n && init_w[ c ] > 1.0f )
            track = init_w[ c ];   /* explicit fixed width */

        tracks[ s ] = track;
    }

    /* Zero gap between columns -- dividers are chrome lines, not gaps. */
    layout_tracks_resolve( tracks, (u32)ndisp, x, w, 0.0f, out_x, out_w );
}

/* Pair-resize: the dragged boundary grows the column on its left and gives the difference back
   to its right neighbor, so the pair's combined width -- and therefore every other column --
   stays put and the grabbed edge tracks the cursor exactly.  Both sides are written as fixed-px
   tracks (values > 1), which table_tracks_resolve then honors over setup / stretch.  The grab is
   the bare item_grab protocol (core/gui_item.c), claiming the left button like the dock
   splitter; a live drag re-resolves col_x / col_w so the columns laid out this frame reflect
   the drag with no lag. */
/* The boundary whose current press was spent on a size-to-fit double-click.  item_grab claims the
   button on that second press like any other, so without this latch the drag that follows would
   walk the freshly fitted width straight back to wherever the cursor sits.  Cleared on release. */
static gui_id_t s_table_fit_press;

i32
table_resize_drag( gui_id_t id, gui_table_persist_t* p, const f32* init_w, i32 init_n,
                   const i32* disp, i32 ndisp, u32 pin_mask, gui_rect_t band_box, bool front,
                   f32 x, f32 w, f32* col_x, f32* col_w, i32* out_dbl )
{
    const f32 thick = 6.0f;          /* grab band width, centered on the boundary */
    const f32 min_w = WIDGET_MIN_W;  /* floor for each side of the resized pair    */
    i32       hot   = -1;

    if ( out_dbl ) *out_dbl = -1;
    if ( !s_io.mouse_down[ 0 ] ) s_table_fit_press = GUI_ID_NONE;

    for ( i32 i = 0; i < ndisp - 1; ++i )
    {
        /* A pinned boundary (NO_RESIZE column) offers no band. */
        if ( pin_mask & ( 1u << i ) ) continue;

        f32        bx = col_x[ i + 1 ];   /* boundary between slot i and slot i+1 */
        gui_rect_t hr = { bx - thick * 0.5f, band_box.y, thick, band_box.h };

        /* Key the grab off the LOGICAL column: a reorder drag moves a column between display
           slots mid-frame, and a slot-keyed id would hand the live grab to its new neighbour. */
        gui_id_t rid = id_combine( id, (gui_id_t)( 0x5200u + disp[ i ] ) );

        bool active = false;
        bool over   = item_grab( rid, hr, front, &active );
        if ( over || active ) hot = i;

        /* Double-click on the boundary = size-to-fit, the standard spreadsheet gesture.
           Reported out: the fit measure lives with the chrome that emitted the cells. */
        if ( over && s_io.mouse_double[ 0 ] )
        {
            if ( out_dbl ) *out_dbl = i;
            s_table_fit_press = rid;
        }

        bool fit_gesture = ( rid == s_table_fit_press );
        if ( !active || fit_gesture ) continue;

        /* Pair-resize proper: only with room to keep both sides above the floor. */
        f32  pair_w = col_w[ i ] + col_w[ i + 1 ];
        bool room   = ( pair_w >= 2.0f * min_w );
        if ( !room ) continue;

        f32 new_left = clampf( s_io.mouse_x - col_x[ i ], min_w, pair_w - min_w );
        p->col_w[ disp[ i ]     ] = new_left;
        p->col_w[ disp[ i + 1 ] ] = pair_w - new_left;

        table_tracks_resolve( p, init_w, init_n, disp, ndisp, x, w, col_x, col_w );
    }

    return hot;
}

void
table_sort_click( gui_table_persist_t* p, i32 col, bool tristate, bool prefer_desc )
{
    bool same_col = ( (i32)p->sort_col == col + 1 );
    if ( !same_col )
    {
        p->sort_col = (i8)( col + 1 );   /* stored 1-based; 0 = unsorted */
        p->sort_dir = (i8)( prefer_desc ? 1 : 0 );
        return;
    }

    /* Same column: flip.  Tristate adds a third step -- leaving the SECOND direction (the one
       opposite the column's preferred first) drops back to unsorted, so a table can be returned
       to its source order without a reset. */
    i8   second_dir = (i8)( prefer_desc ? 0 : 1 );
    bool at_second  = ( p->sort_dir == second_dir );

    if ( tristate && at_second )
        p->sort_col = 0;
    else
        p->sort_dir = (i8)( p->sort_dir == 0 ? 1 : 0 );
}

/* Compare two user rows by the sort column via the value callback.  Returns the ascending
   ordering (<0 / 0 / >0); the caller applies the sort direction.  A numeric key on either side
   forces a numeric compare (a missing key counts as zero); otherwise both sides compare as text. */
static i32
table_sort_value_cmp( i32 a, i32 b, i32 col, gui_table_sort_value_fn fn, void* user )
{
    gui_table_sort_value_t va = { 0 }, vb = { 0 };
    fn( a, col, &va, user );
    fn( b, col, &vb, user );

    if ( va.is_num || vb.is_num )
    {
        f64 da = va.is_num ? va.num : 0.0;
        f64 db = vb.is_num ? vb.num : 0.0;
        return (i32)( da > db ) - (i32)( da < db );
    }

    const char* sa = va.str ? va.str : "";
    const char* sb = vb.str ? vb.str : "";
    return strcmp( sa, sb );
}

bool
table_order_sort( i32* order, i32 count, i32 col, bool desc,
                  gui_table_sort_value_fn val_fn, gui_table_sort_cmp_fn cmp_fn, void* user )
{
    if ( !order || count < 2 )  return false;
    if ( !val_fn && !cmp_fn )   return false;

    /* Stable insertion sort -- keeps the input order among equal keys and needs no scratch
       buffer.  It runs only on the click frame, so the O(n^2) worst case is paid once per sort,
       not per frame; swap in a faster stable sort here if very large tables ever need it. */
    for ( i32 i = 1; i < count; ++i )
    {
        i32 key = order[ i ];
        i32 j   = i - 1;
        while ( j >= 0 )
        {
            i32 c = cmp_fn ? cmp_fn( order[ j ], key, col, desc, user )
                           : table_sort_value_cmp( order[ j ], key, col, val_fn, user );
            if ( !cmp_fn && desc ) c = -c;   /* val_fn compares ascending; cmp_fn owns direction */

            if ( c <= 0 ) break;   /* <= keeps equal keys stable (no swap on tie) */
            order[ j + 1 ] = order[ j ];
            --j;
        }
        order[ j + 1 ] = key;
    }

    return true;
}

gui_span_t
table_rows_span( i32 count, f32 h, f32 top )
{
    layout_frame_t* f     = lf();
    f32             pitch = h + (f32)WIDGET_GAP;

    /* Reserve all `count` rows of extent (last row's bottom, no trailing gap) so the scrollbar
       range and clamp see the full table regardless of how few rows the loop emits. */
    extent_track( f, f->content_x, top + (f32)count * pitch - (f32)WIDGET_GAP );

    /* The visible window in row indices, clamped into [ 0, count ] and ordered. */
    i32 first = (i32)floorf( ( f->view.y - top ) / pitch );
    i32 last  = (i32)ceilf ( ( f->view.y + f->view.h - top ) / pitch );

    if ( first < 0 )     first = 0;
    if ( first > count ) first = count;
    if ( last  > count ) last  = count;
    if ( last  < first ) last  = first;

    layout_pen_jump( f, top + (f32)first * pitch );

    return ( gui_span_t ){ first, last };
}

// clang-format on
/*============================================================================================*/
