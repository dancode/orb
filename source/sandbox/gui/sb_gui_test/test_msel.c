/*==============================================================================================

    sandbox/gui/sb_gui_test/test_msel.c -- the multi-select protocol's pure surface.

    The click/modifier rule (msel_click_op, interact seam) and the dense-array application
    (gui_msel_apply, public) are both pure -- index math with no context -- so the whole
    selection CONTRACT pins down headlessly: which modifier produces which op, that ranges
    normalize regardless of click direction, that shift with no anchor degenerates to the
    row itself, and that apply clamps foreign ranges instead of walking off the array.
    The scope bracket + keyed anchor (begin/feed/end) need a live context and stay with the
    visual demos.

==============================================================================================*/
// clang-format off

#include "runtime_service/gui/gui_host.h"   /* gui_msel_apply (msel_click_op rides
                                               gui_interact.h, included by test_edit.c) */

/*==============================================================================================
    Cases
==============================================================================================*/

static void
test_msel_click_rule( void )
{
    gui_msel_t a;

    /* Plain click: replace with the row. */
    a = msel_click_op( 2, 7, false, false );
    test_true( a.op == GUI_MSEL_SET && a.lo == 7 && a.hi == 7 );

    /* Ctrl+click: toggle the row alone. */
    a = msel_click_op( 2, 7, true, false );
    test_true( a.op == GUI_MSEL_TOGGLE && a.lo == 7 && a.hi == 7 );

    /* Shift+click: replace with anchor..row -- normalized either direction. */
    a = msel_click_op( 2, 7, false, true );
    test_true( a.op == GUI_MSEL_SET && a.lo == 2 && a.hi == 7 );
    a = msel_click_op( 7, 2, false, true );
    test_true( a.op == GUI_MSEL_SET && a.lo == 2 && a.hi == 7 );

    /* Ctrl+Shift+click: the same range, ADDED to what is there. */
    a = msel_click_op( 2, 7, true, true );
    test_true( a.op == GUI_MSEL_ADD && a.lo == 2 && a.hi == 7 );

    /* Shift with no anchor yet (-1): the range is the row itself. */
    a = msel_click_op( -1, 4, false, true );
    test_true( a.op == GUI_MSEL_SET && a.lo == 4 && a.hi == 4 );

    /* A degenerate range (click on the anchor) is one row, not zero. */
    a = msel_click_op( 3, 3, false, true );
    test_true( a.op == GUI_MSEL_SET && a.lo == 3 && a.hi == 3 );
}

static void
test_msel_apply_ops( void )
{
    bool sel[ 8 ];

    /* SET clears everything outside the range. */
    for ( u32 i = 0; i < 8; ++i ) sel[ i ] = true;
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_SET, 2, 4 }, sel, 8 );
    for ( u32 i = 0; i < 8; ++i )
        test_true( sel[ i ] == ( i >= 2 && i <= 4 ) );

    /* ADD keeps what is there. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_ADD, 6, 7 }, sel, 8 );
    for ( u32 i = 0; i < 8; ++i )
        test_true( sel[ i ] == ( ( i >= 2 && i <= 4 ) || i >= 6 ) );

    /* TOGGLE inverts only the range. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_TOGGLE, 3, 6 }, sel, 8 );
    test_true(  sel[ 2 ] );                     /* untouched below      */
    test_true( !sel[ 3 ] && !sel[ 4 ] );        /* was on, now off      */
    test_true(  sel[ 5 ] );                     /* was off, now on      */
    test_true( !sel[ 6 ] );                     /* was on, now off      */
    test_true(  sel[ 7 ] );                     /* untouched above      */

    /* ALL / CLEAR ignore the range fields. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_ALL, 0, 0 }, sel, 8 );
    for ( u32 i = 0; i < 8; ++i ) test_true( sel[ i ] );
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_CLEAR, 0, 0 }, sel, 8 );
    for ( u32 i = 0; i < 8; ++i ) test_true( !sel[ i ] );

    /* NONE is a no-op. */
    sel[ 0 ] = true;
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_NONE, 0, 7 }, sel, 8 );
    test_true( sel[ 0 ] && !sel[ 1 ] );
}

static void
test_msel_apply_clamp( void )
{
    bool sel[ 4 ] = { false, false, false, false };

    /* A range hanging off both ends clamps to the array. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_ADD, -3, 9 }, sel, 4 );
    for ( u32 i = 0; i < 4; ++i ) test_true( sel[ i ] );

    /* A SET fully past the end intersects to the EMPTY range: it still clears (that is the
       op), but selects nothing -- no phantom row appears from a stale count. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_SET, 9, 9 }, sel, 4 );
    test_true( !sel[ 0 ] && !sel[ 1 ] && !sel[ 2 ] && !sel[ 3 ] );

    /* NULL array / empty list are safe no-ops. */
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_ALL, 0, 3 }, NULL, 4 );
    gui_msel_apply( ( gui_msel_t ){ GUI_MSEL_ALL, 0, 3 }, sel, 0 );
}

static void
test_msel_scenario( void )
{
    /* An Explorer session over 10 rows, anchor tracked by the documented rule (plain and ctrl
       clicks move it, shift never does).  Chains the two pure halves the way the engine does. */
    bool sel[ 10 ] = { 0 };
    i32  anchor    = -1;

    /* Click row 2. */
    gui_msel_apply( msel_click_op( anchor, 2, false, false ), sel, 10 );
    anchor = 2;
    test_true( sel[ 2 ] && !sel[ 1 ] && !sel[ 3 ] );

    /* Shift+click row 6: 2..6. */
    gui_msel_apply( msel_click_op( anchor, 6, false, true ), sel, 10 );
    for ( u32 i = 0; i < 10; ++i ) test_true( sel[ i ] == ( i >= 2 && i <= 6 ) );

    /* Ctrl+click row 4 off; anchor follows. */
    gui_msel_apply( msel_click_op( anchor, 4, true, false ), sel, 10 );
    anchor = 4;
    test_true( !sel[ 4 ] && sel[ 3 ] && sel[ 5 ] );

    /* Ctrl+Shift+click row 8: add 4..8 over what is there. */
    gui_msel_apply( msel_click_op( anchor, 8, true, true ), sel, 10 );
    for ( u32 i = 0; i < 10; ++i ) test_true( sel[ i ] == ( i >= 2 && i <= 8 ) );

    /* Plain click row 0 collapses it all back to one row. */
    gui_msel_apply( msel_click_op( anchor, 0, false, false ), sel, 10 );
    for ( u32 i = 0; i < 10; ++i ) test_true( sel[ i ] == ( i == 0 ) );
}

// clang-format on
/*============================================================================================*/
