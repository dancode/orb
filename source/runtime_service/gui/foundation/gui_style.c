/*==============================================================================================

    runtime_service/gui/foundation/gui_style.c -- Style stacks: colors + layout metrics.

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
    once-per-widget boundary -- see gui_ctx.c.

    The payoff is reach with no churn: the COL_*, WIDGET_*, and WIN_* vocabulary macros at the
    bottom of this file resolve through style_col / style_var, so every existing read site honors
    an override without changing a single widget.

    Included by gui.c after gui_theme.c and before gui_ctx.c (ctx_new_frame drives
    style_new_frame) so the accessors are in scope for the macros and the resolve seam.
    s_style (foundation/gui_theme.c) and GUI_COLOR (gui.h) are already visible.

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
        case GUI_VAR_FIELD_LABEL_W:   return (f32)s_style.field_label_w;

        /* 2. SKIN */
        case GUI_VAR_WIN_ROUNDING:    return (f32)s_style.win_rounding;
        case GUI_VAR_WIDGET_ROUNDING: return (f32)s_style.widget_rounding;
        case GUI_VAR_GRAB_ROUNDING:   return (f32)s_style.grab_rounding;
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

static u32 style_col( gui_col_t slot )       { return style_read( (u32)slot ); }
static f32 style_var( gui_style_var_t slot ) { return style_bits_f32( style_read( STYLE_VAR_BASE + (u32)slot ) ); }

static void style_push_color( gui_col_t slot, u32 abgr ) 
{
    if ( slot < GUI_COL_COUNT ) 
         style_push( &s_col_stack, (u32)slot, abgr );
}
static void style_push_var( gui_style_var_t slot, f32 value )
{
    if ( slot < GUI_VAR_COUNT ) 
         style_push( &s_var_stack, STYLE_VAR_BASE + (u32)slot, style_f32_bits( value ) );
}
static void style_pop_color( u32 count ) 
{ 
    style_pop( &s_col_stack, count ); 
}
static void style_pop_var  ( u32 count ) 
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
    Seam hooks -- called from the shared item boundary in gui_ctx.c.
==============================================================================================*/

/* Promote the pending next-item overrides into the active per-item layer and clear the pending.
   Called once per widget from item_flags_resolve, so the override that next_style_* queued just
   before this widget applies for this widget's whole draw, then is gone for the following one. */

static void
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

static void
style_chrome_reset( void )
{
    s_item_n = 0;
}

/* Reset the per-frame style state: re-seed the working set from the base (so an unbalanced push
   cannot leak across frames), empty the stacks, and clear both next-item layers.  Called from
   ctx_new_frame. */

static void
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
    
    The style vocabulary -- the macros every later tier reads style through.

    Each read resolves through style_var / style_col above (the theme base with any push_style_*
    / next_style_* override applied), so every call site honors the stacks with no change.  They
    live here with the resolver -- foundation -- because every tier-2 role consumes them: the
    composer sizes cells and gutters with the METRICS group, widgets measure natural sizes and
    seat labels with the same numbers, and presentation paints with the SKIN group.  Grouped by
    the two gui_style_t categories (see gui.h).

==============================================================================================*/

/* 1. METRICS -- can move a rect */
#define WIDGET_H      style_var( GUI_VAR_LINE_SIZE     )
#define WIDGET_GAP    style_var( GUI_VAR_WIDGET_GAP    )
#define WIDGET_PAD    style_var( GUI_VAR_WIDGET_PAD    )
#define WIDGET_MIN_W  style_var( GUI_VAR_MIN_CELL_W    )
#define WIN_BORDER    style_var( GUI_VAR_WIN_BORDER    )
#define WIN_TITLE_H   style_var( GUI_VAR_WIN_TITLE_H   )
#define CHECKBOX_SZ   style_var( GUI_VAR_CHECKBOX_SZ   )
#define SLIDER_KNOB_W style_var( GUI_VAR_SLIDER_KNOB_W )
#define FIELD_LABEL_W style_var( GUI_VAR_FIELD_LABEL_W )

/* 2. SKIN -- paint-only.  The roundings are corner-radius categories, fed to draw_set_rounding
   (gui_backend) so a draw site can pick the right rounding before emitting.  The item seam
   defaults to ROUND_WIDGET and the chrome seam to ROUND_WIN; grabs and squared-off marks
   override locally. */

#define ROUND_WIN       style_var( GUI_VAR_WIN_ROUNDING    )
#define ROUND_WIDGET    style_var( GUI_VAR_WIDGET_ROUNDING )
#define ROUND_GRAB      style_var( GUI_VAR_GRAB_ROUNDING   )

/* SKIN: color palette (GUI_COLOR: byte order R,G,B,A in memory = ABGR u32).  Theme defaults
   come from the active theme (k_themes in gui_theme.c, seeded into s_style.colors); see
   gui_col_t for the slots. */

#define COL_WIN_BG       style_col( GUI_COL_WINDOW_BG     )
#define COL_CHILD_BG     style_col( GUI_COL_CHILD_BG      )
#define COL_TITLE_BG     style_col( GUI_COL_TITLE_BG      )
#define COL_BORDER       style_col( GUI_COL_BORDER        )
#define COL_TEXT         style_col( GUI_COL_TEXT          )
#define COL_TEXT_DIM     style_col( GUI_COL_TEXT_DIM      )
#define COL_WIDGET_BG    style_col( GUI_COL_WIDGET_BG     )
#define COL_WIDGET_HOT   style_col( GUI_COL_WIDGET_HOT    )
#define COL_WIDGET_ACT   style_col( GUI_COL_WIDGET_ACT    )
#define COL_WIDGET_FG    style_col( GUI_COL_WIDGET_FG     )
#define COL_CHECK_MARK   style_col( GUI_COL_CHECK_MARK    )
#define COL_SLIDER_TRACK style_col( GUI_COL_SLIDER_TRACK  )
#define COL_RESIZE_HOT   style_col( GUI_COL_RESIZE_HOT    )
#define COL_INPUT_BG     style_col( GUI_COL_INPUT_BG      )
#define COL_INPUT_FOCUS  style_col( GUI_COL_INPUT_FOCUS   )
#define COL_CURSOR       style_col( GUI_COL_CURSOR        )
#define COL_NAV          style_col( GUI_COL_NAV_HIGHLIGHT )

// clang-format on
/*============================================================================================*/
