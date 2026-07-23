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

/* The stock press button: a flat, hover/press-animated fill + a centered (or ellipsized) label;
   id from the label ("##"/"###" rules apply).  col_item_bg_anim rides the keyed damper over the
   SAME element palette style_el_col resolves, so el and chrome animate alike -- this IS the face
   chrome's gui_button now delegates to.  True on click. */
bool
gui_el_button( gui_rect_t r, const char* label )
{
    gui_id_t         id = item_id( label );
    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_fill( r, col_item_bg_anim( id, st ) );

    char vis[ 128 ];
    el_button_label( r, el_visible_text( label, vis, sizeof vis ) );

    /* A click is a host state change this build cannot show yet -- ask for the next emit, or
       the retained cache replays the stale screen until the mouse moves. */
    if ( st.clicked )
        gui_request_redraw();
    return st.clicked;
}

/* Square toggle inscribed centered in r (side = min(r.w, r.h)).  True on the change frame. */
bool
gui_el_check( gui_rect_t r, const char* id_str, bool* v )
{
    f32        side = ( r.w < r.h ) ? r.w : r.h;
    gui_rect_t box  = gui_rect_align( r, side, side, GUI_ALIGN_CENTER );

    gui_item_state_t st = gui_item( id_str, box );
    if ( st.clicked )
    {
        *v = !*v;
        gui_request_redraw();
    }

    gui_draw_frame( box, EL_COL( BG, DIM ),
                    ( st.hover || st.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE ),
                    s_el_style.border_w );
    if ( *v )
        gui_draw_check_mark( gui_rect_pad( box, side * 0.22f ), EL_COL( ACCENT, IDLE ) );

    return st.clicked;
}

/* Horizontal drag track filling r; keyboard nav steps 5% per arrow.  The bare control -- the
   caller draws the value text from *v where it wants it.  True on the change frame. */
bool
gui_el_slider( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi )
{
    gui_item_state_t st  = gui_item( id_str, r );
    f32              old = *v;

    if ( st.active )    /* captured drag: value follows the cursor across the track */
    {
        f32 mx, my;
        gui_get_mouse_pos( &mx, &my );
        f32 t = ( r.w > 0.0f ) ? ( mx - r.x ) / r.w : 0.0f;
        t  = ( t < 0.0f ) ? 0.0f : ( t > 1.0f ) ? 1.0f : t;
        *v = lo + t * ( hi - lo );
    }
    if ( st.nav_adjust )
        *v += (f32)st.nav_adjust * ( hi - lo ) * 0.05f;
    *v = ( *v < lo ) ? lo : ( *v > hi ) ? hi : *v;

    f32 frac = ( hi > lo ) ? ( *v - lo ) / ( hi - lo ) : 0.0f;

    gui_rect_t track = gui_rect_align( r, r.w, r.h * 0.30f, GUI_ALIGN_CENTER );
    gui_draw_frame( track, EL_COL( ACCENT, DIM ), EL_COL( BORDER, DIM ), 1.0f );
    gui_rect_t fill = gui_rect_pad( track, 1.0f );
    fill.w *= frac;
    if ( fill.w > 0.0f )
        gui_draw_rect( fill.x, fill.y, fill.w, fill.h, EL_COL( ACCENT, IDLE ) );

    gui_rect_t handle = { r.x + frac * ( r.w - 8.0f ), r.y + r.h * 0.10f, 8.0f, r.h * 0.80f };
    gui_el_state_t s  = el_state( st );
    gui_draw_frame( handle, style_el_col( GUI_EL_BG, s ),
                    ( s != GUI_EL_IDLE ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE ), 1.0f );

    if ( *v != old )
        gui_request_redraw();
    return *v != old;
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

/* The "< value >" selector: square chevron caps at r's ends, items[*idx] centered between;
   wraps.  Rect-in-element carving (the caps) is gui_rect math at element scale.  The two caps
   id under a push of id_str, so repeated cycles need distinct id_str (or a caller push_id). */
bool
gui_el_cycle( gui_rect_t r, const char* id_str, i32* idx, const char* const* items, i32 count )
{
    gui_rect_t lbox = gui_rect_cut_left ( &r, r.h );
    gui_rect_t rbox = gui_rect_cut_right( &r, r.h );

    gui_push_id( id_str );
    i32 old = *idx;
    gui_item_state_t ls = gui_item( "##el_cyc_l", lbox );
    gui_item_state_t rs = gui_item( "##el_cyc_r", rbox );
    gui_pop_id();
    if ( ls.clicked && count > 0 ) *idx = ( *idx + count - 1 ) % count;
    if ( rs.clicked && count > 0 ) *idx = ( *idx + 1 ) % count;

    u32 lb = ( ls.hover || ls.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE );
    u32 rb = ( rs.hover || rs.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE );
    gui_draw_frame( lbox, EL_COL( BG, DIM ), lb, s_el_style.border_w );
    gui_draw_frame( rbox, EL_COL( BG, DIM ), rb, s_el_style.border_w );
    gui_draw_chevron( gui_rect_pad( lbox, lbox.w * 0.30f ), GUI_DIR_LEFT,  2.0f, EL_COL( TEXT, IDLE ) );
    gui_draw_chevron( gui_rect_pad( rbox, rbox.w * 0.30f ), GUI_DIR_RIGHT, 2.0f, EL_COL( TEXT, IDLE ) );

    if ( count > 0 )
        gui_draw_text_in( r, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), items[ *idx ] );

    if ( *idx != old )
        gui_request_redraw();
    return *idx != old;
}

/* Single-line text field filling r: the shared edit ENGINE (interact/gui_edit.c -- keys, mouse
   selection drag, clipboard, undo, horizontal scroll, caret blink) under an element-styled box.
   The proof the engine needs no chrome: this core and chrome's input_text drive the same
   edit_field(); only the paint differs.  A press claims keyboard focus (ITEM_FOCUSABLE); the
   engine owns every mutation and the redraw; this core paints selection / run / caret from the
   keyed slot the engine resolved.  True on any frame the buffer changes. */
bool
gui_el_input( gui_rect_t r, const char* id_str, char* buf, u32 bufsz )
{
    gui_id_t         id = item_id( id_str );
    gui_item_state_t st = item_state( id, r, ITEM_FOCUSABLE );

    gui_draw_frame( r,
                    st.focused ? EL_COL( BG, ACTIVE ) : style_el_col( GUI_EL_BG, el_state( st ) ),
                    ( st.focused || st.hover || st.nav ) ? EL_COL( BORDER, HOT )
                                                         : EL_COL( BORDER, IDLE ),
                    s_el_style.border_w );

    /* The engine works in the content rect (box inset by the element pad on left / right) so it
       never sees widget padding; it leaves cursor / anchor / scroll_x / blink_t on the keyed slot. */
    gui_rect_t content = { r.x + s_el_style.pad, r.y, r.w - 2.0f * s_el_style.pad, r.h };
    input_field_result_t res = edit_field( id, content, st, buf, bufsz );

    /* Paint the resolved state: selection band, glyph-clipped run, blinking caret. */
    const gui_edit_state_t* es = GUI_STATE( gui_edit_state_t, id );

    f32 text_x  = content.x - es->scroll_x;
    f32 text_y  = text_center_y( content.y, content.h );
    f32 clip_x0 = content.x;
    f32 clip_x1 = content.x + content.w;

    if ( st.focused )
    {
        u32  sel_lo, sel_hi;
        bool has_sel;
        edit_sel( es, &sel_lo, &sel_hi, &has_sel );
        if ( has_sel )
        {
            f32 sx0 = text_x + text_x_at( buf, sel_lo );
            f32 sx1 = text_x + text_x_at( buf, sel_hi );
            if ( sx0 < clip_x0 ) sx0 = clip_x0;
            if ( sx1 > clip_x1 ) sx1 = clip_x1;
            if ( sx1 > sx0 )
                draw_fill( ( gui_rect_t ){ sx0, content.y + 1.0f, sx1 - sx0, content.h - 2.0f },
                           EL_COL( ACCENT, DIM ) );
        }
    }

    draw_push_text_clip_n( text_x, text_y, EL_COL( TEXT, IDLE ), buf, 0xFFFFFFFFu,
                           clip_x0, clip_x1 );

    /* Caret: visible the first half of each 1 s blink cycle; 1px column, 2px vertical inset
       (presentation constants of THIS core, not style slots -- per-widget detail is kit business). */
    if ( st.focused && ( ( (u32)( es->blink_t * 2.0f ) & 1u ) == 0u ) )
    {
        f32 cx = text_x + text_x_at( buf, es->cursor );
        draw_fill( ( gui_rect_t ){ cx, content.y + 2.0f, 1.0f, content.h - 4.0f },
                   EL_COL( ACCENT, ACTIVE ) );
    }

    return res.changed;
}

/* Full-width selectable row: transparent when idle so the surface behind shows through, the HOT
   tint on hover / nav, the ACTIVE tint when selected.  THE row primitive under lists, combos,
   trees, and menus -- the pure core, carrying none of chrome's policy (type-ahead stamp, popup /
   combo dismiss on click).  That policy stays in chrome's gui_selectable, which is free to compose
   this.  selected NULL = a click-only row (the caller drives its own index from the return).  id
   comes from the label ("##"/"###" rules apply).  True on the frame it is clicked. */
bool
gui_el_selectable( gui_rect_t r, const char* label, bool* selected )
{
    gui_item_state_t st = gui_item( label, r );

    bool on = ( selected && *selected );
    if ( on || st.hover || st.nav )
        gui_draw_rect( r.x, r.y, r.w, r.h, on ? EL_COL( BG, ACTIVE ) : EL_COL( BG, HOT ) );

    char        vis[ 128 ];
    const char* text = el_visible_text( label, vis, sizeof vis );
    gui_rect_t  tr   = { r.x + s_el_style.pad, r.y, r.w - 2.0f * s_el_style.pad, r.h };
    gui_draw_text_in( tr, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, EL_COL( TEXT, IDLE ), text );

    if ( st.clicked )
    {
        if ( selected ) *selected = !( *selected );
        gui_request_redraw();   /* a click drives a caller selection not visible until next emit */
    }
    return st.clicked;
}

#undef EL_COL

// clang-format on
/*============================================================================================*/
