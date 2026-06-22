/*==============================================================================================

    runtime_service/imgui/imgui_table.c -- Table layout: multi-column rows over the columns engine.

    A table is a region whose content is laid out in a column grid -- columns resolved once per
    table_begin, rows accumulated per table_next_row.  table_next_column just points the layout pen
    at the column's cell (cellx / cellw); content stays inside the column because widgets self-fit
    to that width (text ellipsizes, labeled widgets shrink), exactly like the layout engine's
    columns mode.  There is therefore NO per-cell clip.

    ONE clip, like a window: the table pushes a single clip around the whole table box (header
    included) and runs the body scroll region inside it with own_clip=false -- the same pattern a
    window body uses.  The header is then drawn LAST (as chrome, like a title bar) so it overpaints
    any rows that scrolled up under it.  So the entire table -- header, rows, scrollbar -- lives in
    that one clip, and the body needs no clip of its own.

    Phase 1 -- column layout, next_row / next_column, auto-height rows.
    Phase 2 -- headers_row: header strip with sort click and sort indicator (drawn as chrome).
    Phase 3 -- decoration: row stripes, H/V dividers + outer frame, row/cell background overrides.
    Phase 4 -- scrolling body (IMGUI_TABLE_SCROLL_Y / _X): rows scroll under the one table clip and
               the chrome header covers the top, so the header stays frozen with no extra clip.
               Columns resolve inside the region (after the scrollbar gutter is reserved) so header
               and body cells stay aligned.

    Deferred open model:
      table_begin records outer_rect and exits.  table_open_body() (called lazily from
      table_headers_row or the first table_next_row) pushes the one table clip, opens the body
      region (own_clip=false), and resolves columns.  table_headers_row runs the header sort
      interaction up front but defers the header DRAW to table_end so it lands on top as chrome.

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

/* Advance the layout cursor past the current row. */
static void
table_end_row( imgui_table_t* t )
{
    if ( t->cur_row >= 0 )
        lf()->cursor_y = t->row_top + t->row_h + (f32)WIDGET_GAP;
}

/* Open the body region below the (optional) header strip and resolve columns inside it.
   Called from table_headers_row and from table_next_row (as an auto-open when the caller skips
   the header). */
static void
table_open_body( imgui_table_t* t )
{
    /* The ONE clip: the whole table box, header included.  Like a window body, the scroll region
       below runs own_clip=false inside this single clip, and the header is drawn last (as chrome)
       to overpaint rows that scroll up under it -- so the entire table needs just this one clip. */
    t->saved_clip = s_build.clip_rect;
    draw_push_clip_rect( t->outer_rect.x, t->outer_rect.y, t->outer_rect.w, t->outer_rect.h );
    s_build.clip_rect = rect_intersect( t->saved_clip, t->outer_rect );

    imgui_rect_t body = { t->outer_rect.x,
                          t->outer_rect.y + t->header_h,
                          t->outer_rect.w,
                          t->outer_rect.h - t->header_h };
    t->body_rect = body;

    /* Scrolling body needs persistent scroll + content storage (the region biases the pen by
       *scroll and writes *content back at pop).  Without a persist slot we cannot scroll, so fall
       back to the zeroed dummies + NOSCROLL.  SCROLL_Y leaves the vertical bar dynamic (shown only
       when content overflows); SCROLL_X adds the horizontal bar. */
    bool scroll = ( t->flags & ( IMGUI_TABLE_SCROLL_Y | IMGUI_TABLE_SCROLL_X ) ) && t->persist;

    imgui_win_flags_t rflags = IMGUI_WIN_NOSCROLL;
    f32 *sx = &s_tab_scroll_x, *sy = &s_tab_scroll_y, *cw = &s_tab_content_w, *ch = &s_tab_content_h;
    if ( scroll )
    {
        rflags = ( t->flags & IMGUI_TABLE_SCROLL_X ) ? IMGUI_WIN_HSCROLL : IMGUI_WIN_NONE;
        sx = &t->persist->scroll_x;  sy = &t->persist->scroll_y;
        cw = &t->persist->content_w; ch = &t->persist->content_h;
    }

    /* own_clip=false: the region does NOT push its own clip -- it reuses the table clip for drawing
       (so rows scroll under where the header will be) and only narrows the hit-test clip to the body
       box.  Exactly the window-body-with-chrome pattern. */
    layout_push_region( t->id, body, ( imgui_pad_t ){ 0, 0, 0, 0 }, rflags,
                        sx, sy, cw, ch, /* own_clip */ false );

    /* STACK mode so widget_next_rect_w uses cellx[0] / cellw[0], overridden per column. */
    layout_set_default( lf() );

    /* Resolve from the newly-opened region's content geometry -- content_w already excludes the
       vertical scrollbar gutter, so header and body columns share one authoritative width. */
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

    /* Tables are square: force a zero radius so the outer frame's corners meet flush with the
       rectangular table scissor instead of leaving a rounded-corner gap (see table_next_row). */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    /* Vertical dividers between columns, full used height (run through the header strip too). */
    if ( t->flags & IMGUI_TABLE_BORDERS_V )
    {
        for ( i32 i = 1; i < t->ncols; ++i )
            draw_push_rect_filled( t->col_x[ i ], y0, 1.0f, h, 0, 0, 0, 0, 0, COL_BORDER );
    }

    /* Outer frame around the used table box. */
    if ( t->flags & IMGUI_TABLE_BORDERS_OUTER )
        draw_push_rect_outline( x0, y0, w, h, 1.0f, 0, COL_BORDER );

    draw_set_rounding( save_round );
}

/* Defined below; table_end draws the header (as chrome) before its definition appears. */
static void table_draw_header( imgui_table_t* t );

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

    table_end_row( t );

    /* Bottom edge the borders frame to.  A scrolling table frames its fixed viewport box (rows
       scroll inside it); a non-scrolling table frames exactly the content it laid out -- the last
       row's bottom edge, or the body top when no rows were emitted.  Captured before pop. */
    f32 content_bottom;
    if ( t->flags & ( IMGUI_TABLE_SCROLL_Y | IMGUI_TABLE_SCROLL_X ) )
        content_bottom = t->outer_rect.y + t->outer_rect.h;
    else
        content_bottom = ( t->cur_row >= 0 ) ? ( t->row_top + t->row_h ) : t->body_rect.y;

    /* Restore the full-width content column before pop so layout_pop_region measures the correct
       horizontal extent (content_max_x tracks the rightmost draw edge for hscroll decisions). */
    layout_frame_t* f = lf();
    f->content_x  = t->body_rect.x;
    f->content_w  = t->body_rect.w;
    f->cellx[ 0 ] = t->body_rect.x;
    f->cellw[ 0 ] = t->body_rect.w;

    /* own_clip=false: layout_pop_region pops no draw clip; it restores the hit clip to the table
       box, measures content, and draws the scrollbar.  The one table clip is still on the draw
       stack, so the header (chrome) drawn next is bounded by it and lands on top of the rows. */
    layout_pop_region();

    if ( t->want_header )
        table_draw_header( t );

    /* Done with the one table clip: pop it and restore the caller's clip.  Borders render after, in
       the parent clip, so the outer frame outline is not half-clipped by the table box edge. */
    draw_pop_clip_rect();
    s_build.clip_rect = t->saved_clip;

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

/* Header sort interaction, run up front in table_headers_row.  The header strip sits above the body
   box, so the body hit clip set by table_open_body would reject clicks on it -- widen the hit clip to
   the whole table box for the queries, then restore it for the rows.  Records which column is hot /
   active so the deferred draw (table_draw_header, called as chrome in table_end) can tint it. */
static void
table_header_interact( imgui_table_t* t )
{
    imgui_rect_t body_hit = s_build.clip_rect;
    s_build.clip_rect = rect_intersect( t->saved_clip, t->outer_rect );

    const f32 hy = t->outer_rect.y;
    const f32 hh = t->header_h;

    bool sortable = ( t->flags & IMGUI_TABLE_SORTABLE ) != 0;
    i8   sort_col = t->persist ? t->persist->sort_col : -1;
    t->hdr_hot = -1;
    t->hdr_act = -1;

    for ( i32 i = 0; i < t->ncols; ++i )
    {
        imgui_table_col_t* col = ( i < t->col_setup_n ) ? &t->cols[ i ] : NULL;
        bool no_sort = !sortable || ( col && ( col->flags & IMGUI_TABLE_COL_NO_SORT ) );

        imgui_id_t     hid = id_combine( t->id, (imgui_id_t)( i + 1 ) );
        imgui_rect_t   cr  = { t->col_x[ i ], hy, t->col_w[ i ], hh };
        widget_state_t st  = widget_behavior( hid, cr, WIDGET_KIND_BUTTON );

        if ( !no_sort && st.hover )  t->hdr_hot = (i8)i;
        if ( !no_sort && st.active ) t->hdr_act = (i8)i;

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
    }

    s_build.clip_rect = body_hit;
}

/* Draw the header strip.  Called LAST (from table_end) within the one table clip so it overpaints
   any rows that scrolled up under it -- the same "chrome drawn last" trick a window uses for its
   title bar.  Visual only: the sort interaction already ran in table_header_interact. */
static void
table_draw_header( imgui_table_t* t )
{
    const f32 hy = t->outer_rect.y;
    const f32 hh = t->header_h;

    /* Square header fills -- tables never round (see table_next_row).  Text and the sort triangle
       below are unaffected by the ambient radius, so holding it at zero for the whole strip is safe. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    /* Full-width opaque header background (also the cover for rows scrolled under the header). */
    draw_push_rect_filled( t->outer_rect.x, hy, t->outer_rect.w, hh, 0, 0, 0, 0, 0, COL_TITLE_BG );

    i8 sort_col = t->persist ? t->persist->sort_col : -1;

    for ( i32 i = 0; i < t->ncols; ++i )
    {
        f32 cx = t->col_x[ i ];
        f32 cw = t->col_w[ i ];

        imgui_table_col_t* col = ( i < t->col_setup_n ) ? &t->cols[ i ] : NULL;

        /* Hover / active tint (state captured in table_header_interact). */
        if ( i == (i32)t->hdr_act || i == (i32)t->hdr_hot )
        {
            u32 tint = ( i == (i32)t->hdr_act ) ? COL_WIDGET_ACT : COL_WIDGET_HOT;
            draw_push_rect_filled( cx, hy, cw, hh, 0, 0, 0, 0, 0, tint );
        }

        /* Column label, self-fitted to the column (reserving the right pad for the sort triangle)
           so a long label ellipsizes instead of bleeding into the next column -- no per-column clip. */
        const char* lbl  = ( col && col->label[ 0 ] ) ? col->label : "";
        f32         lblx = cx + (f32)WIDGET_PAD;
        f32         lblw = ( cx + cw - (f32)WIDGET_PAD ) - lblx;
        draw_text_fit_n( lblx, hy + (f32)WIDGET_GAP, COL_TEXT, lbl, 0xFFFFFFFFu, lblw );

        /* Sort indicator triangle on the active sort column. */
        if ( t->persist && sort_col == (i8)i )
        {
            const f32 aw = 6.0f, ah = 4.0f;
            f32 tx = cx + cw - (f32)WIDGET_PAD - aw * 0.5f;  /* right edge inset by WIDGET_PAD */
            f32 ty = hy + ( hh - ah ) * 0.5f;                /* vertically centered */

            if ( t->persist->sort_dir == 0 )    /* ascending: tip at top */
                draw_push_triangle( tx - aw * 0.5f, ty + ah, tx, ty, tx + aw * 0.5f, ty + ah,
                                    0, COL_TEXT );
            else                                 /* descending: tip at bottom */
                draw_push_triangle( tx - aw * 0.5f, ty, tx, ty + ah, tx + aw * 0.5f, ty,
                                    0, COL_TEXT );
        }
    }

    draw_set_rounding( save_round );
}

/* Reserve the header strip and run its sort interaction up front; the strip itself is drawn last
   (as chrome) in table_end.  Must be called after all table_setup_column calls, before the first
   table_next_row. */
void
imgui_table_headers_row( void )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;

    t->header_h    = (f32)WIDGET_H;
    t->want_header = true;
    table_open_body( t );        /* opens body below the header, resolves columns */
    table_header_interact( t );  /* sort clicks now; the draw is deferred to table_end */
}

void
imgui_table_next_row( f32 min_h )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;

    /* Auto-open the body if table_headers_row was not called. */
    if ( !t->header_done )
        table_open_body( t );

    table_end_row( t );   /* advance past the previous row if any */

    f32 h    = ( min_h > 0.0f ) ? min_h : (f32)WIDGET_H;
    t->cur_row++;
    t->cur_col  = -1;
    t->row_h    = h;
    t->row_top  = lf()->cursor_y;

    /* Table fills are always square -- a rounded fill under the rectangular table scissor would
       leave a gap at each rounded corner that content behind shows through.  Save the ambient
       radius, force square for the chrome below, and restore so cell widgets keep their rounding. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    /* Alternating row tint, drawn first so cell content (emitted after next_column) sits on top.
       Auto-clipped to the body region by the active draw clip. */
    if ( ( t->flags & IMGUI_TABLE_ROW_STRIPES ) && ( t->cur_row & 1 ) )
        draw_push_rect_filled( t->body_rect.x, t->row_top, t->body_rect.w, t->row_h,
                               0, 0, 0, 0, 0, IMGUI_COLOR( 0xFF, 0xFF, 0xFF, 0x12 ) );

    /* Horizontal divider in the gap above this row (between the previous row and this one). */
    if ( ( t->flags & IMGUI_TABLE_BORDERS_H ) && t->cur_row > 0 )
        draw_push_rect_filled( t->body_rect.x, t->row_top, t->body_rect.w, 1.0f,
                               0, 0, 0, 0, 0, COL_BORDER );

    draw_set_rounding( save_round );
}

bool
imgui_table_next_column( void )
{
    if ( !s_tab_active ) return false;
    imgui_table_t* t = &s_tab;

    t->cur_col++;
    if ( t->cur_col >= t->ncols ) return false;

    /* Set the layout pen and column geometry so widget_next_rect_w returns the correct cell.
       Content stays inside the column because widgets self-fit to cellw (text ellipsizes, labeled
       widgets shrink) -- the same contract as the layout engine's columns mode, so no per-cell clip
       is pushed.  The one exterior clip (the body region) bounds the table as a whole. */
    layout_frame_t* f = lf();
    f32 cx = t->col_x[ t->cur_col ];
    f32 cw = t->col_w[ t->cur_col ];

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

    f->cursor_y   = t->row_top;   /* reset to the row's top for each column */
    f->col        = 0;            /* ensure row_y is latched by the first widget in the cell */
    f->content_x  = ix;
    f->content_w  = iw;
    f->cellx[ 0 ] = ix;
    f->cellw[ 0 ] = iw;

    return true;
}

bool
imgui_table_set_column_index( i32 col )
{
    if ( !s_tab_active ) return false;
    imgui_table_t* t = &s_tab;

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

/* Compare two user rows by the active sort column via the value callback.  Returns the ascending
   ordering (<0 / 0 / >0); the caller applies the sort direction.  A numeric key on either side
   forces a numeric compare (a missing key counts as zero); otherwise both sides compare as text. */
static i32
table_sort_value_cmp( i32 a, i32 b, i32 col, imgui_table_sort_value_fn fn, void* user )
{
    imgui_table_sort_value_t va = { 0 }, vb = { 0 };
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

/* Reorder a user-owned display-order index array to match the table's active sort.  order holds
   the user data indices in display order; count is its length.  Sorts ONLY on the frame a header
   click changed the sort (consuming the same dirty flag table_get_sort_specs reads), so it is cheap
   to call unconditionally every frame and the order is preserved across frames.  Pass val_fn for the
   built-in alphabetical / numeric sort (direction handled here), or cmp_fn for a full-control
   comparator (cmp_fn wins if both are given).  Returns true when it reordered the array. */
bool
imgui_table_sort_order( i32* order, i32 count, imgui_table_sort_value_fn val_fn,
                        imgui_table_sort_cmp_fn cmp_fn, void* user )
{
    if ( !s_tab_active || !order || count < 2 ) return false;
    imgui_table_t* t = &s_tab;
    if ( !t->sort_dirty || !t->persist || t->persist->sort_col < 0 ) return false;

    /* Consume the dirty flag regardless: with no comparator there is nothing to do, but we should
       not keep re-reporting the same click on later frames. */
    t->sort_dirty = false;
    if ( !val_fn && !cmp_fn ) return false;

    i32  col  = (i32)t->persist->sort_col;
    bool desc = ( t->persist->sort_dir != 0 );

    /* Stable insertion sort -- keeps the input order among equal keys and needs no scratch buffer.
       It runs only on the click frame, so the O(n^2) worst case is paid once per sort, not per
       frame; swap in a faster stable sort here if very large tables ever need it. */
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

/* Tint the current row or cell.  Call after table_next_row (for ROW) or after table_next_column
   (for CELL) and before emitting the cell's content, so the fill lands under that content. */
void
imgui_table_set_bg_color( imgui_table_bg_target_t target, u32 abgr )
{
    if ( !s_tab_active ) return;
    imgui_table_t* t = &s_tab;
    if ( t->cur_row < 0 ) return;

    /* Square fills only -- tables never round (see table_next_row). */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

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

    draw_set_rounding( save_round );
}

// clang-format on
/*============================================================================================*/
