/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_draw.c -- "Draw" category demos.

    The custom-draw surface: line / polyline / path stroking, the parametric draw_* shape
    palette, the runtime icon atlas, building a real custom widget from canvas + item(), and
    the volatile-widget replay path.  Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/* Palette shared by the draw canvases. */
#define EX_INK    GUI_COLOR( 0xE6, 0xE6, 0xE6, 0xFF )   /* near-white stroke      */
#define EX_CYAN   GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF )   /* cool accent            */
#define EX_AMBR   GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF )   /* warm accent (opaque)   */
#define EX_AMBR_T GUI_COLOR( 0xFF, 0xB0, 0x40, 0xC8 )   /* warm, translucent      */
#define EX_GUIDE  GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xFF )   /* the ideal path / guide */
#define EX_BG     GUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF )   /* canvas backdrop        */

/*==============================================================================================
    Lines & Paths -- draw_line / draw_polyline / path_stroke, alignment vs the ideal path.
==============================================================================================*/

/* Stroke a closed point ring: optionally the ideal path as a 1px white guide, then the thick
   selected stroke translucent on top -- so inside / center / outside reads against the guide. */
static void
ex_line_shape( const gui_vec2_t* pts, u32 n, f32 thickness, gui_stroke_align_t align, bool guide )
{
    if ( guide )
        gui()->draw_polyline( pts, n, 1.0f, GUI_STROKE_CENTER, true, EX_GUIDE );
    gui()->draw_polyline( pts, n, thickness, align, true, EX_AMBR_T );
}

static void
ex_draw_lines( void )
{
    static i32  thickness_px = 6;     /* shared stroke width                  */
    static i32  align_idx    = 0;     /* index into the alignment table       */
    static f32  spread       = 1.0f;  /* zig-zag corner amplitude (0..1)      */
    static i32  poly_pts     = 5;     /* sides of the path-built polygon      */
    static bool star_mode    = true;  /* star vs convex polygon               */
    static bool show_guides  = true;  /* draw the 1px white ideal-path guides */

    static const char*              align_names[ 4 ] = { "CENTER_BIASED", "CENTER", "INSIDE", "OUTSIDE" };
    static const gui_stroke_align_t align_mode[ 4 ]  = {
        GUI_STROKE_CENTER_BIASED, GUI_STROKE_CENTER, GUI_STROKE_INSIDE, GUI_STROKE_OUTSIDE };

    if ( ex_begin( "Lines & Paths", 500, 680, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "draw_line / draw_polyline / path_stroke into a canvas()." );

        /* ---- controls ---- */
        gui()->separator_text( "Parameters" );
        gui()->slider_int( "Thickness", &thickness_px, 0, 16 );
        f32 thickness = (f32)thickness_px;
        gui()->textf( "alignment: %s", align_names[ align_idx ] );

        gui()->bar();
        gui()->radio_button( "Biased",  &align_idx, 0 );
        gui()->radio_button( "Center",  &align_idx, 1 );
        gui()->radio_button( "Inside",  &align_idx, 2 );
        gui()->radio_button( "Outside", &align_idx, 3 );
        gui()->stack();

        gui()->checkbox( "Show guide lines", &show_guides );

        /* ---- (a) crisp axis-aligned ladder ---- */
        gui()->separator_text( "Axis-aligned: crisp 1..6 px (pixel-snapped)" );
        {
            gui_rect_t r = gui()->canvas( 168.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );

            gui_rect_t area = gui_rect_pad( r, 12.0f );
            f32        lblw = gui()->text_size( "16 px" ).x + 12.0f;
            for ( i32 t = 1; t <= 6; ++t )
            {
                gui_rect_t row = gui_rect_cut_top( &area, 24.0f );
                gui_rect_t lbl = gui_rect_cut_right( &row, lblw );
                f32        cy  = row.y + row.h * 0.5f;
                char       tag[ 8 ];
                snprintf( tag, sizeof( tag ), "%d px", t );
                gui()->draw_line( row.x, cy, row.x + row.w, cy, (f32)t, EX_INK );
                gui()->draw_text_in( lbl, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, EX_INK, tag );
            }
        }

        /* ---- (b) antialiased diagonal fan ---- */
        gui()->separator_text( "Antialiased diagonals (Thickness)" );
        {
            gui_rect_t r = gui()->canvas( 140.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );
            f32 cx  = r.x + 24.0f;
            f32 cy  = r.y + r.h - 20.0f;
            f32 len = ( r.h - 36.0f );
            for ( i32 i = 0; i <= 8; ++i )
            {
                f32 a = ( GUI_PI * 0.5f ) * (f32)i / 8.0f;
                gui()->draw_line( cx, cy, cx + cosf( a ) * len, cy - sinf( a ) * len,
                                  thickness, EX_CYAN );
            }
        }

        /* ---- (c) the four alignments against the ideal path ---- */
        gui()->separator_text( "Stroke alignment vs the ideal path" );
        {
            gui_rect_t r = gui()->canvas( 162.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );

            gui_rect_t area = gui_rect_pad( r, 12.0f );
            gui_rect_t cap  = gui_rect_cut_top( &area, gui()->sz_line_h() );
            if ( show_guides )
                gui()->draw_text_in( cap, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER,
                                     EX_GUIDE, "white = ideal path" );

            f32 label_w = 0.0f;
            for ( i32 i = 0; i < 4; ++i )
            {
                f32 w = gui()->text_size( align_names[ i ] ).x;
                if ( w > label_w ) label_w = w;
            }
            gui_rect_t labels = gui_rect_cut_left( &area, label_w + 12.0f );
            for ( i32 i = 0; i < 4; ++i )
            {
                f32        ly       = area.y + 16.0f + (f32)i * 30.0f;
                gui_vec2_t seg[ 2 ] = { { area.x, ly }, { area.x + area.w, ly } };
                gui()->draw_polyline( seg, 2, thickness, align_mode[ i ], false, EX_AMBR_T );
                if ( show_guides )
                    gui()->draw_line( area.x, ly, area.x + area.w, ly, 1.0f, EX_GUIDE );

                gui_rect_t lbl = { labels.x, ly - gui()->sz_line_h() * 0.5f, labels.w, gui()->sz_line_h() };
                gui()->draw_text_in( lbl, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                                     i == align_idx ? EX_CYAN : EX_INK, align_names[ i ] );
            }
        }

        /* ---- (d) closed shapes: square + circle ---- */
        gui()->separator_text( "Closed shapes: draw_polyline (square / circle)" );
        {
            gui_rect_t r = gui()->canvas( 200.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );
            f32 cy = r.y + r.h * 0.5f - 6.0f;

            f32 sx = r.x + r.w * 0.27f, hs = 44.0f;
            gui_vec2_t sq[ 4 ] = {
                { sx - hs, cy - hs }, { sx + hs, cy - hs },
                { sx + hs, cy + hs }, { sx - hs, cy + hs } };
            ex_line_shape( sq, 4, thickness, align_mode[ align_idx ], show_guides );
            gui()->draw_text( sx - 22.0f, cy + hs + 14.0f, EX_INK, "square" );

            f32 ox = r.x + r.w * 0.70f, rr = 48.0f;
            gui_vec2_t cir[ 48 ];
            for ( i32 i = 0; i < 48; ++i )
            {
                f32 a = ( 2.0f * GUI_PI ) * (f32)i / 48.0f;
                cir[ i ] = ( gui_vec2_t ){ ox + cosf( a ) * rr, cy + sinf( a ) * rr };
            }
            ex_line_shape( cir, 48, thickness, align_mode[ align_idx ], show_guides );
            gui()->draw_text( ox - 20.0f, cy + rr + 14.0f, EX_INK, "circle" );
        }

        /* ---- (e) mitered polyline corners, adjustable spread ---- */
        gui()->separator_text( "Polyline: mitered corners" );
        gui()->slider_float( "Corner spread", &spread, 0.0f, 1.0f );
        {
            gui_rect_t r = gui()->canvas( 130.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );
            f32 cy   = r.y + r.h * 0.5f;
            f32 amp  = ( r.h * 0.5f - 16.0f ) * spread;
            f32 x    = r.x + 24.0f;
            f32 step = ( r.w - 48.0f ) / 6.0f;
            gui_vec2_t zig[ 7 ];
            for ( i32 i = 0; i < 7; ++i )
                zig[ i ] = ( gui_vec2_t ){ x + step * (f32)i, cy + ( ( i & 1 ) ? -amp : amp ) };
            gui()->draw_polyline( zig, 7, thickness, GUI_STROKE_CENTER, false, EX_CYAN );
        }

        /* ---- (f) closed path via the retained builder: polygon / star ---- */
        gui()->separator_text( "Closed path: path_stroke (polygon / star)" );
        gui()->checkbox( "Star", &star_mode ); gui()->same_line( 12.0f );
        gui()->drag_int( "Points", &poly_pts, 0.1f, 3, 10, "%d" );
        {
            gui_rect_t r = gui()->canvas( 200.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, EX_BG );
            f32 pcx = r.x + r.w * 0.5f;
            f32 pcy = r.y + r.h * 0.5f;
            f32 pr  = ( r.h * 0.5f ) - 22.0f;

            gui()->path_clear();
            if ( star_mode )
            {
                i32 n = poly_pts * 2;
                for ( i32 i = 0; i < n; ++i )
                {
                    f32 a  = -GUI_PI * 0.5f + ( 2.0f * GUI_PI ) * (f32)i / (f32)n;
                    f32 rr = ( i & 1 ) ? pr * 0.45f : pr;
                    gui()->path_line_to( pcx + cosf( a ) * rr, pcy + sinf( a ) * rr );
                }
            }
            else
            {
                for ( i32 i = 0; i < poly_pts; ++i )
                {
                    f32 a = -GUI_PI * 0.5f + ( 2.0f * GUI_PI ) * (f32)i / (f32)poly_pts;
                    gui()->path_line_to( pcx + cosf( a ) * pr, pcy + sinf( a ) * pr );
                }
            }
            gui()->path_stroke( thickness, GUI_STROKE_CENTER, true, EX_AMBR );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Shape Primitives -- every render_* primitive in a 50/50 preview + parameter list.
==============================================================================================*/

/* Center a sz x sz preview box inside the cell `c` (clamped to the cell). */
static gui_rect_t
ex_sym_box( gui_rect_t c, f32 sz )
{
    if ( sz > c.h ) sz = c.h;
    if ( sz > c.w ) sz = c.w;
    return ( gui_rect_t ){ c.x + ( c.w - sz ) * 0.5f, c.y + ( c.h - sz ) * 0.5f, sz, sz };
}

static void
ex_draw_shapes( void )
{
    if ( ex_begin( "Shape Primitives", 460, 720, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Each row: one draw_* primitive on the left," );
        gui()->text( "a slider driving one of its facets on the right." );
        gui()->separator();

        const u32 col = 0xFFE0E0E0u;
        const u32 acc = 0xFF60C0F0u;
        const u32 grn = 0xFF48E618u;
        const f32 H   = 26.0f;
        f32       t   = (f32)gui()->get_time();

        gui()->row2( 1.0f, 1.0f );
        {
            gui_rect_t r;

            static i32 p_arrow = 18;     /* px */
            r = gui()->canvas( H ); gui()->draw_arrow( ex_sym_box( r, (f32)p_arrow ), GUI_DIR_RIGHT, col );
            gui()->slider_int( "Arrow size (px)", &p_arrow, 8, 26 );

            static i32 p_check = 22;     /* px */
            r = gui()->canvas( H ); gui()->draw_check_mark( ex_sym_box( r, (f32)p_check ), grn );
            gui()->slider_int( "Check size (px)", &p_check, 8, 26 );

            static i32 p_chev = 2;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_chevron( ex_sym_box( r, H ), GUI_DIR_RIGHT, (f32)p_chev, col );
            gui()->slider_int( "Chevron weight", &p_chev, 1, 5 );

            static i32 p_pm = 2;         /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_plus_minus( ex_sym_box( r, H ), true, (f32)p_pm, col );
            gui()->slider_int( "Plus weight", &p_pm, 1, 5 );

            static i32 p_close = 18;     /* px */
            r = gui()->canvas( H ); gui()->draw_close( ex_sym_box( r, (f32)p_close ), col );
            gui()->slider_int( "Close size (px)", &p_close, 8, 26 );

            static i32 p_bull = 4;       /* radius, px */
            r = gui()->canvas( H ); gui()->draw_bullet( r.x + r.w * 0.5f, r.y + H * 0.5f, (f32)p_bull, col );
            gui()->slider_int( "Bullet radius", &p_bull, 2, 10 );

            static i32 p_sides = 6;
            r = gui()->canvas( H ); gui()->draw_ngon( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, (u32)p_sides, t*0.3f, true, 0.0f, acc );
            gui()->slider_int( "Polygon sides", &p_sides, 3, 12 );

            static i32 p_ring = 2;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_circle( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, false, (f32)p_ring, acc );
            gui()->slider_int( "Ring weight", &p_ring, 1, 6 );

            static i32 p_arc = 240;      /* whole degrees */
            r = gui()->canvas( H ); gui()->draw_arc( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, 0.0f, gui_radians( (f32)p_arc ), 3.0f, acc );
            gui()->slider_int( "Arc sweep (deg)", &p_arc, 20, 360 );

            static i32 p_pie = 150;      /* whole degrees, swept from -90 */
            r = gui()->canvas( H ); gui()->draw_pie( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, gui_radians( -90.0f ), gui_radians( -90.0f + (f32)p_pie ), acc );
            gui()->slider_int( "Pie sweep (deg)", &p_pie, 20, 360 );

            static i32 p_round = 8;      /* corner radius, px */
            r = gui()->canvas( H ); gui()->draw_round_rect( ex_sym_box( r, H ), (f32)p_round, (f32)p_round, 0.0f, 0.0f, true, 0.0f, 0xFF4A90D0u );
            gui()->slider_int( "Tab corner (px)", &p_round, 0, 13 );

            static i32 p_border = 2;     /* frame border, px */
            r = gui()->canvas( H ); gui()->draw_frame( ex_sym_box( r, H ), 0xFF303840u, acc, (f32)p_border );
            gui()->slider_int( "Frame border (px)", &p_border, 1, 6 );

            static f32 p_bow = 0.4f;     /* continuous curve shape */
            r = gui()->canvas( H ); gui()->draw_bezier_quad( r.x+4, r.y+H*0.5f, r.x+r.w*0.5f, r.y+H*0.5f - H*p_bow, r.x+r.w-4, r.y+H*0.5f, 2.0f, acc );
            gui()->slider_float( "Quad bow", &p_bow, -0.45f, 0.45f );

            static f32 p_s = 0.4f;       /* cubic S strength */
            r = gui()->canvas( H ); gui()->draw_bezier_cubic( r.x+4, r.y+H*0.5f,
                                                              r.x+r.w*0.33f, r.y+H*0.5f - H*p_s,
                                                              r.x+r.w*0.66f, r.y+H*0.5f + H*p_s,
                                                              r.x+r.w-4, r.y+H*0.5f, 2.0f, acc );
            gui()->slider_float( "Cubic S", &p_s, -0.45f, 0.45f );

            static i32 p_dash = 5;       /* dash length, px */
            r = gui()->canvas( H ); gui()->draw_dashed_line( r.x+4, r.y+H*0.5f, r.x+r.w-4, r.y+H*0.5f, (f32)p_dash, 3.0f, 2.0f, col );
            gui()->slider_int( "Dash length (px)", &p_dash, 2, 12 );

            static i32 p_cell = 6;       /* cell size, px */
            r = gui()->canvas( H ); gui()->draw_checker( ex_sym_box( r, H ), (f32)p_cell, 0xFF808080u, 0xFF404040u );
            gui()->slider_int( "Checker cell (px)", &p_cell, 3, 14 );

            static i32 p_hatch = 5;      /* line spacing, px */
            r = gui()->canvas( H ); gui()->draw_hatch( ex_sym_box( r, H ), (f32)p_hatch, 1.0f, 0xFF909090u );
            gui()->slider_int( "Hatch spacing (px)", &p_hatch, 3, 14 );

            static bool p_horiz = true;  /* gradient axis */
            r = gui()->canvas( H ); gui()->draw_gradient( ex_sym_box( r, H ), acc, 0xFF102030u, p_horiz );
            gui()->checkbox( "Gradient horizontal", &p_horiz );

            static i32 p_spread = 6;     /* shadow spread, px */
            r = gui()->canvas( H );
            {
                gui_rect_t box = ex_sym_box( r, H - 8.0f );
                gui()->draw_shadow( box, (f32)p_spread, 0xC0000000u );
                gui()->draw_rect( box.x, box.y, box.w, box.h, 0xFF3A4450u );
            }
            gui()->slider_int( "Shadow spread", &p_spread, 1, 12 );

            /* The same surface one row up, resolved exponentially instead of linearly.  Sitting
               beside the shadow on purpose: the two differ in the falloff CURVE and nothing else,
               and that is the whole thing worth looking at. */
            static i32 p_glow = 8;       /* glow reach, px */
            r = gui()->canvas( H );
            {
                gui_rect_t box = ex_sym_box( r, H - 10.0f );
                gui()->draw_glow( box, (f32)p_glow, 0xFF40C8FFu );
                gui()->draw_rect( box.x, box.y, box.w, box.h, 0xFF102028u );
            }
            gui()->slider_int( "Glow reach (px)", &p_glow, 2, 16 );

            /* The lattice: every count below is the SAME one quad and one style record, which is
               the whole reason a 40-tick ruler is now a thing you can draw. */
            static i32 p_ticks = 24;     /* tick count across the bar */
            r = gui()->canvas( H );
            gui()->draw_ticks( ( gui_rect_t ){ r.x + 4.0f, r.y + 6.0f, r.w - 8.0f, H - 12.0f },
                               (u32)p_ticks, 1.0f, 0.0f, false, col );
            gui()->slider_int( "Ruler ticks", &p_ticks, 2, 64 );

            static i32 p_dots = 5;       /* dots per side of the field */
            r = gui()->canvas( H );
            gui()->draw_dot_grid( ex_sym_box( r, H ), (u32)p_dots, (u32)p_dots,
                                  H / (f32)( p_dots + 1 ), H / (f32)( p_dots + 1 ), 2.0f, acc );
            gui()->slider_int( "Dot field N x N", &p_dots, 2, 9 );

            /* The angular fold, spinning on the shader clock: the count moves, the cost does not.
               Compare with the arc spinner two rows down -- both re-tessellate nothing. */
            static i32 p_sdots = 8;      /* dots around the ring */
            r = gui()->canvas( H );
            gui()->draw_dot_spinner( ex_sym_box( r, H ), (u32)p_sdots, 3.0f, 0.6f, acc );
            gui()->slider_int( "Spinner dots", &p_sdots, 3, 16 );

            static i32 p_dial = 12;      /* marks around the dial face */
            r = gui()->canvas( H );
            gui()->draw_dial_ticks( ex_sym_box( r, H ), (u32)p_dial, 1.0f, 5.0f, 0.0f, col );
            gui()->slider_int( "Dial marks", &p_dial, 4, 36 );

            static i32 p_grip = 20;      /* grip box, px */
            r = gui()->canvas( H ); gui()->draw_grip( ex_sym_box( r, (f32)p_grip ), col );
            gui()->slider_int( "Grip size (px)", &p_grip, 10, 26 );

            static i32 p_spin = 3;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_spinner( ex_sym_box( r, H ), 1.0f, (f32)p_spin, acc );
            gui()->slider_int( "Spinner weight", &p_spin, 1, 6 );

            static f32 p_prog = 0.66f;   /* continuous 0..1 fraction */
            r = gui()->canvas( H ); gui()->draw_progress_arc( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, p_prog, 3.0f, acc );
            gui()->slider_float( "Progress frac", &p_prog, 0.0f, 1.0f );

            /* The border tracer: the indeterminate arc, travelling the outline on the shader
               clock.  Widened out of the square the marks above use -- the whole point is the
               run down the long edges and around the corners. */
            static i32 p_trace = 12;     /* lit fraction of the border, percent */
            r = gui()->canvas( H );
            gui()->draw_border_tracer( ( gui_rect_t ){ r.x + 4.0f, r.y + 3.0f, r.w - 8.0f, H - 6.0f },
                                       6.0f, 2.0f, (f32)p_trace * 0.01f, 0.4f, acc );
            gui()->slider_int( "Tracer arc (%)", &p_trace, 2, 100 );

            /* The determinate twin: same arc, placed by a value instead of the clock. */
            static f32 p_bprog = 0.30f;  /* position around the border, 0..1 from top-left */
            r = gui()->canvas( H );
            gui()->draw_border_progress( ( gui_rect_t ){ r.x + 4.0f, r.y + 3.0f, r.w - 8.0f, H - 6.0f },
                                         6.0f, 2.0f, 0.25f, p_bprog, grn );
            gui()->slider_float( "Border progress at", &p_bprog, 0.0f, 1.0f );

            static i32 p_half = 6;       /* pointer half-extent, px */
            r = gui()->canvas( H ); gui()->draw_arrow_pointing_at( r.x + r.w*0.5f, r.y + H*0.5f, (f32)p_half, GUI_DIR_DOWN, col );
            gui()->slider_int( "Pointer half (px)", &p_half, 3, 12 );
        }
        gui()->row( 0 );

        gui()->separator_text( "Text effects" );
        {
            gui_rect_t tr = gui()->empty( 0.0f, 22.0f );
            gui()->draw_text_outline( tr.x + 4.0f, tr.y + 4.0f, "Outlined text", 0xFFFFFFFFu, 0xFF000000u );
            tr = gui()->empty( 0.0f, 22.0f );
            gui()->draw_text_shadow( tr.x + 4.0f, tr.y + 4.0f, "Shadowed text", 0xFFFFFFFFu, 0xFF000000u, 2.0f, 2.0f );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Icons & Images -- procedural rasterizers feed the runtime icon atlas.

    The icon atlas takes raw R8 coverage (row-major, w*h bytes, 0..255); pixel sourcing is the
    caller's.  These draw three simple icons (folder / check / gear) straight into a buffer for
    registration.  Crude on purpose -- the point is to feed the atlas real bytes.
==============================================================================================*/

/* Distance from point (px,py) to segment a-b -- strokes the checkmark with soft edges. */
static f32
ex_seg_dist( f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by )
{
    f32 dx = bx - ax, dy = by - ay;
    f32 len2 = dx * dx + dy * dy;
    f32 t = len2 > 0.0f ? ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
    f32 ex = px - ( ax + t * dx );
    f32 ey = py - ( ay + t * dy );
    return sqrtf( ex * ex + ey * ey );
}

/* Coverage for "inside by margin m", with a ~1px soft edge. */
static u8
ex_cov( f32 m )
{
    f32 a = m + 0.5f;
    if ( a <= 0.0f ) return 0;
    if ( a >= 1.0f ) return 255;
    return (u8)( a * 255.0f );
}

static void
ex_make_folder( u8* p, i32 n )
{
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            bool body = ( x >= 3 && x <= 28 && y >= 11 && y <= 26 );   /* folder body */
            bool tab  = ( x >= 3 && x <= 14 && y >=  7 && y <= 11 );   /* raised tab  */
            p[ y * n + x ] = ( body || tab ) ? 255 : 0;
        }
}

static void
ex_make_check( u8* p, i32 n )
{
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
            f32 d1 = ex_seg_dist( px, py,  7.0f, 17.0f, 13.0f, 23.0f );
            f32 d2 = ex_seg_dist( px, py, 13.0f, 23.0f, 26.0f,  8.0f );
            f32 d  = d1 < d2 ? d1 : d2;
            p[ y * n + x ] = ex_cov( 2.5f - d );
        }
}

static void
ex_make_gear( u8* p, i32 n )
{
    f32 cx = (f32)n * 0.5f, cy = (f32)n * 0.5f;
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            f32 dx = (f32)x + 0.5f - cx, dy = (f32)y + 0.5f - cy;
            f32 rad = sqrtf( dx * dx + dy * dy );
            f32 ang = atan2f( dy, dx );

            bool body  = rad <= 11.0f;
            bool tooth = false;
            if ( rad <= 15.0f )
            {
                f32 sect = ang / ( GUI_PI / 4.0f );
                f32 frac = sect - floorf( sect + 0.5f );
                tooth = fabsf( frac ) < 0.30f;
            }
            bool hole = rad <= 5.0f;
            p[ y * n + x ] = ( ( body || tooth ) && !hole ) ? 255 : 0;
        }
}

static void
ex_draw_icons( void )
{
    static gui_icon_id_t ic_folder = GUI_ICON_NONE;
    static gui_icon_id_t ic_check  = GUI_ICON_NONE;
    static gui_icon_id_t ic_gear   = GUI_ICON_NONE;

    /* Register once.  Safe to call any frame -- the GPU upload defers to the next frame_begin. */
    if ( ic_folder == GUI_ICON_NONE )
    {
        static u8 buf[ 32 * 32 ];
        ex_make_folder( buf, 32 ); ic_folder = gui()->register_icon( "ex_folder", 32, 32, buf );
        ex_make_check ( buf, 32 ); ic_check  = gui()->register_icon( "ex_check",  32, 32, buf );
        ex_make_gear  ( buf, 32 ); ic_gear   = gui()->register_icon( "ex_gear",   32, 32, buf );
    }

    if ( ex_begin( "Icons & Images", 400, 480, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "register_icon -> tinted quads, same flush as text." );
        gui()->separator();

        /* Parametric: size + tint drive the image() row live. */
        static i32 ic_sz   = 48;
        static f32 tint[ 3 ] = { 1.0f, 0.8f, 0.4f };
        gui()->slider_int( "image size", &ic_sz, 16, 96 );
        gui()->color_edit3( "tint", tint, GUI_COLOR_EDIT_NONE );
        u32 tcol = GUI_COLOR( (u8)( tint[ 0 ] * 255.0f ), (u8)( tint[ 1 ] * 255.0f ),
                              (u8)( tint[ 2 ] * 255.0f ), 0xFF );

        gui()->row_cols_n( 0, 3 );
        gui()->image( ic_folder, (f32)ic_sz, (f32)ic_sz, tcol );
        gui()->image( ic_check,  (f32)ic_sz, (f32)ic_sz, 0xFF66DD66u );
        gui()->image( ic_gear,   (f32)ic_sz, (f32)ic_sz, 0xFFDDDDDDu );
        gui()->row( 0 );

        /* Registry queries. */
        gui()->separator_text( "find_icon / icon_size" );
        gui_icon_id_t found = gui()->find_icon( "ex_gear" );
        gui_vec2_t    isz   = gui()->icon_size( ic_gear );
        gui()->textf( "find_icon(\"ex_gear\") = %u   native %.0f x %.0f", found, isz.x, isz.y );

        /* icon + caption rows -- draw_icon_in into a manual slot beside a label. */
        gui()->separator_text( "draw_icon_in (manual slots)" );
        static const struct { gui_icon_id_t* id; const char* label; } rows[] = {
            { &ic_folder, "Open Folder" },
            { &ic_gear,   "Settings"    },
            { &ic_check,  "Apply"       },
        };
        for ( i32 i = 0; i < 3; ++i )
        {
            gui_rect_t slot = gui()->empty( 240, 28 );
            gui()->draw_icon_in( ( gui_rect_t ){ slot.x + 2, slot.y + 2, 24, 24 }, *rows[ i ].id, 0xFFFFFFFFu );
            gui()->draw_text_in( ( gui_rect_t ){ slot.x + 34, slot.y, slot.w - 34, slot.h },
                                 GUI_ALIGN_VCENTER, 0xFFFFFFFFu, rows[ i ].label );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Custom Widgets -- rect + item() + draw_*: a widget the gui system has never heard of.
==============================================================================================*/

static void
ex_draw_custom( void )
{
    if ( ex_begin( "Custom Widgets", 440, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "A custom widget = a rect + item() + draw_* --" );
        gui()->text( "it hovers, captures, clicks and navs like stock." );

        /* --- a click strip built from empty + hover test + invisible_button ---------------- */
        gui()->separator_text( "invisible_button + is_mouse_hovering_rect" );
        static i32 strip_clicks = 0;
        {
            gui_rect_t sw  = gui()->empty( 0.0f, 22.0f );
            bool       hot = gui()->is_mouse_hovering_rect( sw );
            if ( hot )
                gui()->cursor_set( APP_CURSOR_HAND );   /* a shape gui cannot infer */
            gui()->draw_rect( sw.x, sw.y, sw.w, sw.h, hot ? EX_CYAN : EX_BG );
            gui()->draw_text_in( sw, GUI_ALIGN_CENTER, EX_INK, "click me (hand cursor on hover)" );
            if ( gui()->invisible_button( "strip##click", sw ) )
                strip_clicks++;
        }
        gui()->textf( "clicks: %d", strip_clicks );

        /* --- a rotary knob: full item() behavior over caller drawing ----------------------- */
        gui()->separator_text( "item() -- a rotary knob" );
        static f32 knob = 0.35f;    /* 0..1 */
        {
            gui_rect_t r    = gui()->canvas( 90.0f );
            gui_rect_t box  = ex_sym_box( r, 80.0f );
            gui_item_state_t st = gui()->item( "knob##custom", box );

            if ( st.active )                    /* drag: horizontal mouse motion turns it */
            {
                f32 mx, my;
                gui()->get_mouse_pos( &mx, &my );
                f32 cx = box.x + box.w * 0.5f;
                knob  = ( mx - cx ) / ( box.w * 2.0f ) + 0.5f;
            }
            if ( st.nav_adjust )                /* keyboard: arrow steps while captured */
                knob += (f32)st.nav_adjust * 0.05f;
            if ( knob < 0.0f ) knob = 0.0f;
            if ( knob > 1.0f ) knob = 1.0f;

            f32 kcx = box.x + box.w * 0.5f;
            f32 kcy = box.y + box.h * 0.5f;
            f32 kr  = box.w * 0.42f;
            u32 body = st.active ? 0xFF3E5A78u : st.hover ? 0xFF35485E : 0xFF2C3A4A;
            gui()->draw_circle( kcx, kcy, kr, true, 0.0f, body );
            gui()->draw_progress_arc( kcx, kcy, kr - 3.0f, knob, 3.0f, EX_AMBR );
            f32 a = ( -0.75f + knob * 1.5f ) * GUI_PI;      /* -135..+135 deg */
            gui()->draw_line( kcx, kcy, kcx + sinf( a ) * kr * 0.8f, kcy - cosf( a ) * kr * 0.8f,
                              2.0f, EX_INK );
            if ( st.nav )
                gui()->draw_circle( kcx, kcy, kr + 4.0f, false, 1.0f, EX_CYAN );
        }
        gui()->slider_float( "knob (same value)", &knob, 0.0f, 1.0f );

        /* --- draw_text_in / draw_text_clipped, alignment picked live ----------------------- */
        gui()->separator_text( "draw_text_in (alignment picker)" );
        static i32 ta = 4;      /* 0..8: 3x3 alignment grid index */
        gui()->slider_int( "alignment cell (3x3)", &ta, 0, 8 );
        {
            gui_align_t ah = ( ta % 3 ) == 1 ? GUI_ALIGN_HCENTER : ( ta % 3 ) == 2 ? GUI_ALIGN_RIGHT  : GUI_ALIGN_LEFT;
            gui_align_t av = ( ta / 3 ) == 1 ? GUI_ALIGN_VCENTER : ( ta / 3 ) == 2 ? GUI_ALIGN_BOTTOM : GUI_ALIGN_TOP;
            gui_rect_t r = gui()->canvas( 80.0f );
            gui()->draw_frame( r, EX_BG, 0xFF505050u, 1.0f );
            gui()->draw_text_in( gui_rect_pad( r, 4.0f ), ah | av, EX_INK, "aligned" );
        }

        gui()->separator_text( "draw_text_clipped (ellipsize)" );
        {
            gui_rect_t r = gui()->canvas( 24.0f );
            gui()->draw_frame( r, EX_BG, 0xFF505050u, 1.0f );
            gui()->draw_text_clipped( gui_rect_pad( r, 2.0f ), GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, EX_INK,
                                      "a single line that ellipsizes to the rect width -- shrink the window" );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Volatile Widgets -- animation that survives idle frames without a full rebuild.
==============================================================================================*/

/* Ordinary emit calls, bracketed by volatile_begin/end so gui can replay just this block on
   clean frames.  CONTRACT: fixed layout footprint -- pixels may change, size must not (the
   fixed-width formats + the fixed canvas keep it constant). */
static void
ex_volatile_block_cb( gui_id_t id, bool is_replay )
{
    UNUSED( id );
    UNUSED( is_replay );
    gui()->volatile_begin();

    f32 t = (f32)sys_tick_seconds();

    /* A pulsing square + spinner in one fixed-height canvas. */
    gui_rect_t r = gui()->canvas( 32.0f );
    f32 s  = 0.5f + 0.5f * sinf( t * 3.0f );
    u8  g  = (u8)( 80.0f + 175.0f * s );
    gui()->draw_rect( r.x, r.y, 32.0f, 32.0f, GUI_COLOR( 0x00, g, g, 0xFF ) );
    gui()->draw_spinner( ( gui_rect_t ){ r.x + 44.0f, r.y + 4.0f, 24.0f, 24.0f }, 1.0f, 3.0f, EX_AMBR );

    /* Fixed-width digits keep the footprint constant while they animate. */
    f32 dt  = gui()->get_delta_time();
    f32 ms  = dt * 1000.0f;
    f32 fps = ( dt > 0.0f ) ? 1.0f / dt : 0.0f;
    gui()->textf( "%8.3f ms/frame (%7.1f FPS)  t=%8.2f", ms, fps, t );

    gui()->volatile_end();
}

static void
ex_draw_volatile( void )
{
    if ( ex_begin( "Volatile Widgets", 440, 380, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "The block below is wrapped in volatile_cb: on idle frames the rest "
                             "of this window is NOT re-emitted, yet the block keeps animating -- "
                             "gui replays just its command range into a reserved slot." );
        gui()->separator();

        gui()->volatile_cb( "ex_volatile_block", ex_volatile_block_cb );

        gui()->separator();
        gui_render_stats_t rs = gui()->render_stats();
        gui()->textf( "volatile_patched last frame: %u", rs.volatile_patched );
        gui()->textf( "windows retained: %u / %u", rs.win_retained, rs.win_total );
        gui()->text_disabled( "Toggle idle skip (I, or Debug & Stats) and leave the" );
        gui()->text_disabled( "mouse still: the animation keeps running regardless." );
    }
    gui()->window_end();
}

/*============================================================================================*/
