/*==============================================================================================

    runtime_service/gui/element/gui_element_core.c -- The el_* rect-consuming widget cores.

    The building-block tier, lifted from the proven
    sb_gui_diablo ui layer.  Every el_* fills EXACTLY the rect it is handed -- no hidden
    padding, no flow, no layout reservation -- and composes the three ambient services:
    behavior (gui_item over the caller rect), presentation (the public draw_* surface),
    and the slim element style (gui_el_style_t, gui_element.h).

    Style contract: elements resolve every color through style_el_col (the style unit) -- the
    INSTALLED element style by default, an active push_style_color override winning.  So a
    delegating stock widget reads exactly what chrome reads (chrome's COL_* macros ARE
    style_el_col), and a caller can push_style_color around an el_* the same way; absent any
    override this is byte-identical to the raw installed palette, so a kit that owns the look
    (gui_style_source_set) still wins by default.  The installed style is re-derived at every
    landing (gui_style_apply / theme_set / theme_reset / font activation) through
    el_style_install(): the registered style source, else the default S2 compile
    (el_style_derive).  A kit may also poke values ad hoc via gui_el_style().  Elements still
    never NAME a gui_col_t slot or walk the theme -- style_el_col is the one seam.

    Dependency contract: the el_* cores call gui_core (item, ids, io, redraw) + gui_draw
    (draw_*) + gui_rect only -- NEVER the flow layout engine -- and resolve through the
    public gui_* declarations (gui_host.h) plus the style_active() seam.  A constituent of
    the element unit (root gui_element.c).

==============================================================================================*/

// clang-format off

/*==============================================================================================
    The installed element style (S1) + the S2 compiler hook.
==============================================================================================*/

static gui_el_style_t s_el_style;

/* THE role x state -> theme slot map: the single source of truth for how the element
   vocabulary projects onto the chrome slot table.  Drives both directions of the strata
   bridge: el_style_derive below (S2 -> S1: theme lands, slots compile into the installed
   style) and style_el_col (style/gui_style_core.c: stock chrome reads element-shaped values back
   through the installed style, stack overrides winning). */
const u8 g_gui_el_slot_map[ GUI_EL_ROLE_COUNT ][ GUI_EL_STATE_COUNT ] =
{
    /*             IDLE                HOT                    ACTIVE              DIM                  */
    /* BG     */ { GUI_COL_WIDGET_BG,  GUI_COL_WIDGET_HOT,    GUI_COL_WIDGET_ACT, GUI_COL_CHILD_BG     },
    /* BORDER */ { GUI_COL_BORDER,     GUI_COL_WIDGET_FG,     GUI_COL_WIDGET_FG,  GUI_COL_BORDER       },
    /* TEXT   */ { GUI_COL_TEXT,       GUI_COL_TEXT,          GUI_COL_TEXT,       GUI_COL_TEXT_DIM     },
    /* ACCENT */ { GUI_COL_WIDGET_FG,  GUI_COL_NAV_HIGHLIGHT, GUI_COL_CHECK_MARK, GUI_COL_SLIDER_TRACK },
};

/* Re-derive the element style from the active theme -- the S2 -> S1 compile step, and the
   DEFAULT style source: el_style_install (below) routes every landing here unless a kit has
   registered its own owner, so elements track the chrome look without ever reading it. */
void
el_style_derive( void )
{
    const gui_style_t* s = style_active();   /* the ACTIVE (font-scaled) style, not the em=12 base */
    gui_el_style_t*    e = &s_el_style;

    e->pad      = (f32)s->widget_pad;
    e->gap      = (f32)s->widget_gap;
    e->border_w = (f32)s->win_border;
    e->line_h   = 0.0f;                          /* live active-font basis */

    for ( u32 role = 0; role < GUI_EL_ROLE_COUNT; ++role )
        for ( u32 state = 0; state < GUI_EL_STATE_COUNT; ++state )
            e->col[ role ][ state ] = s->colors[ g_gui_el_slot_map[ role ][ state ] ];
}

/* Mutable access to the installed style -- the kit (S3) tuning door.  Ad-hoc writes last until
   the next style landing re-installs; a kit that OWNS the look registers a style source below
   so its values are re-derived, not clobbered, at every landing. */
gui_el_style_t*
gui_el_style( void )
{
    return &s_el_style;
}

/* The registered style source -- the S3 promotion seam: the OWNER of the installed style.
   NULL = the default owner, chrome's theme compiler (el_style_derive above). */
static gui_style_source_fn s_style_source      = NULL;
static void*               s_style_source_user = NULL;

/* The landing funnel: gui_style_apply calls this at every style landing (font activation,
   theme_set / theme_reset), after the metrics rescale.  Routes to whichever owner is
   registered, so a kit's promoted look survives theme / font / scale landings. */
void
el_style_install( void )
{
    if ( s_style_source ) s_style_source( s_style_source_user );
    else                  el_style_derive();
}

void
gui_style_source_set( gui_style_source_fn fn, void* user )
{
    s_style_source      = fn;
    s_style_source_user = user;
    el_style_install();             /* promotion / restoration lands immediately */
    if ( g_ctx )                    /* guard: callable before any context exists */
        gui_request_redraw();       /* the restyle must survive an idle frame    */
}

/* col shorthand for the element bodies below -- routed through style_el_col so a push_style_color
   override wins and a delegating stock widget sees the SAME value chrome does; with no override
   this is exactly the installed element palette (kit-owned when a style source is registered).
   For a runtime (non-token) state, call style_el_col( GUI_EL_<role>, s ) directly. */
#define EL_COL( role, state ) style_el_col( GUI_EL_##role, GUI_EL_##state )

/* Interaction state -> palette state for a body fill / border line. */
static gui_el_state_t
el_state( gui_item_state_t st )
{
    return st.active           ? GUI_EL_ACTIVE
         : st.hover || st.nav  ? GUI_EL_HOT
                               : GUI_EL_IDLE;
}

/* The "##id" label grammar for a DISPLAYED label: the suffix carries identity, never pixels.
   Returns the visible span -- the original pointer when there is no suffix (no copy), else the
   head copied into buf.  Shared by the label-bearing cores (el_button, el_selectable). */
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

/* Inert framed backdrop: the DIM surface.  Chrome for grouping, never interactive. */
void
gui_el_panel( gui_rect_t r )
{
    gui_draw_frame( r, EL_COL( BG, DIM ), EL_COL( BORDER, DIM ), s_el_style.border_w );
}

/* A text run seated in r per align.  The one-role element; a colored variant is just
   draw_text_in with the caller's color -- no el_ wrapper needed. */
void
gui_el_label( gui_rect_t r, gui_align_t align, const char* text )
{
    gui_draw_text_in( r, align, EL_COL( TEXT, IDLE ), text );
}

/* Button face label: centered when it fits the frame, else left-anchored and ellipsized so an
   oversized label truncates cleanly instead of spilling past both edges.  The el twin of chrome's
   draw_button_label -- both paint the same face, which is what lets a stock button delegate here.
   text is the already-visible span (the caller stripped the "##id" suffix). */
static void
el_button_label( gui_rect_t r, const char* text )
{
    f32 avail = r.w - 2.0f * s_el_style.pad;
    if ( label_width( text ) <= avail )
        gui_draw_text_in( r, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), text );
    else
        draw_label_fit( r.x + s_el_style.pad, text_center_y( r.y, r.h ),
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
    el_button_label( r, el_visible_text( label, vis, sizeof vis ) );

    return b.clicked;
}

/* el_button -- the prior name for the stock button; delegates so existing callers (kits, chrome's
   gui_button, the sandboxes) are unchanged while the reference lives in one place, now over the
   component.  True on click. */
bool
gui_el_button( gui_rect_t r, const char* label )
{
    return gui_stock_button( r, label );
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
                    s_el_style.border_w );
    if ( *v )
        gui_draw_check_mark( gui_rect_pad( c.box, c.box.w * 0.22f ), EL_COL( ACCENT, IDLE ) );

    return c.changed;
}

/* el_check -- the prior name; delegates to the stock render over the component. */
bool
gui_el_check( gui_rect_t r, const char* id_str, bool* v )
{
    return gui_stock_check( r, id_str, v );
}

/* stock_slider -- THE reference render over gui_comp_slider: an el-styled groove, value bar, and
   handle, drawn from the component's geometry.  The batteries-included slider a user forks --
   keep gui_comp_slider, swap only these draw_* calls (see sb_gui_base tier 3).  The bare control:
   the caller draws any value text from *v where it wants it.  True on the change frame. */

bool
gui_stock_slider( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi )
{
    gui_comp_slider_t s = gui_comp_slider( &( gui_comp_slider_desc_t ){
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

/* el_slider -- the prior name for the stock slider; delegates so existing callers (kits, the
   sandboxes, chrome) are unchanged while the reference lives in one place, now over the
   component.  True on the change frame. */
bool
gui_el_slider( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi )
{
    return gui_stock_slider( r, id_str, v, lo, hi );
}

/* Horizontal fill bar (XP strip, cast bar): framed track filled left-to-right to frac (0..1).
   The fill color is a CALL PARAMETER -- per-widget color is kit business, not a style slot. */
void
gui_el_meter( gui_rect_t r, f32 frac, u32 fill_abgr )
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
    gui_draw_frame( cy.prev_box, EL_COL( BG, DIM ), lb, s_el_style.border_w );
    gui_draw_frame( cy.next_box, EL_COL( BG, DIM ), rb, s_el_style.border_w );
    gui_draw_chevron( gui_rect_pad( cy.prev_box, cy.prev_box.w * 0.30f ), GUI_DIR_LEFT,  2.0f, EL_COL( TEXT, IDLE ) );
    gui_draw_chevron( gui_rect_pad( cy.next_box, cy.next_box.w * 0.30f ), GUI_DIR_RIGHT, 2.0f, EL_COL( TEXT, IDLE ) );

    if ( count > 0 )
        gui_draw_text_in( cy.label, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), items[ *idx ] );

    return cy.changed;
}

/* el_cycle -- the prior name; delegates.  Repeated cycles still need distinct id_str (the two caps
   id under a push of it). */
bool
gui_el_cycle( gui_rect_t r, const char* id_str, i32* idx, const char* const* items, i32 count )
{
    return gui_stock_cycle( r, id_str, idx, items, count );
}

/* stock_input -- the reference render over gui_comp_input: an element-styled box, then the
   component's selection band, glyph-clipped run, and blinking caret painted from the geometry it
   returned (this render never touches the edit state).  The proof the engine needs no chrome:
   this core and chrome's input_text drive the same component; only the paint differs.  True on
   any frame the buffer changes. */
bool
gui_stock_input( gui_rect_t r, const char* id_str, char* buf, u32 bufsz )
{
    gui_comp_input_t in = gui_comp_input( id_str, r, s_el_style.pad, buf, bufsz );
    gui_item_state_t st = in.state;

    gui_draw_frame( r,
                    st.focused ? EL_COL( BG, ACTIVE ) : style_el_col( GUI_EL_BG, el_state( st ) ),
                    ( st.focused || st.hover || st.nav ) ? EL_COL( BORDER, HOT )
                                                         : EL_COL( BORDER, IDLE ),
                    s_el_style.border_w );

    if ( in.selection.w > 0.0f )
        draw_fill( in.selection, EL_COL( ACCENT, DIM ) );

    draw_push_text_clip_n( in.text_x, in.text_y, EL_COL( TEXT, IDLE ), buf, 0xFFFFFFFFu,
                           in.content.x, in.content.x + in.content.w );

    if ( in.caret.w > 0.0f )
        draw_fill( in.caret, EL_COL( ACCENT, ACTIVE ) );

    return in.changed;
}

/* el_input -- the prior name; delegates to the stock render over the component. */
bool
gui_el_input( gui_rect_t r, const char* id_str, char* buf, u32 bufsz )
{
    return gui_stock_input( r, id_str, buf, bufsz );
}

/* Full-width selectable row: transparent when idle so the surface behind shows through, the HOT
   tint on hover / nav, the ACTIVE tint when selected.  THE row primitive under lists, combos,
   trees, and menus -- the pure core, carrying none of chrome's policy (type-ahead stamp, popup /
   combo dismiss on click).  That policy stays in chrome's gui_selectable, which is free to compose
   this.  selected NULL = a click-only row (the caller drives its own index from the return).  id
   comes from the label ("##"/"###" rules apply).  True on the frame it is clicked. */
/* stock_selectable -- the reference render over gui_comp_selectable: transparent when idle so the
   surface behind shows through, HOT on hover/nav, ACTIVE when *selected, with a left-aligned
   label.  The component owns the press + the *selected toggle; this draws the row.  True on the
   frame it is clicked. */
bool
gui_stock_selectable( gui_rect_t r, const char* label, bool* selected )
{
    gui_comp_selectable_t s = gui_comp_selectable( label, r, selected );

    bool on = ( selected && *selected );
    if ( on || s.state.hover || s.state.nav )
        gui_draw_rect( r.x, r.y, r.w, r.h, on ? EL_COL( BG, ACTIVE ) : EL_COL( BG, HOT ) );

    char        vis[ 128 ];
    const char* text = el_visible_text( label, vis, sizeof vis );
    gui_rect_t  tr   = { r.x + s_el_style.pad, r.y, r.w - 2.0f * s_el_style.pad, r.h };
    gui_draw_text_in( tr, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, EL_COL( TEXT, IDLE ), text );

    return s.clicked;
}

/* el_selectable -- the prior name; delegates to the stock render over the component. */
bool
gui_el_selectable( gui_rect_t r, const char* label, bool* selected )
{
    return gui_stock_selectable( r, label, selected );
}

#undef EL_COL

// clang-format on
/*============================================================================================*/
