/*==============================================================================================

    sandbox/gui/sb_gui_line/sb_gui_line.c -- the STROKE test bed: what a line looks like when the
    fragment resolves it instead of the tessellator approximating it.

    A diagonal line used to be a RIBBON: a solid core quad flanked by two bands dropped to alpha 0,
    so the hardware's colour interpolation faked an edge gradient.  It is now a CAPSULE -- the
    distance from a pixel to the segment, minus the half-thickness, evaluated per fragment
    (GUI_FX_SEG, gui.h).  The panels are ordered to make each consequence of that separately
    visible, and each one is a claim you can disprove on screen:

        1  THE FAN.  The same spokes drawn twice: LEFT by draw_line (capsule), RIGHT by a 2-point
           draw_polyline (ribbon).  Identical requests, two code paths, side by side.  What to look
           for is the SHALLOW angles -- near-horizontal and near-vertical spokes are where a
           geometric feather has the least room to work and where the field does not care.
        2  THE CAPS.  One thick stroke, magnified, with its true endpoints marked.  A capsule has
           round caps because the field IS round there; they extend half a thickness past the
           endpoint, and this panel is where you decide whether that matters to you.
        3  THE JOINT.  The negative result, and the reason polylines were NOT converted: a zigzag
           drawn as a real polyline vs the same zigzag as a chain of independent segments, with an
           ALPHA slider.  Drop the alpha and the chain grows a bead at every joint, because two
           overlapping translucent capsules composite darker than one.  The ribbon's miter solve
           exists precisely to emit each pixel once.
        4  THE HAIRLINE.  A thickness sweep through and below one pixel.  Sub-pixel strokes hold a
           1 px footprint and fade alpha instead of shrinking, matched to the ribbon on purpose so
           the two paths weigh a hairline the same.
        5  THE COST.  Geometry per stroke, measured rather than asserted.

    The rotation is not decoration: antialiasing is hard to judge on a still image and obvious in
    motion, because a bad edge CRAWLS as the angle changes.  Let the fan spin.

==============================================================================================*/

#include <stdio.h>
#include <math.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

#define INK       GUI_COLOR( 0xE8, 0xE0, 0xD0, 0xFF )
#define INK_DIM   GUI_COLOR( 0x90, 0x8C, 0x84, 0xFF )
#define PANEL     GUI_COLOR( 0x16, 0x16, 0x1C, 0xFF )
#define EDGE      GUI_COLOR( 0x3A, 0x3A, 0x44, 0xFF )
#define STROKE    GUI_COLOR( 0xF0, 0xC8, 0x60, 0xFF )
#define STROKE_B  GUI_COLOR( 0x70, 0xC0, 0xFF, 0xFF )
#define MARK      GUI_COLOR( 0xFF, 0x50, 0x50, 0xFF )

static f32 s_time = 0.0f;

/* Every panel here animates or wants to, and the gui only presents a frame when something asks
   for one -- a shader-driven or time-driven change raises no emit of its own (gui.h). */
static void
keep_awake( void )
{
    gui()->request_redraw();
}

/*==============================================================================================
    1  THE FAN -- capsule vs ribbon, same spokes, side by side
==============================================================================================*/

static f32  s_fan_th   = 3.0f;
static bool s_fan_spin = true;
static f32  s_fan_rot  = 0.0f;
static i32  s_fan_n    = 16;

/* One half of the fan.  `capsule` picks the path: draw_line reaches tess_fx_segment for a
   diagonal, while a 2-point draw_polyline is the ribbon stroker no matter the angle -- which is
   exactly what makes this a controlled comparison rather than two different pictures. */
static void
fan_half( gui_rect_t box, bool capsule, f32 rot, u32 col )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 r  = ( box.w < box.h ? box.w : box.h ) * 0.5f - 12.0f;

    for ( i32 i = 0; i < s_fan_n; ++i )
    {
        /* Half a turn is a whole fan: a spoke and its opposite are the same line. */
        f32 a  = rot + 3.14159265f * (f32)i / (f32)s_fan_n;
        f32 dx = cosf( a ) * r, dy = sinf( a ) * r;

        if ( capsule )
            gui()->draw_line( cx - dx, cy - dy, cx + dx, cy + dy, s_fan_th, col );
        else
        {
            gui_vec2_t pts[ 2 ] = { { cx - dx, cy - dy }, { cx + dx, cy + dy } };
            gui()->draw_polyline( pts, 2, s_fan_th, GUI_STROKE_CENTER, false, col );
        }
    }
}

static void
panel_fan( void )
{
    gui()->separator_text( "1  the fan -- capsule (left) vs ribbon (right), the same spokes" );

    gui()->checkbox( "spin", &s_fan_spin );
    gui()->slider_float( "thickness (px)", &s_fan_th, 0.25f, 24.0f );
    gui()->slider_int( "spokes", &s_fan_n, 2, 64 );

    if ( s_fan_spin )
    {
        keep_awake();
        s_fan_rot = s_time * 0.25f;
    }

    gui_rect_t cell = gui()->canvas( 340.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 half = cell.w * 0.5f;
    gui()->draw_line( cell.x + half, cell.y, cell.x + half, cell.y + cell.h, 1.0f, EDGE );

    fan_half( ( gui_rect_t ){ cell.x,        cell.y, half, cell.h }, true,  s_fan_rot, STROKE );
    fan_half( ( gui_rect_t ){ cell.x + half, cell.y, half, cell.h }, false, s_fan_rot, STROKE_B );

    gui()->draw_text( cell.x + 10.0f,        cell.y + 8.0f, INK_DIM, "capsule  (draw_line)" );
    gui()->draw_text( cell.x + half + 10.0f, cell.y + 8.0f, INK_DIM, "ribbon  (draw_polyline, n=2)" );

    gui()->pop_clip();

    gui()->text( "Both sides are one batch with the panel behind them -- an effect rides the "
                 "VERTEX, so a stroke that resolves in the fragment still cannot split a draw.  "
                 "Watch for a spoke passing through exact horizontal or vertical: on the LEFT it "
                 "snaps to the crisp axis-aligned quad for that instant, which is a third path and "
                 "deliberately not the capsule." );
}

/*==============================================================================================
    2  THE CAPS -- what the round end actually does
==============================================================================================*/

static f32 s_cap_th = 18.0f;

static void
panel_caps( void )
{
    gui()->separator_text( "2  the caps -- round, and half a thickness long" );

    gui()->slider_float( "thickness (px)", &s_cap_th, 2.0f, 48.0f );

    gui_rect_t cell = gui()->canvas( 190.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    /* Two strokes sharing an endpoint, so the cap is visible both against the background and
       against a neighbour.  The endpoints are marked because the claim is specifically about how
       far past them the ink reaches: r = thickness / 2, and no further. */
    f32 y0 = cell.y + 50.0f;
    f32 x0 = cell.x + 70.0f;
    f32 x1 = cell.x + cell.w * 0.45f;
    f32 y1 = cell.y + cell.h - 50.0f;

    gui()->draw_line( x0, y0, x1, y1, s_cap_th, STROKE );
    gui()->draw_line( x1, y1, cell.x + cell.w - 70.0f, y0, s_cap_th, STROKE_B );

    /* The true endpoints -- a 3 px dot each.  A capsule's ink stops exactly `thickness / 2` past
       these, which is what a round cap IS; the old ribbon stopped square on them. */
    gui()->draw_circle( x0, y0, 2.5f, 0.0f, MARK );
    gui()->draw_circle( x1, y1, 2.5f, 0.0f, MARK );
    gui()->draw_circle( cell.x + cell.w - 70.0f, y0, 2.5f, 0.0f, MARK );

    gui()->pop_clip();

    gui()->textf( "red dots are the requested endpoints; the ink runs %.1f px past each "
                  "(thickness / 2)", s_cap_th * 0.5f );
}

/*==============================================================================================
    3  THE JOINT -- why polylines were not converted

    This panel is a NEGATIVE result kept on screen on purpose.  It would have been easy to route
    every stroke through the capsule and call the item finished; what stopped it is visible here
    the moment the alpha comes down.
==============================================================================================*/

static f32 s_joint_a  = 1.0f;
static f32 s_joint_th = 12.0f;

static void
panel_joint( void )
{
    gui()->separator_text( "3  the joint -- one ribbon vs a chain of capsules" );

    gui()->slider_float( "alpha", &s_joint_a, 0.05f, 1.0f );
    gui()->slider_float( "thickness (px)", &s_joint_th, 2.0f, 32.0f );

    gui_rect_t cell = gui()->canvas( 200.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    u32 col = ( STROKE & 0x00FFFFFFu ) | ( (u32)( s_joint_a * 255.0f + 0.5f ) << 24 );

    /* The same zigzag twice.  Left: ONE polyline, whose miter solve emits every pixel once.
       Right: the identical points as independent segments, so the capsules overlap at each
       vertex and the overlap composites over itself. */
    for ( u32 side = 0; side < 2; ++side )
    {
        f32 ox = cell.x + ( side ? cell.w * 0.5f : 0.0f ) + 40.0f;
        f32 w  = cell.w * 0.5f - 80.0f;

        gui_vec2_t pts[ 5 ];
        for ( u32 i = 0; i < 5; ++i )
        {
            pts[ i ].x = ox + w * (f32)i / 4.0f;
            pts[ i ].y = cell.y + ( ( i & 1u ) ? cell.h - 60.0f : 60.0f );
        }

        if ( side == 0 )
            gui()->draw_polyline( pts, 5, s_joint_th, GUI_STROKE_CENTER, false, col );
        else
            for ( u32 i = 0; i + 1 < 5; ++i )
                gui()->draw_line( pts[ i ].x, pts[ i ].y,
                                  pts[ i + 1 ].x, pts[ i + 1 ].y, s_joint_th, col );
    }

    gui()->draw_line( cell.x + cell.w * 0.5f, cell.y,
                      cell.x + cell.w * 0.5f, cell.y + cell.h, 1.0f, EDGE );
    gui()->draw_text( cell.x + 10.0f, cell.y + 8.0f, INK_DIM, "polyline (ribbon, mitered)" );
    gui()->draw_text( cell.x + cell.w * 0.5f + 10.0f, cell.y + 8.0f, INK_DIM,
                      "4 separate lines (capsules)" );

    gui()->pop_clip();

    gui()->text( "At alpha 1 the two are equivalent.  Bring the alpha down and the right side "
                 "beads at every vertex: overlapping translucent strokes composite darker.  That "
                 "is the whole reason draw_polyline still builds a single non-overlapping ribbon." );
}

/*==============================================================================================
    4  THE HAIRLINE -- through and below one pixel
==============================================================================================*/

static void
panel_hairline( void )
{
    gui()->separator_text( "4  the hairline -- sub-pixel strokes hold their weight" );

    gui_rect_t cell = gui()->canvas( 150.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    /* A ladder of widths at a fixed shallow angle.  Below 1 px the stroke keeps a one-pixel
       footprint and fades its alpha rather than thinning -- a genuinely 0.3 px capsule would be
       correct and would also make hairlines here disagree with hairlines everywhere else. */
    static const f32 k_w[] = { 0.15f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };
    u32   n  = (u32)( sizeof k_w / sizeof k_w[ 0 ] );
    f32   dx = ( cell.w - 80.0f ) / (f32)n;

    for ( u32 i = 0; i < n; ++i )
    {
        f32  x = cell.x + 40.0f + dx * ( (f32)i + 0.5f );
        char tag[ 12 ];
        gui()->draw_line( x - 10.0f, cell.y + 34.0f, x + 10.0f, cell.y + cell.h - 26.0f,
                          k_w[ i ], STROKE );
        snprintf( tag, sizeof( tag ), "%.2f", k_w[ i ] );
        gui()->draw_text( x - 14.0f, cell.y + cell.h - 20.0f, INK_DIM, tag );
    }

    gui()->pop_clip();
}

/*==============================================================================================
    5  THE COST
==============================================================================================*/

static void
panel_cost( void )
{
    gui()->separator_text( "5  the cost" );

    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "frame: %u draw calls   %u quads   %u styles", rs.draw_calls, rs.quad_count,
                  rs.prim_count );
    gui()->text( "A stroke costs ONE quad record whatever its angle: the capsule field resolves "
                 "the rounded ends and the antialiased edge in the fragment.  An AXIS-ALIGNED line "
                 "still takes the grid-snapped path instead, because a horizontal edge has nothing "
                 "to antialias and the field would only make it blurrier." );
}

/*==============================================================================================
    Frame
==============================================================================================*/

static void
build_frame( void )
{
    gui()->window_set_next_pos( 24.0f, 24.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 1120.0f, 960.0f, GUI_COND_ONCE );

    if ( gui()->window_begin( "strokes", GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->text( "A diagonal stroke is a CAPSULE resolved in the fragment: the distance to "
                     "the segment, minus its half-thickness.  Exact at every angle, round caps "
                     "for free, and two quads instead of three bands." );

        panel_fan();
        panel_caps();
        panel_joint();
        panel_hairline();
        panel_cost();
    }
    gui()->window_end();
}

/*==============================================================================================
    Host
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_line] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui strokes",
        .w         = 1180, .h = 1020,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.10f, 0.10f, 0.13f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_line] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        s_time += dt;

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        gui()->boot_pace ( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
/*============================================================================================*/
