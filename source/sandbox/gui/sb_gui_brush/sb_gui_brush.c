/*==============================================================================================

    sandbox/gui/sb_gui_brush/sb_gui_brush.c -- the BRUSH test bed: sprites, nine-slice, and the
    widened paint floor.

    What this proves, in the order the panels prove it:

        1  the four brush kinds fill the SAME rect through the SAME call (draw_brush), so a
           surface's look is a value a caller passes, not a function a caller picks
        2  a nine-slice holds its authored corners at every size -- the whole reason sprites
           carry slice insets, shown as one sprite drawn at six sizes at once
        3  stretch vs TILE, the flips, the tint, and the `scale` field, all live
        4  a CUSTOM WIDGET skinned entirely by a brush: rect + item() + draw_brush, no chrome,
           no style grid -- the payoff, since that widget's face is now data its caller owns

    Every sprite here is authored PROCEDURALLY at startup (paint_* below) and registered through
    gui()->register_sprite, so the sandbox has zero asset dependencies and the art is readable as
    code.  A real kit would gui()->load_sprite a PNG instead; nothing downstream can tell the
    difference -- same atlas, same tenant, same batching.

    Controls live in the left window; the specimen and the proof strips are drawn into the right
    one.  Both are ordinary stock windows, because none of this needed a new container.

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
#define PLUM      GUI_COLOR( 0xB0, 0x60, 0xE0, 0xFF )

/*==============================================================================================
    A tiny RGBA painting kit -- enough to author the demo art in code.

    Straight (non-premultiplied) alpha, RGBA byte order, row-major: exactly what
    register_sprite takes and what stb_image would have decoded a PNG into.
==============================================================================================*/

#define ART_MAX  ( 64 * 64 )

typedef struct
{
    u8  px[ ART_MAX * 4 ];
    u32 w, h;

} art_t;

static void
art_begin( art_t* a, u32 w, u32 h )
{
    memset( a, 0, sizeof( *a ) );
    a->w = w;
    a->h = h;
}

/* Blend one RGBA texel over what is already there (source-over, straight alpha). */
static void
art_px( art_t* a, i32 x, i32 y, u32 r, u32 g, u32 b, u32 alpha )
{
    if ( x < 0 || y < 0 || (u32)x >= a->w || (u32)y >= a->h || alpha == 0 )
        return;

    u8* p  = &a->px[ ( (u32)y * a->w + (u32)x ) * 4 ];
    f32 sa = (f32)alpha / 255.0f;
    f32 da = (f32)p[ 3 ] / 255.0f;
    f32 oa = sa + da * ( 1.0f - sa );
    if ( oa <= 0.0001f ) { p[ 0 ] = p[ 1 ] = p[ 2 ] = p[ 3 ] = 0; return; }

    f32 sc[ 3 ] = { (f32)r, (f32)g, (f32)b };
    for ( u32 i = 0; i < 3; ++i )
        p[ i ] = (u8)( ( sc[ i ] * sa + (f32)p[ i ] * da * ( 1.0f - sa ) ) / oa + 0.5f );
    p[ 3 ] = (u8)( oa * 255.0f + 0.5f );
}

static void
art_rect( art_t* a, i32 x, i32 y, i32 w, i32 h, u32 r, u32 g, u32 b, u32 alpha )
{
    for ( i32 j = y; j < y + h; ++j )
        for ( i32 i = x; i < x + w; ++i )
            art_px( a, i, j, r, g, b, alpha );
}

/*==============================================================================================
    The demo art

    Four pieces, each chosen to exercise one thing the brush has to get right.
==============================================================================================*/

/* PANEL FRAME -- 48x48, sliced 16 all round.  An outer rule, an inner bevel (light top-left,
   dark bottom-right), a stud in each corner, and a flat interior.  The studs are the test: they
   live entirely inside the corner slices, so they must stay 16x16 and perfectly round no matter
   how far the frame is stretched. */
static void
paint_frame( art_t* a )
{
    art_begin( a, 48, 48 );

    art_rect( a, 0, 0, 48, 48, 0x2A, 0x2D, 0x34, 0xFF );          /* interior fill        */
    art_rect( a, 0, 0, 48,  1, 0x6E, 0x76, 0x88, 0xFF );          /* outer rule           */
    art_rect( a, 0, 47, 48,  1, 0x6E, 0x76, 0x88, 0xFF );
    art_rect( a, 0, 0,  1, 48, 0x6E, 0x76, 0x88, 0xFF );
    art_rect( a, 47, 0,  1, 48, 0x6E, 0x76, 0x88, 0xFF );

    art_rect( a, 2, 2, 44,  1, 0x9A, 0xA4, 0xB8, 0xFF );          /* inner bevel, lit     */
    art_rect( a, 2, 2,  1, 44, 0x9A, 0xA4, 0xB8, 0xFF );
    art_rect( a, 2, 45, 44, 1, 0x1A, 0x1C, 0x20, 0xFF );          /* inner bevel, shaded  */
    art_rect( a, 45, 2,  1, 44, 0x1A, 0x1C, 0x20, 0xFF );

    /* A stud in each corner: a filled disc with a lit rim, drawn with per-texel coverage so the
       edges are smooth -- this is the detail that would smear if the slice were wrong. */
    const i32 cx[ 4 ] = { 8, 39, 8, 39 };
    const i32 cy[ 4 ] = { 8, 8, 39, 39 };
    for ( u32 k = 0; k < 4; ++k )
        for ( i32 j = -6; j <= 6; ++j )
            for ( i32 i = -6; i <= 6; ++i )
            {
                f32 d = sqrtf( (f32)( i * i + j * j ) );
                if ( d > 5.5f ) continue;
                u32 cov = ( d > 4.5f ) ? (u32)( ( 5.5f - d ) * 255.0f ) : 255u;
                bool rim = ( d > 3.4f );
                art_px( a, cx[ k ] + i, cy[ k ] + j,
                        rim ? 0xC8 : 0x50, rim ? 0xA0 : 0x54, rim ? 0x40 : 0x5E, cov );
            }
}

/* BUTTON FACE -- 24x24, sliced 8 all round.  Rounded corners carried in ALPHA (so the corners
   really are transparent, not "the background colour painted in"), a vertical gloss ramp, and a
   lit top edge.  The 8x8 middle is what stretches across a button of any width. */
static void
paint_button( art_t* a )
{
    art_begin( a, 24, 24 );

    for ( i32 y = 0; y < 24; ++y )
    {
        f32 t   = (f32)y / 23.0f;                                  /* gloss ramp, top to bottom */
        u32 lum = (u32)( 92.0f - 40.0f * t );
        for ( i32 x = 0; x < 24; ++x )
        {
            /* Corner coverage: distance from the nearest corner's 7px arc centre. */
            f32 cxp = ( x < 12 ) ? 7.0f : 16.0f;
            f32 cyp = ( y < 12 ) ? 7.0f : 16.0f;
            f32 dx  = ( x < 7 || x > 16 ) ? (f32)x - cxp : 0.0f;
            f32 dy  = ( y < 7 || y > 16 ) ? (f32)y - cyp : 0.0f;
            f32 d   = sqrtf( dx * dx + dy * dy );
            if ( d > 7.5f ) continue;
            u32 cov = ( d > 6.5f ) ? (u32)( ( 7.5f - d ) * 255.0f ) : 255u;

            art_px( a, x, y, lum, lum + 4, lum + 12, cov );
        }
    }
    art_rect( a, 7, 1, 10, 1, 0xC0, 0xC8, 0xD8, 0xB0 );            /* lit top edge */
}

/* CHEVRON RIBBON -- 24x16, sliced { 8, 8, 0, 0 }: horizontal slices only, so the caps hold and
   only the 8px middle repeats.  The middle carries ONE chevron, which is what makes the
   difference between TILE and stretch obvious at a glance -- stretched it smears into a wedge,
   tiled it marches. */
static void
paint_ribbon( art_t* a )
{
    art_begin( a, 24, 16 );

    art_rect( a, 0, 3, 24, 10, 0x30, 0x34, 0x3C, 0xFF );           /* band */
    art_rect( a, 0, 3, 24,  1, 0x60, 0x68, 0x78, 0xFF );
    art_rect( a, 0, 12, 24, 1, 0x18, 0x1A, 0x1E, 0xFF );

    art_rect( a, 1, 5,  4, 6, 0xC8, 0xA0, 0x40, 0xFF );            /* left cap  */
    art_rect( a, 19, 5, 4, 6, 0xC8, 0xA0, 0x40, 0xFF );            /* right cap */

    for ( i32 i = 0; i < 4; ++i )                                  /* one chevron in the middle */
    {
        art_rect( a, 8 + i, 5 + i, 2, 2, 0x40, 0xC0, 0xB0, 0xFF );
        art_rect( a, 15 - i, 5 + i, 2, 2, 0x40, 0xC0, 0xB0, 0xFF );
    }
}

/* ORB -- 32x32, NO slice.  A shaded sphere with an off-centre highlight: unsliced art, so it
   stretches whole.  Its asymmetric highlight is what makes FLIP_X / FLIP_Y visible. */
static void
paint_orb( art_t* a )
{
    art_begin( a, 32, 32 );

    for ( i32 y = 0; y < 32; ++y )
        for ( i32 x = 0; x < 32; ++x )
        {
            f32 dx = (f32)x - 15.5f, dy = (f32)y - 15.5f;
            f32 d  = sqrtf( dx * dx + dy * dy );
            if ( d > 15.5f ) continue;
            u32 cov = ( d > 14.5f ) ? (u32)( ( 15.5f - d ) * 255.0f ) : 255u;

            f32 hx = (f32)x - 10.0f, hy = (f32)y - 9.0f;           /* highlight, up and left */
            f32 hd = sqrtf( hx * hx + hy * hy );
            f32 lit = 1.0f - hd / 22.0f;
            if ( lit < 0.0f ) lit = 0.0f;

            art_px( a, x, y, (u32)( 40.0f + 200.0f * lit ), (u32)( 70.0f + 170.0f * lit ),
                    (u32)( 130.0f + 120.0f * lit ), cov );
        }
}

/*==============================================================================================
    Registration -- one pass at startup, after gui()->boot (the atlas needs a live rhi context).
==============================================================================================*/

typedef struct
{
    gui_sprite_id_t frame, button, ribbon, orb;

} sprites_t;

static sprites_t s_art;

static gui_sprite_id_t
register_art( const char* name, const art_t* a, gui_pad_t slice )
{
    gui_sprite_id_t id = gui()->register_sprite( name, a->w, a->h, a->px );
    if ( id == GUI_SPRITE_NONE )
    {
        printf( "[sb_gui_brush] sprite '%s' failed to register\n", name );
        return id;
    }
    gui()->sprite_set_slice( id, slice );
    return id;
}

static void
build_art( void )
{
    art_t a;

    paint_frame ( &a );  s_art.frame  = register_art( "frame",  &a, ( gui_pad_t ){ 16, 16, 16, 16 } );
    paint_button( &a );  s_art.button = register_art( "button", &a, ( gui_pad_t ){  8,  8,  8,  8 } );
    paint_ribbon( &a );  s_art.ribbon = register_art( "ribbon", &a, ( gui_pad_t ){  8,  8,  0,  0 } );
    paint_orb   ( &a );  s_art.orb    = register_art( "orb",    &a, ( gui_pad_t ){  0,  0,  0,  0 } );

    /* Report what landed, and what slice each carries -- a test bed should say what it built, and
       a zero id here is the one failure that would otherwise show up as an empty panel. */
    const gui_sprite_id_t ids [ 4 ] = { s_art.frame, s_art.button, s_art.ribbon, s_art.orb };
    const char* const     name[ 4 ] = { "frame", "button", "ribbon", "orb" };
    for ( u32 i = 0; i < 4; ++i )
    {
        gui_vec2_t sz = gui()->sprite_size( ids[ i ] );
        gui_pad_t  sl = gui()->sprite_slice( ids[ i ] );
        printf( "[sb_gui_brush] sprite %-7s id=%u  %.0fx%.0f  slice l%.0f r%.0f t%.0f b%.0f\n",
                name[ i ], ids[ i ], sz.x, sz.y, sl.l, sl.r, sl.t, sl.b );
    }
    fflush( stdout );
}

/*==============================================================================================
    THE FACE PLANE -- the payoff, and the reason step 1 was worth building.

    Everything above draws art the caller asked for BY NAME.  This installs art on the style GRID
    instead, at (look, role, phase) cells, and then draws nothing at all: the windows, buttons,
    checkboxes, menus, sliders and scrollbars you see change because every one of them already
    paints its surface through the grid.  Not one widget below is edited, and there is no widget
    below -- this is chrome, the shipped product set, restyled by a theme.

    A style SOURCE is the right home for it (rather than a one-time poke) because a source is
    re-run at every style landing, which is also when the brush pool is cleared -- so registering
    art here is what keeps the handles valid across a theme, font, or scale change.
==============================================================================================*/

static bool s_skin = false;   /* is the art theme installed? */

/* The three motion rates, owned HERE rather than left in the style.

   style_edit() writes the INSTALLED layer, and a landing -- a font change, a theme change, ticking
   the skin box below -- re-derives that layer from the active theme, so an ad-hoc poke is live
   until the next landing and then gone.  A kit that OWNS a value installs it from its style
   SOURCE, which is re-run at every landing by definition.  That is the whole reason this source is
   registered unconditionally instead of only while the art skin is on. */
static f32 s_rate_hot = 10.0f;
static f32 s_rate_act = 20.0f;
static f32 s_rate_sel = 12.0f;

static void
sb_style_source( void* user )
{
    UNUSED( user );

    gui_style_t* st = gui()->style_edit();

    /* Always ours: the motion budget, re-installed over whatever the theme just landed. */
    st->var[ GUI_VAR_ANIM_HOT    ] = s_rate_hot;
    st->var[ GUI_VAR_ANIM_ACTIVE ] = s_rate_act;
    st->var[ GUI_VAR_ANIM_SELECT ] = s_rate_sel;

    if ( !s_skin || s_art.frame == GUI_SPRITE_NONE )
        return;                       /* no art (or none registered yet) -- theme colours stand */

    /* Register this theme's art, then name the handles from cells.  Tinting is how ONE piece of
       art serves a whole phase ramp: the same button face, lit for hover and warmed for press,
       so the widget set keeps reacting exactly as it did on flat colour. */
    gui_style_face_t panel = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.frame,  .scale = 1.0f } );
    gui_style_face_t title = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.ribbon, .scale = 2.0f } );

    gui_style_face_t face_idle = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f } );
    gui_style_face_t face_hot  = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                          .col_a = GUI_COLOR( 0xFF, 0xE0, 0xB0, 0xFF ) } );
    gui_style_face_t face_act  = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                          .col_a = AMBER } );
    gui_style_face_t grab      = gui()->style_brush_add(
        &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                          .col_a = GUI_COLOR( 0xC0, 0xD8, 0xFF, 0xFF ) } );

    /* Container surfaces. */
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_PANEL ][ GUI_PHASE_IDLE   ] = panel;
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_PANEL ][ GUI_PHASE_DIM    ] = panel;
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_TITLE ][ GUI_PHASE_IDLE   ] = title;
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_TITLE ][ GUI_PHASE_DIM    ] = title;

    /* Control faces, across the phase ramp -- this row is what reaches buttons, checkboxes, combo
       fields, menu rows, tree nodes, input boxes and toolbar buttons all at once. */
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_BG ][ GUI_PHASE_IDLE   ] = face_idle;
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_BG ][ GUI_PHASE_HOT    ] = face_hot;
    st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ] = face_act;
    st->face[ GUI_LOOK_SELECT ][ GUI_ROLE_BG ][ GUI_PHASE_IDLE   ] = face_act;

    /* Knobs and thumbs: slider handles and both scrollbar grabs. */
    for ( u32 p = 0; p < GUI_PHASE_COUNT; ++p )
        st->face[ GUI_LOOK_NORMAL ][ GUI_ROLE_GRAB ][ p ] = grab;
}

/* Install / remove the art theme.  style_source_set runs a landing immediately, so the whole UI
   changes on the frame the box is ticked -- and changes back just as completely, because a face
   cell reverts to 0 and the colour underneath it was never touched. */
static void
skin_apply( bool on )
{
    s_skin = on;
    gui()->style_source_set( sb_style_source, NULL );   /* re-registering runs the landing */
}

/*==============================================================================================
    Live controls -- the brush the specimen panels are filled with
==============================================================================================*/

static i32  s_kind    = GUI_BRUSH_NINE;
static i32  s_pick    = 0;          /* index into the sprite list below */
static f32  s_w       = 320.0f;
static f32  s_h       = 200.0f;
static f32  s_scale   = 1.0f;
static bool s_tile    = false;
static bool s_flip_x  = false;
static bool s_flip_y  = false;
static bool s_tinted  = false;
static bool s_guides  = true;

static const char* const k_kind_name[] = { "SOLID", "GRADIENT", "SPRITE", "NINE" };
static const char* const k_pick_name[] = { "frame", "button", "ribbon", "orb" };

static gui_sprite_id_t
picked_sprite( void )
{
    switch ( s_pick )
    {
        case 1:  return s_art.button;
        case 2:  return s_art.ribbon;
        case 3:  return s_art.orb;
        default: return s_art.frame;
    }
}

/* THE brush the whole demo paints through -- one value, assembled from the controls. */
static gui_brush_t
current_brush( void )
{
    u16 flags = 0;
    if ( s_tile )   flags |= GUI_BRUSH_TILE;
    if ( s_flip_x ) flags |= GUI_BRUSH_FLIP_X;
    if ( s_flip_y ) flags |= GUI_BRUSH_FLIP_Y;

    return ( gui_brush_t ){
        .kind   = (u8)s_kind,
        .flags  = flags,
        .col_a  = ( s_kind == GUI_BRUSH_SPRITE || s_kind == GUI_BRUSH_NINE )
                      ? ( s_tinted ? AMBER : 0u )          /* 0 = untinted, the sprite's own colours */
                      : GUI_COLOR( 0x30, 0x50, 0x80, 0xFF ),
        .col_b  = PLUM,
        .sprite = picked_sprite(),
        .scale  = s_scale,
    };
}

/*==============================================================================================
    Panel 1 -- the specimen: one rect, one draw_brush call, over a checker so alpha reads true
==============================================================================================*/

static void
draw_slice_guides( gui_rect_t r )
{
    gui_pad_t sl = gui()->sprite_slice( picked_sprite() );
    if ( sl.l + sl.r + sl.t + sl.b <= 0.0f || s_kind != GUI_BRUSH_NINE )
        return;

    /* Where the nine-slice actually cut, in destination pixels -- the same insets the
       tessellator used, so a mismatch between art and expectation shows up here first. */
    f32 L = sl.l * s_scale, R = sl.r * s_scale, T = sl.t * s_scale, B = sl.b * s_scale;
    u32 g = GUI_COLOR( 0xFF, 0x40, 0x40, 0x90 );

    gui()->draw_dashed_line( r.x + L, r.y, r.x + L, r.y + r.h, 4.0f, 4.0f, 1.0f, g );
    gui()->draw_dashed_line( r.x + r.w - R, r.y, r.x + r.w - R, r.y + r.h, 4.0f, 4.0f, 1.0f, g );
    gui()->draw_dashed_line( r.x, r.y + T, r.x + r.w, r.y + T, 4.0f, 4.0f, 1.0f, g );
    gui()->draw_dashed_line( r.x, r.y + r.h - B, r.x + r.w, r.y + r.h - B, 4.0f, 4.0f, 1.0f, g );
}

static void
panel_specimen( void )
{
    gui()->separator_text( "specimen -- one rect, one draw_brush" );

    /* Reserve the tallest the sliders can ask for, so the panels below never jump as it resizes. */
    gui_rect_t cell = gui()->canvas( 420.0f );
    gui_rect_t r    = { cell.x + 8.0f, cell.y + 8.0f, s_w, s_h };

    /* Coarse cells on purpose.  draw_checker is clamped to 64x64, and every cell used to cost a
       command slot -- a fine checker over a panel this size is how this sandbox first ate the
       entire frame's command budget and drew nothing but its own backdrop.  The checker batches
       now, but a 20px cell is also just easier to read alpha against. */
    gui()->draw_checker( cell, 20.0f, GUI_COLOR( 0x24, 0x24, 0x28, 0xFF ),
                                      GUI_COLOR( 0x2C, 0x2C, 0x32, 0xFF ) );

    gui_brush_t b = current_brush();
    gui()->draw_brush( r, &b );

    if ( s_guides )
        draw_slice_guides( r );
}

/*==============================================================================================
    Panel 2 -- the proof: ONE sprite, six sizes, corners unchanged

    This is the panel the whole feature exists for.  Every box below is the same 48x48 art through
    the same brush; only the rect differs.  A stretched quad would smear the corner studs into
    ovals -- they stay round, because the corner pieces are never scaled.
==============================================================================================*/

static void
panel_sizes( void )
{
    gui()->separator_text( "one sprite, six sizes -- the corners never scale" );

    gui_rect_t cell = gui()->canvas( 190.0f );
    gui_brush_t b   = ( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_art.frame, .scale = 1.0f };

    static const f32 W[ 6 ] = { 48.0f, 64.0f, 96.0f, 140.0f, 200.0f, 280.0f };
    static const f32 H[ 6 ] = { 48.0f, 96.0f, 64.0f, 170.0f,  90.0f, 130.0f };

    f32 x = cell.x + 6.0f;
    for ( u32 i = 0; i < 6; ++i )
    {
        gui()->draw_brush( ( gui_rect_t ){ x, cell.y + 8.0f, W[ i ], H[ i ] }, &b );
        x += W[ i ] + 10.0f;
    }
}

/*==============================================================================================
    Panel 3 -- the four kinds, same rect, same call

    SOLID is exactly what draw_fill always did.  The point of the row is that nothing about the
    CALL changes across it: four values, one verb.
==============================================================================================*/

static void
panel_kinds( void )
{
    gui()->separator_text( "the four kinds -- identical rects, identical call" );

    gui_rect_t cell = gui()->canvas( 120.0f );

    const gui_brush_t kinds[ 4 ] = {
        { .kind = GUI_BRUSH_SOLID,    .col_a = GUI_COLOR( 0x30, 0x50, 0x80, 0xFF ) },
        { .kind = GUI_BRUSH_GRADIENT, .col_a = TEAL, .col_b = PLUM, .flags = GUI_BRUSH_VERTICAL },
        { .kind = GUI_BRUSH_SPRITE,   .sprite = s_art.frame, .scale = 1.0f },
        { .kind = GUI_BRUSH_NINE,     .sprite = s_art.frame, .scale = 1.0f },
    };

    f32 x = cell.x + 6.0f;
    for ( u32 i = 0; i < 4; ++i )
    {
        gui_rect_t r = { x, cell.y + 6.0f, 150.0f, 76.0f };
        gui()->draw_brush( r, &kinds[ i ] );
        gui()->draw_text_in( ( gui_rect_t ){ x, cell.y + 88.0f, 150.0f, 20.0f },
                             GUI_ALIGN_CENTER, INK_DIM, k_kind_name[ i ] );
        x += 162.0f;
    }
}

/*==============================================================================================
    Panel 4 -- stretch vs tile, on the sprite authored to show the difference
==============================================================================================*/

static void
panel_tile( void )
{
    gui()->separator_text( "stretch vs TILE -- the middle track is what repeats" );

    gui_rect_t cell = gui()->canvas( 110.0f );

    gui_brush_t stretch = { .kind = GUI_BRUSH_NINE, .sprite = s_art.ribbon, .scale = 2.0f };
    gui_brush_t tiled   = { .kind = GUI_BRUSH_NINE, .sprite = s_art.ribbon, .scale = 2.0f,
                            .flags = GUI_BRUSH_TILE };

    gui()->draw_brush( ( gui_rect_t ){ cell.x + 6.0f, cell.y + 8.0f,  560.0f, 32.0f }, &stretch );
    gui()->draw_text( cell.x + 576.0f, cell.y + 16.0f, INK_DIM, "stretch" );

    gui()->draw_brush( ( gui_rect_t ){ cell.x + 6.0f, cell.y + 52.0f, 560.0f, 32.0f }, &tiled );
    gui()->draw_text( cell.x + 576.0f, cell.y + 60.0f, INK_DIM, "TILE" );
}

/*==============================================================================================
    Panel 5 -- a custom widget skinned entirely by a brush.

    rect + item() + draw_brush, and nothing else: no chrome, no style grid, no stock render.  The
    widget picks its face by PHASE the way every widget in the library does (item_phase), but the
    three faces it picks between are brushes its CALLER handed it -- which is the whole point.
    Swap the array and the widget restyles without being touched.
==============================================================================================*/

static bool
brush_button( const char* label, gui_rect_t r, const gui_brush_t face[ GUI_PHASE_COUNT ] )
{
    gui_item_state_t st = gui()->item( label, r );

    gui()->draw_brush( r, &face[ gui()->item_phase( st ) ] );
    gui()->draw_text_in( r, GUI_ALIGN_CENTER, INK, label );

    return st.clicked;
}

static void
panel_widget( void )
{
    static u32 s_clicks = 0;

    gui()->separator_text( "a custom widget whose face is a brush (hover / press it)" );

    gui_rect_t cell = gui()->canvas( 96.0f );

    /* Three faces, one per phase -- the caller's data, not the library's. */
    const gui_brush_t art_face[ GUI_PHASE_COUNT ] = {
        [ GUI_PHASE_IDLE   ] = { .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f },
        [ GUI_PHASE_HOT    ] = { .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                                 .col_a = GUI_COLOR( 0xFF, 0xD8, 0xA0, 0xFF ) },
        [ GUI_PHASE_ACTIVE ] = { .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                                 .col_a = AMBER },
        [ GUI_PHASE_DIM    ] = { .kind = GUI_BRUSH_NINE, .sprite = s_art.button, .scale = 1.0f,
                                 .col_a = GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ) },
    };

    /* The SAME widget over flat brushes -- same code path, different data. */
    const gui_brush_t flat_face[ GUI_PHASE_COUNT ] = {
        [ GUI_PHASE_IDLE   ] = { .kind = GUI_BRUSH_GRADIENT, .col_a = GUI_COLOR( 0x3A, 0x3E, 0x48, 0xFF ),
                                 .col_b = GUI_COLOR( 0x24, 0x26, 0x2C, 0xFF ), .flags = GUI_BRUSH_VERTICAL },
        [ GUI_PHASE_HOT    ] = { .kind = GUI_BRUSH_GRADIENT, .col_a = GUI_COLOR( 0x50, 0x56, 0x64, 0xFF ),
                                 .col_b = GUI_COLOR( 0x30, 0x34, 0x3C, 0xFF ), .flags = GUI_BRUSH_VERTICAL },
        [ GUI_PHASE_ACTIVE ] = { .kind = GUI_BRUSH_SOLID, .col_a = GUI_COLOR( 0x60, 0x40, 0x18, 0xFF ) },
        [ GUI_PHASE_DIM    ] = { .kind = GUI_BRUSH_SOLID, .col_a = GUI_COLOR( 0x28, 0x28, 0x2C, 0xFF ) },
    };

    if ( brush_button( "nine-slice face", ( gui_rect_t ){ cell.x + 6.0f, cell.y + 8.0f, 190.0f, 40.0f },
                       art_face ) )
        ++s_clicks;
    if ( brush_button( "gradient face", ( gui_rect_t ){ cell.x + 208.0f, cell.y + 8.0f, 190.0f, 40.0f },
                       flat_face ) )
        ++s_clicks;

    char msg[ 64 ];
    snprintf( msg, sizeof( msg ), "%u clicks -- one widget, two skins", s_clicks );
    gui()->draw_text( cell.x + 6.0f, cell.y + 62.0f, INK_DIM, msg );
}

/*==============================================================================================
    MOTION -- the mix, watched

    Every widget in the library now travels between style cells instead of snapping between them,
    and this panel is where that becomes visible rather than merely felt.  One probe widget reads
    ONE mix and spends it on three different rows of the grid -- its surface, its border and its
    ink -- which is the whole argument for splitting the read from the spend: three parts, one
    damper slot, and they arrive together because they share the weights.

    The bars underneath are the raw channels.  Hover the probe and watch `hot` climb; hold the
    button and watch `act` overtake it; click to toggle and watch `sel` cross on its own rate.
==============================================================================================*/

static bool s_probe_on = false;

static void
weight_bar( gui_rect_t r, const char* name, f32 v, u32 col )
{
    char txt[ 32 ];

    gui()->draw_brush( ( gui_rect_t ){ r.x, r.y, r.w, r.h },
                       &( gui_brush_t ){ .kind = GUI_BRUSH_SOLID,
                                         .col_a = GUI_COLOR( 0x22, 0x22, 0x28, 0xFF ) } );
    if ( v > 0.0f )
        gui()->draw_brush( ( gui_rect_t ){ r.x, r.y, r.w * v, r.h },
                           &( gui_brush_t ){ .kind = GUI_BRUSH_SOLID, .col_a = col } );

    snprintf( txt, sizeof( txt ), "%s %.2f", name, (double)v );
    gui()->draw_text( r.x + r.w + 10.0f, r.y - 2.0f, INK_DIM, txt );
}

static void
panel_motion( void )
{
    /* Any stable id keys a mix -- it does not have to be a widget's own.  A constant is honest
       here because there is exactly one probe. */
    const gui_id_t PROBE = 0x51DE0001u;

    gui()->separator_text( "motion -- ONE mix, spent on three rows (hover / hold / click)" );

    gui_rect_t cell = gui()->canvas( 150.0f );

    gui_rect_t       r  = { cell.x + 6.0f, cell.y + 8.0f, 300.0f, 44.0f };
    gui_item_state_t st = gui()->item( "probe", r );
    if ( st.clicked )
        s_probe_on = !s_probe_on;

    gui_style_mix_t m = gui()->style_mix( PROBE, st, s_probe_on );

    gui()->draw_face_mix( r, GUI_ROLE_BG, m );                                 /* surface */
    gui()->draw_frame( r, 0x00000000u,
                       gui()->style_color_mix( GUI_ROLE_BORDER, m ), 1.0f );   /* border  */
    gui()->draw_text_in( r, GUI_ALIGN_CENTER,
                         gui()->style_color_mix( GUI_ROLE_TEXT, m ),           /* ink     */
                         s_probe_on ? "selected -- click to clear" : "hover, hold, click me" );

    f32 by = cell.y + 66.0f;
    weight_bar( ( gui_rect_t ){ cell.x + 6.0f, by,         180.0f, 12.0f }, "hot", m.hot,
                GUI_COLOR( 0x60, 0xA0, 0xE0, 0xFF ) );
    weight_bar( ( gui_rect_t ){ cell.x + 6.0f, by + 20.0f, 180.0f, 12.0f }, "act", m.act, AMBER );
    weight_bar( ( gui_rect_t ){ cell.x + 6.0f, by + 40.0f, 180.0f, 12.0f }, "sel", m.sel,
                GUI_COLOR( 0x80, 0xD0, 0x80, 0xFF ) );

    gui()->draw_text( cell.x + 330.0f, cell.y + 12.0f, INK_DIM,
                      "one gui_anim4 slot, three channels," );
    gui()->draw_text( cell.x + 330.0f, cell.y + 30.0f, INK_DIM,
                      "evicted the moment all three settle." );
}

/*==============================================================================================
    panel_surface -- the EFFECT BAND: shapes the fragment shader resolves

    Everything above this panel is geometry the CPU tessellated.  A rounded corner was an arc
    table fanned into triangles: ~37 vertices, hard stair-stepped edges, and a texture could not
    ride on it.  A soft shadow was six stacked rects pretending to be a gaussian.

    Here the vertex carries a signed-distance coordinate and a packed word naming the shape, so
    each of these is FOUR quads and the boundary is computed per pixel.  Three consequences, in
    the order the rows show them:

        1  a radius is exact at any size -- including a full pill, where radius = half the height
        2  the same distance drives a BAND, so a rounded frame is one surface, not a stroked loop
        3  widen the falloff and the same surface IS the shadow -- and it can go behind a
           textured, rounded quad, which is the combination nothing before this could draw

    The whole row is one GPU batch with the text beside it.  That is the point of putting the
    mode in the VERTEX rather than a push constant: an effect that split the batch would be a
    thing you spent sparingly, and this one is free enough to put under every panel.
==============================================================================================*/

static void
panel_surface( void )
{
    gui()->separator_text( "surface -- SDF shapes: analytic AA, soft edges, rounded art" );

    gui_rect_t cell = gui()->canvas( 250.0f );
    f32        x    = cell.x + 6.0f;
    f32        y    = cell.y + 6.0f;

    /* 1 -- radius sweep, ending in a pill (radius clamps to half the short side). */
    gui()->draw_text( x, y, INK_DIM, "radius 0 / 3 / 8 / 16 / pill" );
    {
        static const f32 radii[ 5 ] = { 0.0f, 3.0f, 8.0f, 16.0f, 100.0f };
        f32 bx = x;
        for ( u32 i = 0; i < 5; ++i )
        {
            gui()->draw_round_rect( ( gui_rect_t ){ bx, y + 20.0f, 78.0f, 44.0f },
                                    radii[ i ], radii[ i ], radii[ i ], radii[ i ],
                                    true, 0.0f, TEAL );
            bx += 86.0f;
        }
    }

    /* 2 -- the same boundary as a BAND.  One surface per frame, at any border width. */
    y += 78.0f;
    gui()->draw_text( x, y, INK_DIM, "ring -- border 1 / 2 / 4 / 6 px, same one surface each" );
    {
        static const f32 bw[ 4 ] = { 1.0f, 2.0f, 4.0f, 6.0f };
        f32 bx = x;
        for ( u32 i = 0; i < 4; ++i )
        {
            gui()->draw_round_rect( ( gui_rect_t ){ bx, y + 20.0f, 98.0f, 44.0f },
                                    12.0f, 12.0f, 12.0f, 12.0f, false, bw[ i ], AMBER );
            bx += 106.0f;
        }
    }

    /* 3 -- widen the falloff and the same surface is a shadow.  Drawn BEFORE the body, and the
       body here is a rounded TEXTURED quad: ambient rounding applies to the image too, because
       the coverage the shader computes multiplies whatever the sampler returned. */
    y += 78.0f;
    gui()->draw_text( x, y, INK_DIM, "shadow spread 3 / 8 / 18 -- last one under rounded art" );
    {
        static const f32 spread[ 3 ] = { 3.0f, 8.0f, 18.0f };
        f32 bx = x + 10.0f;
        for ( u32 i = 0; i < 3; ++i )
        {
            gui_rect_t body = { bx, y + 26.0f, 90.0f, 52.0f };

            f32 save = gui()->draw_rounding();
            gui()->draw_set_rounding( 10.0f );
            gui()->draw_shadow( body, spread[ i ], GUI_COLOR( 0, 0, 0, 0xB0 ) );

            if ( i == 2 && s_art.frame != GUI_SPRITE_NONE )
                gui()->draw_sprite_in( body, s_art.frame, 0 );
            else
                gui()->draw_rect( body.x, body.y, body.w, body.h,
                                  GUI_COLOR( 0x38, 0x3C, 0x46, 0xFF ) );
            gui()->draw_set_rounding( save );

            bx += 130.0f;
        }
    }

    gui()->draw_text( cell.x + 470.0f, cell.y + 168.0f, INK_DIM, "16 verts per shape (the fan" );
    gui()->draw_text( cell.x + 470.0f, cell.y + 186.0f, INK_DIM, "cost 37), and no batch split:" );
    gui()->draw_text( cell.x + 470.0f, cell.y + 204.0f, INK_DIM, "the mode rides the vertex." );
}

/*==============================================================================================
    panel_pulse -- animation that costs no re-emit

    Every other moving thing in this sandbox animates the way an immediate-mode UI normally does:
    the CPU eases a number, the widget re-emits with the new value, the window's hash changes, and
    its geometry is thrown away and tessellated again.  That is correct, and for a slider grab
    chasing the mouse it is exactly right -- the SHAPE is what moved.

    A pulse is the case where nothing moved.  The rect is the same rect, frame after frame; only
    its alpha is different, and alpha is something the fragment shader can work out for itself if
    you hand it the clock.  So GUI_FX_PULSE does: the command's bytes are byte-identical every
    frame, the window's hash never changes, and the breathing costs zero re-tessellation for as
    long as it runs.  (The vertex upload is unchanged -- retention saves the tessellation, not
    the buffer write.)

    The right-hand pair is the A/B.  Both boxes use the same wave, one evaluated on the CPU and
    one in the shader, and they are meant to look identical -- that is the point.  The numbers are
    in the "pulse -- retention meter" window (and are there for a reason worth reading: see
    window_pulse_meter below).  Turn the CPU box on and this window drops out of the retained set
    every single frame, for an effect the GPU one gets for nothing.

    What a pulse still owes: a frame to be PRESENTED.  The clock advancing is not what schedules
    a frame, so the panel calls request_redraw once while it is visible.  One call covers every
    pulse on the panel -- which is why this is cheap to overuse and the CPU version is not.
==============================================================================================*/

static bool s_pulse_cpu_on = false;

/* The shader's wave, on the CPU: 1 - depth * (0.5 - 0.5*cos(2*pi*rate*t)).  Duplicated here on
   purpose -- the demo is only honest if both sides compute the same number. */
static f32
pulse_wave( f32 rate, f32 depth, f64 t )
{
    f32 phase = (f32)( 6.28318531 * (f64)rate * t );
    return 1.0f - depth * ( 0.5f - 0.5f * cosf( phase ) );
}

static void
panel_pulse( void )
{
    gui()->separator_text( "pulse -- the shader reads the clock, so nothing re-tessellates" );

    /* The whole per-frame cost of every pulse below. */
    gui()->request_redraw();

    gui_rect_t cell = gui()->canvas( 200.0f );
    f32        x    = cell.x + 6.0f;
    f32        y    = cell.y + 6.0f;

    /* 1 -- rate sweep.  Quantized to 1/4 Hz by the packing, which is what lets every one of them
       cross the clock's 1024 s wrap without a jump. */
    gui()->draw_text( x, y, INK_DIM, "rate 0.5 / 1 / 2 Hz  (depth 0.85)" );
    {
        static const f32 rate[ 3 ] = { 0.5f, 1.0f, 2.0f };
        f32 bx = x;
        for ( u32 i = 0; i < 3; ++i )
        {
            gui()->draw_pulse( ( gui_rect_t ){ bx, y + 20.0f, 108.0f, 42.0f },
                               rate[ i ], 0.85f, TEAL );
            bx += 116.0f;
        }
    }

    /* 2 -- depth sweep at one rate: how much alpha the trough takes, 0 = a still box. */
    y += 76.0f;
    gui()->draw_text( x, y, INK_DIM, "depth 0.25 / 0.5 / 0.75 / 1.0  (1 Hz)" );
    {
        static const f32 depth[ 4 ] = { 0.25f, 0.5f, 0.75f, 1.0f };
        f32 bx = x;
        for ( u32 i = 0; i < 4; ++i )
        {
            gui()->draw_pulse( ( gui_rect_t ){ bx, y + 20.0f, 84.0f, 42.0f },
                               1.0f, depth[ i ], AMBER );
            bx += 92.0f;
        }
    }

    /* 3 -- the A/B.  Both boxes run the same wave; only where it is evaluated differs.  The
       numbers live in the meter window, NOT here -- see the banner. */
    f32 rx = cell.x + 470.0f;
    gui()->draw_text( rx, cell.y + 6.0f, INK_DIM, "same wave, two ways:" );

    gui()->draw_pulse( ( gui_rect_t ){ rx, cell.y + 30.0f, 110.0f, 44.0f }, 1.0f, 0.8f, PLUM );
    gui()->draw_text( rx, cell.y + 82.0f, INK_DIM, "GPU: shader" );

    if ( s_pulse_cpu_on )
    {
        f32 a   = pulse_wave( 1.0f, 0.8f, gui()->get_time() );
        u32 col = GUI_COLOR( 0xB0, 0x60, 0xE0, (u32)( a * 255.0f + 0.5f ) );
        gui()->draw_round_rect( ( gui_rect_t ){ rx + 160.0f, cell.y + 30.0f, 110.0f, 44.0f },
                                4.0f, 4.0f, 4.0f, 4.0f, true, 0.0f, col );
    }
    gui()->draw_text( rx + 160.0f, cell.y + 82.0f, INK_DIM,
                      s_pulse_cpu_on ? "CPU: re-emit" : "CPU: off" );

    gui()->draw_text( rx, cell.y + 116.0f, INK_DIM, "toggle the CPU box below;" );
    gui()->draw_text( rx, cell.y + 134.0f, INK_DIM, "the meter window reports" );
    gui()->draw_text( rx, cell.y + 152.0f, INK_DIM, "what each one costs." );


    gui()->checkbox( "animate the CPU box (re-emits this window every frame)", &s_pulse_cpu_on );
}

/*==============================================================================================
    window_pulse_meter -- the readout, in a window that does not disturb what it reads

    This started life as three lines of text inside panel_pulse, and it quietly broke the whole
    demo: a readout whose digits change every frame is a command whose bytes change every frame,
    so it dirtied the specimen window's hash -- the very window whose retention it was reporting.
    The meter was measuring itself, and the CPU box's contribution vanished into the noise.

    GUI_WIN_DEBUG_BAND is the library's answer to exactly this.  A debug-band window packs into
    the second arena band, is EXCLUDED from the render stats it may itself display, and never
    raises frame_dirty -- so a live readout can neither pollute its own numbers nor defeat the
    idle skip for the rest of the app.  Any self-measuring UI wants this flag; a diagnostic that
    perturbs the thing it measures is worse than none.
==============================================================================================*/

static void
window_pulse_meter( void )
{
    gui()->window_set_next_pos( 20.0f, 510.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 300.0f, 190.0f, GUI_COND_ONCE );

    if ( gui()->window_begin( "pulse -- retention meter", GUI_WIN_DEBUG_BAND ) )
    {
        gui()->stack();

        gui_render_stats_t st = gui()->render_stats();
        gui()->textf( "windows retained  %u / %u", st.win_retained, st.win_total );
        gui()->textf( "verts retained    %u / %u", st.vert_retained, st.vert_count );
        gui()->separator();

        if ( s_pulse_cpu_on )
        {
            gui()->text( "CPU box ON: the stage window" );
            gui()->text( "re-tessellates every frame." );
        }
        else
        {
            gui()->text( "shader pulses only: nothing" );
            gui()->text( "re-tessellates. Scroll the" );
            gui()->text( "stage to the pulse panel." );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    panel_box -- the DECORATOR: a surface behind a run of widgets

    Every other surface in this sandbox belongs to a THING -- a widget's face, a window body --
    because a surface needs a rect and only a thing has one.  A group of widgets is not a thing;
    it is whatever got emitted between two calls, and its rect does not exist until the second.

    box_begin / box_end is that missing rect.  It owns no region, no clip, and no scroll (the
    reason it is not just child_begin), and it paints through the same style ROLE the rest of the
    library paints through, so a theme that authors a FACE for that cell skins every box at once.

    The row of buttons below adds and removes widgets INSIDE the box, which is the whole point:
    the box has to find its own height, and it finds it from last frame's measure.  Drag the size
    rate to 0 in the control window and the same edit becomes a snap after one wrong frame --
    which is exactly the artifact GUI_VAR_ANIM_SIZE exists to cover.
==============================================================================================*/

static i32  s_box_rows = 3;
static f32  s_row_val[ 8 ] = { 0.2f, 0.5f, 0.8f, 0.3f, 0.6f, 0.9f, 0.4f, 0.7f };
static bool s_box_nested_on = true;

static void
panel_box( void )
{
    gui()->separator_text( "box -- a styled surface sized to whatever it ends up containing" );

    /* Natural (0) tracks -- so this row is also the size damper's other half on show: the two
       buttons size to their labels through the very feedback GUI_VAR_ANIM_SIZE eases. */
    gui()->row_cols( 0.0f, ( f32[] ){ 0, 0, 1, GUI_END } );
    if ( gui()->button( "add row" )    ) ++s_box_rows;
    if ( gui()->button( "remove row" ) ) --s_box_rows;
    gui()->text( "" );
    if ( s_box_rows < 0 ) s_box_rows = 0;
    if ( s_box_rows > 8 ) s_box_rows = 8;

    gui()->stack();

    gui()->box_begin( "summary", GUI_ROLE_PANEL );
    {
        gui()->text( "PANEL role -- the container surface" );
        for ( i32 i = 0; i < s_box_rows; ++i )
        {
            char name[ 32 ];
            snprintf( name, sizeof( name ), "row %d", i );
            gui()->slider_float( name, &s_row_val[ i ], 0.0f, 1.0f );
        }
    }
    gui()->box_end();

    /* A second box, nested, on the control-surface row of the same grid -- so the two are the
       same call with one argument changed, which is the argument for a role in the first place. */
    gui()->box_begin( "nested", GUI_ROLE_PANEL );
    {
        gui()->text( "a box inside a box:" );
        gui()->box_begin( "inner", GUI_ROLE_BG );
        {
            gui()->text( "BG role -- the control surface" );
            gui()->checkbox( "each is its own id scope", &s_box_nested_on );
        }
        gui()->box_end();
    }
    gui()->box_end();
}

/*==============================================================================================
    The control window
==============================================================================================*/

/* A multi-line tooltip on the slider just emitted.  Each rate drives ONE channel of the mix, and
   which widgets that channel can even reach is the part that is not guessable from a number --
   which is why the select tooltip spends its last three lines saying where it does nothing. */
static void
rate_help( const char* title, const char* const* lines )
{
    /* tooltip_begin does NOT test hover -- it opens the window unconditionally, and the CALLER
       guards.  (set_item_tooltip is the one-liner that does this for you.)  Without this line all
       three tooltips open every frame on the one shared tooltip window, stacked at the cursor, and
       the narrower one's right border lands in the middle of the wider one's text. */
    if ( !gui()->is_item_hovered() )
        return;                       /* nothing was opened, so there is nothing to close */

    /* begin GATES THE BODY, end is UNCONDITIONAL -- it reattaches the overlay that begin detached
       whether or not the body ran, so an early return between the two leaks that detach. */
    if ( gui()->tooltip_begin() )
    {
        gui()->stack();   /* a tooltip body is a fresh region -- declare a layout mode first */

        gui()->separator_text( title );
        for ( u32 i = 0; lines[ i ]; ++i )
            gui()->text( lines[ i ] );

        gui()->separator();
        gui()->text( "Hz-like damper speed: 10 ~ 250 ms to 95%," );
        gui()->text( "20 ~ 150 ms. 0 snaps with no animation." );
        gui()->text( "Live readout: the hot / act / sel bars in" );
        gui()->text( "the motion panel of the stage window." );
    }
    gui()->tooltip_end();
}

/* Three side-by-side cells, each isolating ONE mix channel.  Hover the first, hold the second,
   click the third; each shows only the weight its slider drives, plus the live number. */
static void
rate_swatch( void )
{
    static const char* const k_name[ 3 ] = { "hover", "press", "select" };
    static bool              s_sel       = false;

    gui_rect_t row = gui()->canvas( 42.0f );
    f32        w   = ( row.w - 16.0f ) / 3.0f;

    for ( u32 i = 0; i < 3; ++i )
    {
        gui_rect_t       r  = { row.x + (f32)i * ( w + 8.0f ), row.y, w, 26.0f };
        gui_item_state_t st = gui()->item( k_name[ i ], r );

        if ( i == 2 && st.clicked )
            s_sel = !s_sel;

        /* The full mix, then spend ONE weight of it -- the other two are zeroed so this cell can
           only ever show the channel its slider owns. */
        gui_style_mix_t full = gui()->style_mix( 0xB0A70000u + i, st, ( i == 2 ) && s_sel );
        gui_style_mix_t one  = { ( i == 0 ) ? full.hot : 0.0f,
                                 ( i == 1 ) ? full.act : 0.0f,
                                 ( i == 2 ) ? full.sel : 0.0f };

        gui()->draw_face_mix( r, GUI_ROLE_BG, one );
        gui()->draw_frame( r, 0x00000000u, gui()->style_color_mix( GUI_ROLE_BORDER, one ), 1.0f );

        char txt[ 32 ];
        f32  v = ( i == 0 ) ? one.hot : ( i == 1 ) ? one.act : one.sel;
        snprintf( txt, sizeof( txt ), "%s %.2f", k_name[ i ], (double)v );
        gui()->draw_text_in( r, GUI_ALIGN_CENTER,
                             gui()->style_color_mix( GUI_ROLE_TEXT, one ), txt );
    }
}

static void
window_controls( void )
{
    gui()->window_set_next_pos( 20.0f, 20.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 300.0f, 470.0f, GUI_COND_ONCE );

    if ( gui()->window_begin( "brush", GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->separator_text( "brush" );
        gui()->combo( "kind", &s_kind, k_kind_name, 4 );
        gui()->combo( "sprite", &s_pick, k_pick_name, 4 );

        gui()->separator_text( "specimen rect" );
        gui()->slider_float( "width", &s_w, 32.0f, 560.0f );
        gui()->slider_float( "height", &s_h, 32.0f, 400.0f );

        gui()->separator_text( "sprite options" );
        gui()->slider_float( "scale", &s_scale, 0.25f, 4.0f );
        gui()->checkbox( "tile middle", &s_tile );
        gui()->checkbox( "flip x", &s_flip_x );
        gui()->checkbox( "flip y", &s_flip_y );
        gui()->checkbox( "tint amber", &s_tinted );

        gui()->separator_text( "debug" );
        gui()->checkbox( "slice guides", &s_guides );

        /* THE demo.  Tick it and every window, button, checkbox, combo, menu row, slider and
           scrollbar in this application is drawn from art -- through the style grid, with no
           widget code involved and none of chrome edited. */
        gui()->separator_text( "the face plane" );
        bool skin = s_skin;
        if ( gui()->checkbox( "skin chrome with art", &skin ) )
            skin_apply( skin );

        gui()->text( "installs brushes on style cells;" );
        gui()->text( "every widget already paints through" );
        gui()->text( "the grid, so all of them change." );

        /* The motion budget of the ENTIRE widget set, in three numbers.  Drag them and every
           transition in the application retimes live; drop them to 0 and the library snaps,
           down the same code path -- there is no animation branch to turn off. */
        gui()->separator_text( "motion (Hz -- 0 snaps)" );
        {
            gui()->slider_float( "hover", &s_rate_hot, 0.0f, 40.0f );
            rate_help( "hover  -- the HOT weight", ( const char*[] ){
                "How fast a surface lights UNDER THE CURSOR",
                "and fades back out once it leaves.",
                "Watch: the [hover] swatch below, or sweep the",
                "cursor across the buttons. At 2 Hz they glow in",
                "slowly and linger behind the cursor; at 40 Hz",
                "they snap.",
                "Reaches EVERY button, menu row, tree node, list",
                "row, combo, input box, slider track and",
                "scrollbar grab -- the whole widget set.", NULL } );

            gui()->slider_float( "press", &s_rate_act, 0.0f, 40.0f );
            rate_help( "press  -- the ACTIVE weight", ( const char*[] ){
                "How fast a surface darkens while you HOLD the",
                "mouse button down on it.",
                "Watch: press and HOLD the [press] swatch below",
                "without releasing.",
                "Default is faster than hover on purpose -- a",
                "hover is an invitation and may drift, a press is",
                "an answer and must land.",
                "Drop it to 2 Hz and every button in the app",
                "feels mushy under the finger.", NULL } );

            gui()->slider_float( "select", &s_rate_sel, 0.0f, 40.0f );
            rate_help( "select -- the SELECT weight", ( const char*[] ){
                "How fast a thing crosses between the NORMAL and",
                "SELECT colour planes once it is CHOSEN.",
                "Watch: CLICK the [select] swatch below to toggle",
                "it, or click the probe in the stage window.",
                "IF THIS SLIDER SEEMS TO DO NOTHING, that is",
                "correct and worth knowing: only a widget that can",
                "be CHOSEN has a SELECT plane to cross to. A plain",
                "button never leaves NORMAL, so no value here will",
                "ever move one.", NULL } );

            /* One swatch per channel, each painted from a mix with only ITS channel live, so a
               slider has something to watch that CANNOT be confused with the other two.  The
               same style_mix every widget uses -- these just spend one weight instead of all
               three, which is the freedom that splitting the read from the spend buys. */
            rate_swatch();

            if ( gui()->button( "snap all" ) )
                s_rate_hot = s_rate_act = s_rate_sel = 0.0f;
            gui()->same_line( -1.0f );          /* -1 = the theme gap; 0 means a LITERAL zero gap */
            if ( gui()->button( "default" ) )
            {
                s_rate_hot = 10.0f;
                s_rate_act = 20.0f;
                s_rate_sel = 12.0f;
            }

            /* Push the live values into the installed layer every frame.  The source above is what
               makes them SURVIVE a landing; this is what makes a drag visible on the very next
               frame without paying a full landing per frame of the drag.  Note what is NOT here:
               style_apply(), which re-derives the installed layer from the theme base and would
               throw this write away -- the bug that made these three sliders spring back. */
            gui_style_t* est = gui()->style_edit();
            est->var[ GUI_VAR_ANIM_HOT    ] = s_rate_hot;
            est->var[ GUI_VAR_ANIM_ACTIVE ] = s_rate_act;
            est->var[ GUI_VAR_ANIM_SELECT ] = s_rate_sel;
        }

        gui()->separator();
        gui()->text( "scale multiplies the slice insets," );
        gui()->text( "so one sprite serves many UI scales." );
    }
    gui()->window_end();
}

static void
window_stage( void )
{
    gui()->window_set_next_pos( 340.0f, 20.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 920.0f, 900.0f, GUI_COND_ONCE );

    if ( gui()->window_begin( "sprites + nine-slice", GUI_WIN_NONE ) )
    {
        gui()->stack();

        panel_specimen();
        panel_sizes();
        panel_kinds();
        panel_tile();
        panel_widget();
        panel_motion();
        panel_surface();
        panel_pulse();
        panel_box();
    }
    gui()->window_end();
}

static void
build_frame( void )
{
    window_controls();
    window_stage();
    window_pulse_meter();
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
        fprintf( stderr, "[sb_gui_brush] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui brush",
        .w         = 1280, .h = 940,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.12f, 0.12f, 0.15f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_brush] gui->boot failed\n" );
        goto shutdown;
    }

    /* After boot: the sprite atlas creates itself on the first registration, and that needs the
       live rhi context boot just stood up. */
    build_art();

    /* Register the kit's style source once, for good: it owns the motion rates from the first
       frame, and the art skin is a branch INSIDE it rather than a second source swapped in. */
    gui()->style_source_set( sb_style_source, NULL );


    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
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
