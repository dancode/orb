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
                      i32 ncols, f32 x, f32 w, f32* out_x, f32* out_w )
{
    f32 tracks[ GUI_TABLE_COLS_MAX ];

    for ( i32 i = 0; i < ncols; ++i )
    {
        /* Priority: user-resized persist width > setup fixed width > default stretch. */
        f32 track = 1.0f;    /* stretch / fill by default */

        if ( p->col_w[ i ] > 0.0f )
            track = p->col_w[ i ];
        else if ( i < init_n && init_w[ i ] > 1.0f )
            track = init_w[ i ];   /* explicit fixed width */

        tracks[ i ] = track;
    }

    /* Zero gap between columns -- dividers are chrome lines, not gaps. */
    layout_resolve_tracks( tracks, (u32)ncols, x, w, 0.0f, out_x, out_w );
}

/* Pair-resize: the dragged boundary grows the column on its left and gives the difference back
   to its right neighbor, so the pair's combined width -- and therefore every other column --
   stays put and the grabbed edge tracks the cursor exactly.  Both sides are written as fixed-px
   tracks (values > 1), which table_tracks_resolve then honors over setup / stretch.  The grab is
   the bare item_grab protocol (core/gui_item.c), claiming the left button like the dock
   splitter; a live drag re-resolves col_x / col_w so the columns laid out this frame reflect
   the drag with no lag. */
i32
table_resize_drag( gui_id_t id, gui_table_persist_t* p, const f32* init_w, i32 init_n,
                   i32 ncols, u32 pin_mask, gui_rect_t band_box, bool front,
                   f32 x, f32 w, f32* col_x, f32* col_w )
{
    const f32 thick = 6.0f;          /* grab band width, centered on the boundary */
    const f32 min_w = WIDGET_MIN_W;  /* floor for each side of the resized pair    */
    i32       hot   = -1;

    for ( i32 i = 0; i < ncols - 1; ++i )
    {
        /* A pinned boundary (NO_RESIZE column) offers no band. */
        if ( pin_mask & ( 1u << i ) ) continue;

        f32        bx  = col_x[ i + 1 ];   /* boundary between col i and col i+1 */
        gui_rect_t hr  = { bx - thick * 0.5f, band_box.y, thick, band_box.h };
        gui_id_t   rid = id_combine( id, (gui_id_t)( 0x5200u + i ) );

        bool active = false;
        if ( item_grab( rid, hr, front, &active ) )
            hot = i;

        if ( active )
        {
            hot = i;

            f32 pair_w = col_w[ i ] + col_w[ i + 1 ];
            if ( pair_w >= 2.0f * min_w )   /* enough room to keep both sides above the floor */
            {
                f32 new_left = clampf( s_io.mouse_x - col_x[ i ], min_w, pair_w - min_w );
                p->col_w[ i ]     = new_left;
                p->col_w[ i + 1 ] = pair_w - new_left;

                table_tracks_resolve( p, init_w, init_n, ncols, x, w, col_x, col_w );
            }
        }
    }

    return hot;
}

void
table_sort_click( gui_table_persist_t* p, i32 col )
{
    if ( (i32)p->sort_col == col + 1 )
    {
        p->sort_dir = ( p->sort_dir == 0 ) ? 1 : 0;
    }
    else
    {
        p->sort_col = (i8)( col + 1 );   /* stored 1-based; 0 = unsorted */
        p->sort_dir = 0;
    }
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
            i32 c;
            if ( cmp_fn )
            {
                c = cmp_fn( order[ j ], key, col, desc, user );
            }
            else
            {
                c = table_sort_value_cmp( order[ j ], key, col, val_fn, user );
                if ( desc ) c = -c;
            }
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
