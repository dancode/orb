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

    Each painter MIRRORS one colour projection, and the mirroring is the point: a site converting
    from colour to face changes one call and nothing else, because the coordinates it was already
    passing are the coordinates the face plane is indexed by.

        style_col        ( role, phase )        ->  draw_face      ( r, role, phase )
        style_col_look   ( role, phase, look )  ->  draw_face_look ( r, role, phase, look )
        col_item_bg      ( st )                 ->  draw_face_item ( r, st )
        col_item_bg_look ( st, look )           ->  draw_face_item_look( r, st, look )
        col_item_bg_anim ( id, st )             ->  draw_face_item_anim( r, id, st )
        col_frame_bg     ( st, idle )           ->  draw_face_field( r, st, idle_role, idle_phase )
        col_grab         ( st )                 ->  draw_face_grab ( r, st )

    Included by gui_stock.c before the widget renders, which paint through it.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The one primitive -- every painter below is this with its coordinates worked out.
==============================================================================================*/

/* Fill r for the cell (look, role, phase): the cell's face brush if it has one, else a flat fill
   of its colour with an optional border.  border_w <= 0 means no border either way.

   `fallback_col` exists for the one shape the grid cannot express: col_frame_bg's caller-supplied
   idle colour (a slider groove rests on ACCENT/DIM, an input on BG/IDLE).  Pass 0 to mean "use
   the cell's own colour", which is what every other caller wants. */
static void
face_paint( gui_rect_t r, u8 role, u8 phase, u8 look, u32 fallback_col,
            u32 border_col, f32 border_w )
{
    const gui_brush_t* b = style_face_look( role, phase, look );
    if ( b )
    {
        draw_fill_brush( r, b );      /* art carries its own edge -- no border over the top */
        return;
    }

    u32 col = fallback_col ? fallback_col : style_col_look( role, phase, look );
    if ( border_w > 0.0f )
        gui_draw_frame( r, col, border_col, border_w );
    else
        draw_fill( r, col );
}

/*==============================================================================================
    The cell painters -- a (role, phase) the caller knows.
==============================================================================================*/

void draw_face( gui_rect_t r, u8 role, u8 phase )
{
    face_paint( r, role, phase, GUI_LOOK_NORMAL, 0u, 0u, 0.0f );
}

void draw_face_look( gui_rect_t r, u8 role, u8 phase, u8 look )
{
    face_paint( r, role, phase, look, 0u, 0u, 0.0f );
}

void draw_face_frame( gui_rect_t r, u8 role, u8 phase, u32 border_col, f32 border_w )
{
    face_paint( r, role, phase, GUI_LOOK_NORMAL, 0u, border_col, border_w );
}

/*==============================================================================================
    The state painters -- a phase distilled from live interaction, the common case.

    Each takes the interact state a widget already holds and folds it through gui_item_phase, the
    same three-way rule every render uses, so a face-painted widget and a colour-painted one pick
    the same cell from the same evidence.
==============================================================================================*/

void draw_face_item( gui_rect_t r, gui_item_state_t st )
{
    face_paint( r, GUI_ROLE_BG, (u8)gui_item_phase( st ), GUI_LOOK_NORMAL, 0u, 0u, 0.0f );
}

void draw_face_item_look( gui_rect_t r, gui_item_state_t st, gui_style_look_t look )
{
    face_paint( r, GUI_ROLE_BG, (u8)gui_item_phase( st ), (u8)look, 0u, 0u, 0.0f );
}

void draw_face_item_frame( gui_rect_t r, gui_item_state_t st, u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_BG, (u8)gui_item_phase( st ), GUI_LOOK_NORMAL, 0u,
                border_col, border_w );
}

void draw_face_grab( gui_rect_t r, gui_item_state_t st, u32 border_col, f32 border_w )
{
    face_paint( r, GUI_ROLE_GRAB, (u8)gui_item_phase( st ), GUI_LOOK_NORMAL, 0u,
                border_col, border_w );
}

/* The col_frame_bg shape: a framed FIELD that rests on a colour of its caller's choosing and
   lifts to the shared BG hot / active cells when engaged.  The resting cell is named as a
   (role, phase) rather than passed as a colour so the rest state can carry a face too -- a slider
   groove and an input box are exactly the widgets a skin most wants to author. */
void
draw_face_field( gui_rect_t r, gui_item_state_t st, u8 idle_role, u8 idle_phase,
                 u32 border_col, f32 border_w )
{
    if ( st.active || st.hover || st.nav )
    {
        u8 phase = st.active ? GUI_PHASE_ACTIVE : GUI_PHASE_HOT;
        face_paint( r, GUI_ROLE_BG, phase, GUI_LOOK_NORMAL, 0u, border_col, border_w );
        return;
    }
    face_paint( r, idle_role, idle_phase, GUI_LOOK_NORMAL, 0u, border_col, border_w );
}

/* The animated face.  A brush cannot be interpolated -- its phases are separate pieces of art, and
   crossfading two nine-slices is not a blend, it is two draws -- so a cell WITH a face takes the
   phase step cleanly and a cell without keeps the damped colour it always had.  That asymmetry is
   honest rather than a gap: authored art expresses its own state change, which is why the sandbox's
   hover face is a different brush and not the same one lerped. */
void
draw_face_item_anim( gui_rect_t r, gui_id_t id, gui_item_state_t st )
{
    const gui_brush_t* b = style_face_look( GUI_ROLE_BG, (u8)gui_item_phase( st ), GUI_LOOK_NORMAL );
    if ( b )
        draw_fill_brush( r, b );
    else
        draw_fill( r, col_item_bg_anim( id, st ) );
}

/*==============================================================================================
    The caller's door -- the same seam, published.

    A user widget paints its surface through gui_draw_face for exactly the reason a stock one
    does: it is what makes the widget SKINNABLE by whoever installs the theme, rather than by
    whoever wrote the widget.  Reading gui_style_edit()->face[][][] at paint time instead bypasses
    the stacks, the same trap reading ->col[][][] is.
==============================================================================================*/

const gui_brush_t* gui_style_face     ( gui_style_role_t r, gui_style_phase_t p )                     { return style_face( (u8)r, (u8)p ); }
const gui_brush_t* gui_style_face_look( gui_style_role_t r, gui_style_phase_t p, gui_style_look_t l ) { return style_face_look( (u8)r, (u8)p, (u8)l ); }

void gui_draw_face     ( gui_rect_t box, gui_style_role_t r, gui_style_phase_t p )                     { draw_face( box, (u8)r, (u8)p ); }
void gui_draw_face_look( gui_rect_t box, gui_style_role_t r, gui_style_phase_t p, gui_style_look_t l ) { draw_face_look( box, (u8)r, (u8)p, (u8)l ); }
void gui_draw_face_item( gui_rect_t box, gui_item_state_t st )                                         { draw_face_item( box, st ); }

// clang-format on
/*============================================================================================*/
