#ifndef GUI_DRAW_H
#define GUI_DRAW_H
/*==============================================================================================

    runtime_service/gui/draw/gui_draw.h -- drawing routines (the draw unit).

    The drawing-routine library over the render server's push primitives: rect-taking
    wrappers, text painters, and the shape palette.  Widgets speak rects; only the render
    server's emit layer (draw_push_*) speaks scalar x/y/w/h with UV + texture arguments.
    Included by gui_internal.h after style/gui_style.h.

    Assembled in R3 (gui_draw.c at the gui root): gui_paint.c + gui_symbol.c + gui_canvas.c
    + the font/icon resources -- the render server renders from a pushed atlas and does not
    know what a font is, so glyph metrics and baking live here, one level up.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    Draw scope (state in render/pipeline/gui_emit_draw.c; accessors in gui_render.h)

    The paint cursor as one record: the command segment tag (owning window, sort key,
    viewport, arena band -- the ambient font stays global by design) plus the ambient glyph-clip
    window (a table cell sets it for its span).  draw_scope / draw_scope_set read and write it
    wholesale for the overlay seam.
==============================================================================================*/

typedef struct
{
    gui_id_t window;         // s_draw.cur_win (retained-cache key)
    u32      sort_key;       // s_draw.cur_z (paint order)
    u32      viewport;       // s_draw.cur_vp (target surface routing)
    u32      band;           // s_draw.cur_band (arena band: debug UI isolation)
    f32      text_clip_x0;   // ambient glyph-clip window
    f32      text_clip_x1;

} gui_draw_scope_t;

/*==============================================================================================
    The paint vocabulary -- rect + color in, pixels out
==============================================================================================*/

/* The rect-taking paint floor: a solid fill and a border outline over a gui_rect_t, carrying
   the untextured white-quad defaults so no caller repeats them. */
void draw_fill   ( gui_rect_t r, u32 col );
void draw_outline( gui_rect_t r, f32 t, u32 col );

/* Text painters (font metrics ride the atlas the render server was handed). */
f32  text_center_y( f32 y, f32 h );              /* baseline y centering one line in a row     */
f32  label_width( const char* s );               /* visible-span width per the label grammar   */
f32  label_natural_w( const char* s );
void draw_label ( f32 x, f32 y, u32 c, const char* s );
void draw_label_fit( f32 x, f32 y, u32 c, const char* s, f32 max_w );
void draw_text_fit_n( f32 x, f32 y, u32 c, const char* s, u32 len, f32 max_w );

/* The shape palette (present/gui_symbol.c) -- parameter-pure emitters. */
void draw_arrow( gui_rect_t box, gui_dir_t dir, u32 color );
void draw_bullet( f32 cx, f32 cy, f32 r, u32 color );
void draw_check_indicator( gui_rect_t box, u32 col );
void draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
void draw_collapse_arrow( gui_rect_t box, bool collapsed, u32 color );
void draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
void draw_round_rect_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl, bool filled,
                         f32 thickness, u32 col );
void draw_rule( f32 x, f32 yc, f32 w, f32 thickness, u32 col );
void draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
void draw_close_x( gui_rect_t box, u32 color );
void draw_dropdown_arrow( gui_rect_t box, u32 color );

/* Styled painters -- COL_* / metric consumers; they climb to the element unit in R8 (a draw
   routine takes its colors as parameters; these resolve their own). */
void       draw_child_bg        ( gui_rect_t r );
void       draw_child_border    ( gui_rect_t r );
void       draw_resize_highlight( gui_rect_t r, u8 edges );
void       draw_window_focus_border( gui_rect_t r );
gui_rect_t draw_field_label( gui_rect_t row, const char* label, f32 min_control_w,
                             u32 label_color );

/*==============================================================================================
    Unit lifecycle + resources -- fonts and icons live HERE, one level above the render
    server: glyph metrics and baking write into the shared atlas they hand down.  Booted by
    the frame orchestrator right after the server (gui_draw_boot), torn down before it.
==============================================================================================*/

bool gui_draw_boot    ( bool icons );    /* font registry + optional icon layer (gui_draw.c) */
void gui_draw_shutdown( void );
u32  gui_draw_unit_mem_bytes( void );    /* the draw unit's fixed statics, for mem stats */

/* Font registry + measurement (draw/gui_font.c; loader in draw/gui_font_internal.c).  The
   glyph-source half the render server consumes (font_use / font_active_id / font_valid /
   font_glyph) is declared in render/gui_render.h -- the contract, not here. */
u32  font_load              ( const char* path );           // load a .orb_font into a new id, activate it (0=fail)
bool font_load_into         ( u32 id, const char* path );   // load a .orb_font into an existing id (id 0 = default)
bool font_load_builtin      ( gui_builtin_font_t font );    // load a built-in preset (gui.h) into slot 0; true no-op for GUI_FONT_NONE
const char* font_builtin_rel_path( gui_builtin_font_t font ); // preset's asset path relative to the root; NULL for NONE / out of range
u32  font_slot_atlas_idx    ( u32 id );                     // live bindless atlas index backing a font id (0 if empty)
gui_vec2_t font_slot_atlas_size( u32 id );                  // live atlas pixel dimensions backing a font id
bool font_flush_pending     ( void );                       // commit deferred (re)loads; true if the active font changed

f32  font_char_h            ( void );                       // glyph-box height of the active font (ascent+descent)
f32  font_line_h            ( void );                       // line advance of the active font
f32  font_em                ( void );                       // nominal type size (em) -- the layout proportion base
f32  font_char_advance      ( u8 ch );                      // horizontal advance of one glyph
void font_print_active      ( void );                       // log the active font's name and metrics
f32  font_text_w            ( const char* str );            // pixel width of a NUL-terminated run
f32  font_text_w_n          ( const char* str, u32 n );     // pixel width of the first n characters

/* Icon registry + loading (draw/gui_icon.c, draw/gui_icon_load.c).  icon_get (the sprite-
   source half the emit layer consumes) is declared in render/gui_render.h. */
gui_icon_id_t   icon_register     ( const char* name, u32 w, u32 h, const u8* coverage );
gui_icon_id_t   icon_load_file    ( const char* name, const char* path );  // decode PNG/... -> R8 coverage -> icon_register
void            icon_load_builtins( void );                                // register the engine's built-in icon set from disk
gui_icon_id_t   icon_find         ( const char* name );
bool            icon_atlas_init   ( void );   // enable icon registration (shared atlas owns GPU)
void            icon_atlas_shutdown( void );  // clear the icon table

// clang-format on
/*============================================================================================*/
#endif    // GUI_DRAW_H
