/*==============================================================================================

    sandbox/gui/sb_gui_sdf/sb_gui_sdf.c -- the DISTANCE-FIELD demo suite: everything the SDF
    effect band and the SDF sampling model buy the gui, one window per claim.

    Grown from the original distance-field TEXT bed (its five panels survive intact as the
    "SDF Text" window) into a registry of demo windows, all hidden by default behind a menu:
    the point of the suite is no longer to prove one feature works but to map what the feature
    UNLOCKS -- which compositions are now one quad and one batch that used to be a tessellated
    fan or a second pass.

        SDF Text            the original bed: ladder / turn / hud / atlas / edge
        Shape Economy       every SDF primitive next to its vertex price
        Gauges & Meters     radial gauge, progress rings, spinners -- arcs as instruments
        Charts & Data       donut with hover, sparkline, a rect_list bar wall
        Depth & Motion      shadow elevation, glow, pulse, badges, a capsule toggle
        Radial Menu         pie/arc wedges as hit-tested interactive UI
        Dials               a draggable knob, a clock, a compass with rotated labels
        Frontier Notes      what is one small piece short of working (read it)

    Every animated window owes gui()->request_redraw() per frame (the gui is idle-skipped and
    sandbox memory changing is not an event it can see) -- keep_awake() below.

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
#define EDGE      GUI_COLOR( 0x3A, 0x3A, 0x44, 0xFF )
#define ACCENT    GUI_COLOR( 0x4C, 0x9E, 0xFF, 0xFF )
#define VIOLET    GUI_COLOR( 0xB0, 0x70, 0xFF, 0xFF )

#define TAU       6.28318530717959f

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
    Kit-side helpers the service deliberately does not have

    gui()->draw_text_xf turns a run about its ANCHOR, because that is the one pivot every other
    pivot is expressible in.  Turning about the run's own middle is the common case and it is four
    lines of arithmetic over text_size() -- exactly the sort of composition a UI kit owns.
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

/* A centered SDF label at any scale -- the instrument-readout idiom every dial below uses.
   Falls back to the built-in face when no field bake is present (it draws, just not as well). */
static void
dial_label( f32 cx, f32 cy, u32 col, const char* s, f32 scale )
{
    gui()->font_use( s_font_sdf );
    text_xf_centered( cx, cy, col, s, scale, 0.0f );
    gui()->font_use( 0 );
}

static f32  s_time;            /* the sandbox's own clock, accumulated from dt */

/* Every panel that animates has to ask for the next frame: the gui is idle-skipped, and a value
   changing in sandbox memory is not an event it can see.  One call per animating panel per frame
   covers the whole frame -- the flag is a latch, not a counter. */
static void
keep_awake( void )
{
    gui()->request_redraw();
}

/* Amber one-liner for the windows whose whole point is scaled text. */
static bool
sdf_required( void )
{
    if ( s_font_sdf )
        return true;
    gui()->text_colored( AMBER, "no distance-field bake found" );
    gui()->text( "bake one:  bin\\font_tool.exe CascadiaMono 32 -sdf" );
    return false;
}

/*==============================================================================================
    1  THE LADDER -- the same run through both sampling models at one scale
==============================================================================================*/

/* Five characters, chosen not typeset: at 5x a 32px mono face spends ~96 px each, so a longer word
   runs out of the half-panel it is being compared in.  These five are the ones that BROKE -- 'a',
   'g' and 'e' join a bowl to a stroke and '0' is Cascadia's dotted zero, which are exactly the two
   shapes FreeType's contour merge gets wrong (font_tool's sdf_repair_signs).  If a bake ever
   regresses there, it shows up here first and at the largest size the panel offers. */
static const char* const SPECIMEN = "Page0";

static f32  s_scale   = 2.0f;
static f32  s_rot_deg = -18.0f;
static bool s_spin    = true;

static void
panel_ladder( void )
{
    gui()->separator_text( "1  the ladder -- one string, two sampling models, one scale" );

    if ( !sdf_required() )
        return;
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

        /* Crits ride a rotated PLATE -- the loot-label idiom draw_box_xf exists for: the rounded
           box turns with the run (same angle, same centre), so the pair reads as one object. */
        if ( k_pops[ i ].peak >= 2.0f )
        {
            gui_vec2_t ts = gui()->text_size( k_pops[ i ].text );
            f32 pw = ts.x * s + 20.0f, ph = ts.y * s + 10.0f;
            gui()->draw_box_xf( ( gui_rect_t ){ x - pw * 0.5f, y - ph * 0.5f, pw, ph },
                                8.0f, 0.0f, rot,
                                GUI_COLOR( 0x38, 0x18, 0x00, (u8)( a * 3 / 4 ) ) );
        }

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

/* One atlas preview, fitted rather than filled.
   An atlas is a picture with a fixed shape -- 512x512 for the coverage bake, 1024x512 for the
   field -- and draw_texture_in FILLS the rect it is given.  Handing both the same box therefore
   squashes one and stretches the other, which makes two identical bakes look like different ones
   and hides the fact that the field atlas is the WIDER texture.  So the box gives, not the image:
   scale to the tighter axis, centre the result, and outline the slot so the unused space is
   visible as unused space rather than read as part of the atlas. */
static void
atlas_preview( gui_rect_t slot, const char* label, u32 font_id, gui_vec2_t px )
{
    gui()->draw_text( slot.x, slot.y, INK_DIM, label );

    gui_rect_t box = { slot.x, slot.y + 20.0f, slot.w, slot.h - 20.0f };
    if ( px.x <= 0.0f || px.y <= 0.0f )
        return;

    f32 s = ( box.w / px.x < box.h / px.y ) ? box.w / px.x : box.h / px.y;
    f32 w = px.x * s, h = px.y * s;
    gui_rect_t fit = { box.x + ( box.w - w ) * 0.5f, box.y + ( box.h - h ) * 0.5f, w, h };

    gui()->draw_frame( box, 0u, EDGE, 1.0f );
    gui()->draw_texture_in( fit, gui()->font_atlas_idx( font_id ), 0xFFFFFFFFu );

    char cap[ 64 ];
    snprintf( cap, sizeof( cap ), "%.0fx%.0f at %.0f%%", px.x, px.y, s * 100.0f );
    gui()->draw_text( slot.x, slot.y + slot.h - 14.0f, INK_DIM, cap );
}

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
    gui_rect_t r = gui()->canvas( 270.0f );
    f32        w = ( r.w - 24.0f ) * 0.5f;

    atlas_preview( ( gui_rect_t ){ r.x, r.y, w, 250.0f }, "coverage", s_font_cov, cov_sz );
    atlas_preview( ( gui_rect_t ){ r.x + w + 24.0f, r.y, w, 250.0f },
                   "distance field", s_font_sdf, sdf_sz );
}

/*==============================================================================================
    5  THE EDGE -- a second colour outside the glyph, from the same quad
==============================================================================================*/

static f32  s_edge_w   = 2.0f;
static f32  s_edge_sc  = 2.2f;
static bool s_edge_on  = true;

static void
panel_edge( void )
{
    gui()->separator_text( "5  the edge -- outline and shadow with no second pass" );

    if ( s_font_sdf == 0 )
        return;

    gui()->checkbox( "outline", &s_edge_on );
    gui()->slider_float( "width (px)", &s_edge_w, 0.0f, 8.0f );
    gui()->slider_float( "scale", &s_edge_sc, 0.8f, 4.0f );

    gui_rect_t cell = gui()->canvas( 220.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    /* The width is authored in glyph-space pixels, so it scales with the text -- an outline that
       stayed 2 px while the run grew to 4x would read as a hairline. */
    f32 w   = s_edge_on ? s_edge_w * s_edge_sc : 0.0f;
    f32 x   = cell.x + 24.0f;
    f32 row = cell.y + 46.0f;

    gui()->font_use( s_font_sdf );

    /* Three uses of the SAME mechanism: an outline that separates light text from a busy ground,
       a heavier dark rim, and a coloured glow.  Only the packed word differs between them. */
    static const struct { const char* label; u32 fill; u32 edge; } k_edge[] = {
        { "Outlined",  GUI_COLOR( 0xF4, 0xEC, 0xDC, 0xFF ), GUI_COLOR( 0x00, 0x00, 0x00, 0xFF ) },
        { "Heavy rim", GUI_COLOR( 0xFF, 0xC8, 0x40, 0xFF ), GUI_COLOR( 0x20, 0x10, 0x00, 0xFF ) },
        { "Glow",      GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xFF ), GUI_COLOR( 0x30, 0x90, 0xFF, 0xC0 ) },
    };

    for ( u32 i = 0; i < 3; ++i )
    {
        u32 save = gui()->draw_text_edge();
        gui()->draw_set_text_edge( w, k_edge[ i ].edge );
        gui()->draw_text_xf( x, row, k_edge[ i ].fill, k_edge[ i ].label, s_edge_sc, 0.0f );
        gui()->draw_set_text_edge_raw( save );
        row += 26.0f * s_edge_sc + 8.0f;
    }

    /* The coverage twin, with the identical setting applied -- it should look exactly as it does
       with the outline off.  That is the ignore, not a bug. */
    if ( s_font_cov )
    {
        u32 save = gui()->draw_text_edge();
        gui()->font_use( s_font_cov );
        gui()->draw_set_text_edge( w, GUI_COLOR( 0x00, 0x00, 0x00, 0xFF ) );
        gui()->draw_text_xf( cell.x + cell.w * 0.55f, cell.y + 46.0f,
                             GUI_COLOR( 0xF4, 0xEC, 0xDC, 0xFF ), "coverage", s_edge_sc, 0.0f );
        gui()->draw_set_text_edge_raw( save );
    }

    gui()->font_use( 0 );
    gui()->pop_clip();

    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "%u draw calls   %u verts -- toggle the outline and watch neither move; a "
                  "second offset run would add four verts per glyph",
                  rs.draw_calls, rs.vert_count );
}

/*==============================================================================================
    WINDOW: SDF Text -- the original bed, five panels in their proving order.
==============================================================================================*/

static void
win_text( void )
{
    gui()->stack();
    gui()->text( "One renderer, one vertex format, one batch key.  The two products fork at "
                 "the ATLAS: chrome keeps a NEAREST coverage bake and stays pixel-crisp, a "
                 "game kit loads a LINEAR distance-field bake and scales." );
    panel_ladder();
    panel_turn();
    panel_hud();
    panel_edge();
    panel_field();
}

/*==============================================================================================
    WINDOW: Shape Economy -- every SDF primitive next to the price it pays.

    The claim: shapes that used to be tessellated fans are now ONE QUAD whose fragment resolves
    the boundary analytically.  The ngon at the end is the counter-example on purpose -- it still
    walks a fan, so its vertex price scales with its side count while every field shape's price
    is a constant.  Scrub the sliders and watch the vert counter: parameters move the FIELD, not
    the geometry.
==============================================================================================*/

static f32 s_sh_sweep = 240.0f;   /* arc / pie sweep, degrees */
static f32 s_sh_thick = 10.0f;    /* stroke width             */
static f32 s_sh_round = 14.0f;    /* per-corner radius seed   */

static void
win_shapes( void )
{
    gui()->stack();
    gui()->text( "Each shape below is ONE 4-vert quad (the per-corner box is four).  The "
                 "parameters change the packed effect word, never the vertex count." );

    gui()->slider_float( "sweep (deg)", &s_sh_sweep, 10.0f, 360.0f );
    gui()->slider_float( "thickness",   &s_sh_thick, 2.0f,  15.0f );
    gui()->slider_float( "corner",      &s_sh_round, 0.0f,  30.0f );

    gui_rect_t cell = gui()->canvas( 190.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    /* Seven stations, evenly spread; each labels itself and its vertex price. */
    static const char* k_names[ 7 ] =
        { "disc", "ring", "arc", "pie", "capsule", "corners", "ngon (fan)" };
    f32 step = cell.w / 7.0f;
    f32 cy   = cell.y + cell.h * 0.5f - 12.0f;
    f32 r    = ( step * 0.5f - 14.0f < 34.0f ) ? step * 0.5f - 14.0f : 34.0f;

    f32 a0 = gui_radians( -90.0f );
    f32 a1 = a0 + gui_radians( s_sh_sweep );

    for ( u32 i = 0; i < 7; ++i )
    {
        f32 cx = cell.x + step * ( (f32)i + 0.5f );
        switch ( i )
        {
            case 0: gui()->draw_circle( cx, cy, r, true, 0.0f, TEAL );                    break;
            case 1: gui()->draw_circle( cx, cy, r, false, s_sh_thick, ACCENT );           break;
            case 2: gui()->draw_arc( cx, cy, r, a0, a1, s_sh_thick, AMBER );              break;
            case 3: gui()->draw_pie( cx, cy, r, a0, a1, VIOLET );                         break;
            case 4: gui()->draw_line( cx - r * 0.8f, cy + r * 0.7f,
                                      cx + r * 0.8f, cy - r * 0.7f, s_sh_thick, HEAL );   break;
            case 5: gui()->draw_round_rect( ( gui_rect_t ){ cx - r, cy - r * 0.75f,
                                                            r * 2.0f, r * 1.5f },
                                            s_sh_round, 2.0f, s_sh_round * 0.5f, 2.0f,
                                            true, 0.0f, HIT );                            break;
            case 6: gui()->draw_ngon( cx, cy, r, 6, s_time * 0.4f, true, 0.0f, INK_DIM ); break;
        }
        text_xf_centered( cx, cell.y + cell.h - 16.0f, INK_DIM, k_names[ i ], 0.9f, 0.0f );
    }
    gui()->pop_clip();
    keep_awake();   /* the ngon spins to show a fan re-tessellates while a field never does */

    gui()->text( "full-turn sweep routes ARC to the exact ring and PIE to the exact disc; "
                 "reversed angle ranges normalize at tessellation -- animate angles raw" );

    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "frame: %u verts.  The old sampled arc cost up to 130 verts ALONE; "
                  "six of the seven shapes above cost 4 each.", rs.vert_count );
}

/*==============================================================================================
    WINDOW: Gauges & Meters -- arcs as instruments.

    Composition demos: nothing here is a new primitive.  A radial gauge is a background arc, a
    value arc, tick capsules, and one scaled SDF readout in the middle -- five verbs that already
    exist, arranged.  This window is what the ARC mode was FOR.
==============================================================================================*/

static f32  s_gauge_v   = 0.62f;   /* 0..1 */
static bool s_gauge_auto = true;

/* The 270-degree instrument sweep: down-left, clockwise through up, to down-right. */
#define GAUGE_A0   135.0f
#define GAUGE_SWEEP 270.0f

static void
gauge_radial( f32 cx, f32 cy, f32 r, f32 v, u32 col )
{
    f32 a0 = gui_radians( GAUGE_A0 );
    f32 av = a0 + gui_radians( GAUGE_SWEEP * v );
    f32 a1 = a0 + gui_radians( GAUGE_SWEEP );

    /* Track, then value -- two arcs, two quads.  The value arc's round cap is the "pointer". */
    gui()->draw_arc( cx, cy, r, a0, a1, 6.0f, EDGE );
    gui()->draw_arc( cx, cy, r, a0, av, 6.0f, col );

    /* Ticks: eleven capsule stubs across the sweep.  A tick is a LINE, which is a capsule field
       at any angle -- no fan, no rotated-rect special case. */
    for ( u32 i = 0; i <= 10; ++i )
    {
        f32 a  = a0 + gui_radians( GAUGE_SWEEP * (f32)i / 10.0f );
        f32 c  = cosf( a ), s = sinf( a );
        f32 r0 = r - 14.0f, r1 = ( i % 5 == 0 ) ? r - 24.0f : r - 19.0f;
        gui()->draw_line( cx + c * r0, cy + s * r0, cx + c * r1, cy + s * r1,
                          ( i % 5 == 0 ) ? 2.5f : 1.5f, INK_DIM );
    }

    /* Needle: one capsule + a hub disc. */
    f32 c = cosf( a0 + gui_radians( GAUGE_SWEEP * v ) );
    f32 s = sinf( a0 + gui_radians( GAUGE_SWEEP * v ) );
    gui()->draw_line( cx, cy, cx + c * ( r - 30.0f ), cy + s * ( r - 30.0f ), 3.0f, INK );
    gui()->draw_circle( cx, cy, 5.0f, true, 0.0f, INK );

    char buf[ 16 ];
    snprintf( buf, sizeof( buf ), "%.0f", v * 100.0f );
    dial_label( cx, cy + r * 0.55f, col, buf, 1.6f );
}

static void
win_gauges( void )
{
    gui()->stack();

    gui()->checkbox( "auto", &s_gauge_auto );
    if ( s_gauge_auto )
    {
        s_gauge_v = 0.5f + 0.5f * sinf( s_time * 0.5f );
        keep_awake();
    }
    else
        gui()->slider_float( "value", &s_gauge_v, 0.0f, 1.0f );

    gui_rect_t cell = gui()->canvas( 260.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 cy = cell.y + cell.h * 0.5f;

    /* The full instrument. */
    gauge_radial( cell.x + cell.w * 0.2f, cy + 10.0f, 96.0f, s_gauge_v,
                  s_gauge_v > 0.85f ? HIT : TEAL );

    /* Progress ring: the stock helper + a readout.  frac and the cap travel together. */
    {
        f32 cx = cell.x + cell.w * 0.5f;
        gui()->draw_progress_arc( cx, cy, 52.0f, s_gauge_v, 9.0f, ACCENT );
        char buf[ 16 ];
        snprintf( buf, sizeof( buf ), "%.0f%%", s_gauge_v * 100.0f );
        dial_label( cx, cy, INK, buf, 1.2f );
        text_xf_centered( cx, cy + 74.0f, INK_DIM, "progress ring", 0.9f, 0.0f );
    }

    /* Segment meter: five arc segments lighting up in turn -- a battery / charge readout.
       Each segment is one quad; the whole meter is five. */
    {
        f32 cx  = cell.x + cell.w * 0.78f;
        u32 lit = (u32)( s_gauge_v * 5.0f + 0.999f );
        for ( u32 i = 0; i < 5; ++i )
        {
            f32 sa = gui_radians( 135.0f + 54.0f * (f32)i + 6.0f );
            f32 sb = gui_radians( 135.0f + 54.0f * (f32)( i + 1 ) - 6.0f );
            gui()->draw_arc( cx, cy, 52.0f, sa, sb, 10.0f, i < lit ? HEAL : EDGE );
        }
        text_xf_centered( cx, cy + 74.0f, INK_DIM, "segments", 0.9f, 0.0f );

        /* And a spinner beside it: the stock triple plus a custom comet -- an arc whose tail
           length breathes as it orbits.  Angles animate raw; normalization is the renderer's. */
        gui()->draw_spinner( ( gui_rect_t ){ cx + 76.0f, cy - 18.0f, 36.0f, 36.0f },
                             s_time, 4.0f, INK );
        f32 head = s_time * 3.1f;
        gui()->draw_arc( cx + 94.0f, cy + 52.0f, 16.0f,
                         head - ( 1.2f + 0.8f * sinf( s_time * 1.3f ) ), head, 5.0f, AMBER );
        keep_awake();
    }

    gui()->pop_clip();

    gui()->text( "gauge anatomy: track arc + value arc + 11 tick capsules + needle capsule + "
                 "hub disc + one scaled SDF readout = ~15 quads, one batch" );
}

/*==============================================================================================
    WINDOW: Charts & Data -- the same primitives pointed at data.

    The donut is the interactive one: wedge hit-testing is atan2 over the canvas mouse position,
    done here in the kit, because the renderer's job ended when the wedge became a shape.  The
    bar wall at the bottom is the OTHER batching story -- draw_rects puts N solid fills in ONE
    command slot (GUI_CMD_RECT_LIST), which is what dense data wants.
==============================================================================================*/

static const struct { const char* name; f32 v; u32 col; } k_slices[ 5 ] = {
    { "geometry", 34.0f, TEAL   },
    { "shading",  26.0f, ACCENT },
    { "upload",   18.0f, AMBER  },
    { "present",  12.0f, VIOLET },
    { "other",    10.0f, INK_DIM },
};

static void
win_charts( void )
{
    gui()->stack();

    gui_rect_t cell = gui()->canvas( 250.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 cx = cell.x + cell.w * 0.28f;
    f32 cy = cell.y + cell.h * 0.5f;
    f32 rm = 74.0f;    /* mid radius of the donut band */
    f32 bw = 26.0f;    /* band width                   */

    /* Which wedge is the mouse in?  Distance to the band, then angle to the sector. */
    f32 mx, my;
    gui()->get_mouse_pos( &mx, &my );
    f32 dx = mx - cx, dy = my - cy;
    f32 dist  = sqrtf( dx * dx + dy * dy );
    i32 hover = -1;
    if ( dist > rm - bw && dist < rm + bw )
    {
        f32 ma = atan2f( dy, dx ) - gui_radians( -90.0f );
        while ( ma < 0.0f ) ma += TAU;
        f32 acc = 0.0f, total = 0.0f;
        for ( u32 i = 0; i < 5; ++i ) total += k_slices[ i ].v;
        for ( u32 i = 0; i < 5; ++i )
        {
            acc += k_slices[ i ].v / total * TAU;
            if ( ma < acc ) { hover = (i32)i; break; }
        }
    }

    /* The donut: one thick arc per slice; the hovered one thickens and slides out along its
       bisector.  Five quads, and the highlight costs a parameter, not a rebuild. */
    {
        f32 total = 0.0f;
        for ( u32 i = 0; i < 5; ++i ) total += k_slices[ i ].v;

        f32 a = gui_radians( -90.0f );
        for ( u32 i = 0; i < 5; ++i )
        {
            f32  sweep = k_slices[ i ].v / total * TAU;
            bool hot   = ( (i32)i == hover );
            f32  mid   = a + sweep * 0.5f;
            f32  ox    = hot ? cosf( mid ) * 5.0f : 0.0f;
            f32  oy    = hot ? sinf( mid ) * 5.0f : 0.0f;

            /* Inset each end so the round caps land inside the sector -- a cap extends half the
               band width past its endpoint along the arc. */
            f32 inset = ( bw * 0.5f ) / rm + gui_radians( 1.5f );
            gui()->draw_arc( cx + ox, cy + oy, rm, a + inset, a + sweep - inset,
                             hot ? bw + 6.0f : bw, k_slices[ i ].col );
            a += sweep;
        }

        if ( hover >= 0 )
        {
            char buf[ 32 ];
            snprintf( buf, sizeof( buf ), "%.0f%%", k_slices[ hover ].v );
            dial_label( cx, cy - 8.0f, INK, buf, 1.5f );
            text_xf_centered( cx, cy + 18.0f, INK_DIM, k_slices[ hover ].name, 1.0f, 0.0f );
        }
        else
            text_xf_centered( cx, cy, INK_DIM, "hover a slice", 1.0f, 0.0f );
    }

    /* Legend. */
    {
        f32 lx = cx + rm + bw + 28.0f, ly = cell.y + 34.0f;
        for ( u32 i = 0; i < 5; ++i )
        {
            gui()->draw_circle( lx, ly + 6.0f, 5.0f, true, 0.0f, k_slices[ i ].col );
            gui()->draw_text( lx + 14.0f, ly, INK, k_slices[ i ].name );
            ly += 22.0f;
        }
    }

    /* Sparkline: one polyline, a dashed mean, and a pulsing "live" dot on the newest sample. */
    {
        f32        sx = cell.x + cell.w * 0.55f, sw = cell.w * 0.4f;
        f32        sy = cell.y + 170.0f,          sh = 56.0f;
        gui_vec2_t pts[ 48 ];
        f32        mean = 0.0f;
        for ( u32 i = 0; i < 48; ++i )
        {
            f32 t = s_time * 1.4f + (f32)i * 0.35f;
            f32 v = 0.5f + 0.28f * sinf( t ) + 0.16f * sinf( t * 2.7f );
            pts[ i ] = ( gui_vec2_t ){ sx + sw * (f32)i / 47.0f, sy + sh * ( 1.0f - v ) };
            mean += v;
        }
        mean /= 48.0f;
        f32 my2 = sy + sh * ( 1.0f - mean );
        gui()->draw_dashed_line( sx, my2, sx + sw, my2, 5.0f, 4.0f, 1.0f, INK_DIM );
        gui()->draw_polyline( pts, 48, 2.0f, GUI_STROKE_CENTER, false, TEAL );
        gui()->draw_circle( pts[ 47 ].x, pts[ 47 ].y,
                            3.5f + 1.5f * sinf( s_time * 5.0f ), true, 0.0f, CRIT );
        keep_awake();
    }

    gui()->pop_clip();

    /* The bar wall: one draw_rects call = ONE command slot however many bars, which is the
       dense-data escape valve (GUI_MAX_CMDS is the scarce pool, not vertices). */
    {
        gui_rect_t band = gui()->canvas( 70.0f );
        gui()->draw_rect( band.x, band.y, band.w, band.h, PANEL );

        gui_rect_col_t bars[ 96 ];
        u32            n  = 96;
        f32            bw2 = band.w / (f32)n;
        for ( u32 i = 0; i < n; ++i )
        {
            f32 v = 0.15f + 0.8f * fabsf( sinf( (f32)i * 0.23f ) * 0.7f
                                          + sinf( (f32)i * 0.61f ) * 0.3f );
            f32 h = ( band.h - 10.0f ) * v;
            bars[ i ] = ( gui_rect_col_t ){ band.x + bw2 * (f32)i + 1.0f,
                                            band.y + band.h - 5.0f - h,
                                            bw2 - 2.0f, h,
                                            v > 0.8f ? HIT : ACCENT };
        }
        gui()->draw_rects( bars, n );
        gui()->textf( "the bar wall: %u bars, ONE semantic command (draw_rects / RECT_LIST)", n );
    }
}

/*==============================================================================================
    WINDOW: Depth & Motion -- shadow, glow, pulse: the surface's third dimension.

    A shadow and a glow are the same FX_BOX with a different colour; elevation is nothing but
    feather width.  The pulse is the one to study: its command bytes never change while it
    animates (the clock lives in the fragment), so it is animation that costs zero
    re-tessellation -- the retained window stays byte-identical.
==============================================================================================*/

static bool s_toggle_on  = true;
static f32  s_toggle_pos = 1.0f;   /* knob position 0..1, eased toward the state */

static void
win_depth( void )
{
    gui()->stack();

    /* Elevation: four cards, one variable -- the shadow's spread. */
    gui()->separator_text( "elevation -- feather width is the z axis" );
    {
        gui_rect_t cell = gui()->canvas( 120.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );

        static const f32 k_spread[ 4 ] = { 4.0f, 10.0f, 18.0f, 30.0f };
        f32 cw = 96.0f, ch = 64.0f;
        f32 save = gui()->draw_rounding();
        gui()->draw_set_rounding( 8.0f );
        for ( u32 i = 0; i < 4; ++i )
        {
            f32 x = cell.x + 30.0f + (f32)i * ( cw + 40.0f );
            f32 y = cell.y + ( cell.h - ch ) * 0.5f;
            gui()->draw_shadow( ( gui_rect_t ){ x, y + 4.0f, cw, ch }, k_spread[ i ],
                                GUI_COLOR( 0, 0, 0, 0xA0 ) );
            gui()->draw_rect( x, y, cw, ch, GUI_COLOR( 0x2A, 0x2A, 0x33, 0xFF ) );
            char buf[ 16 ];
            snprintf( buf, sizeof( buf ), "%.0f px", k_spread[ i ] );
            text_xf_centered( x + cw * 0.5f, y + ch * 0.5f, INK_DIM, buf, 1.0f, 0.0f );
        }
        gui()->draw_set_rounding( save );
    }

    /* Glow: a shadow that is not black.  Interactive -- hover widens the halo, press pulses. */
    gui()->separator_text( "glow -- the same surface with a coloured skirt" );
    {
        gui_rect_t cell = gui()->canvas( 96.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );

        static const struct { const char* label; u32 col; } k_glow[ 3 ] = {
            { "PLAY",  HEAL }, { "ARM", AMBER }, { "FIRE", HIT },
        };
        f32 save = gui()->draw_rounding();
        gui()->draw_set_rounding( 10.0f );
        for ( u32 i = 0; i < 3; ++i )
        {
            gui_rect_t r = { cell.x + 40.0f + (f32)i * 160.0f, cell.y + 26.0f, 120.0f, 44.0f };
            char id[ 16 ];
            snprintf( id, sizeof( id ), "glow%u", i );
            gui_item_state_t st = gui()->item( id, r );

            u32 halo = ( k_glow[ i ].col & 0x00FFFFFFu ) | ( st.hover ? 0xC0000000u : 0x70000000u );
            gui()->draw_shadow( r, st.hover ? 26.0f : 14.0f, halo );
            if ( st.active )
                gui()->draw_pulse( r, 3.0f, 0.5f, k_glow[ i ].col );
            else
                gui()->draw_rect( r.x, r.y, r.w, r.h, GUI_COLOR( 0x22, 0x22, 0x2A, 0xFF ) );
            dial_label( r.x + r.w * 0.5f, r.y + r.h * 0.5f,
                        st.hover ? INK : INK_DIM, k_glow[ i ].label, 1.1f );
        }
        gui()->draw_set_rounding( save );
        keep_awake();   /* the pulse clock advances only on presented frames */
    }

    /* Pulse + badge: attention states.  The REC pulse re-tessellates NOTHING while it breathes;
       the badge is a disc + one scaled SDF count -- the notification idiom for free. */
    gui()->separator_text( "pulse and badge -- attention without re-tessellation" );
    {
        gui_rect_t cell = gui()->canvas( 86.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );

        /* Recording chip. */
        f32 save = gui()->draw_rounding();
        gui()->draw_set_rounding( 13.0f );
        gui_rect_t rec = { cell.x + 30.0f, cell.y + 30.0f, 92.0f, 26.0f };
        gui()->draw_pulse( rec, 1.0f, 0.6f, GUI_COLOR( 0x60, 0x10, 0x10, 0xFF ) );
        gui()->draw_set_rounding( save );
        gui()->draw_circle( rec.x + 16.0f, rec.y + 13.0f, 5.0f, true, 0.0f, HIT );
        gui()->draw_text( rec.x + 30.0f, rec.y + 5.0f, INK, "REC" );

        /* Inbox card with a count badge overhanging its corner. */
        gui_rect_t card = { cell.x + 170.0f, cell.y + 22.0f, 120.0f, 42.0f };
        gui()->draw_set_rounding( 6.0f );
        gui()->draw_rect( card.x, card.y, card.w, card.h, GUI_COLOR( 0x2A, 0x2A, 0x33, 0xFF ) );
        gui()->draw_set_rounding( save );
        gui()->draw_text( card.x + 12.0f, card.y + 13.0f, INK_DIM, "inbox" );
        gui()->draw_circle( card.x + card.w - 4.0f, card.y + 4.0f, 11.0f, true, 0.0f, HIT );
        dial_label( card.x + card.w - 4.0f, card.y + 4.0f, INK, "3", 0.85f );

        /* Capsule toggle: a rounding-equals-half-height rect IS a capsule; the knob is a disc.
           The ease is sandbox-side (a frame-rate-friendly approach), the shapes are two quads. */
        gui_rect_t tr = { cell.x + 360.0f, cell.y + 30.0f, 58.0f, 26.0f };
        gui_item_state_t st = gui()->item( "toggle", tr );
        if ( st.clicked )
            s_toggle_on = !s_toggle_on;
        f32 target = s_toggle_on ? 1.0f : 0.0f;
        s_toggle_pos += ( target - s_toggle_pos ) * 0.25f;
        if ( fabsf( target - s_toggle_pos ) > 0.01f )
            keep_awake();

        gui()->draw_set_rounding( tr.h * 0.5f );
        gui()->draw_rect( tr.x, tr.y, tr.w, tr.h, s_toggle_on ? HEAL : EDGE );
        gui()->draw_set_rounding( save );
        f32 kx = tr.x + 13.0f + ( tr.w - 26.0f ) * s_toggle_pos;
        gui()->draw_circle( kx, tr.y + 13.0f, 9.5f, true, 0.0f, INK );
        gui()->draw_text( tr.x + tr.w + 12.0f, tr.y + 5.0f, INK_DIM,
                          s_toggle_on ? "enabled" : "disabled" );

        keep_awake();
    }

    gui()->text( "a pulse's command bytes never change -- its hash never changes -- the window's "
                 "retained geometry stays valid the whole time it breathes" );
}

/*==============================================================================================
    WINDOW: Radial Menu -- wedges as widgets.

    The unexplored half of ARC/PIE: not decoration but INTERACTION.  Wedge hit-testing is a
    distance and an atan2 -- kit-side math over get_mouse_pos, exactly like the donut chart --
    and the render side is one arc quad per wedge, so the hover response (thicken + slide out)
    is a parameter change, not a rebuild.  This is the game-UI staple (weapon wheels, emote
    wheels, marking menus) that a fan-tessellated arc made too expensive to animate per frame.
==============================================================================================*/

static const struct { const char* label; u32 col; } k_wheel[ 6 ] = {
    { "attack", HIT    }, { "guard", ACCENT }, { "magic", VIOLET },
    { "item",   HEAL   }, { "talk",  CRIT   }, { "flee",  INK_DIM },
};

static i32 s_wheel_sel = -1;

static void
win_radial( void )
{
    gui()->stack();
    gui()->text( "hover a wedge, click to select -- six arc quads, hit-tested by atan2" );

    gui_rect_t cell = gui()->canvas( 320.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 cx = cell.x + cell.w * 0.5f;
    f32 cy = cell.y + cell.h * 0.5f;
    f32 rm = 100.0f;   /* wheel band mid radius */
    f32 bw = 40.0f;    /* band width            */

    f32 mx, my;
    gui()->get_mouse_pos( &mx, &my );
    f32 dx = mx - cx, dy = my - cy;
    f32 dist  = sqrtf( dx * dx + dy * dy );
    i32 hover = -1;
    if ( dist > rm - bw * 0.75f && dist < rm + bw )
    {
        f32 ma = atan2f( dy, dx ) - gui_radians( -90.0f );
        while ( ma < 0.0f ) ma += TAU;
        hover = (i32)( ma / ( TAU / 6.0f ) ) % 6;
    }
    if ( hover >= 0 && gui()->is_mouse_clicked( APP_MOUSE_LEFT ) )
        s_wheel_sel = hover;

    for ( u32 i = 0; i < 6; ++i )
    {
        f32  a0  = gui_radians( -90.0f ) + TAU / 6.0f * (f32)i;
        bool hot = ( (i32)i == hover );
        bool sel = ( (i32)i == s_wheel_sel );
        f32  mid = a0 + TAU / 12.0f;

        /* Round caps extend half the band past each endpoint, so inset the sweep by that plus a
           small visual gap -- the segmented-ring look, and the sectors stay distinguishable. */
        f32 inset = ( bw * 0.5f ) / rm + gui_radians( 2.0f );
        f32 ox    = hot ? cosf( mid ) * 8.0f : 0.0f;
        f32 oy    = hot ? sinf( mid ) * 8.0f : 0.0f;

        u32 col = k_wheel[ i ].col;
        if ( !hot && !sel )
            col = ( col & 0x00FFFFFFu ) | 0x90000000u;

        gui()->draw_arc( cx + ox, cy + oy, rm, a0 + inset, a0 + TAU / 6.0f - inset,
                         hot ? bw + 8.0f : bw, col );

        /* Upright labels at the bisector (the compass window shows the rotated variant). */
        f32 lr = rm + ( hot ? 8.0f : 0.0f );
        dial_label( cx + ox + cosf( mid ) * lr, cy + oy + sinf( mid ) * lr,
                    hot || sel ? INK : GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ),
                    k_wheel[ i ].label, 0.85f );
    }

    /* Hub: the selection readout, pulsing gently until one is made. */
    gui()->draw_circle( cx, cy, 40.0f, true, 0.0f, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ) );
    gui()->draw_circle( cx, cy, 40.0f, false, 2.0f, EDGE );
    if ( s_wheel_sel >= 0 )
        dial_label( cx, cy, k_wheel[ s_wheel_sel ].col, k_wheel[ s_wheel_sel ].label, 1.0f );
    else
        dial_label( cx, cy, INK_DIM, "?", 1.6f );

    gui()->pop_clip();
    keep_awake();   /* hover response is per-frame */

    gui()->text( "the whole wheel re-emits each frame and it does not matter: 6 wedges + hub "
                 "is ~8 quads.  The fan version of this shape was ~65 commands per wedge." );
}

/*==============================================================================================
    WINDOW: Dials -- instruments that turn: a draggable knob, a clock, a compass.

    The knob is the interactive keystone: item() supplies hover/active/keyboard over a rect the
    kit owns, the angle IS the value (atan2 of the captured mouse), and the presentation is the
    gauge vocabulary again.  The compass is the text rotation demo with a purpose: cardinal
    labels stay tangent to a turning ring, which is exactly what draw_text_xf's rotation is for.
==============================================================================================*/

static f32 s_knob_v = 0.35f;

static void
dial_knob( f32 cx, f32 cy, f32 r )
{
    gui_rect_t hit = { cx - r, cy - r, r * 2.0f, r * 2.0f };
    gui_item_state_t st = gui()->item( "knob", hit );

    if ( st.active )
    {
        /* The angle under the cursor, mapped into the gauge sweep; outside the sweep's dead
           zone the value holds -- grab anywhere, twist, release. */
        f32 mx, my;
        gui()->get_mouse_pos( &mx, &my );
        f32 a = atan2f( my - cy, mx - cx ) - gui_radians( GAUGE_A0 );
        while ( a < 0.0f ) a += TAU;
        f32 g = a / gui_radians( GAUGE_SWEEP );
        if ( g <= 1.0f )
            s_knob_v = g;
    }
    if ( st.nav_adjust )
        s_knob_v = s_knob_v + 0.05f * (f32)st.nav_adjust;
    if ( s_knob_v < 0.0f ) s_knob_v = 0.0f;
    if ( s_knob_v > 1.0f ) s_knob_v = 1.0f;

    f32 a0 = gui_radians( GAUGE_A0 );
    f32 av = a0 + gui_radians( GAUGE_SWEEP * s_knob_v );

    gui()->draw_circle( cx, cy, r - 18.0f, true, 0.0f,
                        st.active ? GUI_COLOR( 0x30, 0x30, 0x3A, 0xFF )
                                  : GUI_COLOR( 0x26, 0x26, 0x2E, 0xFF ) );
    gui()->draw_arc( cx, cy, r, a0, a0 + gui_radians( GAUGE_SWEEP ), 5.0f, EDGE );
    gui()->draw_arc( cx, cy, r, a0, av, 5.0f, st.hover || st.active ? CRIT : AMBER );

    /* The grip mark: a capsule from hub toward the value angle. */
    f32 c = cosf( av ), s = sinf( av );
    gui()->draw_line( cx + c * ( r - 34.0f ), cy + s * ( r - 34.0f ),
                      cx + c * ( r - 22.0f ), cy + s * ( r - 22.0f ), 4.0f, INK );

    char buf[ 16 ];
    snprintf( buf, sizeof( buf ), "%.2f", s_knob_v );
    dial_label( cx, cy, INK, buf, 1.15f );
}

static void
dial_clock( f32 cx, f32 cy, f32 r )
{
    gui()->draw_circle( cx, cy, r, true, 0.0f, GUI_COLOR( 0x20, 0x20, 0x28, 0xFF ) );
    gui()->draw_circle( cx, cy, r, false, 2.5f, EDGE );

    for ( u32 i = 0; i < 12; ++i )
    {
        f32 a = TAU * (f32)i / 12.0f;
        f32 c = cosf( a ), s = sinf( a );
        gui()->draw_line( cx + c * ( r - 4.0f ), cy + s * ( r - 4.0f ),
                          cx + c * ( r - ( i % 3 == 0 ? 14.0f : 9.0f ) ),
                          cy + s * ( r - ( i % 3 == 0 ? 14.0f : 9.0f ) ),
                          i % 3 == 0 ? 2.5f : 1.5f, INK_DIM );
    }

    /* Hands from the sandbox clock -- sped up so the demo visibly runs. */
    f32 sec  = fmodf( s_time * 4.0f, 60.0f );
    f32 min  = fmodf( s_time * 4.0f / 60.0f, 60.0f );
    f32 hour = fmodf( s_time * 4.0f / 720.0f, 12.0f );

    f32 ah = gui_radians( -90.0f ) + TAU * hour / 12.0f;
    f32 am = gui_radians( -90.0f ) + TAU * min / 60.0f;
    f32 as = gui_radians( -90.0f ) + TAU * sec / 60.0f;

    gui()->draw_line( cx, cy, cx + cosf( ah ) * r * 0.45f, cy + sinf( ah ) * r * 0.45f, 4.0f, INK );
    gui()->draw_line( cx, cy, cx + cosf( am ) * r * 0.68f, cy + sinf( am ) * r * 0.68f, 3.0f, INK );
    gui()->draw_line( cx - cosf( as ) * r * 0.15f, cy - sinf( as ) * r * 0.15f,
                      cx + cosf( as ) * r * 0.8f,  cy + sinf( as ) * r * 0.8f, 1.5f, HIT );
    gui()->draw_circle( cx, cy, 4.0f, true, 0.0f, HIT );
}

static void
dial_compass( f32 cx, f32 cy, f32 r )
{
    f32 heading = s_time * 0.35f;   /* the ring turns; the needle is the fixed frame */

    gui()->draw_circle( cx, cy, r, true, 0.0f, GUI_COLOR( 0x20, 0x20, 0x28, 0xFF ) );
    gui()->draw_circle( cx, cy, r, false, 2.5f, EDGE );

    for ( u32 i = 0; i < 24; ++i )
    {
        f32 a = heading + TAU * (f32)i / 24.0f;
        f32 c = cosf( a ), s = sinf( a );
        f32 t = ( i % 6 == 0 ) ? 12.0f : 6.0f;
        gui()->draw_line( cx + c * ( r - 4.0f ), cy + s * ( r - 4.0f ),
                          cx + c * ( r - 4.0f - t ), cy + s * ( r - 4.0f - t ),
                          ( i % 6 == 0 ) ? 2.0f : 1.0f, INK_DIM );
    }

    /* The cardinal labels ride the ring and stay TANGENT to it: each is rotated by its own
       bearing, which is one scalar into draw_text_xf -- the thing a coverage font cannot do. */
    static const char* k_cards[ 4 ] = { "N", "E", "S", "W" };
    gui()->font_use( s_font_sdf );
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 a = heading + gui_radians( -90.0f ) + TAU * (f32)i / 4.0f;
        text_xf_centered( cx + cosf( a ) * ( r - 28.0f ), cy + sinf( a ) * ( r - 28.0f ),
                          i == 0 ? HIT : INK, k_cards[ i ], 1.1f, a + gui_radians( 90.0f ) );
    }
    gui()->font_use( 0 );

    /* Fixed needle: two slim pies make the classic diamond, plus the hub. */
    gui()->draw_pie( cx, cy - r * 0.1f, r * 0.42f, gui_radians( -100.0f ), gui_radians( -80.0f ), HIT );
    gui()->draw_pie( cx, cy + r * 0.1f, r * 0.42f, gui_radians( 80.0f ), gui_radians( 100.0f ), INK_DIM );
    gui()->draw_circle( cx, cy, 5.0f, true, 0.0f, INK );
}

static void
win_dials( void )
{
    gui()->stack();
    gui()->text( "drag the knob (arrow keys nudge it); the clock and compass run on the "
                 "sandbox clock" );

    gui_rect_t cell = gui()->canvas( 260.0f );
    gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
    gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

    f32 cy = cell.y + cell.h * 0.5f;
    dial_knob(    cell.x + cell.w * 0.2f,  cy, 88.0f );
    dial_clock(   cell.x + cell.w * 0.5f,  cy, 94.0f );
    dial_compass( cell.x + cell.w * 0.8f,  cy, 94.0f );

    text_xf_centered( cell.x + cell.w * 0.2f, cell.y + cell.h - 16.0f, INK_DIM, "knob",    0.9f, 0.0f );
    text_xf_centered( cell.x + cell.w * 0.5f, cell.y + cell.h - 16.0f, INK_DIM, "clock",   0.9f, 0.0f );
    text_xf_centered( cell.x + cell.w * 0.8f, cell.y + cell.h - 16.0f, INK_DIM, "compass", 0.9f, 0.0f );

    gui()->pop_clip();
    keep_awake();

    gui()->text( "every hand, tick and needle is a capsule or a pie -- there is no rotated-"
                 "rectangle special case anywhere in this window" );
}

/*==============================================================================================
    WINDOW: New Verbs -- the five features the frontier list asked for, built 2026-07-29.

    Each section is one verb next to the composition it unlocks.  The needle icon is REGISTERED
    HERE, procedurally: a kite-shaped coverage bitmap pushed through register_icon_sdf, because
    the rotating-icon demo should also prove the 8SSEDT transform path end to end.
==============================================================================================*/

static f32  s_nv_val  = 0.7f;
static bool s_nv_spin = true;

/* One 128x128 kite (a compass needle), coverage with a soft 1 px edge and a wide transparent
   margin -- exactly the source art the SDF transform wants. */
static gui_icon_id_t
nv_needle_icon( void )
{
    static gui_icon_id_t id;
    static bool          made;
    if ( !made )
    {
        made = true;
        static u8 px[ 128 * 128 ];
        for ( u32 yy = 0; yy < 128; ++yy )
            for ( u32 xx = 0; xx < 128; ++xx )
            {
                f32 dx = (f32)xx - 64.0f, dy = (f32)yy - 64.0f;
                f32 hw = 20.0f * ( 1.0f - fabsf( dy ) / 46.0f );   /* kite: width tapers to tips */
                f32 d  = hw - fabsf( dx );
                if ( fabsf( dy ) > 46.0f ) d = -1.0f;
                f32 v = d * 255.0f;
                px[ yy * 128 + xx ] = (u8)( v < 0.0f ? 0.0f : ( v > 255.0f ? 255.0f : v ) );
            }
        id = gui()->register_icon_sdf( "sb_needle", 128, 128, px, 64 );
    }
    return id;
}

static void
win_five( void )
{
    gui()->stack();
    keep_awake();

    /* 1  rotated SDF boxes. */
    gui()->separator_text( "draw_box_xf -- the rounded box, turned" );
    {
        gui_rect_t cell = gui()->canvas( 120.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
        gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

        f32 cy = cell.y + cell.h * 0.5f;
        f32 rot = s_nv_spin ? s_time * 0.5f : gui_radians( -12.0f );

        /* Three fixed tilts, a soft rotated glow, and the spinner -- same four quadrant quads. */
        for ( u32 i = 0; i < 3; ++i )
            gui()->draw_box_xf( ( gui_rect_t ){ cell.x + 40.0f + (f32)i * 130.0f, cy - 24.0f,
                                                96.0f, 48.0f },
                                10.0f, 0.0f, gui_radians( -18.0f + 14.0f * (f32)i ),
                                i == 1 ? TEAL : GUI_COLOR( 0x2A, 0x2A, 0x33, 0xFF ) );
        gui()->draw_box_xf( ( gui_rect_t ){ cell.x + 430.0f, cy - 22.0f, 90.0f, 44.0f },
                            10.0f, 26.0f, gui_radians( 20.0f ), ( AMBER & 0x00FFFFFFu ) | 0x90000000u );
        /* The label TURNS WITH the plate -- same angle, same centre -- so the pair reads as one
           object (the HUD's crit plates are this same composition). */
        gui()->draw_box_xf( ( gui_rect_t ){ cell.x + 560.0f, cy - 20.0f, 110.0f, 40.0f },
                            12.0f, 0.0f, rot, VIOLET );
        gui()->font_use( s_font_sdf );
        text_xf_centered( cell.x + 615.0f, cy, INK, "spin", 0.9f, rot );
        gui()->font_use( 0 );
        gui()->pop_clip();
        gui()->text( "the fx coordinate is box-local and affine, so only the four corner "
                     "positions turn -- the soft one is a ROTATED feather, note the skirt" );
    }

    /* 2  per-corner soft shadow. */
    gui()->separator_text( "draw_round_rect_shadow -- feather meets per-corner radii" );
    {
        gui_rect_t cell = gui()->canvas( 110.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );

        /* A tab: rounded on top, square where it meets its panel -- and its shadow has the SAME
           shape, which draw_shadow (one radius) could never cut. */
        gui_rect_t tab = { cell.x + 50.0f, cell.y + 34.0f, 130.0f, 56.0f };
        gui()->draw_round_rect_shadow( ( gui_rect_t ){ tab.x, tab.y + 6.0f, tab.w, tab.h },
                                       14.0f, 14.0f, 0.0f, 0.0f, 22.0f,
                                       GUI_COLOR( 0, 0, 0, 0xB0 ) );
        gui()->draw_round_rect( tab, 14.0f, 14.0f, 0.0f, 0.0f, true, 0.0f,
                                GUI_COLOR( 0x2E, 0x2E, 0x38, 0xFF ) );
        dial_label( tab.x + tab.w * 0.5f, tab.y + tab.h * 0.5f, INK_DIM, "tab", 0.95f );

        /* The notched card, same trick mirrored. */
        gui_rect_t card = { cell.x + 260.0f, cell.y + 24.0f, 150.0f, 64.0f };
        gui()->draw_round_rect_shadow( ( gui_rect_t ){ card.x, card.y + 8.0f, card.w, card.h },
                                       22.0f, 4.0f, 22.0f, 4.0f, 26.0f,
                                       GUI_COLOR( 0x10, 0x30, 0x60, 0xC0 ) );
        gui()->draw_round_rect( card, 22.0f, 4.0f, 22.0f, 4.0f, true, 0.0f,
                                GUI_COLOR( 0x26, 0x2E, 0x3C, 0xFF ) );
        dial_label( card.x + card.w * 0.5f, card.y + card.h * 0.5f, INK_DIM, "card", 0.95f );
    }

    /* 3 + 4  dashed and gradient arcs. */
    gui()->separator_text( "draw_arc_dashed / draw_arc_gradient -- the self-sampled sectors" );
    {
        gui()->checkbox( "animate", &s_nv_spin );
        gui()->slider_float( "value", &s_nv_val, 0.0f, 1.0f );

        gui_rect_t cell = gui()->canvas( 190.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
        gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

        f32 cy   = cell.y + cell.h * 0.5f;
        f32 ants = s_nv_spin ? s_time * 0.8f : 0.0f;

        /* Dotted ring: a closed dashed arc -- the emit-side period snap is why it has no seam. */
        gui()->draw_arc_dashed( cell.x + 90.0f, cy, 54.0f, 0.0f, TAU, 4.0f, 4.0f, 10.0f, INK_DIM );
        text_xf_centered( cell.x + 90.0f, cy + 78.0f, INK_DIM, "dotted ring", 0.9f, 0.0f );

        /* Marching ants: rotate BOTH angles together and the pattern rides the frame. */
        gui()->draw_arc_dashed( cell.x + 250.0f, cy, 54.0f, ants, ants + TAU,
                                2.0f, 8.0f, 8.0f, CRIT );
        text_xf_centered( cell.x + 250.0f, cy + 78.0f, INK_DIM, "marching ants", 0.9f, 0.0f );

        /* Tick dial: long period, thin duty -- the gauge chapter ring for free. */
        gui()->draw_arc_dashed( cell.x + 410.0f, cy, 54.0f,
                                gui_radians( 135.0f ), gui_radians( 405.0f ),
                                12.0f, 3.0f, 24.0f, TEAL );
        text_xf_centered( cell.x + 410.0f, cy + 78.0f, INK_DIM, "tick dial", 0.9f, 0.0f );

        /* The hot/cold value arc: colour sweeps by ANGLE, which no vertex colour could do. */
        {
            f32 cx = cell.x + 580.0f;
            f32 a0 = gui_radians( 135.0f );
            gui()->draw_arc( cx, cy, 54.0f, a0, a0 + gui_radians( 270.0f ), 9.0f, EDGE );
            gui()->draw_arc_gradient( cx, cy, 54.0f, a0,
                                      a0 + gui_radians( 270.0f * s_nv_val ), 9.0f, TEAL, HIT );
            char buf[ 16 ];
            snprintf( buf, sizeof( buf ), "%.0f%%", s_nv_val * 100.0f );
            dial_label( cx, cy, INK, buf, 1.1f );
            text_xf_centered( cx, cy + 78.0f, INK_DIM, "sweep gradient", 0.9f, 0.0f );
        }

        /* And a full gradient ring -- one quad, the seam is where col_b meets col_a. */
        gui()->draw_arc_gradient( cell.x + 730.0f, cy, 54.0f, gui_radians( -90.0f ),
                                  gui_radians( 270.0f ), 12.0f, VIOLET, AMBER );
        text_xf_centered( cell.x + 730.0f, cy + 78.0f, INK_DIM, "gradient ring", 0.9f, 0.0f );

        gui()->pop_clip();
    }

    /* 5  rotated icons. */
    gui()->separator_text( "draw_icon_xf -- an SDF icon that turns" );
    {
        gui_rect_t cell = gui()->canvas( 130.0f );
        gui()->draw_rect( cell.x, cell.y, cell.w, cell.h, PANEL );
        gui()->push_clip( cell.x, cell.y, cell.w, cell.h );

        gui_icon_id_t needle = nv_needle_icon();
        f32 cy  = cell.y + cell.h * 0.5f;
        f32 rot = s_nv_spin ? s_time * 0.9f : gui_radians( 30.0f );

        /* The same field icon at three sizes and one live rotation -- the edge resolves in the
           fragment, so none of these is a special case and all share the batch. */
        gui()->draw_circle( cell.x + 90.0f, cy, 52.0f, false, 2.0f, EDGE );
        gui()->draw_icon_xf( ( gui_rect_t ){ cell.x + 90.0f - 44.0f, cy - 44.0f, 88.0f, 88.0f },
                             needle, HIT, rot );
        gui()->draw_icon_xf( ( gui_rect_t ){ cell.x + 200.0f, cy - 28.0f, 56.0f, 56.0f },
                             needle, TEAL, -rot * 0.7f );
        gui()->draw_icon_xf( ( gui_rect_t ){ cell.x + 290.0f, cy - 16.0f, 32.0f, 32.0f },
                             needle, CRIT, rot * 1.4f );
        gui()->draw_text( cell.x + 360.0f, cy - 8.0f, INK_DIM,
                          "registered here via register_icon_sdf (8SSEDT), rotated per frame" );
        gui()->pop_clip();
    }

    gui_render_stats_t rs = gui()->render_stats();
    gui()->textf( "everything above: %u draw calls -- new modes ride the vertex, so nothing "
                  "here can cut a batch", rs.draw_calls );
}

/*==============================================================================================
    WINDOW: Backdrops -- the framebuffer-tiling patterns (fx modes 11 CHECKER / 12 GRID).

    Both are ONE quad whose fragment tiles in framebuffer pixels, re-anchored by a phase so the
    pattern rides its shape.  The checker used to be a rect-pool expansion -- 64 commands and up
    to 4096 quads per call, coarsening past its 64x64 clamp; the line grid was not affordable as
    geometry at all.  A full node-graph backdrop here is THREE quads: fill + minor + major.
==============================================================================================*/

static f32  s_bd_cell = 12.0f;
static bool s_bd_pan  = true;

static void
win_backdrops( void )
{
    gui()->stack();

    gui()->slider_float( "cell (px)", &s_bd_cell, 4.0f, 64.0f );

    gui()->separator_text( "checker (mode 11) -- one quad, any area, any cell" );
    {
        gui_rect_t r = gui()->canvas( 110.0f );
        gui()->draw_checker( r, s_bd_cell, GUI_COLOR( 0x2A, 0x2A, 0x30, 0xFF ),
                             GUI_COLOR( 0x1E, 0x1E, 0x24, 0xFF ) );

        /* Translucent plates: the checker is the classic alpha ground, so alpha must read. */
        f32 y = r.y + r.h * 0.5f;
        gui()->draw_circle( r.x + r.w * 0.20f, y, 36.0f, true, 0.0f,
                            GUI_COLOR( 0xFF, 0x70, 0x50, 0xA0 ) );
        gui()->draw_round_rect( ( gui_rect_t ){ r.x + r.w * 0.38f, y - 30.0f, 140.0f, 60.0f },
                                8.0f, 8.0f, 8.0f, 8.0f, true, 0.0f,
                                GUI_COLOR( 0x4C, 0x9E, 0xFF, 0x70 ) );
        gui()->draw_circle( r.x + r.w * 0.78f, y, 36.0f, true, 0.0f,
                            GUI_COLOR( 0x60, 0xE0, 0x80, 0x40 ) );
    }

    gui()->separator_text( "line grid (mode 12) -- fill + minor + major = three quads" );
    gui()->checkbox( "pan", &s_bd_pan );
    {
        gui_rect_t r = gui()->canvas( 240.0f );

        /* The content origin a node graph would own: the lattice AND the nodes hang off it, so
           panning moves both together -- the grid quad re-emits (its origin changed) but it is
           still one quad. */
        f32 ox = r.x, oy = r.y;
        if ( s_bd_pan )
        {
            ox += sinf( s_time * 0.40f ) * 90.0f;
            oy += sinf( s_time * 0.73f ) * 50.0f;
            keep_awake();
        }

        gui()->draw_rect( r.x, r.y, r.w, r.h, PANEL );
        gui()->draw_grid( r, s_bd_cell,        1.0f, ox, oy, GUI_COLOR( 0x2C, 0x2C, 0x36, 0xFF ) );
        gui()->draw_grid( r, s_bd_cell * 4.0f, 1.0f, ox, oy, GUI_COLOR( 0x46, 0x46, 0x54, 0xFF ) );

        /* Two "nodes" and a wire, placed in CONTENT space (offsets from the origin). */
        gui()->push_clip( r.x, r.y, r.w, r.h );
        gui_rect_t na = { ox + 150.0f, oy + 60.0f,  120.0f, 56.0f };
        gui_rect_t nb = { ox + 380.0f, oy + 140.0f, 120.0f, 56.0f };
        gui()->draw_bezier_cubic( na.x + na.w, na.y + na.h * 0.5f,
                                  na.x + na.w + 60.0f, na.y + na.h * 0.5f,
                                  nb.x - 60.0f, nb.y + nb.h * 0.5f,
                                  nb.x, nb.y + nb.h * 0.5f, 2.0f, TEAL );
        gui()->draw_round_rect( na, 6.0f, 6.0f, 6.0f, 6.0f, true, 0.0f, EDGE );
        gui()->draw_round_rect( nb, 6.0f, 6.0f, 6.0f, 6.0f, true, 0.0f, EDGE );
        gui()->pop_clip();
    }

    gui()->text( "the fragment tiles in framebuffer pixels -- exact at any panel size, where the "
                 "half-precision effect coordinate would blur far-corner lines" );
}

/*==============================================================================================
    WINDOW: Frontier Notes -- what the suite could NOT draw, and how close each miss is.

    Kept in the sandbox on purpose: the demos above are the argument for each of these, and the
    next person exploring here should find the map next to the territory.
==============================================================================================*/

static void
win_frontier( void )
{
    gui()->stack();

    gui()->separator_text( "shipped 2026-07-29 -- see the New Verbs window" );
    gui()->text_wrapped( "ROTATED SDF BOXES (draw_box_xf), ROTATED ICONS/IMAGES (draw_icon_xf / "
        "draw_texture_xf), PER-CORNER SOFT SHADOW (draw_round_rect_shadow), DASHED ARCS "
        "(draw_arc_dashed, fx mode 9) and SWEEP GRADIENT ARCS (draw_arc_gradient, fx mode 10) "
        "were all built from this window's original list.  The two new fx modes are SELF-SAMPLED: "
        "the fragment skips the texel and the freed 32-bit uv word carries their parameters -- "
        "no vertex format change, no batch key change." );

    gui()->separator_text( "unblocked, needs an asset pipeline step" );
    gui()->text_wrapped( "MSDF.  The sampling-model field is 4 bits with 3 spent, so multi-"
        "channel SDF is a mode VALUE now, not a format change -- the work is the font_tool bake "
        "and an RGBA atlas page.  Buys sharp corners at extreme scales.  Deliberately deferred." );

    gui()->separator_text( "genuinely far" );
    gui()->text_wrapped( "BACKDROP BLUR / FROST.  Needs a PostProcess seam (sample-what-is-"
        "behind), which the renderer does not have.  Not an fx mode." );
}

/*==============================================================================================
    Fills -- the two shapes that cost the effect band nothing.

    Both are here for the same reason: neither needed an fx mode.  The gradient rides the sixteen
    VERTICES a rounded box already emits, because colour is affine in position and so is vertex
    interpolation; the inset rides an OP BIT in the tex word, because turning a falloff inward is
    a modifier on a coverage the fragment already computed, not a new shape.  A mode was the
    expensive answer to both, and the nibble only has two values left.
==============================================================================================*/

static f32  s_fill_ang    = 90.0f;    /* gradient angle, degrees (90 = top to bottom) */
static f32  s_fill_round  = 10.0f;
static f32  s_fill_depth  = 14.0f;    /* inset falloff depth, px */
static bool s_fill_spin;

static void
win_fills( void )
{
    gui()->stack();

    gui()->slider_float( "angle (deg)", &s_fill_ang,   0.0f, 360.0f );
    gui()->slider_float( "rounding",    &s_fill_round, 0.0f, 40.0f  );
    gui()->checkbox( "spin the angle", &s_fill_spin );
    if ( s_fill_spin )
    {
        s_fill_ang = fmodf( s_time * 40.0f, 360.0f );
        keep_awake();
    }
    f32 ang = gui_radians( s_fill_ang );

    gui()->separator_text( "rounded gradient -- any angle, four quads, one draw call" );
    {
        gui_rect_t r = gui()->canvas( 130.0f );
        gui()->draw_rect( r.x, r.y, r.w, r.h, PANEL );

        /* Three plates at the live angle.  A gradient on a ROUNDED rect had no expression at all
           before this: draw_gradient is one square quad with two corner colours. */
        f32 w = ( r.w - 40.0f ) / 3.0f, h = r.h - 24.0f;
        gui()->draw_round_rect_gradient( ( gui_rect_t ){ r.x + 10.0f, r.y + 12.0f, w, h },
                                         s_fill_round, TEAL, PANEL, ang );
        gui()->draw_round_rect_gradient( ( gui_rect_t ){ r.x + 20.0f + w, r.y + 12.0f, w, h },
                                         s_fill_round,
                                         GUI_COLOR( 0xFF, 0x70, 0x50, 0xFF ),
                                         GUI_COLOR( 0x50, 0x30, 0xA0, 0xFF ), ang );
        /* Alpha ramps too, and it stays LINEAR -- alpha is coverage, never gamma encoded, so a
           fade to transparent is even rather than crowded at one end. */
        gui()->draw_round_rect_gradient( ( gui_rect_t ){ r.x + 30.0f + w * 2.0f, r.y + 12.0f, w, h },
                                         s_fill_round, INK, GUI_COLOR( 0xE8, 0xE0, 0xD0, 0x00 ), ang );
    }

    gui()->separator_text( "the midpoint test -- rounded vs square, same two endpoints" );
    {
        /* The one thing that could go wrong invisibly.  The hardware interpolates the DECODED
           colours, so the ramp is computed in linear light and re-encoded per vertex; lerping the
           sRGB bytes instead would miss the centre by ~45/255 and these two bands would show a
           seam where they meet.  They must read as ONE continuous ramp. */
        gui_rect_t r = gui()->canvas( 76.0f );
        u32 a = GUI_COLOR( 0x10, 0x20, 0xC0, 0xFF ), b = GUI_COLOR( 0xE0, 0xB0, 0x40, 0xFF );

        gui()->draw_gradient( ( gui_rect_t ){ r.x, r.y, r.w, 36.0f }, a, b, true );
        gui()->draw_round_rect_gradient( ( gui_rect_t ){ r.x, r.y + 38.0f, r.w, 36.0f },
                                         0.0f, a, b, 0.0f );
        gui()->text( "no seam at the centre = the linear-light round trip is doing its job" );
    }

    gui()->separator_text( "inset shadow -- the falloff turned INWARD (tex op bit, no fx mode)" );
    gui()->slider_float( "depth (px)", &s_fill_depth, 1.0f, 48.0f );
    {
        gui_rect_t r = gui()->canvas( 150.0f );
        gui()->draw_rect( r.x, r.y, r.w, r.h, PANEL );

        f32 w = ( r.w - 40.0f ) / 3.0f, h = r.h - 24.0f;
        gui()->draw_set_rounding( s_fill_round );

        /* A pressed well: the fill, then the inset against its own inside edge. */
        gui_rect_t w0 = { r.x + 10.0f, r.y + 12.0f, w, h };
        gui()->draw_round_rect( w0, s_fill_round, s_fill_round, s_fill_round, s_fill_round,
                                true, 0.0f, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ) );
        gui()->draw_inset_shadow( w0, s_fill_depth, GUI_COLOR( 0x00, 0x00, 0x00, 0xC0 ) );

        /* The mirror, side by side: a DROP shadow sits on the ground UNDER its subject, an inset
           sits against the inside of the subject's own edge. */
        gui_rect_t w1 = { r.x + 20.0f + w, r.y + 12.0f, w, h };
        gui()->draw_shadow( w1, s_fill_depth * 0.5f, GUI_COLOR( 0x00, 0x00, 0x00, 0xC0 ) );
        gui()->draw_round_rect( w1, s_fill_round, s_fill_round, s_fill_round, s_fill_round,
                                true, 0.0f, EDGE );

        /* Composed: an inset over a gradient, which is the point of an op -- it modifies whatever
           the fill happened to be instead of replacing it with a shape of its own. */
        gui_rect_t w2 = { r.x + 30.0f + w * 2.0f, r.y + 12.0f, w, h };
        gui()->draw_round_rect_gradient( w2, s_fill_round, TEAL, PANEL, ang );
        gui()->draw_inset_shadow( w2, s_fill_depth, GUI_COLOR( 0x00, 0x00, 0x00, 0xA0 ) );

        gui()->draw_set_rounding( 0.0f );
    }

    gui()->text( "left: pressed well   middle: the drop shadow it mirrors   right: inset over a gradient" );
    gui()->text( "an inset's interior is HOLLOW -- the band is only `depth` deep, so one on a "
                 "full-size panel costs the rim, not the panel" );

    gui()->separator_text( "stripes + hatch -- the lattice cut on ONE axis, turned (GRID's spare bits)" );
    {
        gui_rect_t r = gui()->canvas( 130.0f );
        gui()->draw_rect( r.x, r.y, r.w, r.h, PANEL );

        f32 w = ( r.w - 40.0f ) / 3.0f, h = r.h - 24.0f;

        /* The classic 45-degree hatch -- the "disabled / in-progress / diff" fill.  This used to
           be up to 512 stroked line commands under a clip rect; it is now one quad. */
        gui()->draw_hatch( ( gui_rect_t ){ r.x + 10.0f, r.y + 12.0f, w, h },
                           10.0f, 2.0f, GUI_COLOR( 0x50, 0x50, 0x60, 0xFF ) );

        /* Arbitrary angle, live: the fragment turns its own pixel coordinate, so the pattern
           costs the same at every angle. */
        gui()->draw_stripes( ( gui_rect_t ){ r.x + 20.0f + w, r.y + 12.0f, w, h },
                             12.0f, 3.0f, ang, GUI_COLOR( 0x20, 0xC0, 0xB0, 0xB0 ) );

        /* A turned LATTICE -- the same word with the stripe bit clear, so both axes cut. */
        gui_rect_t r3 = { r.x + 30.0f + w * 2.0f, r.y + 12.0f, w, h };
        gui()->draw_rect( r3.x, r3.y, r3.w, r3.h, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ) );
        gui()->draw_grid( r3, 14.0f, 1.0f, r3.x, r3.y, GUI_COLOR( 0x3A, 0x3A, 0x48, 0xFF ) );

        gui()->text( "left: draw_hatch, 1 quad (was up to 512 line commands)   "
                     "middle: draw_stripes at the live angle   right: the unturned lattice" );
    }
}

/*==============================================================================================
    Registry + menu -- every demo window hidden by default, launched from the menu bar or the
    launcher window (both drive the same table; the titlebar X syncs back into it).
==============================================================================================*/

typedef struct
{
    const char* name;     // menu item
    const char* title;    // window title (the id the open latch keys on)
    const char* desc;     // one-liner for the launcher
    void ( *fn )( void ); // body, called between window_begin/end
    f32  w, h;            // first-appearance size
    bool open;

} sdf_demo_t;

static sdf_demo_t s_demos[] = {
    { "SDF Text",       "SDF Text",       "the original bed: ladder / turn / hud / edge / atlas",  win_text,     1180.0f, 940.0f, false },
    { "Shape Economy",  "Shape Economy",  "every SDF primitive next to its vertex price",          win_shapes,    980.0f, 420.0f, false },
    { "Gauges & Meters","Gauges & Meters","radial gauge / progress ring / segments / spinners",    win_gauges,    900.0f, 430.0f, false },
    { "Charts & Data",  "Charts & Data",  "donut with hover / sparkline / one-command bar wall",   win_charts,    900.0f, 480.0f, false },
    { "Depth & Motion", "Depth & Motion", "shadow elevation / glow / pulse / badge / toggle",      win_depth,     820.0f, 560.0f, false },
    { "Radial Menu",    "Radial Menu",    "arc wedges as hit-tested interactive UI",               win_radial,    620.0f, 470.0f, false },
    { "Dials",          "Dials",          "draggable knob / clock / compass with rotated labels",  win_dials,     900.0f, 400.0f, false },
    { "New Verbs",      "New Verbs",      "box_xf / icon_xf / corner shadow / dashed + gradient arcs", win_five,  980.0f, 760.0f, false },
    { "Backdrops",      "Backdrops",      "checker + line grid as one-quad fragment patterns",     win_backdrops, 760.0f, 560.0f, false },
    { "Fills",          "Fills",          "rounded gradients (any angle) + the inset shadow op",   win_fills,     940.0f, 690.0f, false },
    { "Frontier Notes", "Frontier Notes", "what shipped and what is still out",                    win_frontier,  640.0f, 480.0f, false },
};

#define SDF_DEMO_COUNT ( (i32)( sizeof( s_demos ) / sizeof( s_demos[ 0 ] ) ) )

/* Show / hide one demo, keeping gui's internal CLOSEABLE latch in sync (a window the user
   X-closed stays latched shut inside gui until window_set_open re-opens it). */
static void
demo_set_open( sdf_demo_t* d, bool open )
{
    d->open = open;
    if ( open )
        gui()->window_set_open( d->title, true );
}

static void
menu_bar( void )
{
    if ( !gui()->main_menu_bar_begin() )
        return;

    if ( gui()->menu_begin( "Demos" ) )
    {
        if ( gui()->menu_item( "Open all", NULL, NULL ) )
            for ( i32 i = 0; i < SDF_DEMO_COUNT; i++ )
                demo_set_open( &s_demos[ i ], true );
        if ( gui()->menu_item( "Close all", NULL, NULL ) )
            for ( i32 i = 0; i < SDF_DEMO_COUNT; i++ )
                demo_set_open( &s_demos[ i ], false );
        gui()->separator();
        for ( i32 i = 0; i < SDF_DEMO_COUNT; i++ )
            if ( gui()->menu_item( s_demos[ i ].name, NULL, &s_demos[ i ].open ) )
                demo_set_open( &s_demos[ i ], s_demos[ i ].open );
        gui()->menu_end();
    }

    gui()->main_menu_bar_end();
}

/* The launcher: the one window open by default.  A checkbox per demo plus its one-liner --
   the same registry the menu drives, so the two stay in step for free. */
static void
launcher( void )
{
    gui()->window_set_next_pos( 24.0f, 48.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 460.0f, 420.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "SDF Explorer", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "What the SDF effect band and sampling model unlock, one window "
                             "per claim.  All hidden until asked for -- open them here or from "
                             "the Demos menu." );
        gui()->separator();
        for ( i32 i = 0; i < SDF_DEMO_COUNT; i++ )
        {
            sdf_demo_t* d = &s_demos[ i ];
            if ( gui()->checkbox( d->name, &d->open ) )
                demo_set_open( d, d->open );
            gui()->text_disabled( d->desc );
        }
    }
    gui()->window_end();
}

static void
build_frame( void )
{
    menu_bar();
    launcher();

    for ( i32 i = 0; i < SDF_DEMO_COUNT; i++ )
    {
        sdf_demo_t* d = &s_demos[ i ];
        if ( !d->open )
            continue;

        gui()->window_set_next_size( d->w, d->h, GUI_COND_ONCE );
        if ( gui()->window_begin( d->title, GUI_WIN_CLOSEABLE ) )
            d->fn();
        gui()->window_end();

        /* The titlebar X closed the window this frame -- reflect it in the registry. */
        if ( !gui()->window_is_open( d->title ) )
            d->open = false;
    }
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

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui sdf explorer",
        .w         = 1240, .h = 1000,
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
