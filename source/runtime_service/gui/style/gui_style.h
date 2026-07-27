#ifndef GUI_STYLE_H
#define GUI_STYLE_H
/*==============================================================================================

    runtime_service/gui/style/gui_style.h -- style resolution (the style unit).

    Interact-state flags in, colors / metrics out; never paints.  Owns the theme, the style
    stacks, the lattice, and the color/metric vocabulary macros every layer above sizes and
    paints with.  Stack position: after the core pair (each unit .c lists its sub-stack).

    Its own TU (root gui_style.c: gui_theme.c + gui_style_block.c + gui_style_core.c +
    gui_stacks.c).  Values live in the block backend (gui_style_block.c): every slot, whatever
    its vocabulary, is a based run in one store / working-set pair.
    Resolution is PURE: interact state arrives as PARAMETERS (col_item_bg( st )),
    never queried from core, so style resolves with no interact server present -- the one
    sanctioned exception is col_item_bg_anim's explicit ride on core's keyed anim utility.
    The public color id table (gui_col_t, gui.h) is a NAMING layer over the blocks: a name
    resolves to one or more slots, read back through gui_style_color() / gui_style_color_name().

==============================================================================================*/

// clang-format off

/*==============================================================================================
    The element stratum -- resolved reads over the element block
==============================================================================================*/

const gui_style_t* style_active( void );   /* style/gui_theme.c: the active scaled style */

/* A style LANDING (gui_style_core.c): theme / font / scale changed, so every block re-derives
   its installed values.  Driven across the unit seam by gui_style_apply (frame/gui_frame_font.c)
   after the metrics rescale -- which is where the element stratum tracks a theme or font change. */
void style_landing( void );

/* gui_style_core.c: resolve one element value -- the installed element style with any
   push_style_* / next_style_* override applied, since both live in the same slot.  THE seam
   every stock render, every user widget (through gui_el_color), and chrome's element-shaped
   COL_* macros read. */
u32 style_el_col( u8 role, u8 state );

/* The metric twins: the element spacing the rect dispatcher applies (cell_next_w's inter-cell
   gap + the region / label pad) and the frame line width the renders inset by.  WIDGET_PAD /
   WIDGET_GAP read through these and GUI_VAR_WIDGET_PAD / _GAP push onto the same slots, so a
   kit's installed spacing and a scale_push meet in one place and zero means zero. */
f32 style_el_pad( void );
f32 style_el_gap( void );
f32 style_el_border_w( void );

/*==============================================================================================
    Style resolution + the vocabulary macros
==============================================================================================*/

/* style resolution (gui_style_core.c) -- the stack-honoring reads every tier-2 role consumes,
   and the vocabulary macros over them (the composer sizes cells with the same numbers the
   widgets and skin read). */
f32 style_var( gui_style_var_t slot );
u32 style_col( gui_col_t slot );

/* 1. METRICS -- can move a rect */
#define WIDGET_H      style_var( GUI_VAR_LINE_SIZE     )
#define WIDGET_GAP    style_el_gap()   /* installed layout style (base) + var-stack override */
#define WIDGET_PAD    style_el_pad()   /*   -- so a set-once / kit / zero layout style drives flow */
#define WIDGET_MIN_W  style_var( GUI_VAR_MIN_CELL_W    )
#define WIN_BORDER    style_var( GUI_VAR_WIN_BORDER    )
#define WIN_TITLE_H   style_var( GUI_VAR_WIN_TITLE_H   )
#define CHECKBOX_SZ   style_var( GUI_VAR_CHECKBOX_SZ   )
#define SLIDER_KNOB_W style_var( GUI_VAR_SLIDER_KNOB_W )

/* 2. SKIN -- paint-only corner-radius categories + insets (authored per theme in
   style/gui_theme.c; they move no rect, which is what separates them from the metrics above). */
#define ROUND_WIN        style_var( GUI_VAR_WIN_ROUNDING    )
#define ROUND_WIDGET     style_var( GUI_VAR_WIDGET_ROUNDING )
#define ROUND_GRAB       style_var( GUI_VAR_GRAB_ROUNDING   )
#define CHECK_PAD        ( (f32)s_style.checkmark_pad )
#define WIN_FOCUS_BORDER style_var( GUI_VAR_WIN_FOCUS_BORDER )

/* The COL_* color vocabulary, and the block split made visible: the element-shaped subset
   speaks roles x states over the ELEMENT block (the stratum chrome shares with stock widgets
   and a kit's own), the rest are chrome's PRIVATE TOKENS -- the colors with no role.  Both are
   one indexed load; style_col( gui_col_t ) is the generic door for code holding a slot id. */
#define COL_TEXT         style_el_col( GUI_EL_TEXT,   GUI_EL_IDLE   )
#define COL_TEXT_DIM     style_el_col( GUI_EL_TEXT,   GUI_EL_DIM    )
#define COL_WIDGET_BG    style_el_col( GUI_EL_BG,     GUI_EL_IDLE   )
#define COL_WIDGET_HOT   style_el_col( GUI_EL_BG,     GUI_EL_HOT    )
#define COL_WIDGET_ACT   style_el_col( GUI_EL_BG,     GUI_EL_ACTIVE )
#define COL_CHILD_BG     style_el_col( GUI_EL_BG,     GUI_EL_DIM    )
#define COL_BORDER       style_el_col( GUI_EL_BORDER, GUI_EL_IDLE   )
#define COL_WIDGET_FG    style_el_col( GUI_EL_ACCENT, GUI_EL_IDLE   )
#define COL_CHECK_MARK   style_el_col( GUI_EL_ACCENT, GUI_EL_ACTIVE )
#define COL_SLIDER_TRACK style_el_col( GUI_EL_ACCENT, GUI_EL_DIM    )
#define COL_NAV          style_el_col( GUI_EL_ACCENT, GUI_EL_HOT    )

#define COL_WIN_BG       style_col( GUI_COL_WINDOW_BG    )
#define COL_TITLE_BG     style_col( GUI_COL_TITLE_BG     )
#define COL_RESIZE_HOT   style_col( GUI_COL_RESIZE_HOT   )
#define COL_INPUT_BG     style_col( GUI_COL_INPUT_BG     )
#define COL_INPUT_FOCUS  style_col( GUI_COL_INPUT_FOCUS  )
#define COL_CURSOR       style_col( GUI_COL_CURSOR       )
#define COL_NAV_CAPTURE  style_col( GUI_COL_NAV_CAPTURE  )
#define COL_FOCUS_BORDER style_col( GUI_COL_FOCUS_BORDER )

/* style stack push/pop by slot (gui_style_core.c). */
void style_push_var( gui_style_var_t slot, f32 value );
void style_pop_var( u32 count );

/* True while no ambient style scope is open (the volatile-replay precondition). */
bool style_stacks_empty( void );

/* Style-set containment (gui_style_core.c) -- the pair a region uses to keep an unbalanced
   style_set_push from escaping it, exactly as id_restore does for the id scope. */
u32  style_set_depth ( void );
void style_set_unwind( u32 depth );

/* The item / chrome / frame seam hooks (gui_style_core.c) -- driven from OUTSIDE this unit:
   style_item_commit / style_chrome_reset by the impure per-item wrappers (stock/
   gui_adornment.c), style_new_frame by the orchestrator (gui_ctx_begin pairs it with
   ctx_new_frame) and by gui_theme_reset. */
void style_item_commit( void );
void style_chrome_reset( void );
void style_new_frame( void );

/* The em rescale (style/gui_theme.c) -- gui_style_apply (frame/gui_frame_font.c) reads the active
   font's metrics (draw-unit material style must not touch) and passes them down here. */
void metrics_compute( u32 em, u32 char_h, u32 line_h );

/* lattice snapping (style/gui_theme.c) -- the grid-quantum rounders composition and chrome
   share (identity when the lattice is off or q <= 1). */
f32 lat_floor    ( f32 v, u32 q );
f32 lat_floor_min( f32 v, u32 q );
f32 lat_ceil     ( f32 v, u32 q );
f32 lat_round    ( f32 v, u32 q );

extern gui_style_t s_style;     /* style/gui_theme.c -- the ACTIVE (scaled) style       */
extern u32         s_font_size; /* style/gui_theme.c -- active em (0 = never set)       */

/* State -> color projections (gui_style_core.c) -- pure: the state flags arrive as
   parameters; col_item_bg_anim alone rides core's keyed anim utility, explicitly. */
u32 col_item_bg( gui_item_state_t st );
u32 col_item_bg_anim( gui_id_t id, gui_item_state_t st );
u32 col_frame_bg( gui_item_state_t st, u32 idle_color_enum );

/* The per-item ambient wrappers that DRIVE the seam hooks above (item_flags_resolve /
   item_flags_chrome_reset) live in stock/gui_adornment.c, declared in stock/gui_stock_internal.h:
   they apply draw-state consequences, and style never paints. */

/* Decentralized memory accounting -- this unit's fixed statics (root gui_style.c foot),
   summed into cpu_frontend_bytes by gui_ui_memory (gui_ui_mem.c). */
u32 style_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_STYLE_H
