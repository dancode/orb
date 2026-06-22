#ifndef IMGUI_HOST_H
#define IMGUI_HOST_H
/*==============================================================================================

    runtime_service/imgui/imgui_host.h -- Host-only imgui services.
    Includes imgui_api.h.

    Usage:
        mod_static_load( "imgui", imgui_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( imgui );

==============================================================================================*/

#include "runtime_service/imgui/imgui_api.h"

// clang-format off
/*==============================================================================================
    Module Descriptor
==============================================================================================*/

mod_desc_t* imgui_get_mod_desc( void );

/*==============================================================================================
    Direct-call functions (host and sandbox use only)
==============================================================================================*/

bool imgui_init( void );
void imgui_shutdown( void );

imgui_mem_stats_t imgui_mem_stats( void );
void imgui_print_mem_stats( void );

/* font */
bool imgui_load_font( const char* path );

/* frame */
void imgui_new_frame( f32 dt );
void imgui_frame_begin( f32 dt );
void imgui_ctx_begin( imgui_ctx_t ctx );
void imgui_render( imgui_vp_t vp, rhi_cmd_t cmd );

/* multi-context */
imgui_ctx_t imgui_ctx_create       ( const imgui_ctx_config_t* cfg );
void        imgui_ctx_destroy      ( imgui_ctx_t ctx );
void        imgui_ctx_bind         ( imgui_ctx_t ctx );
void        imgui_ctx_set_listening( imgui_ctx_t ctx, bool listen );

/* viewport management */
imgui_vp_t imgui_viewport_open  ( i32 win_id, i32 w, i32 h );
void       imgui_viewport_close ( imgui_vp_t vp );
void       imgui_viewport_resize( imgui_vp_t vp, i32 w, i32 h );

/* imgui-owned floater surfaces (window + context owned by imgui) */
imgui_vp_t imgui_viewport_spawn         ( const char* title, i32 x, i32 y, i32 w, i32 h );
void       imgui_update_platform_windows( void );
void       imgui_render_floaters        ( void );

/* io */
bool imgui_event( const app_event_t* ev );

/* window */
void imgui_set_next_window_pos ( f32 x, f32 y, imgui_cond_t cond );
void imgui_set_next_window_size( f32 w, f32 h, imgui_cond_t cond );
void imgui_set_next_window_viewport( imgui_vp_t vp );
void imgui_set_next_window_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h );
bool imgui_begin_window( const char* title, imgui_win_flags_t flags );
void imgui_end_window( void );

/* docking */
imgui_dock_id_t imgui_dockspace_over_viewport( imgui_vp_t vp, imgui_dockspace_flags_t flags );
imgui_dock_id_t imgui_dock_split( imgui_dock_id_t node, imgui_dir_t dir, f32 ratio, imgui_dock_id_t* out_remain );
void imgui_dock_window( const char* title, imgui_dock_id_t node );
void imgui_dock_undock( const char* title );
bool imgui_is_window_docked( const char* title );
u32  imgui_dock_save( imgui_vp_t vp, char* buf, u32 bufsz );
bool imgui_dock_load( imgui_vp_t vp, const char* text );

/* popup */
void imgui_open_popup( const char* id );
bool imgui_begin_popup( const char* id, imgui_win_flags_t flags );
bool imgui_begin_popup_modal( const char* id, const char* title, imgui_win_flags_t flags );
void imgui_end_popup( void );
void imgui_close_current_popup( void );
bool imgui_is_popup_open( const char* id );
bool imgui_begin_popup_context_item( const char* id );
bool imgui_begin_popup_context_window( const char* id );
void imgui_set_item_tooltip( const char* text );
bool imgui_begin_tooltip( void );
void imgui_end_tooltip( void );

/* menu */
bool imgui_begin_main_menu_bar( void );
void imgui_end_main_menu_bar( void );
bool imgui_begin_menu_bar( void );
void imgui_end_menu_bar( void );
bool imgui_begin_menu( const char* label );
void imgui_end_menu( void );
bool imgui_menu_item( const char* label, const char* shortcut, bool* selected );

/* child layout */
bool imgui_begin_child( const char* id, f32 w, f32 h, imgui_win_flags_t flags );
void imgui_end_child( void );
void imgui_push_layout( void );
void imgui_pop_layout( void );

/* layout */
void imgui_layout( imgui_layout_t desc );
void imgui_layout_default( void );

/* layout - stack */
void imgui_stack( void );

/* layout - rows and columns */
void imgui_row( f32 row_h );
void imgui_columns( const f32* tracks );
void imgui_cols_n( u32 n );
void imgui_row_cols( f32 row_h, u32 n );
void imgui_row2( f32 a, f32 b );
void imgui_row3( f32 a, f32 b, f32 c );
void imgui_row4( f32 a, f32 b, f32 c, f32 d );
void imgui_row_track( f32 row_h, const f32* cols );

/* layout - split forms */
void imgui_form( imgui_label_side_t side, f32 label_w );
void imgui_form_split( imgui_label_side_t side, f32 label, f32 control );
void imgui_field_split( imgui_label_side_t side, f32 label, f32 control );
void imgui_field_label_left( f32 width );
void imgui_field_label_right( f32 width );

/* layout - grids and packing */
void imgui_pad( imgui_pad_t region_pad );

/* layout - grid */
void imgui_grid( imgui_layout_t desc );
void imgui_grid_cells( u32 ncols, u32 nrows );

/* layout - pack */ 
void imgui_pack( imgui_pack_dir_t dir );

/* layout bar + strip */
void imgui_bar( void );
void imgui_strip( void );

void imgui_pack_size( f32 unit );
void imgui_pack_nextline( void );

void imgui_align( imgui_align_t a );
void imgui_same_line( f32 spacing );
void imgui_stack_sameline( f32 spacing );
void imgui_skip( void );
void imgui_spacing( f32 h );
void imgui_separator( void );

/* layout - blank space canvas */
imgui_rect_t imgui_canvas( f32 height );

/* layout - formatting helpers */
f32 imgui_line_h( void );
f32 imgui_text_w( const char* s );
f32 imgui_h_min( void );
f32 imgui_w_min( void );
f32 imgui_calc_row( f32 content_h );
f32 imgui_calc_col( f32 content_w );
imgui_vec2_t imgui_content_avail( void );
imgui_vec2_t imgui_cursor_screen_pos( void );
imgui_rect_t imgui_dummy( f32 w, f32 h );

/* layout - interactive helpers */
bool imgui_invisible_button( const char* id_str, imgui_rect_t r );
bool imgui_is_mouse_hovering_rect( imgui_rect_t r );
void imgui_set_window_drag( imgui_win_drag_t mode );
void imgui_set_nav_window( const char* title );

/* id stack and item flags */
void imgui_push_id( const char* str );
void imgui_push_id_int( i32 i );
void imgui_pop_id( void );

/* item flags */
void imgui_push_item_flag( imgui_item_flags_t flag, bool enable );
void imgui_pop_item_flag( void );
void imgui_next_item_flag( imgui_item_flags_t flag, bool enable );
void imgui_begin_disabled( bool disabled );
void imgui_end_disabled( void );

/* style modifiers */
void imgui_push_style_color( imgui_col_t slot, u32 abgr );
void imgui_pop_style_color( u32 count );
void imgui_next_style_color( imgui_col_t slot, u32 abgr );
void imgui_push_style_var( imgui_style_var_t var, f32 value );
void imgui_pop_style_var( u32 count );
void imgui_next_style_var( imgui_style_var_t var, f32 value );

/* widgets */
void imgui_text( const char* str );
void imgui_textf( const char* fmt, ... );
void imgui_bullet_text( const char* str );
void imgui_text_colored( u32 abgr, const char* str );
void imgui_text_disabled( const char* str );
void imgui_text_wrapped( const char* str );
void imgui_bullet( void );
void imgui_new_line( void );
void imgui_label_text( const char* label, const char* value );
bool imgui_button( const char* label );
bool imgui_small_button( const char* label );
void imgui_progress_bar( f32 fraction, const char* overlay );
bool imgui_arrow_button( const char* id_str, imgui_dir_t dir );
bool imgui_checkbox( const char* label, bool* v );
bool imgui_radio_button( const char* label, i32* v, i32 value );

/* widget - sliders */
bool imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi );
bool imgui_slider_float_step( const char* label, f32* v, f32 lo, f32 hi, f32 step );
bool imgui_slider_int( const char* label, i32* v, i32 lo, i32 hi );
bool imgui_drag_int( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format );
bool imgui_drag_float( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool imgui_drag_float2( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool imgui_drag_float3( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool imgui_drag_float4( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );

/* widget - input boxes */
bool imgui_input_text( const char* label, char* buf, u32 bufsz );
bool imgui_input_text_ex( const char* label, char* buf, u32 bufsz, imgui_text_cb_fn on_change, void* cb_user );
bool imgui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz );
bool imgui_input_int   ( const char* label, i32* v, i32 step, i32 step_fast );
bool imgui_input_float ( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt );
bool imgui_input_double( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt );
bool imgui_input_float2( const char* label, f32* v, const char* fmt );
bool imgui_input_float3( const char* label, f32* v, const char* fmt );
bool imgui_input_float4( const char* label, f32* v, const char* fmt );

/* widget - combo and list box */
bool imgui_selectable( const char* label, bool* selected );
bool imgui_begin_combo( const char* label, const char* preview_value, imgui_combo_flags_t flags );
void imgui_end_combo( void );
bool imgui_combo( const char* label, i32* current_item, const char* const items[], i32 count );
bool imgui_begin_listbox( const char* label, f32 w, f32 h );
void imgui_end_listbox( void );
bool imgui_listbox( const char* label, i32* current_item, const char* const items[], i32 count, i32 height_in_items );

/* widget - collapsing items */
bool imgui_collapsing_header( const char* label );
bool imgui_tree_node( const char* label );
void imgui_tree_pop( void );

/* widget - formatting */
void imgui_indent( f32 w );
void imgui_unindent( f32 w );
void imgui_separator_text( const char* label );
void imgui_help_marker( const char* text );

/* font */
void imgui_set_font      ( imgui_font_t font );
void imgui_set_bmp_scale ( u32 scale );

/* drawing */
void imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr );
void imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str );
imgui_vec2_t imgui_text_size( const char* s );
void imgui_draw_text_in( imgui_rect_t r, imgui_align_t align, u32 col, const char* s );
void imgui_draw_text_clipped( imgui_rect_t r, imgui_align_t align, u32 col, const char* s );

/* icons -- runtime icon atlas */
imgui_icon_id_t imgui_register_icon( const char* name, u32 w, u32 h, const u8* coverage );
imgui_icon_id_t imgui_find_icon( const char* name );
imgui_vec2_t imgui_icon_size( imgui_icon_id_t id );
void imgui_image( imgui_icon_id_t id, f32 w, f32 h, u32 col );
void imgui_draw_icon_in( imgui_rect_t r, imgui_icon_id_t id, u32 col );

/* symbol + shape render primitives -- Dear ImGui Render* / AddXxx family (normal pipeline, not the
   icon atlas).  Implemented in imgui_draw_symbol.c. */
void imgui_render_check_mark( imgui_rect_t box, u32 col );
void imgui_render_arrow( imgui_rect_t box, imgui_dir_t dir, u32 col );
void imgui_render_bullet( f32 cx, f32 cy, f32 r, u32 col );
void imgui_render_close( imgui_rect_t box, u32 col );
void imgui_render_arrow_pointing_at( f32 tx, f32 ty, f32 half, imgui_dir_t dir, u32 col );
void imgui_render_chevron( imgui_rect_t box, imgui_dir_t dir, f32 thickness, u32 col );
void imgui_render_plus_minus( imgui_rect_t box, bool plus, f32 thickness, u32 col );
void imgui_render_frame( imgui_rect_t box, u32 col_bg, u32 col_border, f32 border );
void imgui_render_round_rect( imgui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl, bool filled, f32 thickness, u32 col );
void imgui_render_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col );
void imgui_render_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
void imgui_render_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col );
void imgui_render_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col );
void imgui_render_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
void imgui_render_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
void imgui_render_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col );
void imgui_render_checker( imgui_rect_t box, f32 cell, u32 col_a, u32 col_b );
void imgui_render_hatch( imgui_rect_t box, f32 spacing, f32 thickness, u32 col );
void imgui_render_gradient( imgui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
void imgui_render_shadow( imgui_rect_t box, f32 spread, u32 col );
void imgui_render_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
void imgui_render_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );
void imgui_render_grip( imgui_rect_t box, u32 col );
void imgui_render_spinner( imgui_rect_t box, f32 t, f32 thickness, u32 col );
void imgui_render_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );
void imgui_set_check_style( u32 style );
void imgui_set_bullet_style( u32 style );
void imgui_set_arrow_style( u32 style );

/* draw -- paths */
void imgui_draw_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr );
void imgui_draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 abgr );
void imgui_draw_polyline( const imgui_vec2_t* pts, u32 count, f32 thickness, imgui_stroke_align_t align, bool closed, u32 abgr );
void imgui_path_clear( void );
void imgui_path_line_to( f32 x, f32 y );
void imgui_path_stroke( f32 thickness, imgui_stroke_align_t align, bool closed, u32 abgr );

/* clipping */
void imgui_push_clip( f32 x, f32 y, f32 w, f32 h );
void imgui_pop_clip( void );

/* debug drawing */
void imgui_debug_set_layers( u32 layers );
u32  imgui_debug_get_layers( void );

/* input */
bool imgui_want_capture_mouse( void );
bool imgui_want_capture_keyboard( void );

/* last-item introspection */
bool         imgui_is_item_hovered( void );
bool         imgui_is_item_active( void );
bool         imgui_is_item_clicked( void );
bool         imgui_is_item_focused( void );
bool         imgui_is_item_activated( void );
bool         imgui_is_item_deactivated( void );
bool         imgui_is_item_visible( void );
imgui_rect_t imgui_get_item_rect( void );

bool imgui_is_key_down( app_key_t key );
bool imgui_is_key_pressed( app_key_t key );
bool imgui_is_key_pressed_repeat( app_key_t key );
bool imgui_is_key_released( app_key_t key );
bool imgui_is_mouse_down( app_mouse_button_t b );
bool imgui_is_mouse_clicked( app_mouse_button_t b );
bool imgui_is_mouse_released( app_mouse_button_t b );
bool imgui_is_mouse_double_clicked( app_mouse_button_t b );
void imgui_get_mouse_pos( f32* x, f32* y );
f32  imgui_get_mouse_wheel( void );

/* cursor */
void         imgui_set_mouse_cursor( app_cursor_t c );
app_cursor_t imgui_get_mouse_cursor( void );

/* time */
f32  imgui_get_delta_time( void );
f64  imgui_get_time( void );

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_HOST_H
