/*==============================================================================================

    runtime_service/imgui/imgui_table.c -- Table layout: multi-column rows with cell clipping.

    A table is a region whose content is laid out in a column grid -- columns resolved once per
    table_begin, rows accumulated per table_next_row.  Each table_next_column narrows both the
    GPU scissor and the interaction hit-test clip to the cell rect, then restores them on the
    next transition, so widgets inside a cell render and interact only within that cell.

    Phase 1 -- column layout, cell clipping, next_row / next_column, auto-height rows.
    Phase 2 -- headers_row: non-scrolling header strip with sort click and sort indicator.
    Phase 3 -- decoration: row stripes, H/V dividers + outer frame, row/cell background overrides.

    Deferred open model:
      table_begin records outer_rect and exits.  The body region (layout_push_region) is opened
      lazily by table_open_body(), called from table_headers_row or the first table_next_row.
      This lets table_headers_row draw the header strip in the PARENT layout frame (correct clip,
      correct win_id for hover) before the body region narrows things.

    State model:
      - s_tab (imgui_table_t)             : per-frame active table; one open at a time.
      - s_tpool (imgui_table_persist_t[]) : persistent per-table state (col widths, sort),
                                            keyed by table id and LRU-reclaimed.

    Include order (unity build): included by imgui.c after imgui_layout_child.c so layout_push_region,
    layout_pop_region, layout_set_default, layout_row_break, layout_resolve_tracks, widget_behavior,
    lf, s_layout_sp, s_layout_stack, s_build, s_interaction, s_io, and the draw + style macros are
    all in scope.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Module-static state
==============================================================================================*/

static imgui_table_persist_t  s_tpool[ IMGUI_TABLE_POOL_CAP ];
static imgui_table_t          s_tab;
static bool                   s_tab_active;

/* Dummy scroll / content-size targets for the NOSCROLL body region.  layout_push_region
   writes these back each frame; they stay zero and are never read for anything meaningful. */
static f32  s_tab_scroll_x;
static f32  s_tab_scroll_y;
static f32  s_tab_content_w;
static f32  s_tab_content_h;

/*==============================================================================================
    Internal helpers
==============================================================================================*/

/* Find the persist slot for this table id, creating or reclaiming one as needed. */
static imgui_table_persist_t*
table_persist_get( imgui_id_t id )
{
    u32 cur_frame = g_ctx->retained.frame;

    /* Prefer an exact match. */
    for ( i32 i = 0; i < IMGUI_TABLE_POOL_CAP; ++i )
    {
        if ( s_tpool[ i ].id == id )
        {
            s_tpool[ i ].seen_frame = cur_frame;
            return &s_tpool[ i ];
        }
    }

    /* Fall back to a free or stale slot. */
    for ( i32 i = 0; i < IMGUI_TABLE_POOL_CAP; ++i )
    {
        if ( s_tpool[ i ].id == 0 || ( cur_frame - s_tpool[ i ].seen_frame ) > 2u )
        {
            imgui_table_persist_t* p = &s_tpool[ i ];
            memset( p, 0, sizeof( imgui_table_persist_t ) );
            p->id         = id;
            p->seen_frame = cur_frame;
            p->sort_col   = -1;
            return p;
        }
    }

    return NULL;   /* pool exhausted -- should not happen with TABLE_POOL_CAP=32 */
}

/* Resolve column positions and widths from setup data and persist overrides.
   x / w are the screen-space origin and total width of the column strip. */
static void
table_resolve_columns( imgui_table_t* t, f32 x, f32 w )
{
    f32 tracks[ IMGUI_TABLE_COLS_MAX ];

    for ( i32 i = 0; i < t->ncols; ++i )
    {
        /* Priority: user-resized persist width > setup init_w > default stretch. */
        f32 track = 1.0f;    /* stretch / fill by default */

        if ( t->persist && t->persist->col_w[ i ] > 0.0f )
        {
            track = t->persist->col_w[ i ];
        }
        else if ( i < t->col_setup_n )
        {
            imgui_table_col_t* col = &t->cols[ i ];
            if ( col->init_w > 1.0f )
                track = col->init_w;   /* explicit fixed width */
        }

        tracks[ i ] = track;
    }

    /* Zero gap between columns -- dividers (Phase 3) will be lines, not gaps. */
    layout_resolve_tracks( tracks, (u32)t->ncols, x, w, 0.0f, t->col_x, t->col_w );
}

/* Close the active cell clip and restore the saved interaction clip. */
static void
table_close_cell( imgui_table_t* t )
{
    if ( !t->in_cell ) return;
    draw_pop_clip_rect();
    s_build.clip_rect = t->saved_clip;
    t->in_cell        = false;
}

/* Advance the layout cursor past the current row. */
static void
table_end_row( imgui_table_t* t )
{
    if ( t->cur_row >= 0 )
        lf()->cursor_y = t->row_top + t->row_h + (f32)WIDGET_GAP;
}

/* Open the body region below the (optional) header strip and resolve columns inside it.
   Called from table_headers_row (after drawing the header) and from table_next_row
   (as an auto-open when the caller skips table_headers_row). */
static void
table_open_body( imgui_table_t* t )
{
    imgui_rect_t body = { t->outer_rect.x,
                          t->outer_rect.y + t->header_h,
                          t->outer_rect.w,
                          t->outer_rect.h - t->header_h };
    t->body_rect = body;

    /* NOSCROLL gives a layout frame, a clip to the body box, and automatic
       parent-pen advancement on pop.  The scroll dummies stay zero every frame. */
    layout_push_region( t->id, body, ( imgui_pad_t ){ 0, 0, 0, 0 }, IMGUI_WIN_NOSCROLL,
                        &s_tab_scroll_x, &s_tab_scroll_y,
                        &s_tab_content_w, &s_tab_content_h,
                        /* own_clip */ true );

    /* STACK mode so widget_next_rect_w uses cellx[0] / cellw[0], overridden per column. */
    layout_set_default( lf() );

    /* Resolve from the newly-opened region's content geometry. */
    table_resolve_columns( t, lf()->content_x, lf()->content_w );
    t->body_rect.x = lf()->content_x;
    t->body_rect.w = lf()->content_w;

    t->header_done = true;
}

/* Draw column dividers and the outer frame.  Called from table_end AFTER layout_pop_region so the
   lines render in the parent clip (covering header + body) and sit on top of the cell content.
   content_bottom is the screen-space bottom of the last drawn row (or the body top if no rows). */
static void
table_draw_borders( imgui_table_t* t, f32 content_bottom )
{
    const f32 x0 = t->outer_rect.x;
    const f32 y0 = t->outer_rect.y;
    const f32 w  = t->outer_rect.w;
    const f32 h  = content_bottom - y0;   /* used height: header strip + drawn rows */
    if ( h <= 0.0f ) return;

    /* Vertical dividers between columns, full used height (run through the header strip too). */
    if ( t->flags & IMGUI_TABLE_BORDERS_V )
    {
        for ( i32 i = 1; i < t->ncols; ++i )
            draw_push_rect_filled( t->col_x[ i ], y0, 1.0f, h, 0, 0, 0, 0, 0, COL_BORDER );
    }

    /* Outer frame around the used table box. */
    if ( t->flags & IMGUI_TABLE_BORDERS_OUTER )
        draw_push_rect_outline( x0, y0, w, h, 1.0f, 0, COL_BORDER );
}

/*==============================================================================================
    Public API
==============================================================================================*/

bool
imgui_table_begin( const char* id_str, i32 ncols, imgui_table_flags_t flags, f32 height )
{
    /* Nested tables are not yet supported. */
    if ( s_tab_active ) return false;
    if ( ncols < 1 ) ncols = 1;
    if ( ncols > IMGUI_TABLE_COLS_MAX ) ncols = IMGUI_TABLE_COLS_MAX;

    layout_frame_t* parent = lf();
    layout_row_break( parent );   /* finish any partial row in the parent template */

    imgui_id_t id = id_combine( id_seed(), id_hash( id_str ) );

    f32 w = parent->content_w;
    f32 h = ( height > 0.0f ) ? height : ( (f32)WIDGET_H + (f32)WIDGET_GAP ) * 8.0f;

    imgui_table_t* t = &s_tab;
    memset( t, 0, sizeof( imgui_table_t ) );
    t->id         = id;
    t->flags      = flags;
    t->ncols      = ncols;
    t->cur_col    = -1;
    t->cur_row    = -1;
    t->outer_rect = ( imgui_rect_t ){ parent->content_x, parent->cursor_y, w, h };
    t->persist    = table_persist_get( id );

    /* The body region is opened lazily by table_open_body (called from table_headers_row or
       the first table_next_row).  Column geometry is resolved there. */
    s_tab_active = true;
    return true;
}

void
imgui_table_end( void )
{
    if ( !s_tab_active ) return;

    imgui_table_t* t = &s_tab;

    /* Guard: open body briefly so layout_pop_region can advance the parent cursor correctly
       even when called immediately after table_begin with no rows. */
    if ( !t->header_done )
        table_open_body( t );

    table_close_cell( t );
    table_end_row( t );

    /* Bottom of the used area: last row's bottom edge, or the body top when no rows were emitted.
       Captured before pop so borders (drawn after pop) frame exactly the content that was laid out. */
    f32 content_bottom = ( t->cur_row >= 0 ) ? ( t->row_top + t->row_h ) : t->body_rect.y;

    /* Restore the full-width content column before pop so layout_pop_region measures the correct
       horizontal extent (content_max_x tracks the rightmost draw edge for hscroll decisions). */
    layout_frame_t* f = lf();
    f->content_x  = t->body_rect.x;
    f->content_w  = t->body_rect.w;
    f->cellx[ 0 ] = t->body_rect.x;
    f->cellw[ 0 ] = t->body_rect.w;

    layout_pop_region();

    /* Dividers + outer frame render last, in the parent clip, on top of cell content. */
    table_draw_borders( t, content_bottom );

    s_tab_active = false;
}

void
imgui_table_setup_column( const char* label, imgui_table_col_flags_t flags, f32 width )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;
    if ( t->col_setup_n >= t->ncols ) return;

    imgui_table_col_t* col = &t->cols[ t->col_setup_n ];

    if ( label )
    {
        i32 i = 0;
        while ( i < 31 && label[ i ] ) { col->label[ i ] = label[ i ]; ++i; }
        col->label[ i ] = '\0';
    }

    col->flags  = flags;
    col->init_w = width;
    ++t->col_setup_n;
    /* Column geometry is resolved lazily in table_open_body. */
}

/* Draw the non-scrolling header strip, detect sort clicks, then open the body region.
   Must be called after all table_setup_column calls and before the first table_next_row. */
void
imgui_table_headers_row( void )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;

    /* Resolve column geometry in the parent frame (header lives in the parent clip). */
    table_resolve_columns( t, t->outer_rect.x, t->outer_rect.w );

    const f32 hy = t->outer_rect.y;
    const f32 hh = (f32)WIDGET_H;
    t->header_h  = hh;

    /* --- clip the entire header strip ---------------------------------------- */
    imgui_rect_t hdr     = { t->outer_rect.x, hy, t->outer_rect.w, hh };
    imgui_rect_t old_clip = s_build.clip_rect;
    draw_push_clip_rect( hdr.x, hdr.y, hdr.w, hdr.h );
    s_build.clip_rect = rect_intersect( old_clip, hdr );

    /* Full-width header background. */
    draw_push_rect_filled( t->outer_rect.x, hy, t->outer_rect.w, hh, 0, 0, 0, 0, 0, COL_TITLE_BG );

    /* --- per-column cells ---------------------------------------------------- */
    bool sortable = ( t->flags & IMGUI_TABLE_SORTABLE ) != 0;
    i8   sort_col = t->persist ? t->persist->sort_col : -1;

    for ( i32 i = 0; i < t->ncols; ++i )
    {
        f32 cx = t->col_x[ i ];
        f32 cw = t->col_w[ i ];

        imgui_table_col_t* col  = ( i < t->col_setup_n ) ? &t->cols[ i ] : NULL;
        bool no_sort = !sortable || ( col && ( col->flags & IMGUI_TABLE_COL_NO_SORT ) );

        /* Per-column clip so a long label does not bleed into the next cell. */
        draw_push_clip_rect( cx, hy, cw, hh );

        /* Interaction -- runs in the parent win context so hover detection is correct. */
        imgui_id_t     hid = id_combine( t->id, (imgui_id_t)( i + 1 ) );
        imgui_rect_t   cr  = { cx, hy, cw, hh };
        widget_state_t st  = widget_behavior( hid, cr, WIDGET_KIND_BUTTON );

        /* Sort click: cycle sort column / direction. */
        if ( !no_sort && st.clicked )
        {
            if ( sort_col == (i8)i )
            {
                t->persist->sort_dir = ( t->persist->sort_dir == 0 ) ? 1 : 0;
            }
            else
            {
                t->persist->sort_col = (i8)i;
                t->persist->sort_dir = 0;
                sort_col             = (i8)i;
            }
            t->sort_dirty = true;
        }

        /* Hover / active tint drawn on top of the header bg. */
        if ( !no_sort && ( st.hover || st.active ) )
        {
            u32 tint = st.active ? COL_WIDGET_ACT : COL_WIDGET_HOT;
            draw_push_rect_filled( cx, hy, cw, hh, 0, 0, 0, 0, 0, tint );
        }

        /* Column label text. */
        const char* lbl = ( col && col->label[ 0 ] ) ? col->label : "";
        draw_push_text( cx + (f32)WIDGET_PAD, hy + (f32)WIDGET_GAP, COL_TEXT, lbl );

        /* Sort indicator triangle on the active sort column. */
        if ( t->persist && sort_col == (i8)i )
        {
            const f32 aw = 6.0f, ah = 4.0f;
            f32 tx = cx + cw - 4.0f;           /* x anchor at right edge minus 4px pad */
            f32 ty = hy + ( hh - ah ) * 0.5f;  /* vertically centered */

            if ( t->persist->sort_dir == 0 )    /* ascending: tip at top */
                draw_push_triangle( tx - aw * 0.5f, ty + ah, tx, ty, tx + aw * 0.5f, ty + ah,
                                    0, COL_TEXT );
            else                                 /* descending: tip at bottom */
                draw_push_triangle( tx - aw * 0.5f, ty, tx, ty + ah, tx + aw * 0.5f, ty,
                                    0, COL_TEXT );
        }

        draw_pop_clip_rect();   /* per-column clip */
    }

    draw_pop_clip_rect();       /* header strip clip */
    s_build.clip_rect = old_clip;

    /* Open the body region immediately below the drawn header strip. */
    table_open_body( t );
}

void
imgui_table_next_row( f32 min_h )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;

    /* Auto-open the body if table_headers_row was not called. */
    if ( !t->header_done )
        table_open_body( t );

    table_close_cell( t );
    table_end_row( t );   /* advance past the previous row if any */

    f32 h    = ( min_h > 0.0f ) ? min_h : (f32)WIDGET_H;
    t->cur_row++;
    t->cur_col  = -1;
    t->row_h    = h;
    t->row_top  = lf()->cursor_y;

    /* Alternating row tint, drawn first so cell content (emitted after next_column) sits on top.
       Auto-clipped to the body region by the active draw clip. */
    if ( ( t->flags & IMGUI_TABLE_ROW_STRIPES ) && ( t->cur_row & 1 ) )
        draw_push_rect_filled( t->body_rect.x, t->row_top, t->body_rect.w, t->row_h,
                               0, 0, 0, 0, 0, IMGUI_COLOR( 0xFF, 0xFF, 0xFF, 0x12 ) );

    /* Horizontal divider in the gap above this row (between the previous row and this one). */
    if ( ( t->flags & IMGUI_TABLE_BORDERS_H ) && t->cur_row > 0 )
        draw_push_rect_filled( t->body_rect.x, t->row_top, t->body_rect.w, 1.0f,
                               0, 0, 0, 0, 0, COL_BORDER );
}

bool
imgui_table_next_column( void )
{
    if ( !s_tab_active ) return false;
    imgui_table_t* t = &s_tab;

    table_close_cell( t );   /* pop previous cell clip if any */

    t->cur_col++;
    if ( t->cur_col >= t->ncols ) return false;

    /* Set the layout pen and column geometry so widget_next_rect_w returns the correct cell. */
    layout_frame_t* f = lf();
    f32 cx = t->col_x[ t->cur_col ];
    f32 cw = t->col_w[ t->cur_col ];

    f->cursor_y   = t->row_top;   /* reset to the row's top for each column */
    f->col        = 0;            /* ensure row_y is latched by the first widget in the cell */
    f->content_x  = cx;
    f->content_w  = cw;
    f->cellx[ 0 ] = cx;
    f->cellw[ 0 ] = cw;

    /* Push cell clip.  draw_push_clip_rect intersects with its parent automatically (the body
       clip from layout_push_region).  Mirror the intersection into s_build.clip_rect so
       hit-testing is consistent with what the GPU scissors. */
    imgui_rect_t cell = { cx, t->row_top, cw, t->row_h };
    t->saved_clip = s_build.clip_rect;
    draw_push_clip_rect( cell.x, cell.y, cell.w, cell.h );
    s_build.clip_rect = rect_intersect( t->saved_clip, cell );
    t->in_cell = true;

    return true;
}

bool
imgui_table_set_column_index( i32 col )
{
    if ( !s_tab_active ) return false;
    imgui_table_t* t = &s_tab;

    table_close_cell( t );

    if ( col < 0 || col >= t->ncols ) return false;
    t->cur_col = col - 1;    /* table_next_column will increment to col */
    return imgui_table_next_column();
}

i32
imgui_table_get_column_count( void )
{
    return s_tab_active ? s_tab.ncols : 0;
}

i32
imgui_table_get_column_index( void )
{
    return s_tab_active ? s_tab.cur_col : -1;
}

i32
imgui_table_get_row_index( void )
{
    return s_tab_active ? s_tab.cur_row : -1;
}

/* Return true on the frame a header sort-click occurred; fills out and clears the dirty flag. */
bool
imgui_table_get_sort_specs( imgui_table_sort_specs_t* out )
{
    if ( !s_tab_active ) return false;
    imgui_table_t* t = &s_tab;
    if ( !t->sort_dirty || !t->persist || t->persist->sort_col < 0 ) return false;

    if ( out )
    {
        out->col        = (i32)t->persist->sort_col;
        out->descending = ( t->persist->sort_dir != 0 );
    }
    t->sort_dirty = false;
    return true;
}

/* Tint the current row or cell.  Call after table_next_row (for ROW) or after table_next_column
   (for CELL) and before emitting the cell's content, so the fill lands under that content. */
void
imgui_table_set_bg_color( imgui_table_bg_target_t target, u32 abgr )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;
    if ( t->cur_row < 0 ) return;

    if ( target == IMGUI_TABLE_BG_ROW )
    {
        /* Full-row fill across all columns; auto-clipped to the body region. */
        draw_push_rect_filled( t->body_rect.x, t->row_top, t->body_rect.w, t->row_h,
                               0, 0, 0, 0, 0, abgr );
    }
    else if ( target == IMGUI_TABLE_BG_CELL && t->cur_col >= 0 )
    {
        /* Current cell fill; the active cell clip keeps it in bounds. */
        draw_push_rect_filled( t->col_x[ t->cur_col ], t->row_top, t->col_w[ t->cur_col ],
                               t->row_h, 0, 0, 0, 0, 0, abgr );
    }
}

// clang-format on
/*============================================================================================*/
