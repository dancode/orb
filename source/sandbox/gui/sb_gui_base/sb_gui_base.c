/*==============================================================================================

    sandbox/gui/sb_gui_base/sb_gui_base.c -- the strata curriculum, bottom up.

    The bottom-up proof that every stratum below GUI_CHROME stands alone: each number key
    demonstrates ONE tier using nothing above it (fenced by want_capture_keyboard):

        0  idle        -- empty frame through the whole pipeline (boot + frame loop only)
        1  GUI_DRAW    -- bare draw_* primitives; no surface, no ids, no style
        2  GUI_SURFACE -- pane + feat_move/feat_resize, and a custom widget from
                          rect + item() + draw_* (custom chrome is composition)
        3  GUI_STOCK   -- carve one flat form into leaf rects, fill them with stock_* renders
        4  GUI_FLOW    -- region_begin + a layout header + flow_cell rects feeding stock_*
        5  GUI_STYLE   -- a kit promotes its own palette over the stock_* set
        6  GUI_CHROME  -- one stock window for contrast: the optional policy layer

    Also the reference for what gui()->boot sets up: the minimal loop is boot -> boot_poll ->
    frame_begin -> ctx_begin .. ctx_end -> frame_end -> the boot_present pair -> boot_pace.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    Shared ink
==============================================================================================*/

#define INK       GUI_COLOR( 0xE8, 0xE0, 0xD0, 0xFF )
#define INK_DIM   GUI_COLOR( 0x8A, 0x88, 0x80, 0xFF )
#define AMBER     GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF )
#define TEAL      GUI_COLOR( 0x20, 0xC0, 0xB0, 0xFF )
#define PLUM      GUI_COLOR( 0xB0, 0x60, 0xE0, 0xFF )
#define PANEL_BG  GUI_COLOR( 0x24, 0x26, 0x2B, 0xFF )
#define PANEL_LN  GUI_COLOR( 0x50, 0x54, 0x5C, 0xFF )
#define TITLE_BG  GUI_COLOR( 0x34, 0x38, 0x40, 0xFF )

/*==============================================================================================
    Tier 1 -- GUI_DRAW: primitives under the ambient clip/z.  No surface, no ids, no style.
==============================================================================================*/

static void
tier_draw( void )
{
    gui()->draw_text( 100.0f, 70.0f, INK,
                      "GUI_DRAW -- primitives straight into the draw list; no surface, no ids, no style" );

    gui()->draw_rect( 100.0f, 100.0f, 200.0f, 120.0f, AMBER );
    gui()->draw_round_rect( ( gui_rect_t ){ 320.0f, 100.0f, 200.0f, 120.0f },
                            12.0f, 12.0f, 12.0f, 12.0f, false, 2.0f, TEAL );
    gui()->draw_gradient( ( gui_rect_t ){ 540.0f, 100.0f, 200.0f, 120.0f }, AMBER, PLUM, true );
    gui()->draw_circle( 840.0f, 160.0f, 56.0f, false, 3.0f, INK );

    gui()->draw_line( 100.0f, 270.0f, 740.0f, 300.0f, 2.0f, INK );
    gui()->draw_dashed_line( 100.0f, 300.0f, 740.0f, 330.0f, 8.0f, 6.0f, 2.0f, TEAL );

    /* the retained path form: accumulate points, stroke once */
    for ( i32 i = 0; i <= 12; ++i )
        gui()->path_line_to( 100.0f + ( f32 )i * 54.0f, 380.0f + ( ( i & 1 ) ? 26.0f : -26.0f ) );
    gui()->path_stroke( 2.0f, GUI_STROKE_CENTER, false, PLUM );

    gui()->draw_checker( ( gui_rect_t ){ 100.0f, 440.0f, 200.0f, 90.0f }, 12.0f,
                         GUI_COLOR( 0x30, 0x30, 0x36, 0xFF ), GUI_COLOR( 0x3C, 0x3C, 0x44, 0xFF ) );
    gui()->draw_text_shadow( 320.0f, 470.0f, "shadowed text", INK,
                             GUI_COLOR( 0x00, 0x00, 0x00, 0xC0 ), 2.0f, 2.0f );
}

/*==============================================================================================
    Tier 2 -- GUI_SURFACE + GUI_CORE: a pane is identity + clip + the z contest, nothing else.
    Chrome features are freestanding feat_* mechanisms; a custom widget is rect+item()+draw_*.
==============================================================================================*/

static void
tier_surface( void )
{
    static gui_rect_t s_rect = { 140.0f, 120.0f, 320.0f, 210.0f };  // caller-owned persistence
    static bool       s_on   = false;

    gui_pane_t p = gui()->pane_begin( "t2_pane", s_rect, GUI_REGION_MID, GUI_VP_MAIN,
                                      GUI_WIN_NONE );
    gui_rect_t r = p.rect;

    /* the pane painted nothing -- every pixel is ours */
    gui()->draw_frame( r, PANEL_BG, PANEL_LN, 1.0f );

    gui_rect_t title = gui_rect_cut_top( &r, 26.0f );
    gui()->feat_move( p.id, title, &s_rect.x, &s_rect.y );
    gui()->feat_resize( p.id, &s_rect, GUI_RESIZE_R | GUI_RESIZE_B, 240.0f, 150.0f );
    gui()->draw_rect( title.x, title.y, title.w, title.h, TITLE_BG );
    gui()->draw_text_in( title, GUI_ALIGN_CENTER, INK, "pane + feat_move / feat_resize" );

    /* a custom widget: a rect we carved + item() behavior + draw_* presentation */
    gui_rect_t body = gui_rect_pad( r, 10.0f );
    gui_rect_t box  = gui_rect_cut_top( &body, 34.0f );
    gui_item_state_t st = gui()->item( "t2_toggle", box );
    if ( st.clicked ) { s_on = !s_on; gui()->request_redraw(); }

    /* item_phase folds the interaction flags into the one three-way rule every render uses
       (ACTIVE / HOT / IDLE, nav counting as HOT) -- so a widget of your own picks its face
       exactly the way a stock one does, without re-deriving the mapping. */
    static const u32 FACE[ GUI_PHASE_COUNT ] = {
        [ GUI_PHASE_IDLE   ] = GUI_COLOR( 0x30, 0x32, 0x38, 0xFF ),
        [ GUI_PHASE_HOT    ] = GUI_COLOR( 0x40, 0x40, 0x48, 0xFF ),
        [ GUI_PHASE_ACTIVE ] = GUI_COLOR( 0x50, 0x38, 0x18, 0xFF ),
        [ GUI_PHASE_INERT  ] = GUI_COLOR( 0x28, 0x28, 0x2C, 0xFF ),
    };
    gui()->draw_frame( box, FACE[ gui()->item_phase( st ) ], s_on ? AMBER : PANEL_LN, 1.0f );
    gui()->draw_text_in( box, GUI_ALIGN_CENTER, INK, s_on ? "ON  -- click me" : "OFF -- click me" );

    /* hover and active are LEVELS -- true for as long as the condition holds, so reading them
       straight off st is enough.  pressed and clicked are EDGES: each is true for exactly one
       frame (the button going down, and the release completing over the item).  A live readout
       of an edge therefore reads 0 essentially always -- the single frame carrying the 1 is
       replaced before an eye can catch it.  So the edges are latched instead: a running count,
       plus a lit marker held HOLD seconds past the edge.  request_redraw during the hold keeps
       frames coming while the mouse sits still, so the marker actually goes back out. */

    static const f64 HOLD      = 0.6;
    static u32       s_press_n = 0, s_click_n = 0;
    static f64       s_press_t = -1000.0, s_click_t = -1000.0;

    f64 now = gui()->get_time();
    if ( st.pressed ) { s_press_n++; s_press_t = now; }
    if ( st.clicked ) { s_click_n++; s_click_t = now; }

    bool press_lit = ( now - s_press_t ) < HOLD;
    bool click_lit = ( now - s_click_t ) < HOLD;
    if ( press_lit || click_lit ) gui()->request_redraw();

    char readout[ 64 ];
    snprintf( readout, sizeof readout, "level:  hover %d   active %d", st.hover, st.active );
    body.y += 8.0f;
    gui()->draw_text( body.x, body.y, INK_DIM, readout );

    body.y += gui()->text_size( readout ).y + 4.0f;
    snprintf( readout, sizeof readout, "edge:   pressed %u", s_press_n );
    gui()->draw_text( body.x, body.y, press_lit ? AMBER : INK_DIM, readout );

    f32 gap = gui()->text_size( "edge:   pressed 0000   " ).x;
    snprintf( readout, sizeof readout, "clicked %u", s_click_n );
    gui()->draw_text( body.x + gap, body.y, click_lit ? AMBER : INK_DIM, readout );

    gui()->pane_end();
}

/*==============================================================================================
    Tier 3 -- GUI_RECT + GUI_STOCK: one flat carve form -> leaf rects -> stock_* renders.
    Every stock render fills exactly the rect it is handed; the layout is data.  The slider pair and
    the button pair each share ONE logic (gui()->comp_slider / comp_button): a stock reference
    render beside a custom render -- the component / stock / user-widget sibling model, side by side.
==============================================================================================*/

static void
tier_stock( void )
{
    static bool s_check  = true;
    static f32  s_level  = 0.35f;
    static f32  s_custom = 0.5f;
    static i32  s_mode   = 0;
    static const char* const s_modes[] = { "alpha", "beta", "gamma" };

    gui_rect_t area = { 140.0f, 120.0f, 480.0f, 300.0f };
    gui_pane_t p = gui()->pane_begin( "t3_pane", area, GUI_REGION_MID, GUI_VP_MAIN,
                                      GUI_WIN_NONE );
    gui()->draw_frame( p.rect, PANEL_BG, PANEL_LN, 1.0f );

    /* sidebar | ( header / body / footer ) from one form */
    static const f32 FORM[] = { GUI_CUT_X, 140.0f, 1.0f,
                                    GUI_CUT_Y, 30.0f, 1.0f, 36.0f, GUI_END,
                                GUI_END };
    gui_rect_t leaf[ 4 ];
    gui()->carve( FORM, gui_rect_pad( p.rect, 8.0f ), 6.0f, leaf, 4 );

    gui()->stock_panel( leaf[ 0 ] );
    gui_rect_t side = gui_rect_pad( leaf[ 0 ], 8.0f );
    gui()->stock_check( gui_rect_cut_top( &side, 26.0f ), "t3_check", &s_check );
    side.y += 6.0f;  side.h -= 6.0f;
    gui()->stock_cycle( gui_rect_cut_top( &side, 26.0f ), "t3_cycle", &s_mode, s_modes, 3 );

    gui()->stock_label( leaf[ 1 ], GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                     "stock_* renders over carved rects" );

    static char s_name[ 48 ] = "stock_input";
    gui()->stock_panel( leaf[ 2 ] );
    gui_rect_t rows = gui_rect_pad( leaf[ 2 ], 8.0f );

    /* the reference render: stock_slider (over gui_comp_slider) */

    gui()->stock_slider( gui_rect_cut_top( &rows, 24.0f ), "t3_slider", &s_level, 0.0f, 1.0f );
    
    /* a bar filling a meter based on slider value */
    rows.y += 6.0f;  rows.h -= 6.0f;
    gui()->stock_meter( gui_rect_cut_top( &rows, 18.0f ), s_level, AMBER );
    rows.y += 6.0f;  rows.h -= 6.0f;

    /* a USER widget over the SAME component: a thin groove + round handle.  stock_slider above
       and this differ only in these draw_* calls -- one logic (gui()->comp_slider), two looks. */

    gui_rect_t cs = gui_rect_cut_top( &rows, 24.0f );

    /* comp_slider( id, rect, v, lo, hi ) is the plain form; _ex takes the desc when a component
       needs more than the common case -- here a wider handle than the default. */
    gui_comp_slider_t sl = gui()->comp_slider_ex( &( gui_comp_slider_desc_t ){
        .id_str = "t3_comp", .rect = cs, .v = &s_custom, .lo = 0.0f, .hi = 1.0f, .handle_w = 14.0f } );
    
    f32 gy = cs.y + cs.h * 0.5f;
    gui()->draw_rect( cs.x, gy - 2.0f, cs.w, 4.0f, PANEL_LN );
    if ( sl.fill.w > 0.0f )
        gui()->draw_rect( sl.fill.x, gy - 2.0f, sl.fill.w, 4.0f, TEAL );

    gui()->draw_circle( sl.handle.x + sl.handle.w * 0.5f, gy, 7.0f, true, 0.0f,
                        ( sl.state.hover || sl.state.active ) ? AMBER : INK );

    /* input box */
    rows.y += 6.0f;  rows.h -= 6.0f;
    gui()->stock_input( gui_rect_cut_top( &rows, 26.0f ), "t3_input", s_name, sizeof s_name );

    /* the footer holds a matching button pair: the reference stock_button and a USER button --
       both over the SAME gui()->comp_button, one logic and two looks (as the sliders above). */
    gui_rect_t foot = leaf[ 3 ];
    gui_rect_t bl   = gui_rect_cut_left( &foot, foot.w * 0.5f - 3.0f );
    foot.x += 6.0f;  foot.w -= 6.0f;   /* gap between the pair */

    gui()->stock_button( bl, "stock_button" );

    /* a USER button: a rounded pill over the same component.  style_color reads the INSTALLED
       palette through the style stack -- the same seam stock_button and chrome read -- so this
       fork tracks the theme and honors a push_style_color exactly as the stock render does,
       while owning its own shape.  (Hard-coding colors here instead would opt out of both.) */
    gui_comp_button_t cb    = gui()->comp_button( "t3_pill", foot );
    gui_style_phase_t    phase = gui()->item_phase( cb.state );
    gui()->draw_round_rect( foot, 8.0f, 8.0f, 8.0f, 8.0f, true, 0.0f,
                            gui()->style_color( GUI_ROLE_BG, phase ) );
    gui()->draw_text_in( foot, GUI_ALIGN_CENTER, gui()->style_color( GUI_ROLE_TEXT_PRIMARY, phase ),
                         "comp_button" );

    gui()->pane_end();
}

/*==============================================================================================
    Tier 4 -- GUI_FLOW: a region owns scroll + a layout; flow_cell hands rects back out to stock_*.
==============================================================================================*/

static void
tier_flow( void )
{
    gui()->region_begin( "t4_region", 140.0f, 120.0f, 380.0f, 320.0f, GUI_REGION_MID,
                         GUI_VP_MAIN, GUI_WIN_NONE );
    gui()->row( 26.0f );

    for ( i32 i = 0; i < 30; ++i )
    {
        char label[ 32 ];
        gui()->push_id_int( i );
        gui_rect_t cell = gui()->flow_cell( 0.0f, 0.0f );
        if ( i % 5 == 0 )
        {
            snprintf( label, sizeof label, "stock_button %d", i );
            gui()->stock_button( cell, label );
        }
        else
        {
            snprintf( label, sizeof label, "row %d -- the region scrolls", i );
            gui()->stock_label( cell, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, label );
        }
        gui()->pop_id();
    }

    gui()->region_end();
}

/*==============================================================================================
    Tier 5 -- GUI_STYLE: a kit PROMOTES its own look.  A registered source is invoked immediately
    and again at every style landing (font / theme / scale), so the palette is re-derived instead
    of clobbered by the chrome theme compiler.  Two targets, on the pane's toggle: style_source_set
    owns the DEFAULT set (chrome included), style_set_create owns a set of the kit's own that only
    its pane is bracketed with -- co-existence, both looks installed at once.  Entering/leaving the
    tier registers/restores -- see build_frame.
==============================================================================================*/

static i32 s_t5_pal = 0;

static void
install_palette( i32 which )
{
    gui_style_t* es = gui()->style_edit();

    es->var[ GUI_VAR_PAD    ] = 6.0f;
    es->var[ GUI_VAR_GAP    ] = 6.0f;
    es->var[ GUI_VAR_BORDER ] = 1.0f;

    /* Two looks, each authored as SEEDS and derived -- not as a hand-typed grid.  This used to be
       two 32-literal tables memcpy'd straight over es->col, which is precisely the duplication the
       seed palette exists to retire: a quarter of those cells were restatements of the others, and
       the two tables restated the same ones.  It also could not survive the schema growing -- a
       fixed-size literal grid silently under-fills the moment a role is added.

       Seven seeds and a ramp instead.  The status hues are deliberately NOT set: a kit's style is
       seeded from the active theme before this runs, so INFO / OK / WARN / ERROR arrive already
       filled and a kit inherits a severity ladder it has no opinion about. */

    if ( which == 0 )   /* ember -- warm dark */
    {
        es->palette.seed[ GUI_SEED_SURFACE ] = GUI_COLOR( 0x24, 0x1A, 0x14, 0xFF );
        es->palette.seed[ GUI_SEED_CONTROL ] = GUI_COLOR( 0x2E, 0x20, 0x18, 0xFF );
        es->palette.seed[ GUI_SEED_INK     ] = GUI_COLOR( 0xF0, 0xDC, 0xC0, 0xFF );
        es->palette.seed[ GUI_SEED_LINE    ] = GUI_COLOR( 0x7A, 0x50, 0x2C, 0xFF );
        es->palette.seed[ GUI_SEED_ACCENT  ] = GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF );
        es->palette.seed[ GUI_SEED_MARK    ] = GUI_COLOR( 0xFF, 0xC8, 0x68, 0xFF );
        es->palette.seed[ GUI_SEED_GRAB    ] = GUI_COLOR( 0xE8, 0xD4, 0xB8, 0xFF );
    }
    else                /* ice -- cool dark */
    {
        es->palette.seed[ GUI_SEED_SURFACE ] = GUI_COLOR( 0x14, 0x1C, 0x26, 0xFF );
        es->palette.seed[ GUI_SEED_CONTROL ] = GUI_COLOR( 0x18, 0x22, 0x2E, 0xFF );
        es->palette.seed[ GUI_SEED_INK     ] = GUI_COLOR( 0xC8, 0xE0, 0xF0, 0xFF );
        es->palette.seed[ GUI_SEED_LINE    ] = GUI_COLOR( 0x34, 0x58, 0x7A, 0xFF );
        es->palette.seed[ GUI_SEED_ACCENT  ] = GUI_COLOR( 0x30, 0xC0, 0xE8, 0xFF );
        es->palette.seed[ GUI_SEED_MARK    ] = GUI_COLOR( 0x78, 0xE0, 0xFF, 0xFF );
        es->palette.seed[ GUI_SEED_GRAB    ] = GUI_COLOR( 0xC8, 0xE0, 0xF0, 0xFF );
    }

    /* Both kits share one personality: a high-contrast dark look with a deep press. */
    es->palette.ramp[ GUI_RAMP_HOVER  ] = 0.35f;
    es->palette.ramp[ GUI_RAMP_PRESS  ] = 0.55f;
    es->palette.ramp[ GUI_RAMP_FADE   ] = 0.45f;
    es->palette.ramp[ GUI_RAMP_RECESS ] = 0.30f;
    es->palette.ramp[ GUI_RAMP_NEST   ] = 0.30f;
    es->palette.ramp[ GUI_RAMP_STEP   ] = 0.14f;
    es->palette.ramp[ GUI_RAMP_SELECT ] = 0.50f;

    gui()->style_bake( es );
}

/* the registered owner: re-derives the kit look at every style landing.  ONE source serves both
   modes below -- it writes through style_edit(), which during an install points at whichever set
   is being filled, so the same function installs into set 0 or into the kit's own set. */
static void
t5_style_source( void* user )
{
    UNUSED( user );
    install_palette( s_t5_pal );
}

/*----------------------------------------------------------------------------------------------
    The two ways to own a look, on a toggle so the difference is visible rather than described:

      SET 0    style_source_set -- the kit owns the DEFAULT set, so everything resolves through
               it: the pane, and the chrome around it too.
      OWN SET  style_set_create + push/pop -- the kit owns a set of its own and brackets only its
               pane.  Chrome keeps the theme; both looks stay installed at once.

    Flip it and watch the hint line and tier chrome revert while the pane holds its palette.
----------------------------------------------------------------------------------------------*/

static bool            s_t5_scoped = false;                    // false = own set 0
static gui_style_set_t s_t5_set    = GUI_STYLE_SET_DEFAULT;    // created on first scoped entry

/* Point the kit's look at whichever target the toggle selects, and clear the other one. */
static void
t5_own( bool scoped )
{
    if ( scoped )
    {
        gui()->style_source_set( NULL, NULL );          /* hand set 0 back to the theme */
        if ( s_t5_set == GUI_STYLE_SET_DEFAULT )
            s_t5_set = gui()->style_set_create( t5_style_source, NULL );
    }
    else
    {
        gui()->style_source_set( t5_style_source, NULL );
    }
}

static void
tier_style( void )
{
    static bool s_check  = true;
    static f32  s_level  = 0.6f;
    static const char* const s_pals[] = { "ember", "ice" };

    if ( s_t5_scoped )
        gui()->style_set_push( s_t5_set );

    gui_rect_t area = { 140.0f, 120.0f, 380.0f, 280.0f };
    gui_pane_t p = gui()->pane_begin( "t5_pane", area, GUI_REGION_MID, GUI_VP_MAIN,
                                      GUI_WIN_NONE );
    gui()->stock_panel( p.rect );

    gui_rect_t r = gui_rect_pad( p.rect, 10.0f );
    gui()->stock_label( gui_rect_cut_top( &r, 26.0f ), GUI_ALIGN_LEFT,
                     s_t5_scoped ? "my own set -- chrome keeps the theme"
                                 : "the SAME stock_* renders, the kit palette" );
    r.y += 6.0f;  r.h -= 6.0f;
    if ( gui()->stock_check( gui_rect_cut_top( &r, 26.0f ), "t5_scope", &s_t5_scoped ) )
        t5_own( s_t5_scoped );
    r.y += 6.0f;  r.h -= 6.0f;
    if ( gui()->stock_cycle( gui_rect_cut_top( &r, 26.0f ), "t5_pal", &s_t5_pal, s_pals, 2 ) )
        gui()->style_apply();          /* a landing: every set re-derives, ours through its source */
    r.y += 6.0f;  r.h -= 6.0f;
    gui()->stock_check( gui_rect_cut_top( &r, 26.0f ), "t5_check", &s_check );
    r.y += 6.0f;  r.h -= 6.0f;
    gui()->stock_slider( gui_rect_cut_top( &r, 24.0f ), "t5_slider", &s_level, 0.0f, 1.0f );
    r.y += 6.0f;  r.h -= 6.0f;
    gui()->stock_meter( gui_rect_cut_top( &r, 18.0f ), s_level,
                     gui()->style_color( GUI_ROLE_ACCENT, GUI_PHASE_IDLE ) );
    r.y += 6.0f;  r.h -= 6.0f;
    static char s_field[ 48 ] = "kit-styled field";
    gui()->stock_input( gui_rect_cut_top( &r, 26.0f ), "t5_input", s_field, sizeof s_field );
    r.y += 6.0f;  r.h -= 6.0f;
    gui()->stock_button( gui_rect_cut_top( &r, 30.0f ), "stock_button" );

    gui()->pane_end();

    if ( s_t5_scoped )
        gui()->style_set_pop();
}

/*==============================================================================================
    Tier 6 -- GUI_CHROME: the optional policy layer, for contrast.  One stock window; everything
    it does is assembled from the tiers below.
==============================================================================================*/

static void
tier_chrome( void )
{
    static char s_buf[ 64 ] = "type here";
    static bool s_flag      = true;
    static f32  s_value     = 0.5f;

    gui()->window_set_next_pos ( 160.0f, 140.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 380.0f, 260.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "GUI_CHROME -- optional policy", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "A stock window: pane + feat_* + flow +" );
        gui()->text( "stock_* + style, assembled as policy." );
        gui()->button( "button" );
        gui()->checkbox( "checkbox", &s_flag );
        gui()->slider_float( "slider", &s_value, 0.0f, 1.0f );
        gui()->input_text( "input", s_buf, sizeof s_buf );
    }
    gui()->window_end();
}

/*==============================================================================================
    Build -- key routing, tier dispatch, hint line
==============================================================================================*/

static i32 s_tier = 1;

static void
build_frame( void )
{
    /* number-key tier switching, fenced so typing in tier 6's field never switches */
    if ( !gui()->want_capture_keyboard() )
        for ( i32 k = 0; k <= 6; ++k )
            if ( gui()->is_key_pressed( ( app_key_t )( APP_KEY_0 + k ) ) )
                s_tier = k;

    /* entering the style tier promotes the kit as style owner -- of the default set, or of a set
       of its own (the tier's toggle); leaving hands set 0 back to chrome's theme compiler.  A set
       the kit created stays created: it is installed alongside chrome's, not instead of it, so
       there is nothing to tear down. */
    static i32 s_prev = 1;
    if ( s_tier == 5 && s_prev != 5 )
        t5_own( s_t5_scoped );
    if ( s_prev == 5 && s_tier != 5 )
        gui()->style_source_set( NULL, NULL );
    s_prev = s_tier;

    gui()->draw_text( 12.0f, 8.0f, INK_DIM,
        "sb_gui_base -- 1 draw  2 surface  3 stock  4 flow  5 style  6 chrome  0 idle" );

    switch ( s_tier )
    {
        case 1: tier_draw();    break;
        case 2: tier_surface(); break;
        case 3: tier_stock();   break;
        case 4: tier_flow();    break;
        case 5: tier_style();   break;
        case 6: tier_chrome();  break;
        default:                break;
    }
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_base] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* One-call setup: gui owns the main window + render context end to end. */
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui base",
        .w         = 1280, .h = 720,
        .os_chrome = true,   /* stock OS-framed window instead of the gui-driven borderless viewport */
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.15f, 0.15f, 0.20f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_base] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();

        /* The boot pair: begin opens the main surface's frame (cleared to the boot color), end
           draws the gui and presents.  This is what places the 2D frame in the backend. */
        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        gui()->boot_pace ( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();   /* also tears down the boot window + context */
    rhi()->shutdown();                                 /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

// clang-format on
