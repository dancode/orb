/*==============================================================================================

    runtime_service/gui/style/gui_style_core.c -- Style stacks: colors + layout metrics.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor / PushStyleVar
    analogue.  Three layers resolve into the value a widget sees:

        Base   -- the theme default.  Colors: the active theme's palette (s_style.colors, seeded
                  from k_themes in gui_theme.c).  Vars: the font-derived metrics in s_style, read
                  live so a font change updates them.
        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.
        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.

    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; the
    chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the same
    once-per-widget boundary -- see the impure wrappers in stock/gui_adornment.c.

    The payoff is reach with no churn: the COL_*, WIDGET_*, and WIN_* vocabulary macros
    (style/gui_style.h) resolve through style_col / style_var, so every existing read site
    honors an override without changing a single widget.

    Included by gui_style.c after gui_theme.c -- s_style and GUI_COLOR (gui.h) are already
    visible.  The frame orchestrator drives the per-frame reset across the unit seam
    (gui_ctx_begin pairs ctx_new_frame with style_new_frame).

==============================================================================================*/
// clang-format off

/* Base value of a style var -- read live from the font-derived metrics so a font_load update flows
   through without re-seeding anything.  The single map from slot to s_style field, grouped by the
   two gui_style_t categories (see gui.h). */

static f32
style_var_base( gui_style_var_t v )
{
    switch ( v )
    {
        /* 1. METRICS */
        case GUI_VAR_LINE_SIZE:       return (f32)s_style.line_size;
        case GUI_VAR_WIDGET_GAP:      return (f32)s_style.widget_gap;
        case GUI_VAR_WIDGET_PAD:      return (f32)s_style.widget_pad;
        case GUI_VAR_MIN_CELL_W:      return (f32)s_style.min_cell_w;
        case GUI_VAR_WIN_BORDER:      return (f32)s_style.win_border;
        case GUI_VAR_WIN_TITLE_H:     return (f32)s_style.win_title_h;
        case GUI_VAR_CHECKBOX_SZ:     return (f32)s_style.checkbox_sz;
        case GUI_VAR_SLIDER_KNOB_W:   return (f32)s_style.slider_knob_w;

        /* 2. SKIN */
        case GUI_VAR_WIN_ROUNDING:    return (f32)s_style.win_rounding;
        case GUI_VAR_WIDGET_ROUNDING: return (f32)s_style.widget_rounding;
        case GUI_VAR_GRAB_ROUNDING:   return (f32)s_style.grab_rounding;
        case GUI_VAR_WIN_FOCUS_BORDER:return (f32)s_style.win_focus_border;
        case GUI_VAR_CHECK_STYLE:     return (f32)s_style.check_style;     /* enum-as-var: 0 tick / 1 disc / 2 cross */
        case GUI_VAR_BULLET_STYLE:    return (f32)s_style.bullet_style;    /* enum-as-var: 0 disc / 1 square */
        case GUI_VAR_ARROW_STYLE:     return (f32)s_style.arrow_style;     /* enum-as-var: 0 triangle / 1 chevron */
        case GUI_VAR_SEPARATOR_STYLE: return (f32)s_style.separator_style; /* enum-as-var: 0 solid / 1 dashed */
        case GUI_VAR_PROGRESS_STYLE:  return (f32)s_style.progress_style;  /* enum-as-var: 0 solid / 1 gradient */
        case GUI_VAR_SLIDER_KNOB:     return (f32)s_style.slider_knob;     /* enum-as-var: 0 bar / 1 circle */
        case GUI_VAR_MENU_CHECK:      return (f32)s_style.menu_check;      /* enum-as-var: 0 plain / 1 box */

        default:                      return 0.0f;
    }
}

/*==============================================================================================

    Style State -- ONE slot space, one mechanism.

    Colors and vars are the same machine over different value types, so they share one slot
    space: colors occupy [0, GUI_COL_COUNT), vars [GUI_COL_COUNT, STYLE_SLOT_COUNT) with their
    f32 carried as raw bits.  The typed accessors below are the only place the ranges are mapped.

    Working set: the base with the push/pop stacks applied -- the value an unscoped read returns.

    Stacks: saved (slot, previous) pairs so pop restores regardless of which slots a push touched.  
    
    TWO stacks, one per public pop verb: pop_style_color and pop_style_var each pop their
    own pushes, so an interleaved push_color / push_var sequence unwinds correctly 
    (the same reason Dear ImGui keeps two).

    Next-item layers: a small list of (slot, value) overrides, `next` filled by next_style_* and
    promoted to `item` (the active per-widget override) at the resolve seam, then cleared.  
    
    Both are tiny lists -- usually empty -- so a read scans only what is active.

==============================================================================================*/

#define GUI_STYLE_STACK_DEPTH 32
#define STYLE_VAR_BASE        GUI_COL_COUNT                    /* var slot range starts here */
#define STYLE_SLOT_COUNT      ( GUI_COL_COUNT + GUI_VAR_COUNT )

/* f32 <-> raw bits, so one u32 slot space carries both value types. */
static inline u32 style_f32_bits( f32 f ) { union { f32 f; u32 u; } c = { .f = f }; return c.u; }
static inline f32 style_bits_f32( u32 u ) { union { f32 f; u32 u; } c = { .u = u }; return c.f; }

typedef struct { u16 slot; u32 val; } style_pair_t;   // stack restore pair / next-item override

typedef struct
{
    style_pair_t save[ GUI_STYLE_STACK_DEPTH ];
    u32          sp;                                  // count of pushes, not index

} style_stack_t;

static u32           s_slot[ STYLE_SLOT_COUNT ];      // working set (base + stacks)
static style_stack_t s_col_stack;                     // push_style_color's stack
static style_stack_t s_var_stack;                     // push_style_var's stack

static style_pair_t  s_next[ STYLE_SLOT_COUNT ];      // next-item pending
static u32           s_next_n;
static style_pair_t  s_item[ STYLE_SLOT_COUNT ];      // active for the current item
static u32           s_item_n;

/*==============================================================================================

    The Style Mechanism -- read / push / pop / next over the one slot space.

    Over-deep pushes: the value is still written to the working set (so the UI renders correctly
    in all builds), and sp is still counted so push/pop stay paired.  The save record is only
    written when sp is within bounds -- an over-depth push cannot save and therefore cannot
    restore, which is the documented cap behaviour.  An ORB_ASSERT fires in debug builds so
    callers discover the imbalance immediately at the push site rather than on a silent bad
    restore.  pop takes a count, like ImGui.

==============================================================================================*/

/* This is what every COL_* / metric macro ultimately resolves to. */

static u32
style_read( u32 slot )
{
    /* check all single widget next items first */
    for ( u32 i = 0; i < s_item_n; ++i )
        if ( s_item[ i ].slot == (u16)slot ) return s_item[ i ].val;

    /* normal style value working set (base + push/pop) */
    return s_slot[ slot ];
}

static void
style_push( style_stack_t* st, u32 slot, u32 val )
{
    ORB_ASSERT( st->sp < GUI_STYLE_STACK_DEPTH && "style push: stack overflow -- mismatched push/pop" );
    if ( st->sp < GUI_STYLE_STACK_DEPTH )
        st->save[ st->sp ] = ( style_pair_t ){ (u16)slot, s_slot[ slot ] };
    ++st->sp;
    s_slot[ slot ] = val;
}

static void
style_pop( style_stack_t* st, u32 count )
{
    while ( count-- && st->sp )
    {
        --st->sp;
        if ( st->sp < GUI_STYLE_STACK_DEPTH )
            s_slot[ st->save[ st->sp ].slot ] = st->save[ st->sp ].val;
    }
}

/* Queue a next-item override; replaces a pending entry for the same slot rather than stacking
   duplicates.  Consumed (promoted to the item layer) at the per-item resolve seam -- no pop. */
static void
style_next( u32 slot, u32 val )
{
    for ( u32 i = 0; i < s_next_n; ++i )
        if ( s_next[ i ].slot == (u16)slot ) { s_next[ i ].val = val; return; }
    if ( s_next_n < STYLE_SLOT_COUNT )
        s_next[ s_next_n++ ] = ( style_pair_t ){ (u16)slot, val };
}

/*==============================================================================================
    Typed faces -- the color / var range mapping, in one place each.
==============================================================================================*/

u32 style_col( gui_col_t slot )       { return style_read( (u32)slot ); }
f32 style_var( gui_style_var_t slot ) { return style_bits_f32( style_read( STYLE_VAR_BASE + (u32)slot ) ); }

static void style_push_color( gui_col_t slot, u32 abgr ) 
{
    if ( slot < GUI_COL_COUNT ) 
         style_push( &s_col_stack, (u32)slot, abgr );
}
void style_push_var( gui_style_var_t slot, f32 value )
{
    if ( slot < GUI_VAR_COUNT ) 
         style_push( &s_var_stack, STYLE_VAR_BASE + (u32)slot, style_f32_bits( value ) );
}
static void style_pop_color( u32 count ) 
{ 
    style_pop( &s_col_stack, count ); 
}
void style_pop_var  ( u32 count ) 
{ 
    style_pop( &s_var_stack, count );
}

/*==============================================================================================
    Set next style color or var
==============================================================================================*/

static void style_next_color( gui_col_t slot, u32 abgr )
{
    if ( slot < GUI_COL_COUNT ) style_next( (u32)slot, abgr );
}
static void style_next_var( gui_style_var_t slot, f32 value )
{
    if ( slot < GUI_VAR_COUNT ) style_next( STYLE_VAR_BASE + (u32)slot, style_f32_bits( value ) );
}

/*==============================================================================================
    Seam hooks -- driven from OUTSIDE this unit, at the shared per-item boundary.

    style never paints and never reaches up, so it does not call these itself: the impure
    wrappers in stock/gui_adornment.c (item_flags_resolve / item_flags_chrome_reset) run them
    alongside the flag half in core/gui_ctx.c, and the frame orchestrator runs style_new_frame
    alongside ctx_new_frame.  Flags, colors, and vars therefore all latch on one boundary.
==============================================================================================*/

/* Promote the pending next-item overrides into the active per-item layer and clear the pending.
   Called once per widget from item_flags_resolve (stock/gui_adornment.c, cross-unit), so the
   override that next_style_* queued just before this widget applies for this widget's whole
   draw, then is gone for the following one. */

void
style_item_commit( void )
{
    s_item_n = s_next_n;
    for ( u32 i = 0; i < s_next_n; ++i ) s_item[ i ] = s_next[ i ];
    s_next_n = 0;
}

/* Drop the active per-item overrides before chrome draws.  Chrome (borders, scrollbars, titlebars)
   does not pass through the item seam, so without this it would inherit a lingering next-* override
   from the last body widget.  The push/pop stacks are intentionally left intact -- a push that
   brackets a window_begin / child_begin still applies to the chrome inside it, like ImGui. */

void
style_chrome_reset( void )
{
    s_item_n = 0;
}

/* Reset the per-frame style state: re-seed the working set from the base (so an unbalanced push
   cannot leak across frames), empty the stacks, and clear both next-item layers.  Called from
   gui_ctx_begin (frame/gui_frame_loop.c), paired with ctx_new_frame, and from gui_theme_reset. */

void
style_new_frame( void )
{
    for ( u32 i = 0; i < GUI_COL_COUNT; ++i )
        s_slot[ i ] = s_style.colors[ i ];

    for ( u32 i = 0; i < GUI_VAR_COUNT; ++i )
        s_slot[ STYLE_VAR_BASE + i ] = style_f32_bits( style_var_base( (gui_style_var_t)i ) );

    s_col_stack.sp = s_var_stack.sp = 0;
    s_next_n = s_item_n = 0;
}

/*==============================================================================================
    The element bridge -- resolving against the INSTALLED element style (S1)

    The three reads that source from the stock unit's element style rather than the theme
    directly.  Each layers the same way: a transient push_style_* / next_style_* override on the
    projected slot wins (chrome's own mechanism, unchanged), else the installed element style
    supplies the value -- so a kit that overwrites gui()->el_style() restyles stock widget bodies
    and flow spacing with one dial.  With no override and no kit overwrite each equals its plain
    style_col / style_var exactly, so default chrome is unchanged by construction.

    (The vocabulary macros these back -- COL_*, WIDGET_*, ROUND_* -- are defined in
    style/gui_style.h, beside the cross-unit declarations, since every tier above reads them.)
==============================================================================================*/

/* One element-shaped color: role x state projects onto its theme slot through g_gui_el_slot_map
   (the stock unit's table, shared with el_style_derive so the two directions cannot drift). */
u32
style_el_col( u8 role, u8 state )
{
    gui_col_t slot     = (gui_col_t)g_gui_el_slot_map[ role ][ state ];
    u32       resolved = style_col( slot );
    if ( resolved != s_style.colors[ slot ] )      /* stack override in effect -- it wins */
        return resolved;
    return gui_el_style()->col[ role ][ state ];   /* S1: the installed element style */
}

/* The metric twins, for the two spacing floats the rect dispatcher applies (cell_next_w's
   inter-cell gap + the region / label pad).  The override test is "resolved != the unstacked
   base", the metric form of style_el_col's colour test; zero means zero.  (pad / gap / border_w /
   line_h are the layout-style group inside gui_el_style, looked up separately from the colors --
   this is the seam the color-theme / layout-theme split falls on.) */
static f32
style_el_metric( gui_style_var_t slot, f32 el_value )
{
    f32 resolved = style_var( slot );
    if ( resolved != style_var_base( slot ) ) return resolved;   /* transient stack override wins */
    return el_value;                                             /* installed layout style (base) */
}

f32 style_el_pad( void ) { return style_el_metric( GUI_VAR_WIDGET_PAD, gui_el_style()->pad ); }
f32 style_el_gap( void ) { return style_el_metric( GUI_VAR_WIDGET_GAP, gui_el_style()->gap ); }

/*==============================================================================================
    State -> color projections -- style resolution proper.  The interact state arrives as a
    PARAMETER (the purity rule): these never query the interact server, so they resolve
    identically with no server present.
==============================================================================================*/

/* Frame-background tint for a "framed field" widget (checkbox box, slider track, drag box, input):
   hover / nav / active lift it to the shared hot / active palette entries -- one at a time, since
   hover and nav-highlight are mutually exclusive -- over a caller-supplied idle_color_enum base
   so each field keeps its own resting colour, matching how Dear ImGui's FrameBgHovered lifts every
   framed control, not just buttons. */

u32
col_frame_bg( gui_item_state_t st, u32 idle_color_enum )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return idle_color_enum;
}

/* Common case background color for a pushbutton / knob style widget.
   col_frame_bg with the plain widget background as the idle base. */
u32 col_item_bg( gui_item_state_t st )
{
    return col_frame_bg( st, COL_WIDGET_BG );
}

/* Animated background for a pushbutton-like widget: col_item_bg with the hover/active
   transitions smoothed through the animation service (core/gui_anim.c) -- the ONE projection
   that rides the interact server, explicitly: the damper is
   keyed retained state, so the caller passes the id and core owns the storage.  Two damper
   channels in one gui_anim4 slot -- a hot layer (hover / nav focus) at speed 10 and an active
   layer (pressed) at speed 20 -- both rest at 0 so they ramp up from the palette base; the
   spare two channels sit unused (0/0/0) and are free for a widget-specific flourish later.
   Composite over the palette: BG -> HOT by the hot channel, then that -> ACT by the active
   one.  The primitive owns all storage, settle, and wants_redraw bookkeeping in a single peek;
   an idle widget with no history lands on COL_WIDGET_BG. */

#define ANIM_TAG_BG  0xA501u   /* id_combine salt: keeps this slot distinct from all other per-widget state */

u32
col_item_bg_anim( gui_id_t id, gui_item_state_t st )
{
    gui_anim4_t rest   = { 0.0f, 0.0f, 0.0f, 0.0f };
    gui_anim4_t target = { ( st.hover || st.nav ) ? 1.0f : 0.0f, st.active ? 1.0f : 0.0f, 0.0f, 0.0f };
    gui_anim4_t speed  = { 10.0f, 20.0f, 0.0f, 0.0f };
    gui_anim4_t a      = gui_anim4( id_combine( id, ANIM_TAG_BG ), rest, target, speed );
    return col_lerp( col_lerp( COL_WIDGET_BG, COL_WIDGET_HOT, a.x ), COL_WIDGET_ACT, a.y );
}

/* True while both push_style stacks are empty -- the volatile-replay precondition check
   (chrome/widgets/gui_volatile.c) reads it through this predicate so style_stack_t stays private. */
bool
style_stacks_empty( void )
{
    return s_col_stack.sp == 0 && s_var_stack.sp == 0;
}

// clang-format on
/*============================================================================================*/
