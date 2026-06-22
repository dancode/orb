/*==============================================================================================

    runtime_service/imgui/imgui_style.c -- Style stacks: colors + layout metrics.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor / PushStyleVar
    analogue.  Three layers resolve into the value a widget sees:

        Base   -- the theme default.  Colors: a constant palette (k_col_default).  Vars: the
                  font-derived metrics in s_layout, read live so a font change updates them.
        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.
        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.

    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; the
    chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the same
    once-per-widget boundary -- see imgui_ctx.c.

    The payoff is reach with no churn: the COL_*, WIDGET_*, and WIN_* macros in imgui_widget_core.c
    are redefined to call style_col / style_var, so every existing draw site honors an override
    without changing a single widget.

    Included by imgui.c before imgui_ctx.c (ctx_new_frame drives style_new_frame) so the accessors
    are in scope for the macros and the resolve seam.  s_layout (imgui.c) and IMGUI_COLOR (imgui.h)
    are already visible.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Theme default palette -- the constant base colors (IMGUI_COLOR packs R,G,B,A bytes).  Seeded
    into the working set each frame, so an unbalanced push cannot leak across frames.  Designated
    by slot so the array order is decoupled from the enum order.
----------------------------------------------------------------------------------------------*/

static const u32 k_col_default[ IMGUI_COL_COUNT ] = {
    [ IMGUI_COL_TEXT         ] = IMGUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
    [ IMGUI_COL_TEXT_DIM     ] = IMGUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
    [ IMGUI_COL_WINDOW_BG    ] = IMGUI_COLOR( 0x24, 0x24, 0x24, 0xE8 ),
    [ IMGUI_COL_CHILD_BG     ] = IMGUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF ),
    [ IMGUI_COL_TITLE_BG     ] = IMGUI_COLOR( 0x10, 0x60, 0xA0, 0xFF ),
    [ IMGUI_COL_BORDER       ] = IMGUI_COLOR( 0x80, 0x80, 0x80, 0xFF ),
    [ IMGUI_COL_WIDGET_BG    ] = IMGUI_COLOR( 0x40, 0x40, 0x40, 0xFF ),
    [ IMGUI_COL_WIDGET_HOT   ] = IMGUI_COLOR( 0x50, 0x80, 0xB0, 0xFF ),
    [ IMGUI_COL_WIDGET_ACT   ] = IMGUI_COLOR( 0x30, 0x60, 0x90, 0xFF ),
    [ IMGUI_COL_WIDGET_FG    ] = IMGUI_COLOR( 0x20, 0x90, 0xD0, 0xFF ),
    [ IMGUI_COL_CHECK_MARK   ] = IMGUI_COLOR( 0x20, 0xC0, 0x60, 0xFF ),
    [ IMGUI_COL_SLIDER_TRACK ] = IMGUI_COLOR( 0x30, 0x30, 0x30, 0xFF ),
    [ IMGUI_COL_RESIZE_HOT   ] = IMGUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
    [ IMGUI_COL_INPUT_BG     ] = IMGUI_COLOR( 0x38, 0x38, 0x38, 0xFF ),
    [ IMGUI_COL_INPUT_FOCUS  ] = IMGUI_COLOR( 0x20, 0x50, 0x70, 0xFF ),
    [ IMGUI_COL_CURSOR       ] = IMGUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF ),
    [ IMGUI_COL_NAV_HIGHLIGHT] = IMGUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
};

/* Base value of a style var -- read live from the font-derived metrics so a set_font / load_font
   update flows through without re-seeding anything.  The single map from slot to s_layout field. */
static f32
style_var_base( imgui_style_var_t v )
{
    switch ( v )
    {
        case IMGUI_VAR_LINE_SIZE:       return (f32)s_layout.line_size;
        case IMGUI_VAR_WIDGET_GAP:      return (f32)s_layout.widget_gap;
        case IMGUI_VAR_WIDGET_PAD:      return (f32)s_layout.widget_pad;
        case IMGUI_VAR_WIN_TITLE_H:     return (f32)s_layout.win_title_h;
        case IMGUI_VAR_WIN_BORDER:      return (f32)s_layout.win_border;
        case IMGUI_VAR_CHECKBOX_SZ:     return (f32)s_layout.checkbox_sz;
        case IMGUI_VAR_SLIDER_KNOB_W:   return (f32)s_layout.slider_knob_w;
        case IMGUI_VAR_MIN_CELL_W:      return (f32)s_layout.min_cell_w;
        case IMGUI_VAR_WIN_ROUNDING:    return (f32)s_layout.win_rounding;
        case IMGUI_VAR_WIDGET_ROUNDING: return (f32)s_layout.widget_rounding;
        case IMGUI_VAR_GRAB_ROUNDING:   return (f32)s_layout.grab_rounding;
        case IMGUI_VAR_CHECK_STYLE:     return (f32)s_layout.check_style;     /* enum-as-var: 0 tick / 1 disc / 2 cross */
        case IMGUI_VAR_BULLET_STYLE:    return (f32)s_layout.bullet_style;    /* enum-as-var: 0 disc / 1 square */
        case IMGUI_VAR_ARROW_STYLE:     return (f32)s_layout.arrow_style;     /* enum-as-var: 0 triangle / 1 chevron */
        case IMGUI_VAR_SEPARATOR_STYLE: return (f32)s_layout.separator_style; /* enum-as-var: 0 solid / 1 dashed */
        case IMGUI_VAR_PROGRESS_STYLE:  return (f32)s_layout.progress_style;  /* enum-as-var: 0 solid / 1 gradient */
        case IMGUI_VAR_SLIDER_KNOB:     return (f32)s_layout.slider_knob;     /* enum-as-var: 0 bar / 1 circle */
        default:                        return 0.0f;
    }
}

/*----------------------------------------------------------------------------------------------
    State

    Working set: the base with the push/pop stack applied, the value an unscoped read returns.
    Stack: saved (slot, previous) pairs so pop restores regardless of which slots a push touched.
    Next-item layers: a small list of (slot, value) overrides, `next` filled by next_style_* and
    promoted to `item` (the active per-widget override) at the resolve seam, then cleared.  Both
    are tiny lists -- at most COUNT entries, usually zero -- so a read scans only what is active.
----------------------------------------------------------------------------------------------*/

#define IMGUI_STYLE_STACK_DEPTH 32

/* Stack + override entry types (named so the compound literals avoid the typeof extension). */
typedef struct { u8 slot; u32 prev; } col_save_t;   // push/pop restore pair
typedef struct { u8 slot; u32 val;  } col_ov_t;     // next-item override
typedef struct { u8 slot; f32 prev; } var_save_t;
typedef struct { u8 slot; f32 val;  } var_ov_t;

/* Colors */
static u32        s_col[ IMGUI_COL_COUNT ];                      // working set (base + stack)
static col_save_t s_col_stack[ IMGUI_STYLE_STACK_DEPTH ];
static u32        s_col_sp;

static col_ov_t   s_col_next[ IMGUI_COL_COUNT ];                 // next-item pending
static u32        s_col_next_n;
static col_ov_t   s_col_item[ IMGUI_COL_COUNT ];                 // active for current item
static u32        s_col_item_n;

/* Vars (f32) */
static f32        s_var[ IMGUI_VAR_COUNT ];                      // working set (base + stack)
static var_save_t s_var_stack[ IMGUI_STYLE_STACK_DEPTH ];
static u32        s_var_sp;

static var_ov_t   s_var_next[ IMGUI_VAR_COUNT ];                 // next-item pending
static u32        s_var_next_n;
static var_ov_t   s_var_item[ IMGUI_VAR_COUNT ];                 // active for current item
static u32        s_var_item_n;

/*----------------------------------------------------------------------------------------------
    Accessors -- what every COL_* / metric macro resolves to.  The per-item override (if the slot
    has one this widget) wins; otherwise the working set (base + push/pop).  The item list is
    scanned linearly: it is empty on the common path and never longer than a couple of entries.
----------------------------------------------------------------------------------------------*/

static u32
style_col( imgui_col_t slot )
{
    for ( u32 i = 0; i < s_col_item_n; ++i )
        if ( s_col_item[ i ].slot == (u8)slot ) return s_col_item[ i ].val;
    return s_col[ slot ];
}

static f32
style_var( imgui_style_var_t slot )
{
    for ( u32 i = 0; i < s_var_item_n; ++i )
        if ( s_var_item[ i ].slot == (u8)slot ) return s_var_item[ i ].val;
    return s_var[ slot ];
}

/*----------------------------------------------------------------------------------------------
    Push / pop / next -- the operations the public API wraps.  Over-deep pushes are dropped (the
    value still applies for the read, just not restorable past the cap) but counted truthfully so
    push/pop stay paired, mirroring the id / item-flag stacks.  pop takes a count, like ImGui.
----------------------------------------------------------------------------------------------*/

static void
style_push_color( imgui_col_t slot, u32 abgr )
{
    if ( slot >= IMGUI_COL_COUNT ) return;
    if ( s_col_sp < IMGUI_STYLE_STACK_DEPTH )
        s_col_stack[ s_col_sp ] = ( col_save_t ){ (u8)slot, s_col[ slot ] };
    ++s_col_sp;
    s_col[ slot ] = abgr;
}

static void
style_pop_color( u32 count )
{
    while ( count-- && s_col_sp )
    {
        --s_col_sp;
        if ( s_col_sp < IMGUI_STYLE_STACK_DEPTH )
            s_col[ s_col_stack[ s_col_sp ].slot ] = s_col_stack[ s_col_sp ].prev;
    }
}

static void
style_next_color( imgui_col_t slot, u32 abgr )
{
    if ( slot >= IMGUI_COL_COUNT ) return;
    /* Replace a pending entry for the same slot rather than stacking duplicates. */
    for ( u32 i = 0; i < s_col_next_n; ++i )
        if ( s_col_next[ i ].slot == (u8)slot ) { s_col_next[ i ].val = abgr; return; }
    if ( s_col_next_n < IMGUI_COL_COUNT )
        s_col_next[ s_col_next_n++ ] = ( col_ov_t ){ (u8)slot, abgr };
}

static void
style_push_var( imgui_style_var_t slot, f32 value )
{
    if ( slot >= IMGUI_VAR_COUNT ) return;
    if ( s_var_sp < IMGUI_STYLE_STACK_DEPTH )
        s_var_stack[ s_var_sp ] = ( var_save_t ){ (u8)slot, s_var[ slot ] };
    ++s_var_sp;
    s_var[ slot ] = value;
}

static void
style_pop_var( u32 count )
{
    while ( count-- && s_var_sp )
    {
        --s_var_sp;
        if ( s_var_sp < IMGUI_STYLE_STACK_DEPTH )
            s_var[ s_var_stack[ s_var_sp ].slot ] = s_var_stack[ s_var_sp ].prev;
    }
}

static void
style_next_var( imgui_style_var_t slot, f32 value )
{
    if ( slot >= IMGUI_VAR_COUNT ) return;
    for ( u32 i = 0; i < s_var_next_n; ++i )
        if ( s_var_next[ i ].slot == (u8)slot ) { s_var_next[ i ].val = value; return; }
    if ( s_var_next_n < IMGUI_VAR_COUNT )
        s_var_next[ s_var_next_n++ ] = ( var_ov_t ){ (u8)slot, value };
}

/*----------------------------------------------------------------------------------------------
    Seam hooks -- called from the shared item boundary in imgui_ctx.c.
----------------------------------------------------------------------------------------------*/

/* Promote the pending next-item overrides into the active per-item layer and clear the pending.
   Called once per widget from item_flags_resolve, so the override that next_style_* queued just
   before this widget applies for this widget's whole draw, then is gone for the following one. */
static void
style_item_commit( void )
{
    s_col_item_n = s_col_next_n;
    for ( u32 i = 0; i < s_col_next_n; ++i ) s_col_item[ i ] = s_col_next[ i ];
    s_col_next_n = 0;

    s_var_item_n = s_var_next_n;
    for ( u32 i = 0; i < s_var_next_n; ++i ) s_var_item[ i ] = s_var_next[ i ];
    s_var_next_n = 0;
}

/* Drop the active per-item overrides before chrome draws.  Chrome (borders, scrollbars, titlebars)
   does not pass through the item seam, so without this it would inherit a lingering next-* override
   from the last body widget.  The push/pop stack is intentionally left intact -- a push that
   brackets a begin_window / begin_child still applies to the chrome inside it, like ImGui. */
static void
style_chrome_reset( void )
{
    s_col_item_n = 0;
    s_var_item_n = 0;
}

/* Reset the per-frame style state: re-seed the working set from the base (so an unbalanced push
   cannot leak across frames), empty the stacks, and clear both next-item layers.  Called from
   ctx_new_frame. */
static void
style_new_frame( void )
{
    for ( u32 i = 0; i < IMGUI_COL_COUNT; ++i ) s_col[ i ] = k_col_default[ i ];
    for ( u32 i = 0; i < IMGUI_VAR_COUNT; ++i ) s_var[ i ] = style_var_base( (imgui_style_var_t)i );

    s_col_sp = s_var_sp = 0;
    s_col_next_n = s_col_item_n = 0;
    s_var_next_n = s_var_item_n = 0;
}

// clang-format on
/*============================================================================================*/
