#ifndef GUI_STYLE_H
#define GUI_STYLE_H
/*==============================================================================================

    runtime_service/gui/style/gui_style.h -- style resolution (the style unit).

    Interact-state flags in, colors / metrics out; never paints.  Owns the theme, the style
    stacks, the lattice, and the color/metric vocabulary macros every layer above sizes and
    paints with.  Stack position: after the core pair (each unit .c lists its sub-stack).

    Its own TU (root gui_style.c: gui_theme.c + gui_style_core.c + gui_stacks.c).  Values live
    in gui_style_core.c: one whole gui_style_t installed per style set, plus the resolved working
    run every read indexes -- so chrome and a kit each own a complete style rather than a kit
    owning a subset of one.
    Resolution is PURE: interact state arrives as PARAMETERS (col_item_bg( st )),
    never queried from core, so style resolves with no interact server present -- the one
    sanctioned exception is style_mix's explicit ride on core's keyed anim utility.

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
   slot -- one indexed load, nothing to scan.

   style_col is the plain read, which is what every macro below resolves to; style_col_selected
   washes the same cell toward the theme's accent for the handful of widgets whose caller knows
   the item is selected -- see GUI_STYLE -- SELECTED in gui.h. */
u32 style_col( u8 role, u8 phase );
f32 style_var( gui_style_var_t var );

/* Wash a resolved colour toward the theme's accent by GUI_RAMP_SELECT * travel (travel 0..1).
   The one primitive both style_col_selected and style_col_mix's sel branch spend, and the one a
   face painter reaches for to wash a resolved brush's tint (stock/gui_face.c) instead of naming
   a second stored cell -- see GUI_STYLE -- SELECTED in gui.h. */
u32 style_wash_selected( u32 color, f32 travel );

/* style_col washed all the way toward the accent (travel = 1) -- the static "this item IS
   selected" read a still (non-animated) painter wants, e.g. a selection-rect fill. */
u32 style_col_selected( u8 role, u8 phase );

/* The FACE read -- the same (role, phase) coordinate into the parallel brush-handle plane,
   resolved to the brush itself.  NULL means the cell names no face, which is every cell of a
   theme that authors no art: the caller falls back to style_col and nothing has changed.  Style
   still never paints -- this RESOLVES a brush; the painters that consume one live in stock
   (stock/gui_face.c), on the far side of the purity line, exactly like the colour projections'
   painters do.  There is no style_face_selected: a selected brush is the SAME brush with its
   tint washed by style_wash_selected, not a second stored handle. */
const gui_brush_t* style_face( u8 role, u8 phase );

/* The extended-palette read: a flat, unramped colour by id -- the severity ladder's new home,
   and where a kit's own gui_style_ext_add colours resolve.  See GUI_STYLE -- the EXTENDED
   palette, gui.h, for the whole story. */
u32 style_ext( gui_style_ext_t ext );

/* A GUI_CLASS_SHAPE var read back as its enum -- a rounding, and the ONE spelling for it.  Never
   compare a shape var with >= 0.5f (a two-value assumption) or cast it truncating (0.999999 is a
   legal slot value and truncates to the wrong pick).  See the definition for why each of those
   idioms picks the wrong glyph. */
u32 style_shape( gui_style_var_t var );

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
#define WIN_SHADOW    style_var( GUI_VAR_SHADOW      )   /* elevation feather; 0 = flat  */

/* The disabled dim, as a style value like any other -- so a kit can soften or disable it, and
   push_style_var can scope it.  Living in the schema keeps the one number every disabled widget
   reads inside the vocabulary, rather than off in a private #define no push can reach. */
#define DISABLED_ALPHA style_var( GUI_VAR_DISABLED_ALPHA )

/*==============================================================================================
    3. COLORS -- the color grid, one macro per cell and no cell with two names.

    This table IS gui_style_t.col: ten roles down, four phases across.  A name here is chrome's
    SPELLING of a cell, not a cell of its own: nothing reachable through these macros is
    unreachable through style_col, which is what makes chrome an ordinary consumer.  PANEL_CHILD
    is the one role with no macro family: its only consumer (draw_child_bg, stock/
    gui_adornment.c) takes the phase as a parameter, so it reads the grid generically.

    The cells themselves are DERIVED -- gui_style_bake (style/gui_bake.c) writes all 40 from the
    seven-seed palette, and a theme authors no colour directly.  Nothing below this line changes
    because of that: a read is still one indexed load of the same slot, and a kit that overwrites
    a cell after baking still wins.  It only means the answer to "why is this cell that colour"
    lives in the bake, not in a hex literal.

    THE NAME IS THE CELL: COL_<ROLE>_<PHASE>, both halves spelled exactly as the enum suffixes in
    gui.h.  So a macro can be read off the grid and the grid read off a macro, with no table in
    between -- COL_BORDER_ACTIVE is BORDER x ACTIVE and nothing else.  Renaming a macro is free
    (these are aliases); ADDING a second name for the same cell is not -- a second name is a token
    divorced from its (role, phase) coordinate, free to imply a vocabulary the grid does not
    actually have and to drift out of sync with what the cell means.  One cell, one name.

    Reading across a row shows what a phase MEANS for that role; the grid is documented once, in
    gui.h.  A render that speaks roles and phases generically wants style_col (or gui()->
    style_color from outside the library) instead -- same seam, no macro.

    Not every cell has a reader, and that is not a gap: the grid is wired uniformly because the
    schema is uniform.  TEXT never goes hot because any text that can be hot is inside a widget,
    and the text widgets return no state.  The cells stay filled so a role behaves like every
    other role; nothing is missing -- and since the bake fills every cell uniformly, an unread
    one costs a theme author nothing: there is no literal to hand-write for a cell nobody reads.

    THERE IS NO COL_SEL_* FAMILY, and the omission is deliberate rather than unfinished.  A macro
    can only spell a CONSTANT, and role and phase are routinely constant at a read site -- chrome
    genuinely knows it is painting the border, idle.  Whether an item is selected never is: every
    single selection read in the library is of the form `chosen ? ... : ...`, because selection
    is a fact about the caller's DATA, not about the draw -- which is why it is not a grid cell at
    all, but a wash applied to whichever cell above already resolved (style_col_selected,
    style_wash_selected).  Widgets that can be selected go through those, or the
    col_item_bg_selected projection below, and pass the bool along.
==============================================================================================*/

/*                             role               phase                                          */
#define COL_PANEL_IDLE     style_col( GUI_ROLE_PANEL,  GUI_PHASE_IDLE   )  /* window body        */
#define COL_PANEL_HOT      style_col( GUI_ROLE_PANEL,  GUI_PHASE_HOT    )  /* hovered surface    */
#define COL_PANEL_ACTIVE   style_col( GUI_ROLE_PANEL,  GUI_PHASE_ACTIVE )  /* pressed surface    */
#define COL_PANEL_INERT    style_col( GUI_ROLE_PANEL,  GUI_PHASE_INERT  )  /* inert backdrop     */

#define COL_TITLE_IDLE     style_col( GUI_ROLE_TITLE,  GUI_PHASE_IDLE   )  /* bar, inactive tab  */
#define COL_TITLE_HOT      style_col( GUI_ROLE_TITLE,  GUI_PHASE_HOT    )  /* hovered tab        */
#define COL_TITLE_ACTIVE   style_col( GUI_ROLE_TITLE,  GUI_PHASE_ACTIVE )  /* focused bar / tab  */
#define COL_TITLE_INERT    style_col( GUI_ROLE_TITLE,  GUI_PHASE_INERT  )  /* de-emphasized bar  */

#define COL_BG_IDLE        style_col( GUI_ROLE_BG,     GUI_PHASE_IDLE   )  /* control face       */
#define COL_BG_HOT         style_col( GUI_ROLE_BG,     GUI_PHASE_HOT    )  /* hovered face       */
#define COL_BG_ACTIVE      style_col( GUI_ROLE_BG,     GUI_PHASE_ACTIVE )  /* pressed / focused  */
#define COL_BG_INERT       style_col( GUI_ROLE_BG,     GUI_PHASE_INERT  )  /* plot backdrop      */

#define COL_BORDER_IDLE    style_col( GUI_ROLE_BORDER, GUI_PHASE_IDLE   )  /* frame line         */
#define COL_BORDER_HOT     style_col( GUI_ROLE_BORDER, GUI_PHASE_HOT    )  /* hovered edge       */
#define COL_BORDER_ACTIVE  style_col( GUI_ROLE_BORDER, GUI_PHASE_ACTIVE )  /* focused ring       */
#define COL_BORDER_INERT   style_col( GUI_ROLE_BORDER, GUI_PHASE_INERT  )  /* subdued frame      */

#define COL_TEXT_PRIMARY_IDLE    style_col( GUI_ROLE_TEXT_PRIMARY,   GUI_PHASE_IDLE   )  /* body text, caret   */
#define COL_TEXT_PRIMARY_HOT     style_col( GUI_ROLE_TEXT_PRIMARY,   GUI_PHASE_HOT    )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */
#define COL_TEXT_PRIMARY_ACTIVE  style_col( GUI_ROLE_TEXT_PRIMARY,   GUI_PHASE_ACTIVE )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */
#define COL_TEXT_PRIMARY_INERT   style_col( GUI_ROLE_TEXT_PRIMARY,   GUI_PHASE_INERT  )  /* gui_text_disabled's ink */

#define COL_TEXT_SECONDARY_IDLE   style_col( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_IDLE   )  /* hint, caption, shortcut */
#define COL_TEXT_SECONDARY_HOT    style_col( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_HOT    )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */
#define COL_TEXT_SECONDARY_ACTIVE style_col( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_ACTIVE )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */
#define COL_TEXT_SECONDARY_INERT  style_col( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_INERT  )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */

#define COL_ACCENT_IDLE    style_col( GUI_ROLE_ACCENT, GUI_PHASE_IDLE   )  /* value fill         */
#define COL_ACCENT_HOT     style_col( GUI_ROLE_ACCENT, GUI_PHASE_HOT    )  /* engaged fill       */
#define COL_ACCENT_ACTIVE  style_col( GUI_ROLE_ACCENT, GUI_PHASE_ACTIVE )  /* dragged fill       */
#define COL_ACCENT_INERT   style_col( GUI_ROLE_ACCENT, GUI_PHASE_INERT  )  /* empty track        */

#define COL_MARK_IDLE      style_col( GUI_ROLE_MARK,   GUI_PHASE_IDLE   )  /* check mark, dot    */
#define COL_MARK_HOT       style_col( GUI_ROLE_MARK,   GUI_PHASE_HOT    )  /* nav ring           */
#define COL_MARK_ACTIVE    style_col( GUI_ROLE_MARK,   GUI_PHASE_ACTIVE )  /* captured-nav ring  */
#define COL_MARK_INERT     style_col( GUI_ROLE_MARK,   GUI_PHASE_INERT  )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */

#define COL_GRAB_IDLE      style_col( GUI_ROLE_GRAB,   GUI_PHASE_IDLE   )  /* knob / thumb       */
#define COL_GRAB_HOT       style_col( GUI_ROLE_GRAB,   GUI_PHASE_HOT    )  /* hovered knob       */
#define COL_GRAB_ACTIVE    style_col( GUI_ROLE_GRAB,   GUI_PHASE_ACTIVE )  /* dragged knob       */
#define COL_GRAB_INERT     style_col( GUI_ROLE_GRAB,   GUI_PHASE_INERT  )  /* unused -- BAKE_UNUSED sentinel, see gui_bake.c */

/* The severity ladder used to close the grid here as four more roles.  It now lives in the
   extended palette instead (gui_style_ext_t, gui.h) -- style_ext( GUI_EXT_WARN ) and friends,
   a flat colour with no phase to spell a macro family over. */

/*==============================================================================================
    Stacks, sets, and the seam hooks
==============================================================================================*/

/* style stack push/pop by slot (gui_style_core.c). */
void style_push_var( gui_style_var_t var, f32 value );
void style_pop_var ( u32 count );

/* Scope an extended-palette override -- push_style_var's shape exactly, since a flat colour has
   no ramp to re-derive: a plain save / restore of the one slot. */
void style_push_ext( gui_style_ext_t ext, u32 abgr );
void style_pop_ext ( u32 count );

/* Register a brush in a set's pool; the handle is what a face cell names.  Reset per landing --
   a source re-registers its art each time it is asked to install (see the definition). */
gui_style_face_t gui_style_brush_add( const gui_brush_t* b );

/* Claim a slot in a set's extended palette beyond the reserved severity four, seeded with a
   default; the id is what a caller then reads with style_ext / overrides with push_style_ext.
   Same idempotent-per-landing contract as gui_style_brush_add (see the definition). */
gui_style_ext_t gui_style_ext_add( u32 default_abgr );

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
void metrics_compute( u32 em, u32 char_h, u32 line_h, f32 dpi_scale );

/* lattice snapping (style/gui_theme.c) -- the grid-quantum rounders composition and chrome
   share (identity when the lattice is off or q <= 1). */
f32 lat_floor    ( f32 v, u32 q );
f32 lat_floor_min( f32 v, u32 q );
f32 lat_ceil     ( f32 v, u32 q );
f32 lat_round    ( f32 v, u32 q );

/* The ACTIVE (font-scaled) style is PRIVATE to gui_theme.c -- style_active() above is the only
   read door, and metrics_compute the only writer.  Nothing outside the theme file may poke it:
   the next rescale rebuilds it wholesale from the base, so a poke there silently evaporates. */
u32 style_font_size( void );    /* style/gui_theme.c -- active em (0 = never set) */

/* The two state predicates every projection below is written in (gui_style_core.c).  style_phase
   is the ONE authoring of the interact-state -> phase rule: the public gui_item_phase
   (stock/gui_stock_widgets.c) is a cast over it, so a user widget and a stock render can never
   pick different faces.  style_is_hot stays separate because the MIX needs the hot weight even
   while an item is also active, where style_phase reports ACTIVE. */
bool style_is_hot( gui_item_state_t st );
u8   style_phase ( gui_item_state_t st );

/* State -> color projections (gui_style_core.c) -- pure: the state flags arrive as
   parameters, so these resolve identically with no interact server present. */
u32 col_item_bg( gui_item_state_t st );
u32 col_item_bg_selected( gui_item_state_t st, bool selected );
u32 col_frame_bg( gui_item_state_t st, u32 idle_color );
u32 col_grab( gui_item_state_t st );

/* The MIX (gui_style_core.c) -- the same projections over a CONTINUOUS grid coordinate, which is
   what lets a widget move between cells instead of snapping between them.  style_mix is the one
   read here that rides core's keyed anim utility (explicitly, and it is the only sanctioned
   exception to the purity rule); everything spending a mix is as pure as the reads above.
   Call style_mix ONCE per item and spend the result on every row that item paints. */
gui_style_mix_t style_mix( gui_id_t id, gui_item_state_t st, bool selected );

u32 style_col_mix   ( u8 role, gui_style_mix_t m );
u32 col_frame_bg_mix( gui_style_mix_t m, u32 idle_color );
u32 col_item_bg_mix ( gui_id_t id, gui_item_state_t st, bool selected );
u32 col_grab_mix    ( gui_id_t id, gui_item_state_t st );

/* Ink for a glyph on a bare icon button (fills only when hot/active) -- caption buttons, the dock
   maximize pin, a tab close cross.  Uses the SAME hover-or-active predicate those callers fill
   on; diverging from it would put DIM ink on an ACTIVE fill.  See the definition. */
u32 col_btn_glyph( gui_item_state_t st );

/* Border of a focusable FIELD (input box, numeric field, drag box, slider track): the face rests
   on its ground and the border alone carries focus, on BORDER[ACTIVE].  One spelling for the rule
   the whole field family shares; the reasoning is written out once, at input_text_begin. */
u32 col_field_border( gui_item_state_t st );

/* Border of a bare TRACK (a gradient ramp, a checker bar, a swatch): it paints its own ground, so
   the edge is the only thing left to light on hover.  col_field_border's sibling on the hot axis. */
u32 col_track_border( gui_item_state_t st );

/* Tab chip face / ink -- the TITLE band speaking ( state, current ).  The current chip reads
   TITLE[ACTIVE], the body colour (a live tab IS its panel -- see the bake); a pressed chip reads
   the same cell, previewing the join a release commits; the rest lift along the band.  See the
   definitions for why the current chip shows no hover. */
u32 col_tab_bg ( gui_item_state_t st, bool current );
u32 col_tab_ink( gui_item_state_t st, bool current );

/* The per-item ambient wrappers that DRIVE the seam hooks above (item_flags_resolve /
   item_flags_chrome_reset) live in stock/gui_adornment.c, declared in stock/gui_stock_internal.h:
   they apply draw-state consequences, and style never paints. */

/* Decentralized memory accounting -- this unit's fixed statics (root gui_style.c foot),
   summed into cpu_frontend_bytes by gui_ui_memory (gui_ui_mem.c). */
u32 style_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_STYLE_H
