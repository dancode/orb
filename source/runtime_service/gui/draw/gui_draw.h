#ifndef GUI_DRAW_H
#define GUI_DRAW_H
/*==============================================================================================

    runtime_service/gui/draw/gui_draw.h -- drawing routines (the draw unit).

    The drawing-routine library over the render server's push primitives: rect-taking
    wrappers, text painters, and the shape palette.  Widgets speak rects; only the render
    server's emit layer (draw_push_*) speaks scalar x/y/w/h with UV + texture arguments.
    Stack position: over the render server, below interact (each unit .c lists its sub-stack).

    Assembled at the gui root (gui_draw.c): gui_paint.c + gui_symbol.c + gui_canvas.c
    + the font/icon resources -- the render server renders from a pushed atlas and does not
    know what a font is, so glyph metrics and baking live here, one level up.

==============================================================================================*/

/* Font metrics + the loaded-font registry are the font/ resource (below both servers): measuring a
   glyph run is sizes-and-math, not drawing.  Pulled here so the drawing routines -- and everyone
   who includes this header -- measure through font_char_advance / font_text_w / font_line_h. */
#include "runtime_service/gui/font/gui_font.h"

// clang-format off

/* gui_draw_scope_t (the paint cursor as one record) lives in render/gui_render.h:
   the render unit defines the state (pipeline/gui_emit_draw.c) and its accessors, so the
   type lives on the definer's side of the seam. */

/*==============================================================================================
    The paint vocabulary -- rect + color in, pixels out
==============================================================================================*/

/* The rect-taking paint floor: a solid fill and a border outline over a gui_rect_t, carrying
   the untextured white-quad defaults so no caller repeats them. */
void draw_fill   ( gui_rect_t r, u32 col );
void draw_outline( gui_rect_t r, f32 t, u32 col );

/* The same floor, widened: fill a rect with a gui_brush_t (solid / gradient / sprite / nine-slice)
   instead of a bare colour.  draw_fill IS its SOLID case; see the definition for why both exist. */
void draw_fill_brush( gui_rect_t r, const gui_brush_t* b );

/* Text painters (font metrics ride the atlas the render server was handed). */
f32  text_center_y( f32 y, f32 h );              /* baseline y centering one line in a row     */
f32  label_width( const char* s );               /* visible-span width per the label grammar   */
void draw_label ( f32 x, f32 y, u32 c, const char* s );
void draw_label_fit( f32 x, f32 y, u32 c, const char* s, f32 max_w );
void draw_text_fit_n( f32 x, f32 y, u32 c, const char* s, u32 len, f32 max_w );

/* The shape palette (draw/gui_symbol.c) -- parameter-pure emitters.  The styled half of the
   family (draw_arrow, draw_check_indicator, draw_rule, draw_close_x, draw_frame -- emitters
   that resolve their own look), the styled painters, and label_natural_w all live in the
   stock unit (stock/gui_stock_internal.h). */
void draw_bullet( f32 cx, f32 cy, f32 r, u32 color );
void draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
void draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
void draw_round_rect_gradient( gui_rect_t box, f32 rounding, u32 col_a, u32 col_b, f32 angle );
void draw_inset_shadow( gui_rect_t box, f32 depth, u32 col );
void draw_round_rect_ex( gui_rect_t b, f32 rtl, f32 rtr, f32 rbr, f32 rbl, bool filled,
                         f32 thickness, u32 col );
void draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
void draw_grid( gui_rect_t box, f32 cell, f32 thickness, f32 origin_x, f32 origin_y, u32 col );
void draw_dropdown_arrow( gui_rect_t box, u32 color );

/*==============================================================================================
    Unit lifecycle + resources -- fonts and icons live HERE, one level above the render
    server: glyph metrics and baking write into the shared atlas they hand down.  Booted by
    the frame orchestrator right after the server (gui_draw_boot), torn down before it.
==============================================================================================*/

bool gui_draw_boot      ( void );        /* font registry + icon layer (gui_draw.c) */
void gui_draw_shutdown  ( void );
u32  draw_unit_mem_bytes( void );        /* the draw unit's fixed statics, for mem stats
                                            (also redeclared in render/gui_render.h -- the
                                            seam that fills the server's font bucket) */

/*==============================================================================================
    UPWARD SEAMS -- the draw unit's only calls above its layer.  Do not add more.

    label_vis_len (core/gui_id.c, home decl core/gui_core.h) -- the label grammar's visible
        span ("Text##id" measures "Text"): a pure string function, but the grammar is identity
        derivation and lives with the id system.  Redeclared here because this unit cannot
        see the server's header.

    cell_next / cell_next_w (flow/gui_layout_core.c, home decls flow/gui_flow.h) -- canvas
        placement: the user door to 2d drawing asks the composer for its cell like any widget.
==============================================================================================*/

u32        label_vis_len( const char* s );
gui_rect_t cell_next    ( f32 h );
gui_rect_t cell_next_w  ( f32 natural_w, f32 h );

/* Render-side glyph surface over the font resource (draw/gui_glyph.c; UV dispatch + upload in
   draw/gui_glyph_internal.c).  The (re)load + measurement readers (font_load / font_load_into /
   font_char_advance / font_text_w / font_line_h / ...) are the font/ resource, pulled in at the
   top of this header; the glyph-source half the render server consumes (font_use / font_active_id
   / font_valid / font_glyph) is render/gui_render.h + the font/ resource. */
u32  font_slot_atlas_idx    ( u32 id );                     // live bindless atlas index backing a font id (0 if empty)
gui_vec2_t font_slot_atlas_size( u32 id );                  // live atlas pixel dimensions backing a font id
bool font_atlas_sync        ( void );                       // upload (re)loaded fonts' pixels to the atlas; true if the active font changed
void font_slot_release      ( u32 id );                     // free font id: atlas tenant + registry slot; id 0 refused

/* Icon registry + loading (draw/gui_icon.c, draw/gui_icon_load.c).  icon_get (the sprite-
   source half the emit layer consumes) is declared in render/gui_render.h. */
gui_icon_id_t   icon_register     ( const char* name, u32 w, u32 h, const u8* coverage );
gui_icon_id_t   icon_load_file    ( const char* name, const char* path );  // decode PNG/... -> R8 coverage -> icon_register
/* The distance-field pair.  Same coverage input; the bytes are transformed (draw/gui_icon_sdf.c)
   and land in the SDF atlas instead, so the icon becomes resolution independent and takes an
   outline.  out_max is the longest edge of the STORED field (0 = default); the SOURCE should be
   several times that and should carry a transparent margin. */
gui_icon_id_t   icon_register_sdf ( const char* name, u32 w, u32 h, const u8* coverage, u32 out_max );
gui_icon_id_t   icon_load_file_sdf( const char* name, const char* path, u32 out_max );
void            icon_load_builtins( void );                                // register the engine's built-in icon set from disk
gui_icon_id_t   icon_find         ( const char* name );
bool            icon_atlas_init   ( void );   // enable icon registration (shared atlas owns GPU)
void            icon_atlas_shutdown( void );  // clear the icon table

/* Sprite registry + nine-slice authoring (draw/gui_sprite.c).  The colour sibling of the icon
   table: an icon is coverage the vertex colour paints, a sprite is art the vertex colour tints.
   sprite_get (the source contract the TESSELLATOR resolves through, since the slice expansion
   needs the source size) is declared in render/gui_render.h.  There is no init -- the sprite
   atlas creates itself on the first registration. */
gui_sprite_id_t sprite_register        ( const char* name, u32 w, u32 h, const u8* rgba );
gui_sprite_id_t sprite_load_file       ( const char* name, const char* path );  // decode PNG/... -> RGBA8
gui_sprite_id_t sprite_find            ( const char* name );
bool            sprite_set_slice       ( gui_sprite_id_t id, gui_pad_t slice );  // insets in SOURCE px
gui_pad_t       sprite_slice           ( gui_sprite_id_t id );
gui_vec2_t      sprite_size            ( gui_sprite_id_t id );                   // native pixel size
void            sprite_registry_shutdown( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_DRAW_H
