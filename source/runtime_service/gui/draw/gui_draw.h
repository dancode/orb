#ifndef GUI_DRAW_H
#define GUI_DRAW_H
/*==============================================================================================

    runtime_service/gui/draw/gui_draw.h -- drawing routines (the draw unit).

    The drawing-routine library over the render server's push primitives: rect-taking
    wrappers, text painters, and the shape palette.  Widgets speak rects; only the render
    server's emit layer (draw_push_*) speaks scalar x/y/w/h with UV + texture arguments.
    Included by gui_internal.h after style/gui_style.h.

    Definitions live in present/gui_paint_core.c + present/gui_symbol.c today; R3 assembles
    them (plus user/gui_canvas.c and the font/icon resources -- the render server renders
    from a pushed atlas and does not know what a font is) into the draw unit proper.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    Draw scope (state in backend/pipeline/gui_emit_draw.c; accessors in gui_backend.h)

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

// clang-format on
/*============================================================================================*/
#endif    // GUI_DRAW_H
