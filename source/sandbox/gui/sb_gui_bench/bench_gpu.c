/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench_gpu.c -- the fill-rate ladder and the shader-op matrix.

    Both suites are GPU questions: the scenes are static and gpu_ms is the metric.  Like every
    case they still re-tessellate fully each frame (the suite default), so their tess / submit
    columns are honest too -- but the deltas are read off gpu_ms, which the CPU work cannot
    pollute.

    The op matrix draws the SAME cell grid under every config -- a 16 x 9 grid over one large
    canvas, repeated BENCH_OP_LAYERS times per frame -- so shaded area is held equal and the
    gpu_ms delta against op_self is that op's per-pixel price.  Two normalization caveats the
    report repeats: an SDF field shades its whole quad footprint (a pie's cost is the cell's
    area, not the wedge's), and the soft verbs (glow / shadow) get their box deflated by the
    spread so the widened covering still lands on the cell.  stripes is the one tessellated
    fill in the set -- its quad count differs by design and the column shows it.

==============================================================================================*/
// clang-format off

/* 16 x 9 cells x 6 layers = 864 cells covering the canvas six times over (~5.5 Mpix shaded at
   1280x720).  Cell count is bounded by GUI_MAX_CMDS: total cells must stay under it with the
   window chrome beside them.  Style records no longer bind -- the pattern ops anchor at the
   quad's own rect, so a whole grid of one config dedups to a handful of records.  The bench
   prices the pipeline inside its caps; overflow belongs to sb_gui_stress, and the runner flags
   any case that saturated a pool anyway. */
#define BENCH_OP_COLS    16u
#define BENCH_OP_ROWS    9u
#define BENCH_OP_LAYERS  6u

/* The op-matrix configurations, one case each.  Order is the report order; op_self is first
   because every delta is measured against it. */
typedef enum
{
    BENCH_OP_SELF = 0,     // flat fill: the baseline every delta subtracts
    BENCH_OP_BOX_ROUND,    // rounded box field
    BENCH_OP_GRAD,         // linear gradient
    BENCH_OP_GRAD_RADIAL,  // radial gradient
    BENCH_OP_GRAD_CONIC,   // conic gradient
    BENCH_OP_FRAME,        // body + border in one record
    BENCH_OP_GLOW,         // exponential falloff (box deflated by the spread)
    BENCH_OP_SHADOW,       // linear feather falloff (box deflated by the spread)
    BENCH_OP_RING,         // field-borne band
    BENCH_OP_CHECKER,      // fragment-tiled checker
    BENCH_OP_GRID,         // fragment-tiled line lattice
    BENCH_OP_STRIPES,      // tessellated line fill -- the one many-quad config
    BENCH_OP_REPEAT,       // linear repeat fold (dot grid)
    BENCH_OP_REPEAT_POLAR, // polar repeat fold (dial ticks)
    BENCH_OP_PULSE,        // clock: alpha breath
    BENCH_OP_SWELL,        // clock: boundary breath
    BENCH_OP_SPIN,         // clock: the stock spinner arc
    BENCH_OP_DASH,         // dashed rounded border, marching
    BENCH_OP_CUT_SHAPE,    // true subtract: rect minus rect
    BENCH_OP_NGON,         // hexagon field
    BENCH_OP_STAR,         // 5-point star field
    BENCH_OP_SEG,          // capsule field
    BENCH_OP_ARC,          // annular sector field
    BENCH_OP_PIE,          // filled wedge field
    BENCH_OP_BEZIER,       // stroked quadratic bezier field
    BENCH_OP_TEX_SHAPE,    // baked SDF atlas field (procedural disc)

    BENCH_OP_COUNT

} bench_op_t;

/*==============================================================================================
    The baked test shape -- generated, not loaded, so the suite has no asset to go missing
==============================================================================================*/

#define BENCH_SHAPE_SRC 256u

static gui_shape_id_t s_bench_shape = GUI_SHAPE_NONE;

/* A filled disc with a margin: coverage art several times the stored size, per the bake rules. */
static void
bench_shape_init( void )
{
    static u8 s_cov[ BENCH_SHAPE_SRC * BENCH_SHAPE_SRC ];

    f32 c = ( f32 )BENCH_SHAPE_SRC * 0.5f;
    f32 r = ( f32 )BENCH_SHAPE_SRC * 0.40f;
    for ( u32 y = 0; y < BENCH_SHAPE_SRC; ++y )
        for ( u32 x = 0; x < BENCH_SHAPE_SRC; ++x )
        {
            f32 dx = ( f32 )x + 0.5f - c;
            f32 dy = ( f32 )y + 0.5f - c;
            s_cov[ y * BENCH_SHAPE_SRC + x ] = ( dx * dx + dy * dy <= r * r ) ? 255 : 0;
        }

    s_bench_shape = gui()->register_shape( "bench_disc", BENCH_SHAPE_SRC, BENCH_SHAPE_SRC,
                                           s_cov, NULL );
    if ( s_bench_shape == GUI_SHAPE_NONE )
        fprintf( stderr, "[sb_gui_bench] register_shape failed -- op_tex_shape will draw flat\n" );
}

/*==============================================================================================
    One cell of the matrix
==============================================================================================*/

static void
bench_op_cell( bench_op_t op, gui_rect_t r, u32 col )
{
    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 rr = ( r.h < r.w ? r.h : r.w ) * 0.48f;

    /* The second color of the two-color configs is CONSTANT across cells: it lands in the
       style record (unlike `col`, which rides the quad), and a per-cell value would mint one
       record per cell and overflow GUI_MAX_PRIMS across the grid. */
    u32 c2 = 0xFF804020u;

    switch ( op )
    {
        case BENCH_OP_SELF:        gui()->draw_rect( r.x, r.y, r.w, r.h, col );              break;
        case BENCH_OP_BOX_ROUND:   gui()->draw_round_rect( r, 10.0f, 10.0f, 10.0f, 10.0f,
                                                           0.0f, col );                     break;
        case BENCH_OP_GRAD:        gui()->draw_gradient( r, col, c2, true );                 break;
        case BENCH_OP_GRAD_RADIAL: gui()->draw_round_rect_gradient( r, 0.0f, col, c2,
                                                        GUI_GRAD_RADIAL, 0.0f, 0.0f );      break;
        case BENCH_OP_GRAD_CONIC:  gui()->draw_round_rect_gradient( r, 0.0f, col, c2,
                                                        GUI_GRAD_CONIC, 0.0f, 0.0f );       break;
        case BENCH_OP_FRAME:       gui()->draw_frame( r, col, c2, 2.0f );                    break;

        case BENCH_OP_GLOW:
        {
            gui_rect_t s = { r.x + 12.0f, r.y + 12.0f, r.w - 24.0f, r.h - 24.0f };
            gui()->draw_glow( s, 12.0f, col );
            break;
        }
        case BENCH_OP_SHADOW:
        {
            gui_rect_t s = { r.x + 12.0f, r.y + 12.0f, r.w - 24.0f, r.h - 24.0f };
            gui()->draw_shadow( s, 12.0f, col );
            break;
        }

        case BENCH_OP_RING:         gui()->draw_ring( r, 3.0f, col );                        break;
        case BENCH_OP_CHECKER:      gui()->draw_checker( r, 16.0f, col, c2 );                break;
        case BENCH_OP_GRID:         gui()->draw_grid( r, 16.0f, 1.0f, r.x, r.y, col );       break;
        case BENCH_OP_STRIPES:      gui()->draw_stripes( r, 10.0f, 2.0f, 0.7f, col );        break;
        case BENCH_OP_REPEAT:       gui()->draw_dot_grid( r, 8, 4, 14.0f, 14.0f, 6.0f, col ); break;
        case BENCH_OP_REPEAT_POLAR: gui()->draw_dial_ticks( r, 24, 2.0f, 8.0f, 0.0f, col );  break;
        case BENCH_OP_PULSE:        gui()->draw_pulse( r, 1.0f, 0.5f, 0.0f, col );           break;
        case BENCH_OP_SWELL:        gui()->draw_swell( r, 1.0f, 5.0f, 0.0f, col );           break;
        case BENCH_OP_SPIN:         gui()->draw_spinner( r, 1.0f, 3.0f, col );               break;
        case BENCH_OP_DASH:         gui()->draw_round_rect_dashed( r, 6.0f, 2.0f, 8.0f, 6.0f,
                                                                   20.0f, col );             break;
        case BENCH_OP_CUT_SHAPE:
        {
            gui_rect_t cut = { cx - r.w * 0.2f, cy - r.h * 0.2f, r.w * 0.4f, r.h * 0.4f };
            gui()->draw_rect_cut( r, 6.0f, cut, 6.0f, 0.0f, col );
            break;
        }

        case BENCH_OP_NGON:   gui()->draw_ngon( cx, cy, rr, 6, 0.0f, 0.0f, col );            break;
        case BENCH_OP_STAR:   gui()->draw_star( cx, cy, rr, 5, 0.5f, 0.0f, 0.0f, col );      break;
        case BENCH_OP_SEG:    gui()->draw_capsule( r.x + 6.0f, cy, r.x + r.w - 6.0f, cy,
                                                   r.h * 0.4f, col );                        break;
        case BENCH_OP_ARC:    gui()->draw_arc( cx, cy, rr, 0.0f, 4.5f, 4.0f, col );          break;
        case BENCH_OP_PIE:    gui()->draw_pie( cx, cy, rr, 0.0f, 4.5f, col );                break;
        case BENCH_OP_BEZIER: gui()->draw_bezier_quad( r.x + 4.0f, r.y + r.h - 6.0f, cx,
                                                       r.y - r.h * 0.4f, r.x + r.w - 4.0f,
                                                       r.y + r.h - 6.0f, 3.0f, col );        break;

        case BENCH_OP_TEX_SHAPE:
            if ( s_bench_shape != GUI_SHAPE_NONE ) gui()->draw_shape_in( r, s_bench_shape, col );
            else                                   gui()->draw_rect( r.x, r.y, r.w, r.h, col );
            break;

        default: break;
    }
}

static void
scene_op_matrix( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );

    gui()->window_set_next_pos ( 8.0f, 30.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 1264.0f, 682.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Ops", GUI_WIN_NONE ) )
    {
        gui()->stack();
        f32        avail = gui()->view_avail().y;
        gui_rect_t cv    = gui()->canvas( avail > 40.0f ? avail : 40.0f );

        f32 cw = cv.w / ( f32 )BENCH_OP_COLS;
        f32 ch = cv.h / ( f32 )BENCH_OP_ROWS;

        /* ONE color for the whole grid, derived from the config: several ops keep a color in
           the style record, and per-cell variety there would mint a record per cell -- a
           thousand records against GUI_MAX_PRIMS' 512.  A uniform color dedups every config
           to a handful of records and costs the fragment measurement nothing. */
        u32 col = bh_color( bh_hash( c->param ) );

        gui()->push_clip( cv.x, cv.y, cv.w, cv.h );
        for ( u32 layer = 0; layer < BENCH_OP_LAYERS; ++layer )
            for ( u32 y = 0; y < BENCH_OP_ROWS; ++y )
                for ( u32 x = 0; x < BENCH_OP_COLS; ++x )
                {
                    gui_rect_t r = { cv.x + ( f32 )x * cw, cv.y + ( f32 )y * ch, cw, ch };
                    bench_op_cell( ( bench_op_t )c->param, r, col );
                }
        gui()->pop_clip();
    }
    gui()->window_end();
}

/*==============================================================================================
    Fill-rate ladder -- N full-canvas flat layers; gpu_ms per added layer is the raw fill rate
==============================================================================================*/

static void
scene_fill_layers( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );

    gui()->window_set_next_pos ( 8.0f, 30.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 1264.0f, 682.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Fill", GUI_WIN_NONE ) )
    {
        gui()->stack();
        f32        avail = gui()->view_avail().y;
        gui_rect_t cv    = gui()->canvas( avail > 40.0f ? avail : 40.0f );

        gui()->push_clip( cv.x, cv.y, cv.w, cv.h );
        for ( u32 i = 0; i < c->param; ++i )
            gui()->draw_rect( cv.x, cv.y, cv.w, cv.h, bh_color( bh_hash( i ) ) );
        gui()->pop_clip();
    }
    gui()->window_end();
}

/*==============================================================================================
    Case tables
==============================================================================================*/

static const bench_case_t k_fill_cases[] =
{
    { "fill", "fill_1x", "1 full-canvas flat layer",  false, NULL, scene_fill_layers, 1 },
    { "fill", "fill_2x", "2 layers (2x overdraw)",    false, NULL, scene_fill_layers, 2 },
    { "fill", "fill_4x", "4 layers (4x overdraw)",    false, NULL, scene_fill_layers, 4 },
    { "fill", "fill_8x", "8 layers (8x overdraw)",    false, NULL, scene_fill_layers, 8 },
};

#define BENCH_OP_CASE( id, nm, note ) { "op", nm, note, false, NULL, scene_op_matrix, id }

static const bench_case_t k_op_cases[] =
{
    BENCH_OP_CASE( BENCH_OP_SELF,         "op_self",         "flat fill -- the delta baseline"   ),
    BENCH_OP_CASE( BENCH_OP_BOX_ROUND,    "op_box_round",    "rounded box field"                 ),
    BENCH_OP_CASE( BENCH_OP_GRAD,         "op_grad",         "linear gradient"                   ),
    BENCH_OP_CASE( BENCH_OP_GRAD_RADIAL,  "op_grad_radial",  "radial gradient"                   ),
    BENCH_OP_CASE( BENCH_OP_GRAD_CONIC,   "op_grad_conic",   "conic gradient"                    ),
    BENCH_OP_CASE( BENCH_OP_FRAME,        "op_frame",        "body + border, one record"         ),
    BENCH_OP_CASE( BENCH_OP_GLOW,         "op_glow",         "exponential glow, spread 12"       ),
    BENCH_OP_CASE( BENCH_OP_SHADOW,       "op_shadow",       "feathered shadow, spread 12"       ),
    BENCH_OP_CASE( BENCH_OP_RING,         "op_ring",         "field-borne band, 3 px"            ),
    BENCH_OP_CASE( BENCH_OP_CHECKER,      "op_checker",      "fragment checker, 16 px cells"     ),
    BENCH_OP_CASE( BENCH_OP_GRID,         "op_grid",         "fragment line lattice"             ),
    BENCH_OP_CASE( BENCH_OP_STRIPES,      "op_stripes",      "fragment stripe lattice"           ),
    BENCH_OP_CASE( BENCH_OP_REPEAT,       "op_repeat",       "linear repeat fold, 8x4 dots"      ),
    BENCH_OP_CASE( BENCH_OP_REPEAT_POLAR, "op_repeat_polar", "polar repeat fold, 24 ticks"       ),
    BENCH_OP_CASE( BENCH_OP_PULSE,        "op_pulse",        "clock alpha breath"                ),
    BENCH_OP_CASE( BENCH_OP_SWELL,        "op_swell",        "clock boundary breath"             ),
    BENCH_OP_CASE( BENCH_OP_SPIN,         "op_spin",         "spinner arc on the clock"          ),
    BENCH_OP_CASE( BENCH_OP_DASH,         "op_dash",         "marching dashed border"            ),
    BENCH_OP_CASE( BENCH_OP_CUT_SHAPE,    "op_cut_shape",    "true subtract, rect minus rect"    ),
    BENCH_OP_CASE( BENCH_OP_NGON,         "op_ngon",         "hexagon field"                     ),
    BENCH_OP_CASE( BENCH_OP_STAR,         "op_star",         "5-point star field"                ),
    BENCH_OP_CASE( BENCH_OP_SEG,          "op_seg",          "capsule field"                     ),
    BENCH_OP_CASE( BENCH_OP_ARC,          "op_arc",          "annular sector field"              ),
    BENCH_OP_CASE( BENCH_OP_PIE,          "op_pie",          "filled wedge field"                ),
    BENCH_OP_CASE( BENCH_OP_BEZIER,       "op_bezier",       "stroked quadratic bezier"          ),
    BENCH_OP_CASE( BENCH_OP_TEX_SHAPE,    "op_tex_shape",    "baked SDF disc from the atlas"     ),
};

// clang-format on
/*============================================================================================*/
