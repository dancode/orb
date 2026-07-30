/*==============================================================================================

    sandbox/gui/sb_gui_test/test_rect.c -- the GUI_RECT leaf kit.

    The rect kit is pure by charter -- no ambient state, no draw, no context -- which is exactly
    what makes it testable headlessly, and exactly why it never was.  Every layout track, every
    canvas carve, and every aligned label resolves through these, so an off-by-one here is a
    whole-UI shift that reads as "the layout looks wrong" rather than as a failing function.

    The properties worth pinning are the ones the header PROMISES: rectcut partitions with no
    gap and no overlap, containment is half-open so abutting rects tile the plane exactly, and
    alignment resolves through one shared rule for boxes and single axes alike.

==============================================================================================*/
// clang-format off

static bool
rect_eq( gui_rect_t a, gui_rect_t b )
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/*==============================================================================================
    rectcut -- slice a strip off one edge; *r becomes the remainder.
==============================================================================================*/

static void
test_rect_cut( void )
{
    /* Each cut takes its strip from the named edge and leaves the rest. */
    gui_rect_t r = { 10.0f, 20.0f, 100.0f, 50.0f };
    gui_rect_t c = gui_rect_cut_left( &r, 30.0f );
    test_true( rect_eq( c, ( gui_rect_t ){ 10.0f, 20.0f, 30.0f, 50.0f } ) );
    test_true( rect_eq( r, ( gui_rect_t ){ 40.0f, 20.0f, 70.0f, 50.0f } ) );

    r = ( gui_rect_t ){ 10.0f, 20.0f, 100.0f, 50.0f };
    c = gui_rect_cut_right( &r, 30.0f );
    test_true( rect_eq( c, ( gui_rect_t ){ 80.0f, 20.0f, 30.0f, 50.0f } ) );
    test_true( rect_eq( r, ( gui_rect_t ){ 10.0f, 20.0f, 70.0f, 50.0f } ) );

    r = ( gui_rect_t ){ 10.0f, 20.0f, 100.0f, 50.0f };
    c = gui_rect_cut_top( &r, 20.0f );
    test_true( rect_eq( c, ( gui_rect_t ){ 10.0f, 20.0f, 100.0f, 20.0f } ) );
    test_true( rect_eq( r, ( gui_rect_t ){ 10.0f, 40.0f, 100.0f, 30.0f } ) );

    r = ( gui_rect_t ){ 10.0f, 20.0f, 100.0f, 50.0f };
    c = gui_rect_cut_bottom( &r, 20.0f );
    test_true( rect_eq( c, ( gui_rect_t ){ 10.0f, 50.0f, 100.0f, 20.0f } ) );
    test_true( rect_eq( r, ( gui_rect_t ){ 10.0f, 20.0f, 100.0f, 30.0f } ) );
}

static void
test_rect_cut_partition( void )
{
    /* THE rectcut property: the slice and the remainder tile the original exactly -- no gap,
       no overlap, on either axis.  A layout built on a cut that leaked a pixel drifts further
       the more tracks it carves, which is the hard version of this bug to spot by eye. */
    gui_rect_t orig = { 7.0f, 13.0f, 101.0f, 57.0f };

    gui_rect_t r = orig;
    gui_rect_t c = gui_rect_cut_left( &r, 33.0f );
    test_true( c.x == orig.x );
    test_true( c.x + c.w == r.x );                  /* abut: no gap, no overlap */
    test_true( r.x + r.w == orig.x + orig.w );
    test_true( c.w + r.w == orig.w );

    r = orig;
    c = gui_rect_cut_top( &r, 19.0f );
    test_true( c.y == orig.y );
    test_true( c.y + c.h == r.y );
    test_true( r.y + r.h == orig.y + orig.h );
    test_true( c.h + r.h == orig.h );
}

static void
test_rect_cut_clamp( void )
{
    /* Over-cutting takes everything and leaves a zero-extent remainder -- never a negative one.
       A negative width propagates into a scissor rect, and a negative scissor is a validation
       error rather than a small visual glitch. */
    gui_rect_t r = { 0.0f, 0.0f, 40.0f, 20.0f };
    gui_rect_t c = gui_rect_cut_left( &r, 999.0f );
    test_true( c.w == 40.0f );
    test_true( r.w == 0.0f );

    r = ( gui_rect_t ){ 0.0f, 0.0f, 40.0f, 20.0f };
    c = gui_rect_cut_bottom( &r, 999.0f );
    test_true( c.h == 20.0f );
    test_true( r.h == 0.0f );
    test_true( r.h >= 0.0f && c.h >= 0.0f );
}

/*==============================================================================================
    Containment -- left/top inclusive, right/bottom EXCLUSIVE.
==============================================================================================*/

static void
test_rect_contains( void )
{
    gui_rect_t r = { 10.0f, 10.0f, 20.0f, 20.0f };

    test_true ( gui_rect_contains( r, 10.0f, 10.0f ) );      /* top-left inclusive     */
    test_true ( gui_rect_contains( r, 29.9f, 29.9f ) );
    test_false( gui_rect_contains( r, 30.0f, 20.0f ) );      /* right edge exclusive   */
    test_false( gui_rect_contains( r, 20.0f, 30.0f ) );      /* bottom edge exclusive  */
    test_false( gui_rect_contains( r,  9.9f, 20.0f ) );
    test_false( gui_rect_contains( r, 20.0f,  9.9f ) );

    /* The point of the half-open rule: two abutting rects claim every point exactly once, so a
       click on the seam between adjacent panels hits one of them and never both. */
    gui_rect_t a = {  0.0f, 0.0f, 50.0f, 10.0f };
    gui_rect_t b = { 50.0f, 0.0f, 50.0f, 10.0f };
    test_true ( gui_rect_contains( a, 49.99f, 5.0f ) );
    test_false( gui_rect_contains( b, 49.99f, 5.0f ) );
    test_false( gui_rect_contains( a, 50.0f,  5.0f ) );
    test_true ( gui_rect_contains( b, 50.0f,  5.0f ) );

    /* A zero-size rect contains nothing -- not even its own origin. */
    test_false( gui_rect_contains( ( gui_rect_t ){ 5.0f, 5.0f, 0.0f, 0.0f }, 5.0f, 5.0f ) );
}

/*==============================================================================================
    Intersection, inset, centre.
==============================================================================================*/

static void
test_rect_intersect( void )
{
    gui_rect_t a = {  0.0f,  0.0f, 100.0f, 100.0f };
    gui_rect_t b = { 50.0f, 50.0f, 100.0f, 100.0f };
    test_true( rect_eq( rect_intersect( a, b ), ( gui_rect_t ){ 50.0f, 50.0f, 50.0f, 50.0f } ) );

    /* Disjoint yields ZERO size, not a negative one -- clip nesting depends on it. */
    gui_rect_t far = { 500.0f, 500.0f, 10.0f, 10.0f };
    gui_rect_t none = rect_intersect( a, far );
    test_true( none.w == 0.0f && none.h == 0.0f );

    /* Touching but not overlapping is also empty (half-open again). */
    gui_rect_t touch = rect_intersect( a, ( gui_rect_t ){ 100.0f, 0.0f, 10.0f, 10.0f } );
    test_true( touch.w == 0.0f );

    /* Containment: the intersection is the inner rect, and the operation is commutative. */
    gui_rect_t inner = { 25.0f, 25.0f, 10.0f, 10.0f };
    test_true( rect_eq( rect_intersect( a, inner ), inner ) );
    test_true( rect_eq( rect_intersect( inner, a ), inner ) );

    /* Self-intersection is identity, and intersecting is idempotent. */
    test_true( rect_eq( rect_intersect( a, a ), a ) );
    test_true( rect_eq( rect_intersect( rect_intersect( a, b ), b ), rect_intersect( a, b ) ) );
}

static void
test_rect_inset_center( void )
{
    gui_rect_t r = { 10.0f, 20.0f, 100.0f, 60.0f };

    gui_pad_t  p = { 1.0f, 2.0f, 3.0f, 4.0f };      /* l, r, t, b */
    test_true( rect_eq( gui_rect_inset( r, p ), ( gui_rect_t ){ 11.0f, 23.0f, 97.0f, 53.0f } ) );

    test_true( rect_eq( gui_rect_pad( r, 5.0f ), ( gui_rect_t ){ 15.0f, 25.0f, 90.0f, 50.0f } ) );

    gui_vec2_t c = gui_rect_center( r );
    test_true( c.x == 60.0f && c.y == 50.0f );

    /* A uniform pad is the symmetric case of the per-edge inset. */
    gui_pad_t u = { 5.0f, 5.0f, 5.0f, 5.0f };
    test_true( rect_eq( gui_rect_pad( r, 5.0f ), gui_rect_inset( r, u ) ) );
}

/*==============================================================================================
    Alignment -- the inline box form, the compiled alias, and the two scalar axes.
==============================================================================================*/

static void
test_rect_align( void )
{
    gui_rect_t area = { 0.0f, 0.0f, 100.0f, 50.0f };
    const f32  nw = 20.0f, nh = 10.0f;

    /* 0 is LEFT | TOP and hugs the corner. */
    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_LEFT ),
                        ( gui_rect_t ){ 0.0f, 0.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_RIGHT ),
                        ( gui_rect_t ){ 80.0f, 0.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_HCENTER ),
                        ( gui_rect_t ){ 40.0f, 0.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_BOTTOM ),
                        ( gui_rect_t ){ 0.0f, 40.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_VCENTER ),
                        ( gui_rect_t ){ 0.0f, 20.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_CENTER ),
                        ( gui_rect_t ){ 40.0f, 20.0f, nw, nh } ) );

    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_RIGHT | GUI_ALIGN_BOTTOM ),
                        ( gui_rect_t ){ 80.0f, 40.0f, nw, nh } ) );

    /* The two axes are independent: they read disjoint bits, so setting one never moves the
       other.  (HCENTER is 1<<0, VCENTER is 1<<2 -- a shared bit would couple them.) */
    test_true( rect_eq( gui_rect_align( area, nw, nh, GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM ),
                        ( gui_rect_t ){ 40.0f, 40.0f, nw, nh } ) );

    /* Alignment places, it never resizes. */
    gui_rect_t placed = gui_rect_align( area, nw, nh, GUI_ALIGN_CENTER );
    test_true( placed.w == nw && placed.h == nh );

    /* Content larger than its cell overflows symmetrically when centred rather than clamping --
       widgets self-fit; the placement rule does not silently shrink them. */
    gui_rect_t big = gui_rect_align( area, 200.0f, 10.0f, GUI_ALIGN_HCENTER );
    test_true( big.x == -50.0f && big.w == 200.0f );
}

static void
test_rect_align_compiled( void )
{
    /* rect_align is the compiled alias for gui_rect_align -- the header calls it a thin alias
       so widgets and callers share ONE rule.  Assert they actually agree, on every combination:
       a drift between them would align a button's label differently from a canvas caller's. */
    gui_rect_t area = { 3.0f, 7.0f, 101.0f, 41.0f };
    static const u32 aligns[] = {
        GUI_ALIGN_LEFT,
        GUI_ALIGN_HCENTER,
        GUI_ALIGN_RIGHT,
        GUI_ALIGN_VCENTER,
        GUI_ALIGN_BOTTOM,
        GUI_ALIGN_CENTER,
        GUI_ALIGN_RIGHT   | GUI_ALIGN_BOTTOM,
        GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM,
        GUI_ALIGN_RIGHT   | GUI_ALIGN_VCENTER,
    };

    for ( u32 i = 0; i < ARRAY_COUNT( aligns ); ++i )
    {
        gui_rect_t inl = gui_rect_align( area, 20.0f, 10.0f, ( gui_align_t )aligns[ i ] );
        gui_rect_t cmp = rect_align    ( area, 20.0f, 10.0f, aligns[ i ] );
        test_true( rect_eq( inl, cmp ) );

        /* And the scalar axes are the same rule again, one axis at a time. */
        test_true( align_x( area.x, area.w, 20.0f, aligns[ i ] ) == inl.x );
        test_true( align_y( area.y, area.h, 10.0f, aligns[ i ] ) == inl.y );
    }
}

static void
test_anchor_box( void )
{
    /* The HUD idiom: pin a fixed box to a corner, inset from the edge by a margin. */
    gui_rect_t area = { 0.0f, 0.0f, 200.0f, 100.0f };
    gui_pad_t  m    = { 8.0f, 8.0f, 8.0f, 8.0f };

    test_true( rect_eq( gui_anchor_box( area, 40.0f, 20.0f, GUI_ALIGN_LEFT, m ),
                        ( gui_rect_t ){ 8.0f, 8.0f, 40.0f, 20.0f } ) );

    test_true( rect_eq( gui_anchor_box( area, 40.0f, 20.0f, GUI_ALIGN_RIGHT | GUI_ALIGN_BOTTOM, m ),
                        ( gui_rect_t ){ 152.0f, 72.0f, 40.0f, 20.0f } ) );

    /* It is exactly align-over-inset, which is what makes it compose with the other helpers. */
    test_true( rect_eq( gui_anchor_box( area, 40.0f, 20.0f, GUI_ALIGN_CENTER, m ),
                        gui_rect_align( gui_rect_inset( area, m ), 40.0f, 20.0f, GUI_ALIGN_CENTER ) ) );
}

/*==============================================================================================
    Scalar helpers + colour blend (the compiled half).
==============================================================================================*/

static void
test_saturate_clamp( void )
{
    test_true( saturate( -1.0f ) == 0.0f );
    test_true( saturate(  0.5f ) == 0.5f );
    test_true( saturate(  2.0f ) == 1.0f );

    test_true( clampf( 5.0f, 0.0f, 10.0f ) ==  5.0f );
    test_true( clampf( -5.0f, 0.0f, 10.0f ) == 0.0f );
    test_true( clampf( 50.0f, 0.0f, 10.0f ) == 10.0f );
}

static void
test_col_lerp( void )
{
    const u32 black = 0xFF000000u;   /* a=FF, rgb=0 */
    const u32 white = 0xFFFFFFFFu;

    /* Endpoints are returned exactly -- an animated blend that never quite reaches its target
       colour is the classic symptom of an endpoint that only approximates. */
    test_equal( black, col_lerp( black, white, 0.0f ) );
    test_equal( white, col_lerp( black, white, 1.0f ) );

    /* Out-of-range t clamps to the endpoints rather than extrapolating. */
    test_equal( black, col_lerp( black, white, -1.0f ) );
    test_equal( white, col_lerp( black, white,  2.0f ) );

    /* Midpoint blends every channel independently. */
    test_equal( 0xFF7F7F7Fu, col_lerp( black, white, 0.5f ) );

    /* Channel isolation: blend toward ONE channel and only that channel may move.  A shifted
       channel here tints every hover animation in the UI a colour nobody authored, and the
       blend still looks like a plausible animation, so nothing reads as broken. */
    test_equal( 0x0000007Fu, col_lerp( 0x00000000u, 0x000000FFu, 0.5f ) );   /* R */
    test_equal( 0x00007F00u, col_lerp( 0x00000000u, 0x0000FF00u, 0.5f ) );   /* G */
    test_equal( 0x007F0000u, col_lerp( 0x00000000u, 0x00FF0000u, 0.5f ) );   /* B */
    test_equal( 0x7F000000u, col_lerp( 0x00000000u, 0xFF000000u, 0.5f ) );   /* A */

    /* Blending a colour with itself is identity at every t. */
    test_equal( 0xFF204060u, col_lerp( 0xFF204060u, 0xFF204060u, 0.5f ) );
    test_equal( 0xFF204060u, col_lerp( 0xFF204060u, 0xFF204060u, 0.25f ) );

    /* Alpha participates like the rest. */
    test_equal( 0x7Fu, col_lerp( 0x00000000u, 0xFF000000u, 0.5f ) >> 24 );
}

/*============================================================================================*/
