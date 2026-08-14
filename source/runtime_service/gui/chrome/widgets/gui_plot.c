/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_plot.c -- Array plot widgets.

    plot_lines / plot_histogram: the read-only sparkline over a caller-owned f32 array (the
    Dear ImGui PlotLines / PlotHistogram analogues).  One shared emitter does everything but
    the marks: a framed tall cell with a trailing label (the listbox / multiline pattern --
    gui_field_row is one-row by construction, so tall widgets never route through it), scale
    resolve, hover pick, and the "index: value" tooltip; the kind only selects connected
    segments vs baseline bars.

    The widget never stores or samples data -- the caller owns the array and passes it every
    frame.  `offset` rotates the read order so a ring buffer plots as a scrolling strip
    without the caller ever memmoving: values[(i + offset) % count] is sample i.

    Hover is display assistance, not editing: item_state runs for the hover/tooltip anchor
    only (nav-skipped like a scrollbar -- a plot is no keyboard target) and the returned
    press/click is ignored.

    Included by gui_chrome.c among the widget family files, sharing their vocabulary:
    item_state (core), cell_next (flow), gui_field_row (stock), and the COL_* / WIDGET_ /
    WIN_ macros (style/gui_style.h).

==============================================================================================*/
// clang-format off

/* Most polyline points one plot emits.  More samples than this (or than the cell has pixels)
   stride-sample evenly across the array -- a sparkline past one point per pixel is pure
   vertex waste, and the stack point buffer stays a fixed 4 KB. */
#define PLOT_MAX_PTS 512

/* Sample i in display order, through the ring rotation.  offset may be any sign / magnitude. */
static f32
plot_value( const f32* values, i32 count, i32 offset, i32 i )
{
    i32 j = ( i + offset ) % count;
    if ( j < 0 ) j += count;
    return values[ j ];
}

/* Resolve the vertical scale: a caller range when scale_min < scale_max, else the data's own
   min/max (a flat line centers by widening the range around the value). */
static void
plot_scale( const f32* values, i32 count, f32* lo, f32* hi )
{
    if ( *lo < *hi ) return;

    f32 mn = values[ 0 ], mx = values[ 0 ];
    for ( i32 i = 1; i < count; ++i )
    {
        if ( values[ i ] < mn ) mn = values[ i ];
        if ( values[ i ] > mx ) mx = values[ i ];
    }
    if ( mx - mn < 1e-6f ) { mn -= 0.5f; mx += 0.5f; }   /* flat data still spans a band */
    *lo = mn;
    *hi = mx;
}

/* The shared emitter -- `bars` picks histogram baseline bars over connected line segments. */
static void
plot_emit( bool bars, const char* label, const f32* values, i32 count, i32 offset,
           const char* overlay, f32 scale_min, f32 scale_max, f32 height )
{
    gui_id_t id = item_id( label );
    if ( !values ) count = 0;
    if ( height <= 0.0f ) height = WIDGET_H * 3.0f;

    /* Box width: fill the line after reserving the trailing label (the listbox / multiline
       sizing).  NOT gui_field_row -- that seam is one-row by construction (it consumes a
       WIDGET_H cell and arms its control track as next_item_rect, which would override the
       tall cell below), so like every tall widget the plot reserves and draws its trailing
       label itself. */
    f32 lab_w = ( label_vis_len( label ) > 0 ) ? label_width( label ) + WIDGET_PAD : 0.0f;
    f32 w     = gui_view_avail().x - lab_w;
    if ( w < WIDGET_H * 4.0f ) w = WIDGET_H * 4.0f;

    gui_rect_t r = cell_next_w( w, height );

    /* Hover only: the tooltip anchor and the highlight pick.  nav.skip -- a display plot is no
       keyboard target (like a scrollbar); the returned press/click is deliberately unused. */
    s_scope.nav.skip = true;
    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    /* The data area inside the frame line. */
    gui_rect_t inner = gui_rect_pad( r, WIN_BORDER + 1.0f );

    /* Hovered sample from the cursor's horizontal fraction -- index math, independent of any
       draw-side downsampling below.  Lines pick the nearest vertex; bars the bucket under it. */
    i32 hov = -1;
    if ( st.hover && count > 0 && inner.w > 0.0f )
    {
        f32 t = saturate( ( s_io.mouse_x - inner.x ) / inner.w );
        hov   = bars ? (i32)( t * (f32)count ) : (i32)( t * (f32)( count - 1 ) + 0.5f );
        if ( hov > count - 1 ) hov = count - 1;
    }

    /* Paint gate: a scrolled-out plot skips its whole render prep (the sample walk is real
       per-row cost in a long list); hover/tooltip above already resolved. */
    if ( !draw_cull_box( r.x, r.y, r.w, r.h ) )
    {
        draw_fill( r, COL_BG_DIM );
        draw_outline( r, WIN_BORDER, COL_BORDER_IDLE );

        f32 lo = scale_min, hi = scale_max, inv_range = 0.0f;
        if ( count > 0 )
        {
            plot_scale( values, count, &lo, &hi );   /* guarantees hi > lo on return */
            inv_range = 1.0f / ( hi - lo );
        }

        if ( count > 0 && bars )
        {
            /* One bar per sample, never more bars than pixels: past that each bar shows its
               bucket's FIRST sample (a stride sample, not a max -- this is a sparkline).  Bars
               rise from the zero line when the range spans it, else from the range edge. */
            i32 n = count;
            if ( inner.w >= 1.0f && (f32)n > inner.w ) n = (i32)inner.w;

            f32 base_y = inner.y + ( 1.0f - saturate( ( 0.0f - lo ) * inv_range ) ) * inner.h;
            for ( i32 i = 0; i < n; ++i )
            {
                i32 src = (i32)( ( (i64)i * count ) / n );
                f32 v   = plot_value( values, count, offset, src );
                f32 x0  = inner.x + inner.w * ( (f32)i       / (f32)n );
                f32 x1  = inner.x + inner.w * ( (f32)( i + 1 ) / (f32)n );
                f32 bw  = x1 - x0;
                if ( bw > 2.0f ) bw -= 1.0f;                       /* 1px gap when it fits */

                f32 vy = inner.y + ( 1.0f - saturate( ( v - lo ) * inv_range ) ) * inner.h;
                f32 y0 = vy < base_y ? vy : base_y;
                f32 bh = fabsf( base_y - vy );
                if ( bh < 1.0f ) bh = 1.0f;                        /* a zero bar still shows */

                bool hot = ( hov >= 0 ) && ( (i32)( ( (i64)hov * n ) / count ) == i );
                draw_fill( ( gui_rect_t ){ x0, y0, bw, bh },
                           hot ? COL_ACCENT_HOT : COL_ACCENT_IDLE );
            }
        }
        else if ( count > 1 )
        {
            /* Connected segments; stride-sampled to the point budget / pixel width. */
            gui_vec2_t pts[ PLOT_MAX_PTS ];
            i32 n = count;
            if ( n > PLOT_MAX_PTS ) n = PLOT_MAX_PTS;
            if ( inner.w >= 2.0f && (f32)n > inner.w ) n = (i32)inner.w;
            if ( n < 2 ) n = 2;

            for ( i32 i = 0; i < n; ++i )
            {
                i32 src    = (i32)( ( (i64)i * ( count - 1 ) ) / ( n - 1 ) );
                f32 v      = plot_value( values, count, offset, src );
                pts[ i ].x = inner.x + inner.w * ( (f32)i / (f32)( n - 1 ) );
                pts[ i ].y = inner.y + ( 1.0f - saturate( ( v - lo ) * inv_range ) ) * inner.h;
            }
            gui_draw_polyline( pts, (u32)n, 1.0f, GUI_STROKE_CENTER, false, COL_ACCENT_IDLE );

            /* Hovered vertex marker -- a small accent square on the exact sample position. */
            if ( hov >= 0 )
            {
                f32 v  = plot_value( values, count, offset, hov );
                f32 mx = inner.x + inner.w * ( (f32)hov / (f32)( count - 1 ) );
                f32 my = inner.y + ( 1.0f - saturate( ( v - lo ) * inv_range ) ) * inner.h;
                draw_fill( ( gui_rect_t ){ mx - 2.0f, my - 2.0f, 5.0f, 5.0f }, COL_ACCENT_HOT );
            }
        }
        else if ( count == 1 )
        {
            /* A single sample has no segment: show it as the marker alone, centered. */
            f32 v  = plot_value( values, count, offset, 0 );
            f32 my = inner.y + ( 1.0f - saturate( ( v - lo ) * inv_range ) ) * inner.h;
            draw_fill( ( gui_rect_t ){ inner.x + inner.w * 0.5f - 2.0f, my - 2.0f, 5.0f, 5.0f },
                       COL_ACCENT_IDLE );
        }

        /* Overlay caption, centered at the top of the frame and fitted to the inner width. */
        if ( overlay && overlay[ 0 ] )
        {
            f32 tw = font_text_w( overlay );
            f32 tx = r.x + ( r.w - tw ) * 0.5f;
            if ( tx < r.x + WIDGET_PAD ) tx = r.x + WIDGET_PAD;
            draw_text_fit_n( tx, r.y + WIN_BORDER + 1.0f, COL_TEXT_PRIMARY_IDLE, overlay, 0xFFFFFFFFu,
                             r.w - 2.0f * WIDGET_PAD );
        }

        /* Trailing label past the box's right edge, seated on the first row (listbox_end). */
        if ( lab_w > 0.0f )
            draw_label( r.x + r.w + WIDGET_PAD, text_center_y( r.y, WIDGET_H ),
                        COL_TEXT_PRIMARY_IDLE, label );
    }

    /* The hovered sample as "index: value", bound to the item just emitted. */
    if ( hov >= 0 )
    {
        char buf[ 48 ];
        fmt_snprintf( buf, sizeof( buf ), "%d: %g", hov, plot_value( values, count, offset, hov ) );
        gui_set_item_tooltip( buf );
    }
}

void
gui_plot_lines( const char* label, const f32* values, i32 count, i32 offset,
                const char* overlay, f32 scale_min, f32 scale_max, f32 h )
{
    plot_emit( false, label, values, count, offset, overlay, scale_min, scale_max, h );
}

void
gui_plot_histogram( const char* label, const f32* values, i32 count, i32 offset,
                    const char* overlay, f32 scale_min, f32 scale_max, f32 h )
{
    plot_emit( true, label, values, count, offset, overlay, scale_min, scale_max, h );
}

// clang-format on
/*============================================================================================*/
