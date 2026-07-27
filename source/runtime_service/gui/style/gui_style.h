#ifndef GUI_STYLE_H
#define GUI_STYLE_H
/*==============================================================================================

    runtime_service/gui/style/gui_style.h -- style resolution (the style unit).

    Interact-state flags in, colors / metrics out; never paints.  Owns the theme, the style
    stacks, the lattice, and the color/metric vocabulary macros every layer above sizes and
    paints with.  Stack position: after the core pair (each unit .c lists its sub-stack).

    Its own TU (root gui_style.c: gui_theme.c + gui_style_block.c + gui_style_core.c +
    gui_stacks.c).  Values live in the block backend (gui_style_block.c): ONE block whose slot
    layout is gui_style_t itself, instanced once per style set, so chrome and a kit each own a
    complete style rather than a kit owning a subset of one.
    Resolution is PURE: interact state arrives as PARAMETERS (col_item_bg( st )),
    never queried from core, so style resolves with no interact server present -- the one
    sanctioned exception is col_item_bg_anim's explicit ride on core's keyed anim utility.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    The resolved reads -- the stack-honoring accessors every tier-2 role consumes.
==============================================================================================*/

const gui_style_t* style_active( void );   /* style/gui_theme.c: the active scaled style */

/* A style LANDING (gui_style_core.c): theme / font / scale changed, so every set re-derives its
   installed values.  Driven across the unit seam by gui_style_apply (frame/gui_frame_font.c)
   after the metrics rescale -- which is where the style tracks a theme or font change. */
void style_landing( void );

/* The two coordinate systems, and there are only two.  A color is a (role, phase) cell of the
   color grid; a scalar is a gui_style_var_t.  Both are the installed value with any
   push_style_* / next_style_* override already applied, since an override lands in the same
   slot -- one indexed load, nothing to scan. */
u32 style_col( u8 role, u8 phase );
f32 style_var( gui_style_var_t var );

/* One field of one density-ramp step (field: 0 = row, 1 = pad, 2 = gap).  Read through the block
   like everything else, so a kit's DENSE is the kit's own and not chrome's. */
f32 style_scale( gui_scale_t s, u32 field );

/*==============================================================================================
    1. METRICS -- can move a rect
==============================================================================================*/

#define WIDGET_H      style_var( GUI_VAR_ROW       )
#define WIDGET_PAD    style_var( GUI_VAR_PAD       )
#define WIDGET_GAP    style_var( GUI_VAR_GAP       )
#define WIN_BORDER    style_var( GUI_VAR_BORDER    )
#define CHECKBOX_SZ   style_var( GUI_VAR_INDICATOR )
#define SLIDER_KNOB_W style_var( GUI_VAR_GUTTER    )
#define WIDGET_MIN_W  style_var( GUI_VAR_MIN_CELL  )
#define WIN_TITLE_H   style_var( GUI_VAR_TITLE_H   )

/* The lattice pitch, as the u32 the lat_* rounders take.  A style value like any other, so a
   kit can carry its own grid; 0 / 1 means the lattice is off. */
#define GRID_Q        ( (u32)style_var( GUI_VAR_GRID_Q ) )

/*==============================================================================================
    2. SKIN -- paint-only: corner radii and the mark-shape picks (authored per theme in
    style/gui_theme.c).  They move no rect, which is what separates them from the metrics above.
==============================================================================================*/

#define ROUND_WIDGET  style_var( GUI_VAR_ROUND       )   /* control frames, knobs, grabs */
#define ROUND_WIN     style_var( GUI_VAR_PANEL_ROUND )   /* windows, children, popups    */

/*==============================================================================================
    3. COLORS -- the color grid, one macro per cell and no cell with two names.

    This table IS gui_style_t.col: five roles down, four phases across.  Every name below is the
    established one for that cell, so the 180 read sites above never learned that the flat
    palette and chrome's private tokens went away -- which is exactly why they could.  A name
    here is chrome's SPELLING of a cell, not a cell of its own: nothing reachable through these
    macros is unreachable through style_col, which is what makes chrome an ordinary consumer.

    Reading across a row shows what a phase MEANS for that role; the grid is documented once, in
    gui.h.  A render that speaks roles and phases generically wants style_col (or gui()->
    style_color from outside the library) instead -- same seam, no macro.
==============================================================================================*/

/*                          role              IDLE / HOT / ACTIVE / DIM                        */
#define COL_WIN_BG        style_col( GUI_ROLE_PANEL,  GUI_PHASE_IDLE   )  /* window body        */
#define COL_TITLE_BG      style_col( GUI_ROLE_PANEL,  GUI_PHASE_HOT    )  /* title bar          */
#define COL_TITLE_ACTIVE  style_col( GUI_ROLE_PANEL,  GUI_PHASE_ACTIVE )  /* focused title bar  */
#define COL_CHILD_BG      style_col( GUI_ROLE_PANEL,  GUI_PHASE_DIM    )  /* child / recessed   */

#define COL_WIDGET_BG     style_col( GUI_ROLE_BG,     GUI_PHASE_IDLE   )  /* control face       */
#define COL_WIDGET_HOT    style_col( GUI_ROLE_BG,     GUI_PHASE_HOT    )  /* hovered face       */
#define COL_WIDGET_ACT    style_col( GUI_ROLE_BG,     GUI_PHASE_ACTIVE )  /* pressed / focused  */
#define COL_WIDGET_DIM    style_col( GUI_ROLE_BG,     GUI_PHASE_DIM    )  /* inert face         */

#define COL_BORDER        style_col( GUI_ROLE_BORDER, GUI_PHASE_IDLE   )  /* frame line         */
#define COL_RESIZE_HOT    style_col( GUI_ROLE_BORDER, GUI_PHASE_HOT    )  /* hovered edge       */
#define COL_FOCUS_BORDER  style_col( GUI_ROLE_BORDER, GUI_PHASE_ACTIVE )  /* focused ring       */
#define COL_BORDER_DIM    style_col( GUI_ROLE_BORDER, GUI_PHASE_DIM    )  /* subdued frame      */

#define COL_TEXT          style_col( GUI_ROLE_TEXT,   GUI_PHASE_IDLE   )  /* body text, caret   */
#define COL_TEXT_HOT      style_col( GUI_ROLE_TEXT,   GUI_PHASE_HOT    )  /* on a hot face      */
#define COL_TEXT_ACT      style_col( GUI_ROLE_TEXT,   GUI_PHASE_ACTIVE )  /* on a pressed face  */
#define COL_TEXT_DIM      style_col( GUI_ROLE_TEXT,   GUI_PHASE_DIM    )  /* secondary text     */

#define COL_WIDGET_FG     style_col( GUI_ROLE_ACCENT, GUI_PHASE_IDLE   )  /* value fill         */
#define COL_NAV           style_col( GUI_ROLE_ACCENT, GUI_PHASE_HOT    )  /* nav highlight      */
#define COL_CHECK_MARK    style_col( GUI_ROLE_ACCENT, GUI_PHASE_ACTIVE )  /* mark, captured nav */
#define COL_SLIDER_TRACK  style_col( GUI_ROLE_ACCENT, GUI_PHASE_DIM    )  /* empty track        */

/*==============================================================================================
    Stacks, sets, and the seam hooks
==============================================================================================*/

/* style stack push/pop by slot (gui_style_core.c). */
void style_push_var( gui_style_var_t var, f32 value );
void style_pop_var ( u32 count );

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
u32 col_frame_bg( gui_item_state_t st, u32 idle_color );

/* The per-item ambient wrappers that DRIVE the seam hooks above (item_flags_resolve /
   item_flags_chrome_reset) live in stock/gui_adornment.c, declared in stock/gui_stock_internal.h:
   they apply draw-state consequences, and style never paints. */

/* Decentralized memory accounting -- this unit's fixed statics (root gui_style.c foot),
   summed into cpu_frontend_bytes by gui_ui_memory (gui_ui_mem.c). */
u32 style_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_STYLE_H
