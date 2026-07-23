/*==============================================================================================

    runtime_service/gui/element/gui_element_core.c -- The el_* rect-consuming widget cores.

    The building-block tier, lifted from the proven
    sb_gui_diablo ui layer.  Every el_* fills EXACTLY the rect it is handed -- no hidden
    padding, no flow, no layout reservation -- and composes the three ambient services:
    behavior (gui_item over the caller rect), presentation (the public draw_* surface),
    and the slim element style (gui_el_style_t, gui_element.h).

    Style contract: elements read ONLY s_el_style.  Every style landing (gui_style_apply /
    theme_set / theme_reset / font activation) re-installs it through el_style_install(): the
    registered style source when a kit owns the look (gui_style_source_set), else the default
    S2 compile (el_style_derive).  A kit may also poke values ad hoc via gui_el_style().
    Elements never see a theme, a style stack, or a gui_col_t slot.

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

/* col shorthand for the element bodies below */
#define EL_COL( role, state ) s_el_style.col[ GUI_EL_##role ][ GUI_EL_##state ]

/* Interaction state -> palette state for a body fill / border line. */
static gui_el_state_t
el_state( gui_item_state_t st )
{
    return st.active           ? GUI_EL_ACTIVE
         : st.hover || st.nav  ? GUI_EL_HOT
                               : GUI_EL_IDLE;
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

/* Framed press element: id from the label ("##"/"###" rules apply).  True on click. */
bool
gui_el_button( gui_rect_t r, const char* label )
{
    gui_item_state_t st = gui_item( label, r );
    gui_el_state_t   s  = el_state( st );

    gui_draw_frame( r, s_el_style.col[ GUI_EL_BG ][ s ],
                    ( st.hover || st.nav ) ? EL_COL( BORDER, HOT ) : EL_COL( BORDER, IDLE ),
                    s_el_style.border_w );

    /* Display honors the label grammar: the "##id" suffix carries identity, never pixels. */
    const char* text = label;
    char        vis[ 128 ];
    u32         n = label_vis_len( label );
    if ( label[ n ] != '\0' )
    {
        if ( n >= sizeof( vis ) ) n = sizeof( vis ) - 1;
        for ( u32 i = 0; i < n; ++i ) vis[ i ] = label[ i ];
        vis[ n ] = '\0';
        text = vis;
    }
    gui_draw_text_in( r, GUI_ALIGN_CENTER, EL_COL( TEXT, IDLE ), text );

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
    gui_draw_frame( handle, s_el_style.col[ GUI_EL_BG ][ s ],
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

#undef EL_COL

// clang-format on
/*============================================================================================*/
