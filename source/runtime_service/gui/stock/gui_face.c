/*==============================================================================================

    runtime_service/gui/stock/gui_face.c -- the FACE painters: fill a rect for a style cell.

    The paint half of the face plane.  Style resolves a cell to a brush (style_face, pure, never
    paints); these consume one and put pixels down -- the same division the colour grid already
    has, where style_col resolves and the render fills.

    ONE rule, applied by every painter here:

        a cell with a FACE  -> paint the brush, and DRAW NO BORDER
        a cell without one  -> fill with the cell's colour, and stroke the border as before

    The border suppression is the part worth stating twice.  Authored art carries its own edge --
    that is most of what a nine-slice frame IS -- so stroking a 1px theme border over it would
    draw a line through somebody's artwork every time.  A caller therefore hands its border colour
    and width to the painter rather than drawing them itself, and the painter decides; that is the
    only reason these take border arguments at all.

    EVERY painter here takes an id, and that is the second thing this file is for: the id buys the
    item a MIX (style_mix), so its surface travels between cells instead of snapping between them.
    Motion is therefore the default and stillness the opt-out -- pass GUI_ID_NONE and no damper
    slot is touched at all, which is what a decorative fill and a replaying volatile block want.
    A widget that paints more than one row should read style_mix ITSELF, once, and hand the same
    mix to each painter, so its surface / border / ink move together off one probe.

    Each painter MIRRORS one colour projection, and the mirroring is the point: a site converting
    from colour to face changes one call and nothing else, because the coordinates it was already
    passing are the coordinates the face plane is indexed by.

        style_col        ( role, phase )        ->  draw_face      ( r, role, phase )
        col_item_bg_mix  ( id, st, sel )        ->  draw_face_item ( r, id, st, sel )
        col_grab_mix     ( id, st )             ->  draw_face_grab ( r, id, st, ... )
        col_frame_bg_mix ( mix, idle )          ->  draw_face_field( r, id, st, idle_cell, ... )

    A selected item's brush is not a second stored cell: face_paint washes whichever brush the
    span already resolved to (brush_selected), the same live wash style_col_mix spends on a flat
    colour -- see GUI_STYLE -- SELECTED in gui.h.

    Included by gui_stock.c before the widget renders, which paint through it.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The one primitive -- every painter below is this with its coordinates worked out.

    A cell here is (rest_role, rest_phase) plus a `role` whose HOT and ACTIVE cells the item
    lifts to.  The two collapse for the common widget -- rest IS (role, IDLE) -- and separate
    only for a framed FIELD, which rests somewhere of its caller's choosing (a slider groove on
    ACCENT/DIM, an input box on BG/IDLE) and lifts to the shared BG cells like every other
    framed control.  That is col_frame_bg's shape, expressed as cells so the rest state can
    carry a FACE too: a groove and an input box are exactly what a skin most wants to author.
==============================================================================================*/

/* Collapse the continuous mix onto the two cells the item lies BETWEEN, and how far it has come.

   Art does not interpolate.  Where the colour spender blends the whole row at once, a face can
   only ever be a crossfade of two pieces of art -- so this picks the two, which is all the eye
   reads anyway.

   Travel along the phase axis is ONE monotone position, IDLE -> HOT -> ACTIVE, because that is
   the interaction path itself: you hover before you press, and release before you leave.  Adding
   the two weights therefore gives a single 0..2 coordinate whose halves are the two spans.

   Selection is not a second span here: it is a wash face_paint applies to whichever brush this
   picks, by m.sel -- see brush_selected below. */

static void
face_span( u8 role, u8 rest_role, u8 rest_phase, gui_style_mix_t m,
           u8* a_role, u8* a_phase, u8* b_role, u8* b_phase, f32* w )
{
    f32 pos = m.hot + m.act;              /* 0 = at rest, 1 = fully hot, 2 = fully active */

    if ( pos <= 1.0f )
    {
        *a_role = rest_role;  *a_phase = rest_phase;
        *b_role = role;       *b_phase = GUI_PHASE_HOT;
        *w      = pos;
    }
    else
    {
        *a_role = role;       *a_phase = GUI_PHASE_HOT;
        *b_role = role;       *b_phase = GUI_PHASE_ACTIVE;
        *w      = pos - 1.0f;
    }
}

/* Wash a resolved brush's tint by travel, the brush-plane twin of style_wash_selected.  col_a
   means different things per kind (gui.h): for SOLID/GRADIENT it IS the fill colour, so 0 is a
   legitimate transparent black and washes as-is; for SPRITE/NINE it is a TINT where 0 means
   "untinted" (white), which has to be substituted before washing or a selected sprite would wash
   from black instead of from its own colours.  GRADIENT washes both ends so a selected gradient
   shifts as a whole rather than pinching toward the accent at one end. */
static gui_brush_t
brush_selected( const gui_brush_t* b, f32 travel )
{
    if ( travel <= 0.0f ) return *b;

    gui_brush_t out = *b;
    bool tint_kind = ( out.kind == GUI_BRUSH_SPRITE || out.kind == GUI_BRUSH_NINE );
    u32  base      = ( tint_kind && out.col_a == 0u ) ? GUI_COLOR( 255, 255, 255, 255 ) : out.col_a;

    out.col_a = style_wash_selected( base, travel );
    if ( out.kind == GUI_BRUSH_GRADIENT )
        out.col_b = style_wash_selected( out.col_b, travel );

    return out;
}

/* Fill r for an item at mix `m`: its cells' faces if the theme authored any, else the blended
   colour with an optional border.  border_w <= 0 means no border either way. */
static void
face_paint( gui_rect_t r, u8 role, u8 rest_role, u8 rest_phase,
           gui_style_mix_t m, u32 border_col, f32 border_w )
{
    u8  ar, ap, br, bp;
    f32 w;
    face_span( role, rest_role, rest_phase, m, &ar, &ap, &br, &bp, &w );

    const gui_brush_t* a = style_face( ar, ap );
    const gui_brush_t* b = ( w > 0.0f ) ? style_face( br, bp ) : NULL;

    if ( a || b )
    {
        gui_brush_t a_washed, b_washed;
        if ( a ) { a_washed = brush_selected( a, m.sel ); a = &a_washed; }
        if ( b ) { b_washed = brush_selected( b, m.sel ); b = &b_washed; }

        /* The near end goes down opaque -- its brush, or its flat colour when only the far end
           carries art.  That colour base is what lets a HALF-skinned theme (a hover face over a
           plain resting cell, which is a completely reasonable thing to author) cross-fade into
           its art instead of popping into it. */
        if ( a ) draw_fill_brush( r, a );
        else     draw_fill( r, style_wash_selected( style_col( ar, ap ), m.sel ) );

        if ( w > 0.0f )
        {
            /* Multiply the AMBIENT alpha rather than setting it: the disabled-item span has
               already lowered it, and clobbering that here would make a disabled widget flash
               back to full opacity for the length of every transition. */
            f32 amb = draw_get_alpha();
            draw_set_alpha( amb * w );
            if ( b ) draw_fill_brush( r, b );
            else     draw_fill( r, style_wash_selected( style_col( br, bp ), m.sel ) );
            draw_set_alpha( amb );
        }
        return;                            /* art carries its own edge -- no border over the top */
    }

    /* No art anywhere on this span: the plain color path, and still exactly one quad.  
       The spender is the EXACT one (style_col_mix / col_frame_bg_mix), not the two-cell span above --
       colour has no reason to approximate, and a face-painted widget must read identically to a
       colour-painted one that never asked about art. */

    u32 col = ( rest_role == role && rest_phase == GUI_PHASE_IDLE )
            ? style_col_mix( role, m )                                        /* the item shape */
            : col_frame_bg_mix( m, style_col( rest_role, rest_phase ) );

    if ( border_w > 0.0f )
    {
        /* gui_draw_frame reads its rounding from the ambient, like every other rect-shaped verb --
           set the control-frame radius here, at the call site, rather than have the primitive
           assume it. */
        f32 save = draw_rounding();
        draw_set_rounding( ROUND_WIDGET );
        gui_draw_frame( r, col, border_col, border_w );
        draw_set_rounding( save );
    }
    else
        draw_fill( r, col );
}

/*==============================================================================================
    The cell painters -- a (role, phase) the caller knows, with no interaction behind it.

    These have no id and no mix by design: a static rule, a panel band, a title bar is not an
    item, so there is nothing for it to travel between.
==============================================================================================*/

static gui_style_mix_t
mix_still( void )
{
    return ( gui_style_mix_t ){ 0.0f, 0.0f, 0.0f };
}

void draw_face( gui_rect_t r, u8 role, u8 phase )
{
    face_paint( r, role, role, phase, mix_still(), 0u, 0.0f );
}

void draw_face_frame( gui_rect_t r, u8 role, u8 phase, u32 border_col, f32 border_w )
{
    face_paint( r, role, role, phase, mix_still(), border_col, border_w );
}

/*==============================================================================================
    The item painters -- an id and live interaction, so the surface MOVES.

    Each reads the mix itself, which is the one-call convenience; a widget painting several rows
    should call style_mix once and use the _mix forms below instead, so everything it paints
    shares a single probe and a single set of weights.
==============================================================================================*/

void draw_face_item( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected )
{
    face_paint( r, GUI_ROLE_BG, GUI_ROLE_BG, GUI_PHASE_IDLE,
                style_mix( id, st, selected ), 0u, 0.0f );
}

void draw_face_item_frame( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected,
                           u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_BG, GUI_ROLE_BG, GUI_PHASE_IDLE,
                style_mix( id, st, selected ), border_col, border_w );
}

void draw_face_grab( gui_rect_t r, gui_id_t id, gui_item_state_t st, u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_GRAB, GUI_ROLE_GRAB, GUI_PHASE_IDLE,
                style_mix( id, st, false ), border_col, border_w );
}

/* The col_frame_bg shape: a framed FIELD resting on a cell of its caller's choosing that lifts to
   the shared BG hot / active cells when engaged. */
void
draw_face_field( gui_rect_t r, gui_id_t id, gui_item_state_t st, u8 idle_role, u8 idle_phase,
                 u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_BG, idle_role, idle_phase,
                style_mix( id, st, false ), border_col, border_w );
}

/*==============================================================================================
    The same painters over a mix the caller ALREADY HAS -- the multi-row form.

    A widget that paints a surface, a border and an ink wants all three to arrive together off
    one probe.  It reads style_mix once and spends it here and on style_col_mix, rather than
    calling the item painters above and letting each re-probe the same slot.
==============================================================================================*/

void draw_face_mix( gui_rect_t r, u8 role, gui_style_mix_t m )
{
    face_paint( r, role, role, GUI_PHASE_IDLE, m, 0u, 0.0f );
}

void draw_face_mix_frame( gui_rect_t r, u8 role, gui_style_mix_t m, u32 border_col, f32 border_w )
{
    face_paint( r, role, role, GUI_PHASE_IDLE, m, border_col, border_w );
}

void draw_face_field_mix( gui_rect_t r, gui_style_mix_t m, u8 idle_role, u8 idle_phase,
                          u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_BG, idle_role, idle_phase, m, border_col, border_w );
}

/*==============================================================================================
    The caller's door -- the same seam, published.

    A user widget paints its surface through gui_draw_face for exactly the reason a stock one
    does: it is what makes the widget SKINNABLE by whoever installs the theme, rather than by
    whoever wrote the widget.  Reading gui_style_edit()->face[][][] at paint time instead bypasses
    the stacks, the same trap reading ->col[][][] is.
==============================================================================================*/

const gui_brush_t* gui_style_face( gui_style_role_t r, gui_style_phase_t p ) { return style_face( (u8)r, (u8)p ); }

gui_style_mix_t gui_style_mix      ( gui_id_t id, gui_item_state_t st, bool selected )        { return style_mix( id, st, selected ); }
u32             gui_style_color_mix( gui_style_role_t r, gui_style_mix_t m )                  { return style_col_mix( (u8)r, m ); }

void gui_draw_face     ( gui_rect_t box, gui_style_role_t r, gui_style_phase_t p ) { draw_face( box, (u8)r, (u8)p ); }
void gui_draw_face_item( gui_rect_t box, gui_id_t id, gui_item_state_t st, bool selected ) { draw_face_item( box, id, st, selected ); }
void gui_draw_face_mix ( gui_rect_t box, gui_style_role_t r, gui_style_mix_t m )            { draw_face_mix( box, (u8)r, m ); }

// clang-format on
/*============================================================================================*/
