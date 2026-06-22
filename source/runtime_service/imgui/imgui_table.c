/*==============================================================================================

    runtime_service/imgui/imgui_table.c -- Table layout: multi-column rows with cell clipping.

    A table is a region whose content is laid out in a column grid -- columns resolved once per
    table_begin, rows accumulated per table_next_row.  Each table_next_column narrows both the
    GPU scissor and the interaction hit-test clip to the cell rect, then restores them on the
    next transition, so widgets inside a cell render and interact only within that cell.

    State model:
      - s_tab (imgui_table_t)        : per-frame active table; one open at a time (no nesting).
      - s_tpool (imgui_table_persist_t[]) : small pool of persistent per-table state (column widths,
                                            sort choice), keyed by table id and LRU-reclaimed.

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

/* Resolve column positions and widths from the column setup and persist overrides.
   Uses lf()->content_x and lf()->content_w so the geometry matches the open region. */
static void
table_resolve_columns( imgui_table_t* t )
{
    f32 tracks[ IMGUI_TABLE_COLS_MAX ];

    for ( i32 i = 0; i < t->ncols; ++i )
    {
        /* Priority: user-resized persist width > setup init_w > default stretch. */
        f32 w = 1.0f;    /* stretch / fill by default */

        if ( t->persist && t->persist->col_w[ i ] > 0.0f )
        {
            w = t->persist->col_w[ i ];
        }
        else if ( i < t->col_setup_n )
        {
            imgui_table_col_t* col = &t->cols[ i ];
            if ( col->init_w > 1.0f )
                w = col->init_w;   /* explicit fixed or fraction width from setup */
        }

        tracks[ i ] = w;
    }

    /* Zero gap between columns for Phase 1; borders (Phase 3) will be lines, not gaps. */
    layout_resolve_tracks( tracks, (u32)t->ncols,
                           lf()->content_x, lf()->content_w, 0.0f,
                           t->col_x, t->col_w );

    /* Mirror into body_rect for table_end's restore. */
    t->body_rect.x = lf()->content_x;
    t->body_rect.w = lf()->content_w;
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

/* Advance the layout cursor past the current row and record the next row_top. */
static void
table_end_row( imgui_table_t* t )
{
    if ( t->cur_row >= 0 )
        lf()->cursor_y = t->row_top + t->row_h + (f32)WIDGET_GAP;
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

    imgui_rect_t box = { parent->content_x, parent->cursor_y, w, h };

    imgui_table_t* t = &s_tab;
    memset( t, 0, sizeof( imgui_table_t ) );
    t->id         = id;
    t->flags      = flags;
    t->ncols      = ncols;
    t->cur_col    = -1;
    t->cur_row    = -1;
    t->outer_rect = box;
    t->persist    = table_persist_get( id );

    /* Open a NOSCROLL region for the table body.  This gives us our own layout frame, a
       scrollbars-free clip to the box, and automatic parent-pen advancement on pop. */
    layout_push_region( id, box, ( imgui_pad_t ){ 0, 0, 0, 0 }, IMGUI_WIN_NOSCROLL,
                        &s_tab_scroll_x, &s_tab_scroll_y,
                        &s_tab_content_w, &s_tab_content_h,
                        /* own_clip */ true );

    /* Open STACK mode so widget_next_rect_w uses the STACK path and respects content_x / content_w
       which table_next_column overrides per cell. */
    layout_set_default( lf() );

    /* Resolve column geometry now that the region has set content_x / content_w. */
    table_resolve_columns( t );

    s_tab_active = true;
    return true;
}

void
imgui_table_end( void )
{
    if ( !s_tab_active ) return;

    imgui_table_t* t = &s_tab;

    table_close_cell( t );
    table_end_row( t );

    /* Restore the full-width content column before pop so layout_pop_region measures the correct
       horizontal extent (content_max_x tracks the rightmost draw edge for hscroll decisions). */
    layout_frame_t* f = lf();
    f->content_x  = t->body_rect.x;
    f->content_w  = t->body_rect.w;
    f->cellx[ 0 ] = t->body_rect.x;
    f->cellw[ 0 ] = t->body_rect.w;

    layout_pop_region();

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
        /* Copy the label -- strncpy-style without the strncpy header dependency. */
        i32 i = 0;
        while ( i < 31 && label[ i ] ) { col->label[ i ] = label[ i ]; ++i; }
        col->label[ i ] = '\0';
    }

    col->flags  = flags;
    col->init_w = width;
    ++t->col_setup_n;

    /* Re-resolve now so any change takes effect for subsequent next_column calls.
       The region is already open so lf() content geometry is correct. */
    table_resolve_columns( t );
}

void
imgui_table_next_row( f32 min_h )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;

    table_close_cell( t );
    table_end_row( t );   /* advance past the previous row if any */

    f32 h    = ( min_h > 0.0f ) ? min_h : (f32)WIDGET_H;
    t->cur_row++;
    t->cur_col  = -1;
    t->row_h    = h;
    t->row_top  = lf()->cursor_y;   /* pen is now at the start of the new row */
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

/* Phase 2+ stubs -- no-op / false until the respective phases land. */

void
imgui_table_headers_row( void )
{
    /* Phase 2: will draw the non-scrolling header strip, sort indicators, and resize handles. */
    ( void )s_tab_active;
}

bool
imgui_table_get_sort_specs( imgui_table_sort_specs_t* out )
{
    /* Phase 2: will return true on the frame a header column was clicked. */
    if ( out ) { out->col = -1; out->descending = false; }
    return false;
}

void
imgui_table_set_bg_color( imgui_table_bg_target_t target, u32 abgr )
{
    /* Phase 5: will tint the current row / cell background. */
    UNUSED( target );
    UNUSED( abgr );
}

// clang-format on
/*============================================================================================*/
