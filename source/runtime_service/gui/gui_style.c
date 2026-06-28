/*==============================================================================================

    runtime_service/gui/gui_style.c -- Style stacks: colors + layout metrics.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor / PushStyleVar
    analogue.  Three layers resolve into the value a widget sees:

        Base   -- the theme default.  Colors: a constant palette (k_col_default).  Vars: the
                  font-derived metrics in s_style, read live so a font change updates them.
        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.
        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.

    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; the
    chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the same
    once-per-widget boundary -- see gui_ctx.c.

    The payoff is reach with no churn: the COL_*, WIDGET_*, and WIN_* macros in gui_widget_core.c
    are redefined to call style_col / style_var, so every existing draw site honors an override
    without changing a single widget.

    Included by gui.c before gui_ctx.c (ctx_new_frame drives style_new_frame) so the accessors
    are in scope for the macros and the resolve seam.  s_style (gui.c) and GUI_COLOR (gui.h)
    are already visible.

==============================================================================================*/
// clang-format off

/* Base value of a style var -- read live from the font-derived metrics so a font_set_builtin / font_load
   update flows through without re-seeding anything.  The single map from slot to s_style field. */

static f32
style_var_base( gui_style_var_t v )
{
    switch ( v )
    {
        case GUI_VAR_LINE_SIZE:       return (f32)s_style.line_size;
        case GUI_VAR_WIDGET_GAP:      return (f32)s_style.widget_gap;
        case GUI_VAR_WIDGET_PAD:      return (f32)s_style.widget_pad;
        case GUI_VAR_WIN_TITLE_H:     return (f32)s_style.win_title_h;
        case GUI_VAR_WIN_BORDER:      return (f32)s_style.win_border;
        case GUI_VAR_CHECKBOX_SZ:     return (f32)s_style.checkbox_sz;
        case GUI_VAR_SLIDER_KNOB_W:   return (f32)s_style.slider_knob_w;
        case GUI_VAR_MIN_CELL_W:      return (f32)s_style.min_cell_w;
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

#define GUI_STYLE_STACK_DEPTH 32

/* Stack + override entry types (named so the compound literals avoid the typeof extension). */
typedef struct { u8 slot; u32 prev; } col_save_t;   // push/pop restore pair
typedef struct { u8 slot; u32 val;  } col_ov_t;     // next-item override
typedef struct { u8 slot; f32 prev; } var_save_t;
typedef struct { u8 slot; f32 val;  } var_ov_t;

/* Colors */
static u32        s_col[ GUI_COL_COUNT ];                      // working set (base + stack)
static col_save_t s_col_stack[ GUI_STYLE_STACK_DEPTH ];
static u32        s_col_sp;

static col_ov_t   s_col_next[ GUI_COL_COUNT ];                 // next-item pending
static u32        s_col_next_n;
static col_ov_t   s_col_item[ GUI_COL_COUNT ];                 // active for current item
static u32        s_col_item_n;

/* Vars (f32) */
static f32        s_var[ GUI_VAR_COUNT ];                      // working set (base + stack)
static var_save_t s_var_stack[ GUI_STYLE_STACK_DEPTH ];
static u32        s_var_sp;

static var_ov_t   s_var_next[ GUI_VAR_COUNT ];                 // next-item pending
static u32        s_var_next_n;
static var_ov_t   s_var_item[ GUI_VAR_COUNT ];                 // active for current item
static u32        s_var_item_n;

/*----------------------------------------------------------------------------------------------
    Accessors -- what every COL_* / metric macro resolves to.  The per-item override (if the slot
    has one this widget) wins; otherwise the working set (base + push/pop).  The item list is
    scanned linearly: it is empty on the common path and never longer than a couple of entries.
----------------------------------------------------------------------------------------------*/

static u32
style_col( gui_col_t slot )
{
    for ( u32 i = 0; i < s_col_item_n; ++i )
        if ( s_col_item[ i ].slot == (u8)slot ) return s_col_item[ i ].val;
    return s_col[ slot ];
}

static f32
style_var( gui_style_var_t slot )
{
    for ( u32 i = 0; i < s_var_item_n; ++i )
        if ( s_var_item[ i ].slot == (u8)slot ) return s_var_item[ i ].val;
    return s_var[ slot ];
}

/*----------------------------------------------------------------------------------------------
    Push / pop / next -- the operations the public API wraps.

    Over-deep pushes: the value is still written to the working set (so the UI renders correctly
    in all builds), and sp is still counted so push/pop stay paired.  The save record is only
    written when sp is within bounds -- an over-depth push cannot save and therefore cannot
    restore, which is the documented cap behaviour.  An ORB_ASSERT fires in debug builds so
    callers discover the imbalance immediately at the push site rather than on a silent bad
    restore.  pop takes a count, like ImGui.
----------------------------------------------------------------------------------------------*/

static void
style_push_color( gui_col_t slot, u32 abgr )
{
    if ( slot >= GUI_COL_COUNT ) return;
    ORB_ASSERT( s_col_sp < GUI_STYLE_STACK_DEPTH && "style_push_color: stack overflow -- mismatched push/pop" );
    if ( s_col_sp < GUI_STYLE_STACK_DEPTH )
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
        if ( s_col_sp < GUI_STYLE_STACK_DEPTH )
            s_col[ s_col_stack[ s_col_sp ].slot ] = s_col_stack[ s_col_sp ].prev;
    }
}

static void
style_next_color( gui_col_t slot, u32 abgr )
{
    if ( slot >= GUI_COL_COUNT ) return;
    /* Replace a pending entry for the same slot rather than stacking duplicates. */
    for ( u32 i = 0; i < s_col_next_n; ++i )
        if ( s_col_next[ i ].slot == (u8)slot ) { s_col_next[ i ].val = abgr; return; }
    if ( s_col_next_n < GUI_COL_COUNT )
        s_col_next[ s_col_next_n++ ] = ( col_ov_t ){ (u8)slot, abgr };
}

static void
style_push_var( gui_style_var_t slot, f32 value )
{
    if ( slot >= GUI_VAR_COUNT ) return;
    ORB_ASSERT( s_var_sp < GUI_STYLE_STACK_DEPTH && "style_push_var: stack overflow -- mismatched push/pop" );
    if ( s_var_sp < GUI_STYLE_STACK_DEPTH )
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
        if ( s_var_sp < GUI_STYLE_STACK_DEPTH )
            s_var[ s_var_stack[ s_var_sp ].slot ] = s_var_stack[ s_var_sp ].prev;
    }
}

static void
style_next_var( gui_style_var_t slot, f32 value )
{
    if ( slot >= GUI_VAR_COUNT ) return;
    for ( u32 i = 0; i < s_var_next_n; ++i )
        if ( s_var_next[ i ].slot == (u8)slot ) { s_var_next[ i ].val = value; return; }
    if ( s_var_next_n < GUI_VAR_COUNT )
        s_var_next[ s_var_next_n++ ] = ( var_ov_t ){ (u8)slot, value };
}

/*----------------------------------------------------------------------------------------------
    Seam hooks -- called from the shared item boundary in gui_ctx.c.
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
   brackets a window_begin / child_begin still applies to the chrome inside it, like ImGui. */

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
    for ( u32 i = 0; i < GUI_COL_COUNT; ++i ) s_col[ i ] = s_style.colors[ i ];
    for ( u32 i = 0; i < GUI_VAR_COUNT; ++i ) s_var[ i ] = style_var_base( (gui_style_var_t)i );

    s_col_sp = s_var_sp = 0;
    s_col_next_n = s_col_item_n = 0;
    s_var_next_n = s_var_item_n = 0;
}

// clang-format on
/*============================================================================================*/
