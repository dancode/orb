/*==============================================================================================

    runtime_service/gui/stock/gui_stock_widgets.c -- The stock_* rect-consuming widget renders.

    The REFERENCE widget set: one plain render per component, the thing a user reads and forks,
    not a privileged default.  Every stock_* fills EXACTLY the rect it is handed -- no hidden
    padding, no flow, no layout reservation -- and composes the three ambient services:
    behavior (the comp_* logic core, or gui_item directly for the inert three), presentation
    (the public draw_* surface), and the element style axis (gui_element.h).

    Naming: stock_* is the WIDGET SET; el_* is the STYLE AXIS it paints from (gui_el_color,
    GUI_EL_BG ...).  Two vocabularies, deliberately -- a user widget is a sibling of a stock_*
    render and reads the same cells.

    Style contract: renders resolve every color through style_el_col (the style unit) -- one
    read of the element slot, which holds the installed value with any push_style_color /
    next_style_color override already applied.  So a stock widget reads exactly what chrome
    reads (chrome's element-shaped COL_* macros ARE style_el_col), and a caller can
    push_style_color around a stock_* the same way.  gui_el_color is this same seam published
    for a user's own render.  The installed values are re-derived at every style landing
    (gui_style_apply / theme_set / theme_reset / font activation), through the registered style
    source if a kit owns the look (gui_style_source_set); a kit may also poke gui_style_edit()
    ad hoc.  Renders still never walk the theme -- style_el_col is the one seam.

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
    stock actually is -- renders over that schema, reading it through style_el_*.
==============================================================================================*/

/* col shorthand for the element bodies below -- routed through style_el_col so a push_style_color
   override wins and a delegating stock widget sees the SAME value chrome does; with no override
   this is exactly the installed element palette (kit-owned when a style source is registered).
   For a runtime (non-token) state, call style_el_col( GUI_EL_<role>, s ) directly. */
#define EL_COL( role, state ) style_el_col( GUI_EL_##role, GUI_EL_##state )

/* gui_item_phase -- interaction state -> palette state, the one mapping EVERY render needs.
   Published because a user widget is the stock render's sibling: both pick a face from the same
   three-way rule, so neither should re-derive it.  nav counts as HOT so a keyboard-navigated
   widget lights up exactly like a hovered one.  (GUI_EL_DIM is the inert / disabled variant --
   a render selects it deliberately, never from live interaction.) */
gui_el_state_t
gui_item_phase( gui_item_state_t st )
{
    return st.active           ? GUI_EL_ACTIVE
         : st.hover || st.nav  ? GUI_EL_HOT
                               : GUI_EL_IDLE;
}

/* gui_el_color -- the resolved element palette read: the installed style (kit-owned when a style
   source is registered), with an active push_style_color / next_style_color override winning.
   THE color door for a user widget: reading gui_style_edit()->col[][] directly bypasses the style
   stack, so a push_style_color around a user widget would silently do nothing while it works on
   every stock widget.  This is the same seam the stock renders and chrome's COL_* macros read. */
u32
gui_el_color( gui_el_role_t role, gui_el_state_t state )
{
    return style_el_col( (u8)role, (u8)state );
}

/* Local shorthand: the phase mapping under its old in-file name. */
#define el_state( st ) gui_item_phase( st )

/* The "##id" label grammar for a DISPLAYED label: the suffix carries identity, never pixels.
   Returns the visible span -- the original pointer when there is no suffix (no copy), else the
   head copied into buf.  Shared by the label-bearing renders (stock_button, stock_selectable). */
static const char*
el_visible_text( const char* label, char* buf, u32 bufsz )
{
    u32 n = label_vis_len( label );
    if ( label[ n ] == '\0' ) return label;         /* no suffix: paint the label as-is */
    if ( n >= bufsz ) n = bufsz - 1;
    for ( u32 i = 0; i < n; ++i ) buf[ i ] = label[ i ];
    buf[ n ] = '\0';
    return buf;
}

/*==============================================================================================
    The element cores -- every one fills exactly its rect.
==============================================================================================*/

/* Inert framed backdrop: the DIM surface.  Chrome for grouping, never interactive.  One of the
   three inert stock cores (panel / label / meter) with no component behind them -- there is no
   interaction to extract, so they are render-only by design. */
void
gui_stock_panel( gui_rect_t r )
{
    gui_draw_frame( r, EL_COL( PANEL, DIM ), EL_COL( BORDER, DIM ), WIN_BORDER );
}

/* A text run seated in r per align.  The one-role element; a colored variant is just
   draw_text_in with the caller's color -- no el_ wrapper needed. */
void
gui_stock_label( gui_rect_t r, gui_align_t align, const char* text )
{
    gui_draw_text_in( r, align, EL_COL( TEXT, IDLE ), text );
}

/* Button face label: centered when it fits the frame, else left-anchored and ellipsized so an
   oversized label truncates cleanly instead of spilling past both edges.  The el twin of chrome's
   draw_button_label -- both paint the same face, which is what lets a stock button delegate here.
   text is the already-visible span (the caller stripped the "##id" suffix). */
static void
stock_button_label( gui_rect_t r, const char* text )
{
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( label_width( text ) <= avail )
        gui_draw_text_in( r, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), text );
    else
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ),
                        EL_COL( TEXT, IDLE ), text, avail );
}

/* stock_button -- THE reference render over gui_comp_button: a flat, hover/press-animated fill +
   a centered (or ellipsized) label; the label doubles as the component id ("##"/"###" rules
   apply).  col_item_bg_anim rides the keyed damper over the SAME element palette style_el_col
   resolves, so el and chrome animate alike -- this IS the face chrome's gui_button delegates to.
   A user forks it by keeping gui_comp_button and swapping only these draw_* calls.  True on click. */
bool
gui_stock_button( gui_rect_t r, const char* label )
{
    gui_id_t          id = item_id( label );       /* the keyed id for the animation damper */
    gui_comp_button_t b  = gui_comp_button( label, r );

    draw_fill( r, col_item_bg_anim( id, b.state ) );

    char vis[ 128 ];
    stock_button_label( r, el_visible_text( label, vis, sizeof vis ) );

    return b.state.clicked;
}

/* stock_check -- the reference render over gui_comp_check: a framed square box with a check mark
   when *v is set.  The component inscribes the box and owns the toggle; this only draws it.  True
   on the change frame. */
bool
gui_stock_check( gui_rect_t r, const char* id_str, bool* v )
{
    gui_comp_check_t c = gui_comp_check( id_str, r, v );

    gui_draw_frame( c.box, EL_COL( BG, DIM ),
                    ( c.state.hover || c.state.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE ),
                    WIN_BORDER );
    if ( *v )
        gui_draw_check_mark( gui_rect_pad( c.box, c.box.w * 0.22f ), EL_COL( ACCENT, IDLE ) );

    return c.changed;
}

/* stock_slider -- THE reference render over gui_comp_slider: an el-styled groove, value bar, and
   handle, drawn from the component's geometry.  The batteries-included slider a user forks --
   keep gui_comp_slider, swap only these draw_* calls (see sb_gui_base tier 3).  The bare control:
   the caller draws any value text from *v where it wants it.  True on the change frame. */

bool
gui_stock_slider( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi )
{
    gui_comp_slider_t s = gui_comp_slider_ex( &( gui_comp_slider_desc_t ){
        .id = id_str, .rect = r, .v = v, .lo = lo, .hi = hi, .handle_w = 8.0f } );

    /* Groove: a centered band; the value bar fills it to the component's fraction. */
    gui_rect_t track = gui_rect_align( r, r.w, r.h * 0.30f, GUI_ALIGN_CENTER );
    gui_draw_frame( track, EL_COL( ACCENT, DIM ), EL_COL( BORDER, DIM ), 1.0f );
    gui_rect_t fill = gui_rect_pad( track, 1.0f );
    fill.w *= s.frac;
    if ( fill.w > 0.0f )
        gui_draw_rect( fill.x, fill.y, fill.w, fill.h, EL_COL( ACCENT, IDLE ) );

    /* Handle: the component's x + width, the render's height (80% of r, centered). */
    gui_rect_t handle = { s.handle.x, r.y + r.h * 0.10f, s.handle.w, r.h * 0.80f };
    gui_el_state_t es = el_state( s.state );
    gui_draw_frame( handle, style_el_col( GUI_EL_BG, es ),
                    ( es != GUI_EL_IDLE ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE ), 1.0f );

    return s.changed;
}

/* Horizontal fill bar (XP strip, cast bar): framed track filled left-to-right to frac (0..1).
   The fill color is a CALL PARAMETER -- per-widget color is kit business, not a style slot. */
void
gui_stock_meter( gui_rect_t r, f32 frac, u32 fill_abgr )
{
    frac = ( frac < 0.0f ) ? 0.0f : ( frac > 1.0f ) ? 1.0f : frac;

    gui_draw_frame( r, EL_COL( ACCENT, DIM ), EL_COL( BORDER, DIM ), 1.0f );
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

    u32 lb = ( cy.prev.state.hover || cy.prev.state.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE );
    u32 rb = ( cy.next.state.hover || cy.next.state.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE );
    gui_draw_frame( cy.prev_box, EL_COL( BG, DIM ), lb, WIN_BORDER );
    gui_draw_frame( cy.next_box, EL_COL( BG, DIM ), rb, WIN_BORDER );
    gui_draw_chevron( gui_rect_pad( cy.prev_box, cy.prev_box.w * 0.30f ), GUI_DIR_LEFT,  2.0f, EL_COL( TEXT, IDLE ) );
    gui_draw_chevron( gui_rect_pad( cy.next_box, cy.next_box.w * 0.30f ), GUI_DIR_RIGHT, 2.0f, EL_COL( TEXT, IDLE ) );

    if ( count > 0 )
        gui_draw_text_in( cy.label, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), items[ *idx ] );

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

    gui_draw_frame( r,
                    st.focused ? EL_COL( BG, ACTIVE ) : style_el_col( GUI_EL_BG, el_state( st ) ),
                    ( st.focused || st.hover || st.nav ) ? EL_COL( BORDER, HOT )
                                                         : EL_COL( BORDER, IDLE ),
                    WIN_BORDER );

    if ( in.selection.w > 0.0f )
        draw_fill( in.selection, EL_COL( ACCENT, DIM ) );

    draw_push_text_clip_n( in.text_x, in.text_y, EL_COL( TEXT, IDLE ), buf, 0xFFFFFFFFu,
                           in.content.x, in.content.x + in.content.w );

    if ( in.caret.w > 0.0f )
        draw_fill( in.caret, EL_COL( ACCENT, ACTIVE ) );

    return in.changed;
}

/* Full-width selectable row: transparent when idle so the surface behind shows through, the HOT
   tint on hover / nav, the ACTIVE tint when selected.  THE row primitive under lists, combos,
   trees, and menus -- the pure core, carrying none of chrome's policy (type-ahead stamp, popup /
   combo dismiss on click).  That policy stays in chrome's gui_selectable, which is free to compose
   this.  The component owns the press + the *selected toggle (NULL = a click-only row, the caller
   driving its own index from the return); this draws the row.  id comes from the label ("##" /
   "###" rules apply).  True on the frame it is clicked. */
bool
gui_stock_selectable( gui_rect_t r, const char* label, bool* selected )
{
    gui_comp_selectable_t s = gui_comp_selectable( label, r, selected );

    bool on = ( selected && *selected );
    if ( on || s.state.hover || s.state.nav )
        gui_draw_rect( r.x, r.y, r.w, r.h, on ? EL_COL( BG, ACTIVE ) : EL_COL( BG, HOT ) );

    char        vis[ 128 ];
    const char* text = el_visible_text( label, vis, sizeof vis );
    gui_rect_t  tr   = { r.x + WIDGET_PAD, r.y, r.w - 2.0f * WIDGET_PAD, r.h };
    gui_draw_text_in( tr, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, EL_COL( TEXT, IDLE ), text );

    return s.state.clicked;
}

#undef el_state
#undef EL_COL

// clang-format on
/*============================================================================================*/
