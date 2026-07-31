/*==============================================================================================

    sandbox/gui/sb_gui_test/sb_gui_test.c -- HEADLESS assertion tests for the gui library.

    The other twelve gui sandboxes are visual demos: they prove a thing can be drawn, and a
    human decides whether it looked right.  That covers the half of the library a person can
    see, and none of the half a person cannot -- a bitfield that overlaps its neighbour by one
    position, a saturating quantizer that wraps instead, a half-float that rounds the wrong way
    on a tie.  Those are the failures no screenshot shows and no demo catches.

    So this target opens NO window and needs NO device.  It links three objects out of
    gui.lib -- the GUI_RECT leaf, the GUI_LOG leaf, and the GUI_FONT resource (no atlas, no GPU
    by charter) -- plus the packing helpers, which are static inline in gui.h and cost nothing
    to link at all.  That is what makes it runnable in a build step rather than by hand.

    Scope is deliberately the PURE surface.  Anything needing a live gui context (the style
    bake, dock serialization, the id scope stack, table sort) is out of reach until it can be
    stood up headlessly, and is left for a later pass rather than faked here.

    Exit code: 0 = all cases passed, non-zero = the failure count.

    Constituents, in include order:
        test_fx.c    -- the packed effect word: 11 modes over one u32, saturation, field isolation
        test_pack.c  -- vertex packing: binary16, UV, effect coord, the tex mode/index split
        test_rect.c  -- the GUI_RECT leaf kit: rectcut, containment, alignment, colour blend
        test_log.c   -- the GUI_LOG sink contract + the GUI_WARN_ONCE latch
        test_font.c  -- the two-tier glyph lookup: ASCII dense tier, ext binary search, '?' miss

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "base/test.h"

#include "runtime_service/gui/gui.h"

/*==============================================================================================
    Plain C-string assertions.

    base/test.h's test_str_equal speaks base's str_t slice, and everything under test here
    produces NUL-terminated char buffers (gui_log hands its sink a C string).  Converting at
    every call site would be noise, so the comparison is spelled once, here -- and it prints
    BOTH sides on failure, which is the whole reason not to settle for a bare test_true.
==============================================================================================*/

static u32
cstr_len( const char* s )
{
    u32 n = 0;
    while ( s[ n ] ) ++n;
    return n;
}

static bool
cstr_equal( const char* a, const char* b )
{
    u32 i = 0;
    for ( ; a[ i ] && a[ i ] == b[ i ]; ++i ) { }
    return a[ i ] == b[ i ];
}

#define test_cstr_equal( expected, actual )                                   \
    _TEST_ASSERT( cstr_equal( ( expected ), ( actual ) ),                     \
                  "cstr_equal: \"%s\" != \"%s\"\n", ( expected ), ( actual ) )

/*==============================================================================================
    Unity build -- each constituent is a set of test_* case functions, registered below.
==============================================================================================*/

#include "sandbox/gui/sb_gui_test/test_fx.c"
#include "sandbox/gui/sb_gui_test/test_pack.c"
#include "sandbox/gui/sb_gui_test/test_rect.c"
#include "sandbox/gui/sb_gui_test/test_log.c"
#include "sandbox/gui/sb_gui_test/test_font.c"

/*============================================================================================*/

int
main( int argc, char* argv[] )
{
    UNUSED( argc );
    UNUSED( argv );

    /* GUI_FX -- the packed effect word */
    test_register( "fx_fixed",            test_fx_fixed );
    test_register( "fx_pack_box",         test_fx_pack_box );
    test_register( "fx_no_wrap_regress",  test_fx_no_wrap_regression );
    test_register( "fx_pack_pulse",       test_fx_pack_pulse );
    test_register( "fx_pack_tile_u",      test_fx_pack_tile_u );
    test_register( "fx_pack_arc",         test_fx_pack_arc );
    test_register( "fx_pack_text_edge",   test_fx_pack_text_edge );
    test_register( "fx_mode_nibble",      test_fx_mode_nibble );

    /* Vertex packing */
    test_register( "f16_exact",           test_f16_exact );
    test_register( "f16_saturation",      test_f16_saturation );
    test_register( "f16_round_half_up",   test_f16_round_half_up );
    test_register( "f16_round_trip",      test_f16_round_trip );
    test_register( "uv_pack",             test_uv_pack );
    test_register( "fxc_pack",            test_fxc_pack );
    test_register( "tex_word",            test_tex_word );
    test_register( "vert_ctors",          test_vert_ctors );

    /* GUI_RECT -- the leaf kit */
    test_register( "rect_cut",            test_rect_cut );
    test_register( "rect_cut_partition",  test_rect_cut_partition );
    test_register( "rect_cut_clamp",      test_rect_cut_clamp );
    test_register( "rect_contains",       test_rect_contains );
    test_register( "rect_intersect",      test_rect_intersect );
    test_register( "rect_inset_center",   test_rect_inset_center );
    test_register( "rect_align",          test_rect_align );
    test_register( "rect_align_compiled", test_rect_align_compiled );
    test_register( "anchor_box",          test_anchor_box );
    test_register( "saturate_clamp",      test_saturate_clamp );
    test_register( "col_lerp",            test_col_lerp );

    /* GUI_LOG -- the diagnostics floor */
    test_register( "log_delivery",        test_log_delivery );
    test_register( "log_user_pointer",    test_log_user_pointer );
    test_register( "log_framing",         test_log_framing );
    test_register( "log_truncation",      test_log_truncation );
    test_register( "log_set_fn",          test_log_set_fn );
    test_register( "warn_once",           test_warn_once );

    /* GUI_FONT -- the two-tier glyph lookup */
    test_register( "font_cp_ascii",       test_font_cp_ascii );
    test_register( "font_cp_ext_search",  test_font_cp_ext_search );
    test_register( "font_cp_ext_empty",   test_font_cp_ext_empty );

    return test_run( "sb_gui" );
}

/*============================================================================================*/
