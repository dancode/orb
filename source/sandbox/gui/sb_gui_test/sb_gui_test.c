/*==============================================================================================

    sandbox/gui/sb_gui_test/sb_gui_test.c -- HEADLESS assertion tests for the gui library.

    The other twelve gui sandboxes are visual demos: they prove a thing can be drawn, and a
    human decides whether it looked right.  That covers the half of the library a person can
    see, and none of the half a person cannot -- a bitfield that overlaps its neighbour by one
    position, a saturating quantizer that wraps instead, a half-float that rounds the wrong way
    on a tie.  Those are the failures no screenshot shows and no demo catches.

    So this target opens NO window and needs NO device.  Everything under test is PURE -- the
    rect and log leaves, the font resource (no atlas, no GPU by charter), and the interact
    measurement helpers, which are math over the font tables.  Pulling the interact object does
    drag the wider lib in at LINK time (its unit neighbours reference the app/rhi vtables), so
    the target links the stack's libs -- but nothing here ever boots or calls them, which is
    what keeps it runnable in a build step rather than by hand.

    Scope is deliberately the PURE surface.  Anything needing a live gui context (the style
    bake, dock serialization, the id scope stack, table sort) is out of reach until it can be
    stood up headlessly, and is left for a later pass rather than faked here.

    Exit code: 0 = all cases passed, non-zero = the failure count.

    Constituents, in include order:
        test_pack.c  -- the vertex: UV packing and the constructor; the primitive record layout
        test_rect.c  -- the GUI_RECT leaf kit: rectcut, containment, alignment, colour blend
        test_log.c   -- the GUI_LOG sink contract + the GUI_WARN_ONCE latch
        test_font.c  -- the two-tier glyph lookup: ASCII dense tier, ext binary search, '?' miss
        test_edit.c  -- UTF-8 caret math: boundary-only carets, midpoint snapping, word classes
        test_msel.c  -- multi-select protocol: the click/modifier rule + the range application

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

#include "sandbox/gui/sb_gui_test/test_pack.c"
#include "sandbox/gui/sb_gui_test/test_rect.c"
#include "sandbox/gui/sb_gui_test/test_log.c"
#include "sandbox/gui/sb_gui_test/test_font.c"
#include "sandbox/gui/sb_gui_test/test_edit.c"
#include "sandbox/gui/sb_gui_test/test_msel.c"

/*============================================================================================*/

int
main( int argc, char* argv[] )
{
    UNUSED( argc );
    UNUSED( argv );

    /* Vertex packing + the primitive record */
    test_register( "uv_pack",             test_uv_pack );
    test_register( "prim_layout",         test_prim_layout );
    test_register( "quad_layout",         test_quad_layout );
    test_register( "prim_ops",            test_prim_ops );

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

    /* GUI_FONT -- the two-tier glyph lookup + the resolver's name utilities */
    test_register( "font_cp_ascii",       test_font_cp_ascii );
    test_register( "font_cp_ext_search",  test_font_cp_ext_search );
    test_register( "font_cp_ext_empty",   test_font_cp_ext_empty );
    test_register( "font_measure_utf8",   test_font_measure_utf8 );
    test_register( "font_name_normalize", test_font_name_normalize );
    test_register( "font_ship_name_parse", test_font_ship_name_parse );

    /* Edit seams -- UTF-8 caret math + word classes */
    test_register( "edit_caret_utf8",     test_edit_caret_utf8 );
    test_register( "edit_word_utf8",      test_edit_word_utf8 );

    /* Multi-select protocol -- click rule + range application */
    test_register( "msel_click_rule",     test_msel_click_rule );
    test_register( "msel_apply_ops",      test_msel_apply_ops );
    test_register( "msel_apply_clamp",    test_msel_apply_clamp );
    test_register( "msel_scenario",       test_msel_scenario );

    return test_run( "sb_gui" );
}

/*============================================================================================*/
