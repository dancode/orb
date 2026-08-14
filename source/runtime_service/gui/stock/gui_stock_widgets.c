/*==============================================================================================

    runtime_service/gui/stock/gui_stock_widgets.c -- The stock_* rect-consuming widget renders.

    The REFERENCE widget set: one plain render per component, the thing a user reads and forks,
    not a privileged default.  Every stock_* fills EXACTLY the rect it is handed -- no hidden
    padding, no flow, no layout reservation -- and composes the three ambient services:
    behavior (the comp_* logic core, or gui_item directly for the inert three), presentation
    (the public draw_* surface), and the style grid (gui_style_role_t x gui_style_phase_t, gui.h).

    Naming: stock_* is the WIDGET SET; style_ is the LOOK it paints from.  ONE vocabulary -- a
    user widget is a sibling of a stock_* render, and both name the same cells chrome does.

    Style contract: renders resolve every color through style_col (the style unit) -- one
    read of the grid slot, which holds the installed value with any push_style_color /
    next_style_color override already applied.  So a stock widget reads exactly what chrome
    reads (chrome's COL_* macros ARE style_col under a semantic name), and a caller can
    push_style_color around a stock_* the same way.  gui_style_color is this same seam published
    for a user's own render.  The installed values are re-derived at every style landing
    (gui_style_apply / theme_set / theme_reset / font activation), through the registered style
    source if a kit owns the look (gui_style_source_set); a kit may also poke gui_style_edit()
    ad hoc.  Renders still never walk the theme -- style_col is the one seam.

    Dependency contract: the stock_* renders call gui_core (item, ids, io, redraw) + gui_draw
    (draw_*) + gui_rect + the comp_* components only -- NEVER the flow layout engine -- and
    resolve through the public gui_* declarations (gui_host.h) plus the style_active() seam.
    A constituent of the stock unit (root gui_stock.c).

==============================================================================================*/

// clang-format off

/*==============================================================================================
    The style lives in the style unit.

    Its storage is the style BLOCK (style/gui_style_core.c): the installed run is laid out as
    gui_style_t, so gui_style_edit() hands back a typed view onto it.  What stays here is what
    stock actually is -- renders over that schema, reading it through style_col / style_var.
==============================================================================================*/

/* Compile-time-tag shorthand for style_col, for the constant cells the renders below name.  A
   push_style_color override still wins (it lands in the slot this reads), so a delegating stock
   widget sees the SAME value chrome does.  For a cell picked at RUNTIME -- the usual case, a
   phase from gui_item_phase -- call style_col( GUI_ROLE_<role>, phase ) directly. */
#define STYLE_COL( role, phase ) style_col( GUI_ROLE_##role, GUI_PHASE_##phase )

/* gui_item_phase -- interact state -> style phase, the one mapping EVERY render needs.
   Published because a user widget is the stock render's sibling: both pick a face from the same
   three-way rule, so neither should re-derive it.  nav counts as HOT so a keyboard-navigated
   widget lights up exactly like a hovered one.  (GUI_PHASE_DIM is the inert / disabled variant --
   a render selects it deliberately, never from live interaction.)

   This is the PUBLIC DOOR onto style_phase, not a second copy of the rule: the style unit's own
   projections resolve through the same function one tier down, so the face a user widget picks
   and the face col_item_bg picks are the same face by construction. */
gui_style_phase_t
gui_item_phase( gui_item_state_t st )
{
    return ( gui_style_phase_t )style_phase( st );
}

/* gui_style_color -- the resolved grid read published for a render outside this library: the
   installed style (kit-owned when a style source is registered), with an active
   push_style_color / next_style_color override winning.  THE color door for a user widget:
   reading gui_style_edit()->col[][] directly bypasses the style stacks, so a push_style_color
   around a user widget would silently do nothing while it works on every stock widget.  Inside
   the library this is style_col, which chrome's COL_* macros and the renders below read. */
u32
gui_style_color( gui_style_role_t role, gui_style_phase_t phase )
{
    return style_col( (u8)role, (u8)phase );
}

/* gui_style_color_selected -- the same read washed toward the theme's accent.  A user widget
   whose caller knows the item is chosen reads through this instead, and gets a selection that
   still hovers and presses -- the wash rides on top of whatever gui_style_color already read. */
u32
gui_style_color_selected( gui_style_role_t role, gui_style_phase_t phase )
{
    return style_col_selected( (u8)role, (u8)phase );
}

/* The "##id" label grammar for a DISPLAYED label: the suffix carries identity, never pixels.
   Returns the visible span -- the original pointer when there is no suffix (no copy), else the
   head copied into buf.  Shared by the label-bearing renders (stock_button, stock_selectable). */
static const char*
stock_visible_text( const char* label, char* buf, u32 bufsz )
{
    u32 n = label_vis_len( label );
    if ( label[ n ] == '\0' ) return label;         /* no suffix: paint the label as-is */
    if ( n >= bufsz ) n = bufsz - 1;
    for ( u32 i = 0; i < n; ++i ) buf[ i ] = label[ i ];
    buf[ n ] = '\0';
    return buf;
}

/*==============================================================================================
    The stock renders -- every one fills exactly its rect.
==============================================================================================*/

/* Inert framed backdrop: the DIM surface.  Chrome for grouping, never interactive.  One of the
   three inert stock cores (panel / label / meter) with no component behind them -- there is no
   interaction to extract, so they are render-only by design. */
void
gui_stock_panel( gui_rect_t r )
{
    draw_face_frame( r, GUI_ROLE_PANEL, GUI_PHASE_DIM, STYLE_COL( BORDER, DIM ), WIN_BORDER );
}

/* A text run seated in r per align.  The one-role render; a colored variant is just
   draw_text_in with the caller's color -- no stock wrapper needed. */
void
gui_stock_label( gui_rect_t r, gui_align_t align, const char* text )
{
    gui_draw_text_in( r, align, STYLE_COL( TEXT, IDLE ), text );
}

/* Button face label: centered when it fits the frame, else left-anchored and ellipsized so an
   oversized label truncates cleanly instead of spilling past both edges.  The stock twin of
   chrome's draw_button_label -- same face, which is what lets a stock button delegate here.
   text is the already-visible span (the caller stripped the "##id" suffix). */
static void
stock_button_label( gui_rect_t r, const char* text )
{
    f32 avail = r.w - 2.0f * WIDGET_PAD;

    /* Half-pixel slack, and it is load-bearing.  A natural-width button is sized as
       label_natural_w = label_width + 2 * WIDGET_PAD, so on the overwhelmingly common path this
       comparison is an EQUALITY in disguise -- and an exact float equality that rounds a hair the
       wrong way makes a button ellipsize its OWN label ("snap (all 0)" -> "snap (all..."), which
       is the one thing a self-fitting widget must never do at its natural size.  Nothing
       sub-pixel is drawable, so the slack cannot cost a legible glyph. */
    if ( label_width( text ) <= avail + 0.5f )
        gui_draw_text_in( r, GUI_ALIGN_CENTER, STYLE_COL( TEXT, IDLE ), text );
    else
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ),
                        STYLE_COL( TEXT, IDLE ), text, avail );
}

/* stock_button -- THE reference render over gui_comp_button: a flat, hover/press-animated fill +
   a centered (or ellipsized) label; the label doubles as the component id ("##"/"###" rules
   apply).  draw_face_item rides the keyed mix over the SAME grid cells style_col resolves,
   so stock and chrome animate alike -- this IS the face chrome's gui_button uses.
   A user forks it by keeping gui_comp_button and swapping only these draw_* calls.  True on click. */
bool
gui_stock_button( gui_rect_t r, const char* label )
{
    gui_id_t          id = item_id( label );       /* the keyed id for the animation damper */
    gui_comp_button_t b  = gui_comp_button( label, r );

    draw_face_item( r, id, b.state, false );

    char vis[ 128 ];
    stock_button_label( r, stock_visible_text( label, vis, sizeof vis ) );

    return b.state.clicked;
}

/* stock_check -- the reference render over gui_comp_check: a framed square box with a check mark
   when *v is set.  The component inscribes the box and owns the toggle; this only draws it.  True
   on the change frame.

   The box is a control FACE, so it lifts the way every face in the kit lifts: col_item_bg along
   the BG row, border held at BORDER[IDLE].  It used to do the opposite (fill pinned at BG[DIM],
   border lifting to BORDER[HOT]) -- which made the reference checkbox respond to a hover in the
   mirror image of chrome's, so a reader forking stock got a different answer per widget. */
bool
gui_stock_check( gui_rect_t r, const char* id_str, bool* v )
{
    gui_comp_check_t c = gui_comp_check( id_str, r, v );

    draw_face_item_frame( c.box, item_id( id_str ), c.state, false,
                          STYLE_COL( BORDER, IDLE ), WIN_BORDER );
    if ( *v )
        gui_draw_check_mark( gui_rect_pad( c.box, c.box.w * 0.22f ), STYLE_COL( MARK, IDLE ) );

    return c.changed;
}

/* stock_slider -- THE reference render over gui_comp_slider: a styled groove, value bar, and
   handle, drawn from the component's geometry.  The batteries-included slider a user forks --
   keep gui_comp_slider, swap only these draw_* calls (see sb_gui_base tier 3).  The bare control:
   the caller draws any value text from *v where it wants it.  True on the change frame. */

bool
gui_stock_slider( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi )
{
    gui_comp_slider_t s = gui_comp_slider_ex( &( gui_comp_slider_desc_t ){
        .id_str = id_str, .rect = r, .v = v, .lo = lo, .hi = hi, .handle_w = 8.0f } );

    /* Three layers, three rows -- the assignment chrome's slider_render uses, and the reason GRAB
       exists as a role.  Groove: a control face over its own ACCENT[DIM] rest (col_frame_bg).
       Value bar: the ACCENT row, lifted while engaged so it stays brighter than the lifted groove
       under it.  Handle: the GRAB row, the theme's contrast anchor, which is what keeps it legible
       against BOTH of its neighbours instead of matching one of them in some phase. */
    /* ONE mix for the whole control: groove and handle are two rows of one interaction, so they
       must lift and settle together -- two probes would let them drift a frame apart. */
    gui_style_mix_t mix = style_mix( item_id( id_str ), s.state, false );

    gui_rect_t track = gui_rect_align( r, r.w, r.h * 0.30f, GUI_ALIGN_CENTER );
    draw_face_field_mix( track, mix, GUI_ROLE_ACCENT, GUI_PHASE_DIM,
                         STYLE_COL( BORDER, DIM ), 1.0f );
    gui_rect_t fill = gui_rect_pad( track, 1.0f );
    fill.w *= s.frac;
    if ( fill.w > 0.0f )
        gui_draw_rect( fill.x, fill.y, fill.w, fill.h, style_col_mix( GUI_ROLE_ACCENT, mix ) );

    /* Handle: the component's x + width, the render's height (80% of r, centered). */
    gui_rect_t handle = { s.handle.x, r.y + r.h * 0.10f, s.handle.w, r.h * 0.80f };
    draw_face_mix_frame( handle, GUI_ROLE_GRAB, mix, STYLE_COL( BORDER, IDLE ), 1.0f );

    return s.changed;
}

/* Horizontal fill bar (XP strip, cast bar): framed track filled left-to-right to frac (0..1).
   The fill color is a CALL PARAMETER -- per-widget color is kit business, not a style slot. */
void
gui_stock_meter( gui_rect_t r, f32 frac, u32 fill_abgr )
{
    frac = ( frac < 0.0f ) ? 0.0f : ( frac > 1.0f ) ? 1.0f : frac;

    draw_face_frame( r, GUI_ROLE_ACCENT, GUI_PHASE_DIM, STYLE_COL( BORDER, DIM ), 1.0f );
    gui_rect_t fill = gui_rect_pad( r, 1.0f );
    fill.w *= frac;
    if ( fill.w > 0.0f )
        gui_draw_rect( fill.x, fill.y, fill.w, fill.h, fill_abgr );
}

/* stock_cycle -- the reference render over gui_comp_cycle: framed square chevron caps and the
   current item centered between them.  The component owns the two cap buttons + the wrap; this
   draws the caps (hover-lit) and items[*idx].  count <= 0 shows the empty caps.  True on the
   change frame. */
bool
gui_stock_cycle( gui_rect_t r, const char* id_str, i32* idx, const char* const* items, i32 count )
{
    gui_comp_cycle_t cy = gui_comp_cycle( id_str, r, idx, count );

    /* Each cap is a control face and lifts like one (col_item_bg, border held) -- see stock_check. */
    draw_face_item_frame( cy.prev_box, id_combine( item_id( id_str ), 1u ), cy.prev.state, false,
                          STYLE_COL( BORDER, IDLE ), WIN_BORDER );
    draw_face_item_frame( cy.next_box, id_combine( item_id( id_str ), 2u ), cy.next.state, false,
                          STYLE_COL( BORDER, IDLE ), WIN_BORDER );
    gui_draw_chevron( gui_rect_pad( cy.prev_box, cy.prev_box.w * 0.30f ), GUI_DIR_LEFT,  2.0f, STYLE_COL( TEXT, IDLE ) );
    gui_draw_chevron( gui_rect_pad( cy.next_box, cy.next_box.w * 0.30f ), GUI_DIR_RIGHT, 2.0f, STYLE_COL( TEXT, IDLE ) );

    if ( count > 0 )
        gui_draw_text_in( cy.label, GUI_ALIGN_CENTER, STYLE_COL( TEXT, IDLE ), items[ *idx ] );

    return cy.changed;
}

/* stock_input -- the reference render over gui_comp_input: an element-styled box, then the
   component's selection band, glyph-clipped run, and blinking caret painted from the geometry it
   returned (this render never touches the edit state).  The proof the engine needs no chrome:
   this core and chrome's input_text drive the same component; only the paint differs.  True on
   any frame the buffer changes. */
bool
gui_stock_input( gui_rect_t r, const char* id_str, char* buf, u32 bufsz )
{
    gui_comp_input_t in = gui_comp_input( id_str, r, WIDGET_PAD, buf, bufsz );
    gui_item_state_t st = in.state;

    /* Face rests on BG[IDLE] through every phase, border carries focus alone on BORDER[ACTIVE] --
       the same two rules chrome's input_text uses, so the pair really is one look driven by one
       component.  A field is typed INTO, not clicked, so it does not spend the phase axis (the
       reasoning is written out once, at input_text_begin). */
    draw_face_frame( r, GUI_ROLE_BG, GUI_PHASE_IDLE, col_field_border( st ), WIN_BORDER );

    /* Selection band and caret read the same cells chrome's edit_paint uses: a selection is the
       BG face washed for being chosen -- a control surface, selected -- and a caret is TEXT.
       Neither is an accent: ACCENT is the value a control holds, and a text field's value is its
       glyphs. */
    if ( in.selection.w > 0.0f )
        draw_fill( in.selection, style_col_selected( GUI_ROLE_BG, GUI_PHASE_IDLE ) );

    draw_push_text_clip_n( in.text_x, in.text_y, STYLE_COL( TEXT, IDLE ), buf, 0xFFFFFFFFu,
                           in.content.x, in.content.x + in.content.w );

    if ( in.caret.w > 0.0f )
        draw_fill( in.caret, STYLE_COL( TEXT, IDLE ) );

    return in.changed;
}

/* Full-width selectable row: transparent when idle so the surface behind shows through, and
   otherwise the BG face of whichever plane the selection picks -- so a chosen row reacts to the
   cursor exactly as an unchosen one does.  THE row primitive under lists, combos,
   trees, and menus -- the pure core, carrying none of chrome's policy (type-ahead stamp, popup /
   combo dismiss on click).  That policy stays in chrome's gui_selectable, which is free to compose
   this.  The component owns the press + the *selected toggle (NULL = a click-only row, the caller
   driving its own index from the return); this draws the row.  id comes from the label ("##" /
   "###" rules apply).  True on the frame it is clicked. */
bool
gui_stock_selectable( gui_rect_t r, const char* label, bool* selected )
{
    gui_comp_selectable_t s = gui_comp_selectable( label, r, selected );

    /* GUI_ID_NONE mix: list rows snap, matching chrome's gui_selectable -- a damped hover fading
       out row by row behind a sweeping cursor reads as a growing multi-selection. */
    bool            on  = ( selected && *selected );
    gui_style_mix_t mix = style_mix( GUI_ID_NONE, s.state, on );
    if ( mix.hot > 0.0f || mix.act > 0.0f || mix.sel > 0.0f )
        draw_face_mix( r, GUI_ROLE_BG, mix );

    char        vis[ 128 ];
    const char* text = stock_visible_text( label, vis, sizeof vis );
    gui_rect_t  tr   = { r.x + WIDGET_PAD, r.y, r.w - 2.0f * WIDGET_PAD, r.h };
    /* The ink crosses with the surface, but via the selected wash alone (hot/act held at 0): a
       row's text is not meant to brighten under the cursor, only to change with what the row has
       become. */
    gui_style_mix_t ink = { 0.0f, 0.0f, mix.sel };
    gui_draw_text_in( tr, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                      style_col_mix( GUI_ROLE_TEXT, ink ), text );

    return s.state.clicked;
}

#undef STYLE_COL

// clang-format on
/*============================================================================================*/
