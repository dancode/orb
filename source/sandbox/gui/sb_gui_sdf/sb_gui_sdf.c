/*==============================================================================================

    sandbox/gui/sb_gui_sdf/sb_gui_sdf.c -- the DISTANCE-FIELD TEXT test bed: text that scales and
    turns, in the same draw call as the chrome around it.

    The claim this bed exists to prove, and the order the panels prove it in:

        1  THE LADDER.  One string, one scale slider, drawn twice: once from a COVERAGE bake and
           once from a DISTANCE-FIELD bake of the same face at the same size.  Both are placed by
           identical arithmetic, so every difference on screen is the sampling model and nothing
           else.  Past about 1.5x the coverage run is showing its texels; the field run has no
           size it stops being an edge at.
        2  THE TURN.  The same pair, rotated.  Rotation costs the renderer nothing at all -- a
           glyph quad is four positions and the transform is applied to them -- so what the panel
           is really showing is that the ANTIALIASING survives it.  The field's coverage comes
           from a screen-space derivative in the fragment, which knows nothing about the angle;
           there is no px_range uniform, no per-vertex parameter, and no second pipeline.
        3  THE HUD.  What all of it is for: floating combat numbers that pop, rise, turn and fade.
           Every one is a different scale and a different angle, and they are still one batch.
        4  THE FIELD.  The atlas as a picture, with the numbers -- what a distance field costs in
           texels, which is the one place it is genuinely more expensive than a bitmap.

    The batch readout under the ladder is load-bearing, not decoration: it is what says these are
    not a special path.  Two fonts split it (a coverage atlas is NEAREST, a field atlas LINEAR, so
    they cannot share a texture) but scale, rotation and count never do.

    ASSETS.  assets/font is generated and not tracked, so every font here is loaded by path and
    every panel degrades to the bake command when one is missing:

        bin\font_tool.exe CascadiaMono 32          -- the coverage twin
        bin\font_tool.exe CascadiaMono 32 -sdf     -- the distance field

==============================================================================================*/

#include <stdio.h>
#include <string.h>
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
#define AMBER     GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF )
#define TEAL      GUI_COLOR( 0x20, 0xC0, 0xB0, 0xFF )
#define CRIT      GUI_COLOR( 0xFF, 0xE0, 0x40, 0xFF )
#define HIT       GUI_COLOR( 0xFF, 0x70, 0x50, 0xFF )
#define HEAL      GUI_COLOR( 0x60, 0xE0, 0x80, 0xFF )
#define PANEL     GUI_COLOR( 0x18, 0x18, 0x1E, 0xFF )

/*==============================================================================================
    The two fonts under test

    Deliberately the SAME FACE at the SAME SIZE, so the only variable is what a texel means.  The
    coverage side falls back to the built-in 16px face when its bake is missing -- the comparison
    is then unfair in the field's favour and the panel says so, rather than quietly showing a
    16px run magnified 4x next to a 32px one.
==============================================================================================*/

#define FONT_PX      32
#define COV_ASSET    "CascadiaMono_32px.orb_font"
#define SDF_ASSET    "CascadiaMono_32px_sdf.orb_font"

static u32  s_font_cov;          /* 0 = the built-in 16px face (font id 0) */
static u32  s_font_sdf;          /* 0 = no distance-field bake found       */
static bool s_cov_is_fallback;   /* true when COV_ASSET was missing        */

/* One path under the engine root; font_load ACTIVATES what it loads, so the caller's font is put
   back.  Returns 0 when the asset is not there, which every call site treats as "say so". */
static u32
font_try( const char* asset )
{
    char path[ 576 ];
    snprintf( path, sizeof( path ), "%s/assets/font/%s", sys_root_dir(), asset );

    u32 prev = gui()->font_active_id();
    u32 id   = gui()->font_load( path );
    gui()->font_use( prev );
    return id;
}

/* Called once after boot: a font's pixels reach the GPU through the live rhi context. */
static void
load_fonts( void )
{
    s_font_cov        = font_try( COV_ASSET );
    s_cov_is_fallback = ( s_font_cov == 0 );
    s_font_sdf        = font_try( SDF_ASSET );

    printf( "[sb_gui_sdf] coverage %s   field %s\n",
            s_cov_is_fallback ? "MISSING (using the built-in 16px face)" : COV_ASSET,
            s_font_sdf ? SDF_ASSET : "MISSING" );
    fflush( stdout );
}

/* The nominal pixel size of whichever face landed on the coverage side -- the ladder labels the
   effective size (nominal * scale), and lying about it would make the whole panel misleading. */
static f32
cov_px( void )
{
    return s_cov_is_fallback ? 16.0f : (f32)FONT_PX;
}

/*==============================================================================================
    The kit-side helper the service deliberately does not have

    gui()->draw_text_xf turns a run about its ANCHOR, because that is the one pivot every other
    pivot is expressible in.  Turning about the run's own middle is the common case and it is four
    lines of arithmetic over text_size() -- exactly the sort of composition a UI kit owns.  It
    lives here, in the sandbox, for the same reason the campaign put it here: a kit's vocabulary
    is not the engine's.
==============================================================================================*/

static void
text_xf_centered( f32 cx, f32 cy, u32 col, const char* s, f32 scale, f32 rot )
{
    gui_vec2_t t  = gui()->text_size( s );
    f32        hx = t.x * scale * 0.5f;
    f32        hy = t.y * scale * 0.5f;
    f32        c  = cosf( rot );
    f32        sn = sinf( rot );
    gui()->draw_text_xf( cx - ( hx * c - hy * sn ), cy - ( hx * sn + hy * c ), col, s, scale, rot );
}

/*==============================================================================================
    Shared controls
==============================================================================================*/

/* Short on purpose: at 5x a 32px mono face spends ~96 px per character, so a longer word simply
   runs out of the half-panel it is being compared in -- and a demo that clips its own point is a
   worse demo than a short one. */
static const char* const SPECIMEN = "Sharp";

static f32  s_scale   = 2.0f;
static f32  s_rot_deg = -18.0f;
static bool s_spin    = true;
static f32  s_time;            /* the sandbox's own clock, accumulated from dt */

/* Every panel that animates has to ask for the next frame: the gui is idle-skipped, and a value
   changing in sandbox memory is not an event it can see.  One call per animating panel per frame
   covers the whole frame -- the flag is a latch, not a counter. */
static void
keep_awake( void )
{
    gui()->request_redraw();
}

/*==============================================================================================
    1  THE LADDER -- the same run through both sampling models at one scale
==============================================================================================*/

static void
panel_ladder( void )
{
    gui()->separator_text( "1  the ladder -- one string, two sampling models, one scale" );

    if ( s_font_sdf == 0 )
    {
        gui()->text_colored( AMBER, "no distance-field bake found" );
        gui()->text( "bake one:  bin\\font_tool.exe CascadiaMono 32 -sdf" );
        return;
    }
    if ( s_cov_is_fallback )
        gui()->text_colored( AMBER, "coverage side is the built-in 16px face -- "
                                    "bake CascadiaMono 32 for a like-for-like comparison" );

    gui()->slider_float( "scale", &s_scale, 0.5f, 5.0f );

    /* Tall enough for the largest run either side can produce, so the panel below never moves as
       the slider is dragged -- a layout that reflows while you scrub is a layout you cannot read. */
    gui_rect_t cell = gui()->canvas( 5.0f * (f32)FONT_PX * 1.35f + 48.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 half = cell.w * 0.5f;
    f32 x    = cell.x + 12.0f;
    f32 y    = cell.y + 8.0f;

    char cap[ 96 ];

    /* LEFT: coverage.  Placed by draw_text_xf as well, not draw_text -- if one side went through
       a different call the panel would be comparing two code paths instead of two atlases. */
    snprintf( cap, sizeof( cap ), "coverage  %.0fpx -> %.0fpx", cov_px(), cov_px() * s_scale );
    gui()->draw_text( x, y, INK_DIM, cap );
    gui()->font_use( s_font_cov );
    gui()->draw_text_xf( x, y + 26.0f, INK, SPECIMEN, s_scale, 0.0f );
    gui()->font_use( 0 );

    /* RIGHT: the distance field.  Same anchor arithmetic, same scale, same colour. */
    snprintf( cap, sizeof( cap ), "distance field  %dpx -> %.0fpx", FONT_PX, (f32)FONT_PX * s_scale );
    gui()->draw_text( x + half, y, INK_DIM, cap );
    gui()->font_use( s_font_sdf );
    gui()->draw_text_xf( x + half, y + 26.0f, INK, SPECIMEN, s_scale, 0.0f );
    gui()->font_use( 0 );

    gui()->draw_rect( cell.x + half - 1.0f, cell.y, 1.0f, cell.h, INK_DIM );
    gui()->pop_clip();

    /* The batch readout.  Two fonts is two atlases is two draw calls for the text, plus chrome's
       own -- and that number does NOT move when the scale, the angle or the glyph count does. */
    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "frame: %u draw calls   %u verts   %u tris   (scale and rotation add none)",
                  rs.draw_calls, rs.vert_count, rs.tri_count );
}

/*==============================================================================================
    2  THE TURN -- the same pair rotated
==============================================================================================*/

static void
panel_turn( void )
{
    gui()->separator_text( "2  the turn -- antialiasing that does not know the angle" );

    if ( s_font_sdf == 0 )
        return;

    gui()->checkbox( "spin", &s_spin );
    if ( !s_spin )
        gui()->slider_float( "angle (degrees)", &s_rot_deg, -180.0f, 180.0f );

    f32 rot = s_spin ? s_time * 0.6f : gui_radians( s_rot_deg );
    if ( s_spin )
        keep_awake();

    gui_rect_t cell = gui()->canvas( 220.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 half = cell.w * 0.5f;
    f32 cy   = cell.y + cell.h * 0.5f;

    gui()->draw_text( cell.x + 12.0f, cell.y + 6.0f, INK_DIM, "coverage" );
    gui()->font_use( s_font_cov );
    text_xf_centered( cell.x + half * 0.5f, cy, INK, "Rotate", 1.6f, rot );
    gui()->font_use( 0 );

    gui()->draw_text( cell.x + half + 12.0f, cell.y + 6.0f, INK_DIM, "distance field" );
    gui()->font_use( s_font_sdf );
    text_xf_centered( cell.x + half * 1.5f, cy, INK, "Rotate", 1.6f, rot );
    gui()->font_use( 0 );

    gui()->draw_rect( cell.x + half - 1.0f, cell.y, 1.0f, cell.h, INK_DIM );
    gui()->pop_clip();

    gui()->text( "both runs are turned about their own middle by the kit helper "
                 "(text_size + one rotation); the service verb turns about the anchor" );
}

/*==============================================================================================
    3  THE HUD -- what the door is for

    Floating combat numbers: the canonical thing an in-game UI does that an editor never asks for.
    Each pop owns a scale curve, an angle, a colour and a lifetime, all derived from its INDEX so
    the scene is deterministic and reproducible frame to frame (the same reason a volatile block
    derives its variety from its id and never from its position).
==============================================================================================*/

#define POP_COUNT  9
#define POP_LIFE   2.2f

static const struct { const char* text; u32 col; f32 peak; } k_pops[ POP_COUNT ] = {
    { "127",       HIT,  1.5f },
    { "CRIT 418!", CRIT, 2.4f },
    { "63",        HIT,  1.2f },
    { "+240",      HEAL, 1.4f },
    { "91",        HIT,  1.3f },
    { "MISS",      INK_DIM, 1.1f },
    { "CRIT 502!", CRIT, 2.6f },
    { "34",        HIT,  1.0f },
    { "+80",       HEAL, 1.2f },
};

static void
panel_hud( void )
{
    gui()->separator_text( "3  the hud -- nine scales, nine angles, one batch" );

    if ( s_font_sdf == 0 )
        return;

    keep_awake();

    gui_rect_t cell = gui()->canvas( 300.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    gui()->font_use( s_font_sdf );

    for ( u32 i = 0; i < POP_COUNT; ++i )
    {
        /* Phase from the index: evenly spread, so the field is never empty and never synchronized. */
        f32 phase = fmodf( s_time * 0.55f + (f32)i * ( POP_LIFE / (f32)POP_COUNT ), POP_LIFE );
        f32 t     = phase / POP_LIFE;                       /* 0..1 over the pop's life */

        /* Scale: a fast overshoot to `peak` in the first fifth, then a slow settle to 70% of it --
           the shape that reads as an impact.  This is the whole reason the text has to scale at
           all: at a fixed size the same motion reads as a label sliding, not a hit landing. */
        f32 s = ( t < 0.2f ) ? ( t / 0.2f ) * k_pops[ i ].peak
                             : k_pops[ i ].peak * ( 1.0f - 0.3f * ( t - 0.2f ) / 0.8f );

        /* A small fixed tilt per pop plus a slow drift, so no two are at the same angle. */
        f32 rot = gui_radians( -14.0f + (f32)( i % 5 ) * 7.0f ) + sinf( s_time * 0.7f + (f32)i ) * 0.08f;

        /* Rise and fade over the life.  Three lanes at quarter, half and three-quarter width, so
           even the widest pop at its peak scale stays clear of both edges; the row offset keeps
           the three pops sharing a lane from tracing the same line. */
        f32 x = cell.x + cell.w * ( 0.25f + 0.25f * (f32)( i % 3 ) ) + (f32)( i / 3 ) * 18.0f;
        f32 y = cell.y + cell.h - 40.0f - t * ( cell.h - 90.0f );
        u32 a = (u32)( 255.0f * ( t > 0.75f ? ( 1.0f - t ) / 0.25f : 1.0f ) );

        text_xf_centered( x, y, ( k_pops[ i ].col & 0x00FFFFFFu ) | ( a << 24 ),
                          k_pops[ i ].text, s, rot );
    }

    gui()->font_use( 0 );
    gui()->pop_clip();

    /* One font, one clip, one atlas -- so the nine of them are ONE batch.  The frame's total is
       higher only because each panel above pushes a clip of its own and the coverage side is a
       second atlas; no part of it is the transform, which is why it does not move when the pops
       are at nine different scales and nine different angles. */
    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "%u pops in ONE batch -- the frame's %u draw calls are panels and fonts, "
                  "never transforms", (u32)POP_COUNT, rs.draw_calls );
}

/*==============================================================================================
    4  THE FIELD -- the atlas as a picture, and what it costs
==============================================================================================*/

static bool s_show_field = true;

static void
panel_field( void )
{
    gui()->separator_text( "4  the field -- what is actually in the atlas" );

    if ( s_font_sdf == 0 )
        return;

    gui()->checkbox( "show both atlases", &s_show_field );

    gui_vec2_t cov_sz = gui()->font_atlas_size( s_font_cov );
    gui_vec2_t sdf_sz = gui()->font_atlas_size( s_font_sdf );
    gui()->textf( "coverage atlas %.0fx%.0f (R8, NEAREST)    field atlas %.0fx%.0f (R8, LINEAR)",
                  cov_sz.x, cov_sz.y, sdf_sz.x, sdf_sz.y );

    if ( !s_show_field )
        return;

    /* An R8 texture shown through the RGBA sampling model reads as a RED channel -- a format with
       no green, blue or alpha samples them as 0, 0, 1.  That is fine and it is not what to look
       at: what is worth seeing is the SHAPE of the data.  The coverage atlas holds hard islands
       of ink; the field holds a soft ramp reaching `spread` pixels out from every outline, which
       is exactly the information the fragment differentiates and exactly what the extra texels
       are buying. */
    gui_rect_t r = gui()->canvas( 250.0f );
    f32        w = ( r.w - 24.0f ) * 0.5f;

    gui()->draw_text( r.x, r.y, INK_DIM, "coverage" );
    gui()->draw_texture_in( ( gui_rect_t ){ r.x, r.y + 20.0f, w, 220.0f },
                            gui()->font_atlas_idx( s_font_cov ), 0xFFFFFFFFu );

    gui()->draw_text( r.x + w + 24.0f, r.y, INK_DIM, "distance field" );
    gui()->draw_texture_in( ( gui_rect_t ){ r.x + w + 24.0f, r.y + 20.0f, w, 220.0f },
                            gui()->font_atlas_idx( s_font_sdf ), 0xFFFFFFFFu );
}

/*==============================================================================================
    Frame
==============================================================================================*/

static void
build_frame( void )
{
    gui()->window_set_next_pos( 24.0f, 24.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 1180.0f, 940.0f, GUI_COND_ONCE );

    if ( gui()->window_begin( "distance-field text", GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->text( "One renderer, one vertex format, one batch key.  The two products fork at "
                     "the ATLAS: chrome keeps a NEAREST coverage bake and stays pixel-crisp, a "
                     "game kit loads a LINEAR distance-field bake and scales." );

        panel_ladder();
        panel_turn();
        panel_hud();
        panel_field();
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
        fprintf( stderr, "[sb_gui_sdf] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui sdf text",
        .w         = 1240, .h = 1000,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.10f, 0.10f, 0.13f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_sdf] gui->boot failed\n" );
        goto shutdown;
    }

    load_fonts();

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

        gui()->frame_pace( 4, 16 );
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
