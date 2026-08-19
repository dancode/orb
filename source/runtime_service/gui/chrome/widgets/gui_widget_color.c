/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_widget_color.c -- Color edit + color picker widgets.

    color_edit3 / color_edit4 -- an inline [swatch][R/G/B(/A) drag fields] row (RGB or HSV per
        the color-edit flags); clicking the swatch opens a popup hosting the full picker.
    color_picker3 / color_picker4 -- the Dear ImGui-style picker block: a saturation/value
        square, a hue bar, an alpha bar (picker4 without NO_ALPHA), a per-component drag row,
        and a hex entry field.  Embeddable inline anywhere; the color_edit popup reuses it.

    RGB is the storage; HSV is a per-frame working projection.  rgb->hsv degenerates at zero
    chroma (hue lost) and zero value (saturation lost), so the picker remembers the hue/sat
    that produced the last color it edited (s_hue_mem_*, keyed by the packed RGB it wrote) and
    restores them when the round-trip would collapse -- the pick cursor no longer snaps to red
    while a color passes through gray or black.

    Included by gui_chrome.c after gui_widget_slider.c, whose drag boxes (drag_float_box /
    drag_int_box) lay out every component row, and after gui_text_edit.c, whose
    input_field_edit drives the hex field.  The rest is the shared widget vocabulary:
    item_state (core), cell_next (flow), gui_field_row (stock), and the COL_* / WIDGET_ /
    WIN_ macros (style/gui_style.h).

==============================================================================================*/
// clang-format off

/* HSV -> RGB, h/s/v and r/g/b all in [0,1] (h wraps).  The standard six-sector conversion:
   which sector h falls in selects which of r/g/b holds the max/min/ramp role. */
static void
color_hsv_to_rgb( f32 h, f32 s, f32 v, f32* r, f32* g, f32* b )
{
    if ( s == 0.0f )
    {
        *r = *g = *b = v;
        return;
    }
    h = fmodf( h, 1.0f );
    if ( h < 0.0f ) h += 1.0f;
    h *= 6.0f;
    int i = (int)floorf( h );
    f32 f = h - (f32)i;
    f32 p = v * ( 1.0f - s );
    f32 q = v * ( 1.0f - s * f );
    f32 t = v * ( 1.0f - s * ( 1.0f - f ) );
    switch ( i )
    {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        case 5: *r = v; *g = p; *b = q; break;
        default: *r = v; *g = v; *b = v; break;
    }
}

/* RGB -> HSV, the inverse of color_hsv_to_rgb.  K tracks which channel permutation was applied
   (identity / rg-swap / rgb-rotate) so h can be recovered from the sorted channels; the 1e-20f
   terms guard the two divisions against a zero chroma / zero r (grayscale input). */
static void
color_rgb_to_hsv( f32 r, f32 g, f32 b, f32* h, f32* s, f32* v )
{
    f32 K = 0.0f;
    if ( g < b )
    {
        f32 tmp = g; g = b; b = tmp;
        K = -1.0f;
    }
    if ( r < g )
    {
        f32 tmp = r; r = g; g = tmp;
        K = -2.0f / 6.0f - K;
    }
    f32 chroma = r - ( g < b ? g : b );
    *h = fabsf( K + ( g - b ) / ( 6.0f * chroma + 1e-20f ) );
    *s = chroma / ( r + 1e-20f );
    *v = r;
}

/* Float channel -> u8, saturated and rounded -- the one quantization every preview / pack in
   this file shares. */
static u8
color_chan_u8( f32 x )
{
    return (u8)( saturate( x ) * 255.0f + 0.5f );
}

/* The RGB triple packed for identity comparison (alpha slot zero -- alpha never participates
   in the hue memory). */
static u32
color_pack_rgb( const f32* v )
{
    return GUI_COLOR( color_chan_u8( v[ 0 ] ), color_chan_u8( v[ 1 ] ), color_chan_u8( v[ 2 ] ), 0u );
}

/*==============================================================================================
    Hue/sat memory -- one slot, keyed by the packed RGB the picker last wrote (single-owner,
    like the drag anchor: one color is being edited at a time).  color_restore_hs undoes the
    two degenerate rgb->hsv collapses for that color: zero chroma erases hue (and h == 0 is
    indistinguishable from the h == 1 wrap), zero value erases saturation.
==============================================================================================*/

static u32 s_hue_mem_col = 0xFFFFFFFFu;   // packed RGB of the last edited color (never a valid pack: alpha byte set)
static f32 s_hue_mem_h;                   // hue that produced it
static f32 s_hue_mem_s;                   // saturation that produced it

static void
color_restore_hs( u32 packed_rgb, f32* h, f32* s, f32 v )
{
    if ( packed_rgb != s_hue_mem_col ) return;
    if ( *s <= 0.0f || ( *h == 0.0f && s_hue_mem_h == 1.0f ) ) *h = s_hue_mem_h;
    if ( v <= 0.0f ) *s = s_hue_mem_s;
}

static void
color_remember_hs( const f32* v, f32 h, f32 s )
{
    s_hue_mem_col = color_pack_rgb( v );
    s_hue_mem_h   = h;
    s_hue_mem_s   = s;
}

/*==============================================================================================
    color_comps_boxes -- one row of per-component drag boxes laid equally across `ctrl`,
    shared by the inline color_edit row and the picker's input block.  slot is the IDEAL
    (fractional) box width; the loop snaps cumulative edges rather than flooring slot per box,
    so the sub-pixel remainder is spread one pixel at a time across the borders as the row
    grows -- every border steps monotonically instead of the last box absorbing the whole
    remainder (border wobble).  Edits land in v (RGB mode) or hsv (HSV mode); the RGB <-> HSV
    writeback stays with the caller.
==============================================================================================*/

/* Integer formats are space-padded to a fixed 3-digit field (R/G/B/A max 255, H max 360,
   S/V max 100 -- all <= 3 digits) so the monospace label width never changes with the value.
   Constant width keeps every box's centered text on a stable column: it no longer ticks
   forward at a different resize threshold than its neighbors.  Float labels are already
   constant width ("0.00".."1.00").  File scope so color_comps_min_w measures with the SAME
   formats the boxes print -- the measure and the print cannot drift apart. */
static const char* s_col_fmt_rgb_i[] = { "R:%3d",  "G:%3d",  "B:%3d",  "A:%3d"  };
static const char* s_col_fmt_rgb_f[] = { "R:%.2f", "G:%.2f", "B:%.2f", "A:%.2f" };
static const char* s_col_fmt_hsv_i[] = { "H:%3d",  "S:%3d",  "V:%3d",  "A:%3d"  };
static const char* s_col_fmt_hsv_f[] = { "H:%.2f", "S:%.2f", "V:%.2f", "A:%.2f" };

/* The per-component integer ceiling (H is 0..360, S/V 0..100, everything else 0..255). */
static i32
color_comp_max( u32 i, bool is_hsv )
{
    return ( is_hsv && i == 0 ) ? 360 : ( ( is_hsv && i < 3u ) ? 100 : 255 );
}

/* The row width the comps boxes NEED, measured from the widest value each box can print with
   its own format (the "size to the widest element" rule): every box holds its longest text
   plus the WIDGET_PAD clip margins drag_value_text keeps, plus the inter-box gaps. */
static f32
color_comps_min_w( u32 comps, bool is_hsv, bool is_flt )
{
    f32 box_w = 0.0f;
    for ( u32 i = 0; i < comps; ++i )
    {
        char buf[ 16 ];
        if ( is_flt )
            fmt_snprintf( buf, sizeof( buf ), is_hsv ? s_col_fmt_hsv_f[ i ] : s_col_fmt_rgb_f[ i ], 1.0f );
        else
            fmt_snprintf( buf, sizeof( buf ), is_hsv ? s_col_fmt_hsv_i[ i ] : s_col_fmt_rgb_i[ i ],
                          color_comp_max( i, is_hsv ) );
        f32 w = font_text_w_n( buf, 0xFFFFFFFFu ) + 2.0f * WIDGET_PAD;
        if ( w > box_w ) box_w = w;
    }
    return (f32)comps * box_w + (f32)( comps - 1u ) * WIDGET_GAP;
}

static bool
color_comps_boxes( gui_id_t id, gui_rect_t ctrl, f32* v, f32* hsv, u32 comps, bool is_hsv, bool is_flt )
{
    bool changed = false;
    f32  gap     = WIDGET_GAP;
    f32  slot    = ( ctrl.w - gap * (f32)( comps - 1u ) ) / (f32)comps;   /* ideal fractional width */

    for ( u32 i = 0; i < comps; ++i )
    {
        f32 lead = ctrl.x + (f32)i * ( slot + gap );   /* ideal left edge of this box */
        f32 x0   = floorf( lead );
        f32 x1   = ( i + 1u < comps ) ? floorf( lead + slot ) : floorf( ctrl.x + ctrl.w );
        gui_rect_t drag_r = { x0, ctrl.y, x1 - x0, ctrl.h };

        gui_id_t cid = id_combine( id, 10u + i );
        f32      val = is_hsv ? hsv[ i ] : v[ i ];

        if ( is_flt )
        {
            const char* fmt = is_hsv ? s_col_fmt_hsv_f[ i ] : s_col_fmt_rgb_f[ i ];
            if ( drag_float_box( cid, drag_r, &val, 0.005f, 0.0f, 1.0f, fmt ) )
            {
                if ( is_hsv ) hsv[ i ] = val; else v[ i ] = val;
                changed = true;
            }
        }
        else
        {
            i32         max_v = color_comp_max( i, is_hsv );
            i32         ival  = (i32)( val * (f32)max_v + 0.5f );
            const char* fmt   = is_hsv ? s_col_fmt_hsv_i[ i ] : s_col_fmt_rgb_i[ i ];
            if ( drag_int_box( cid, drag_r, &ival, 1.0f, 0, max_v, fmt ) )
            {
                val = (f32)ival / (f32)max_v;
                if ( is_hsv ) hsv[ i ] = val; else v[ i ] = val;
                changed = true;
            }
        }
    }
    return changed;
}

/*==============================================================================================
    Hex entry field -- a "Hex" labeled row running the num_edit_field seed/edit/parse cycle
    over a "#RRGGBB[AA]" string: seed the scratch on focus gain, edit while focused, parse and
    commit on Enter and on focus loss.  One scratch slot keyed by id -- only one field is
    focused at a time, the same single-owner reason as s_num_edit_buf.
==============================================================================================*/

#define COLOR_HEX_CAP 12u

static gui_id_t s_hex_id;                    // field whose scratch is live (0 = none)
static char     s_hex_buf[ COLOR_HEX_CAP ];  // the one focused hex scratch

static void
color_hex_seed( char* buf, u32 cap, const f32* v, bool has_alpha )
{
    if ( has_alpha )
        fmt_snprintf( buf, (i32)cap, "#%02X%02X%02X%02X",
                      color_chan_u8( v[ 0 ] ), color_chan_u8( v[ 1 ] ),
                      color_chan_u8( v[ 2 ] ), color_chan_u8( v[ 3 ] ) );
    else
        fmt_snprintf( buf, (i32)cap, "#%02X%02X%02X",
                      color_chan_u8( v[ 0 ] ), color_chan_u8( v[ 1 ] ), color_chan_u8( v[ 2 ] ) );
}

static i32
color_hex_nibble( char c )
{
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return 10 + ( c - 'a' );
    if ( c >= 'A' && c <= 'F' ) return 10 + ( c - 'A' );
    return -1;
}

/* Parse "#RRGGBB" / "#RRGGBBAA" (leading '#' optional) into v.  Anything malformed leaves v
   untouched; a 6-digit string leaves an existing alpha untouched.  Returns true only when the
   committed color differs. */
static bool
color_hex_parse( const char* txt, f32* v, bool has_alpha )
{
    if ( *txt == '#' ) ++txt;

    i32 nib[ 8 ];
    u32 count = 0;
    for ( ; *txt && count < 8u; ++count, ++txt )
    {
        nib[ count ] = color_hex_nibble( *txt );
        if ( nib[ count ] < 0 ) return false;
    }
    if ( *txt || ( count != 6u && count != 8u ) ) return false;

    u32  comps   = ( has_alpha && count == 8u ) ? 4u : 3u;
    bool changed = false;
    for ( u32 i = 0; i < comps; ++i )
    {
        f32 nv = (f32)( nib[ i * 2u ] * 16 + nib[ i * 2u + 1u ] ) / 255.0f;
        if ( nv != v[ i ] )
        {
            v[ i ]  = nv;
            changed = true;
        }
    }
    return changed;
}

static bool
color_hex_field( gui_id_t hid, f32* v, bool has_alpha )
{
    gui_field_row( "Hex" );
    gui_rect_t       box = cell_next( WIDGET_H );
    gui_item_state_t st  = item_state( hid, box, ITEM_FOCUSABLE );

    /* The input_text frame draw (see input_text_begin): resting BG face, focus on the border. */
    draw_face( box, GUI_ROLE_BG, GUI_PHASE_IDLE );
    draw_outline( box, WIN_BORDER, col_field_border( st ) );

    bool changed = false;
    if ( st.focused && s_hex_id != hid )
    {
        color_hex_seed( s_hex_buf, COLOR_HEX_CAP, v, has_alpha );
        s_hex_id = hid;
    }

    if ( st.focused )
    {
        /* Hex vocabulary only, typed lowercase upgraded to the display case. */
        edit_filter_set( GUI_INPUT_FILTER_HEX | GUI_INPUT_FILTER_UPPERCASE );
        if ( input_field_edit( hid, box, st, s_hex_buf, COLOR_HEX_CAP, NULL, NULL ).enter )
            changed = color_hex_parse( s_hex_buf, v, has_alpha );
    }
    else
    {
        if ( s_hex_id == hid )
        {
            /* Focus left while our scratch was live: commit it, same as a numeric field. */
            changed  = color_hex_parse( s_hex_buf, v, has_alpha );
            s_hex_id = 0;
        }
        /* At rest the field just displays the current color. */
        char disp[ COLOR_HEX_CAP ];
        color_hex_seed( disp, COLOR_HEX_CAP, v, has_alpha );
        input_field_edit( hid, box, st, disp, sizeof( disp ), NULL, NULL );
    }
    return changed;
}

/*==============================================================================================
    color_picker_body -- the shared picker block: [SV square][hue bar][alpha bar] on one
    canvas row, then (unless NO_INPUTS) the component drag row and the hex field.  `id` scopes
    every sub-item; v is the caller's RGB(A) 0..1 storage.  gui_color_picker3/4 run it inline;
    color_edit_n hosts it in the swatch popup.
==============================================================================================*/

#define PICKER_BAR_W        14.0f    /* hue / alpha bar width */
#define PICKER_SIDE_MAX     240.0f   /* SV square edge cap -- keeps wide windows sane */
#define PICKER_SIDE_MIN     140.0f   /* SV square edge floor -- below this the block overflows the view instead */
#define PICKER_CURSOR_R     4.5f

/* The white-in-black marker line across a vertical bar at fraction t (0 = top). */
static void
picker_bar_marker( gui_rect_t bar, f32 t )
{
    f32 my = floorf( bar.y + t * ( bar.h - 1.0f ) );
    gui_rect_t m = { bar.x - 1.0f, my - 1.5f, bar.w + 2.0f, 4.0f };
    gui_draw_frame( m, GUI_COLOR( 255, 255, 255, 255 ), GUI_COLOR( 0, 0, 0, 200 ), 1.0f );
}

/* The six-sector hue rainbow, top (red) to bottom (red again). */
static const u32 s_hue_ramp[ 7 ] = {
    GUI_COLOR( 255,   0,   0, 255 ),
    GUI_COLOR( 255, 255,   0, 255 ),
    GUI_COLOR(   0, 255,   0, 255 ),
    GUI_COLOR(   0, 255, 255, 255 ),
    GUI_COLOR(   0,   0, 255, 255 ),
    GUI_COLOR( 255,   0, 255, 255 ),
    GUI_COLOR( 255,   0,   0, 255 ),
};

static bool
color_picker_body( gui_id_t id, f32* v, u32 n, gui_color_edit_flags_t flags )
{
    bool has_alpha = ( n == 4u && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) );
    bool is_hsv    = ( flags & GUI_COLOR_EDIT_DISPLAY_HSV ) != 0;
    bool is_flt    = ( flags & GUI_COLOR_EDIT_FLOAT ) != 0;
    bool changed   = false;

    /* HSV projection of the stored RGB, degenerate axes restored (see the file header). */
    f32 h, s, val;
    color_rgb_to_hsv( v[ 0 ], v[ 1 ], v[ 2 ], &h, &s, &val );
    color_restore_hs( color_pack_rgb( v ), &h, &s, val );

    /* ---- Canvas row: [SV square][hue bar][alpha bar] ---- */
    u32 bars = has_alpha ? 2u : 1u;
    f32 gap  = WIDGET_GAP;
    f32 side = gui_view_avail().x - (f32)bars * ( PICKER_BAR_W + gap );
    if ( side > PICKER_SIDE_MAX ) side = PICKER_SIDE_MAX;
    if ( side < PICKER_SIDE_MIN ) side = PICKER_SIDE_MIN;

    gui_rect_t cv    = gui_canvas( side );
    gui_rect_t sv_r  = { cv.x, cv.y, side, side };
    gui_rect_t hue_r = { sv_r.x + side + gap, cv.y, PICKER_BAR_W, side };
    gui_rect_t alp_r = { hue_r.x + PICKER_BAR_W + gap, cv.y, PICKER_BAR_W, side };

    /* Declare the width this block WANTS to the content measure (cell_reach -- the leaf-widget
       overflow seam).  A canvas fills its track, and a filled cell deliberately contributes no
       width to the region's content_w (the anti-feedback rule in line_place_cell) -- so inside
       an autosize popup, whose seed is narrower than the picker, nothing would ever ask the
       window to grow and the whole block would stay clamped at its floor.  The want is built
       around the WIDEST element: the square + bars edge, or the drag row's measured need
       (color_comps_min_w -- each box fits its longest printable value), whichever is wider.
       In a wide window the reach lands inside the view and is a no-op. */
    f32 want_w = side + (f32)bars * ( PICKER_BAR_W + gap );
    if ( !( flags & GUI_COLOR_EDIT_NO_INPUTS ) )
    {
        f32 inputs_min = color_comps_min_w( has_alpha ? 4u : 3u, is_hsv, is_flt );
        if ( want_w < inputs_min ) want_w = inputs_min;
    }
    cell_reach( cv.x + want_w );

    /* State first, paint after -- the house widget order.  All three surfaces are ITEM_DRAG:
       the press captures active_id, so the pick keeps tracking while the cursor sweeps out. */
    gui_item_state_t sv_st  = item_state( id_combine( id, 2u ), sv_r,  ITEM_DRAG );
    gui_item_state_t hue_st = item_state( id_combine( id, 3u ), hue_r, ITEM_DRAG );
    gui_item_state_t alp_st = { 0 };
    if ( has_alpha )
        alp_st = item_state( id_combine( id, 4u ), alp_r, ITEM_DRAG );

    if ( sv_st.active )
    {
        s       = saturate( ( s_io.mouse_x - sv_r.x ) / ( sv_r.w - 1.0f ) );
        val     = 1.0f - saturate( ( s_io.mouse_y - sv_r.y ) / ( sv_r.h - 1.0f ) );
        changed = true;
    }
    if ( hue_st.active )
    {
        h       = saturate( ( s_io.mouse_y - hue_r.y ) / ( hue_r.h - 1.0f ) );
        changed = true;
    }
    if ( alp_st.active )
    {
        v[ 3 ]  = 1.0f - saturate( ( s_io.mouse_y - alp_r.y ) / ( alp_r.h - 1.0f ) );
        changed = true;
    }
    if ( sv_st.active || hue_st.active )
        color_hsv_to_rgb( h, s, val, &v[ 0 ], &v[ 1 ], &v[ 2 ] );

    /* ---- Paint (gated: a scrolled-out picker skips its whole render prep) ---- */
    if ( !draw_cull_box( cv.x, cv.y, cv.w, cv.h ) )
    {
        f32 sv_rounding = draw_rounding();
        draw_set_rounding( 0.0f );   /* gradient quads are square by nature; keep the outlines flush */

        u32 cur_rgb = GUI_COLOR( color_chan_u8( v[ 0 ] ), color_chan_u8( v[ 1 ] ),
                                 color_chan_u8( v[ 2 ] ), 255u );

        /* SV square: white -> pure hue across, transparent -> black down.  Two exact one-quad
           gradients compose the full saturation/value plane. */
        f32 hr, hg, hb;
        color_hsv_to_rgb( h, 1.0f, 1.0f, &hr, &hg, &hb );
        draw_gradient( sv_r, GUI_COLOR( 255, 255, 255, 255 ),
                       GUI_COLOR( color_chan_u8( hr ), color_chan_u8( hg ), color_chan_u8( hb ), 255u ),
                       true );
        draw_gradient( sv_r, GUI_COLOR( 0, 0, 0, 0 ), GUI_COLOR( 0, 0, 0, 255 ), false );
        draw_outline( sv_r, WIN_BORDER, col_track_border( sv_st ) );

        /* Pick cursor: the flat color it sits on, ringed white-in-black so it reads on any
           ground.  Half-pixel centered so the 1.5px ring straddles the pick point evenly. */
        f32 ccx = floorf( sv_r.x + s * ( sv_r.w - 1.0f ) ) + 0.5f;
        f32 ccy = floorf( sv_r.y + ( 1.0f - val ) * ( sv_r.h - 1.0f ) ) + 0.5f;
        draw_circle( ccx, ccy, PICKER_CURSOR_R, true, 0.0f, cur_rgb );
        draw_circle( ccx, ccy, PICKER_CURSOR_R, false, 1.5f, GUI_COLOR( 255, 255, 255, 255 ) );
        draw_circle( ccx, ccy, PICKER_CURSOR_R + 1.5f, false, 1.0f, GUI_COLOR( 0, 0, 0, 160 ) );

        /* Hue bar: the six-sector rainbow, one gradient quad per sector; edges snapped
           cumulatively so sectors tile without seams. */
        for ( u32 i = 0; i < 6u; ++i )
        {
            f32 y0 = floorf( hue_r.y + (f32)i * hue_r.h / 6.0f );
            f32 y1 = ( i < 5u ) ? floorf( hue_r.y + (f32)( i + 1u ) * hue_r.h / 6.0f )
                                : ( hue_r.y + hue_r.h );
            gui_rect_t seg = { hue_r.x, y0, hue_r.w, y1 - y0 };
            draw_gradient( seg, s_hue_ramp[ i ], s_hue_ramp[ i + 1u ], false );
        }
        draw_outline( hue_r, WIN_BORDER, col_track_border( hue_st ) );
        picker_bar_marker( hue_r, h );

        /* Alpha bar: checker ground under an opaque -> transparent fall of the current color. */
        if ( has_alpha )
        {
            draw_checker( alp_r, 4.0f, GUI_COLOR( 200, 200, 200, 255 ), GUI_COLOR( 100, 100, 100, 255 ) );
            draw_gradient( alp_r, cur_rgb, cur_rgb & 0x00FFFFFFu, false );
            draw_outline( alp_r, WIN_BORDER, col_track_border( alp_st ) );
            picker_bar_marker( alp_r, 1.0f - saturate( v[ 3 ] ) );
        }

        draw_set_rounding( sv_rounding );
    }

    /* ---- Input rows: component drag boxes + hex field ---- */
    if ( !( flags & GUI_COLOR_EDIT_NO_INPUTS ) )
    {
        u32 comps      = has_alpha ? 4u : 3u;
        f32 hsv4[ 4 ]  = { h, s, val, has_alpha ? v[ 3 ] : 1.0f };
        gui_rect_t row = cell_next( WIDGET_H );

        if ( is_hsv )
        {
            if ( color_comps_boxes( id, row, v, hsv4, comps, true, is_flt ) )
            {
                h = hsv4[ 0 ]; s = hsv4[ 1 ]; val = hsv4[ 2 ];
                color_hsv_to_rgb( h, s, val, &v[ 0 ], &v[ 1 ], &v[ 2 ] );
                if ( has_alpha ) v[ 3 ] = hsv4[ 3 ];
                changed = true;
            }
        }
        else if ( color_comps_boxes( id, row, v, hsv4, comps, false, is_flt ) )
        {
            /* RGB edited directly: re-project so the memory below stores the new hue. */
            color_rgb_to_hsv( v[ 0 ], v[ 1 ], v[ 2 ], &h, &s, &val );
            color_restore_hs( color_pack_rgb( v ), &h, &s, val );
            changed = true;
        }

        if ( color_hex_field( id_combine( id, 5u ), v, has_alpha ) )
        {
            color_rgb_to_hsv( v[ 0 ], v[ 1 ], v[ 2 ], &h, &s, &val );
            changed = true;
        }
    }

    if ( changed )
        color_remember_hs( v, h, s );
    return changed;
}

/*==============================================================================================
    color_edit_n -- the inline [swatch][component drag fields] row; the swatch click opens the
    picker popup (gated by NO_PICKER).
==============================================================================================*/

static bool
color_edit_n( const char* label, f32* v, u32 n, gui_color_edit_flags_t flags )
{
    gui_id_t id = item_id( label );
    gui_field_row( label );
    gui_rect_t r = cell_next( WIDGET_H );   /* control track (label already emitted by gui_field_row) */

    u32  comps  = ( n == 4 && ( flags & GUI_COLOR_EDIT_NO_ALPHA ) ) ? 3 : n;
    bool is_hsv = ( flags & GUI_COLOR_EDIT_DISPLAY_HSV ) != 0;

    /* HSV working copy (only valid when is_hsv), degenerate axes restored so the H/S boxes
       hold their place while the color is gray or black. */
    f32 hsv[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if ( is_hsv )
    {
        color_rgb_to_hsv( v[ 0 ], v[ 1 ], v[ 2 ], &hsv[ 0 ], &hsv[ 1 ], &hsv[ 2 ] );
        color_restore_hs( color_pack_rgb( v ), &hsv[ 0 ], &hsv[ 1 ], hsv[ 2 ] );
        if ( n == 4 ) hsv[ 3 ] = v[ 3 ];
    }

    /* ABGR preview color -- recomputed after any change. */
    u8 pr = color_chan_u8( v[ 0 ] );
    u8 pg = color_chan_u8( v[ 1 ] );
    u8 pb = color_chan_u8( v[ 2 ] );
    u8 pa = ( n == 4 && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) ) ? color_chan_u8( v[ 3 ] ) : 255u;
    u32 abgr = GUI_COLOR( pr, pg, pb, pa );

    /* ---- Inline row: [preview_sq] [drag0 .. dragN-1] | label ---- */
    f32 preview_w = (f32)WIDGET_H;
    f32 gap       = WIDGET_GAP;
    gui_rect_t ctrl = r;   /* gui_field_row emitted the label; the whole control track is ours */

    /* Clickable color square -- placed first for fast visual identification. */
    gui_rect_t preview_r = { ctrl.x, ctrl.y, preview_w, ctrl.h };
    gui_item_state_t pst = item_state( id_combine( id, 1u ), preview_r, ITEM_BUTTON );
    {
        f32 sv = draw_rounding();
        draw_set_rounding( 2.0f );
        draw_face_item( preview_r, id_combine( id, 1u ), pst, false );
        gui_rect_t inner = { preview_r.x + 2.0f, preview_r.y + 2.0f,
                             preview_r.w - 4.0f,  preview_r.h - 4.0f };
        if ( pa < 255u )
            draw_checker( inner, 3.0f, GUI_COLOR( 200, 200, 200, 255 ), GUI_COLOR( 100, 100, 100, 255 ) );
        draw_fill( inner, abgr );
        /* A line is BORDER, never a BG face -- the swatch paints its own ground. */
        draw_outline( preview_r, WIN_BORDER, col_track_border( pst ) );
        draw_set_rounding( sv );
    }

    /* Hover tooltip: swatch + hex + component values.
       tooltip_begin does NOT check hover -- it always opens the window; caller must guard.
       Pre-compute all strings and the required content width before opening the tooltip so
       that gui_empty() can force content_w to the correct value on this frame.  Without this,
       gui_text() inside would call draw_text_fit_n which truncates text at the (still-narrow)
       window width, preventing content_w from ever growing to fit the actual text. */
    if ( pst.hover )
    {
        char tip_hex[ 12 ], tip_vals[ 32 ], tip_alp[ 24 ];
        tip_alp[ 0 ] = '\0';
        fmt_snprintf( tip_hex, sizeof( tip_hex ), "#%02X%02X%02X%02X", pr, pg, pb, pa );
        if ( is_hsv )
            fmt_snprintf( tip_vals, sizeof( tip_vals ), "H:%d  S:%d  V:%d",
                      (i32)( hsv[ 0 ] * 360.0f + 0.5f ),
                      (i32)( hsv[ 1 ] * 100.0f + 0.5f ),
                      (i32)( hsv[ 2 ] * 100.0f + 0.5f ) );
        else
            fmt_snprintf( tip_vals, sizeof( tip_vals ), "R:%d  G:%d  B:%d",
                      (i32)pr, (i32)pg, (i32)pb );
        bool tip_has_alpha = ( n == 4 && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) );
        if ( tip_has_alpha )
            fmt_snprintf( tip_alp, sizeof( tip_alp ), "A:%d  (%.0f%%)", (i32)pa, (f64)v[ 3 ] * 100.0 );

        f32 tip_w = 72.0f;
        f32 hw = font_text_w_n( tip_hex,  0xFFFFFFFFu ); if ( hw > tip_w ) tip_w = hw;
        f32 vw = font_text_w_n( tip_vals, 0xFFFFFFFFu ); if ( vw > tip_w ) tip_w = vw;
        if ( tip_has_alpha ) {
            f32 aw = font_text_w_n( tip_alp, 0xFFFFFFFFu ); if ( aw > tip_w ) tip_w = aw;
        }

        if ( gui_tooltip_begin() )
        {
            gui_stack();
            gui_empty( tip_w, 0.0f );   /* force content_w immediately so autosize is right */
            gui_rect_t tp = gui_canvas( 56.0f );
            {
                f32 sv = draw_rounding();
                draw_set_rounding( 3.0f );
                if ( pa < 255u )
                    draw_checker( tp, 6.0f, GUI_COLOR( 200, 200, 200, 255 ),
                                  GUI_COLOR( 100, 100, 100, 255 ) );
                draw_fill( tp, abgr );
                draw_outline( tp, WIN_BORDER, COL_BORDER_IDLE );
                draw_set_rounding( sv );
            }
            gui_text( tip_hex );
            gui_text( tip_vals );
            if ( tip_has_alpha ) gui_text( tip_alp );
        }
        gui_tooltip_end();
    }

    /* Drag fields -- one per component, sharing the remaining control width equally. */
    gui_rect_t comps_r = { ctrl.x + preview_w + gap, ctrl.y,
                           ctrl.w - preview_w - gap, ctrl.h };
    bool changed = color_comps_boxes( id, comps_r, v, hsv,
                                      comps, is_hsv, ( flags & GUI_COLOR_EDIT_FLOAT ) != 0 );
    if ( changed && is_hsv )
    {
        color_hsv_to_rgb( hsv[ 0 ], hsv[ 1 ], hsv[ 2 ], &v[ 0 ], &v[ 1 ], &v[ 2 ] );
        if ( n == 4 ) v[ 3 ] = hsv[ 3 ];
        color_remember_hs( v, hsv[ 0 ], hsv[ 1 ] );
    }

    /* ---- Picker popup (click on the color square to open) ---- */
    if ( !( flags & GUI_COLOR_EDIT_NO_PICKER ) )
    {
        char pid[ 64 ];
        fmt_snprintf( pid, sizeof( pid ), "##cpick_%u", id );

        if ( pst.clicked )
            gui_popup_open( pid );

        if ( gui_popup_begin( pid, GUI_WIN_ALWAYS_AUTOSIZE ) )
        {
            gui_stack();
            /* No width pin: the picker body declares its own wanted width through cell_reach,
               which is what actually grows an autosize popup (a gui_empty wider than the seed
               track clamps to it and measures nothing). */
            if ( color_picker_body( id_combine( id, 0xC01Bu ), v, n, flags ) )
                changed = true;
            gui_popup_end();
        }
    }

    return changed;
}

bool gui_color_edit3( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags )
{ return color_edit_n( label, col, 3u, flags ); }

bool gui_color_edit4( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags )
{ return color_edit_n( label, col, 4u, flags ); }

/* color_picker3/4 -- the picker block inline.  label is an id only (Dear ImGui parity):
   nothing is rendered from it, so "##id" and visible strings behave the same. */
bool gui_color_picker3( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags )
{ return color_picker_body( item_id( label ), col, 3u, flags ); }

bool gui_color_picker4( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags )
{ return color_picker_body( item_id( label ), col, 4u, flags ); }

// clang-format on
/*============================================================================================*/
