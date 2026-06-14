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
bool imgui_load_font( const char* path );
void imgui_new_frame( i32 win_w, i32 win_h, f32 dt );
void imgui_render( rhi_cmd_t cmd, i32 win_w, i32 win_h );

bool imgui_event( const app_event_t* ev );

void imgui_set_next_window_pos ( f32 x, f32 y, imgui_cond_t cond );
void imgui_set_next_window_size( f32 w, f32 h, imgui_cond_t cond );
bool imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h, imgui_win_flags_t flags );
void imgui_end_window( void );

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

bool imgui_begin_child( const char* id, f32 w, f32 h, imgui_win_flags_t flags );
void imgui_end_child( void );
void imgui_push_layout( void );
void imgui_pop_layout( void );

void imgui_layout( imgui_layout_t desc );
void imgui_layout_default( void );
void imgui_stack( void );
void imgui_row( f32 row_h );
void imgui_columns( const f32* tracks );
void imgui_cols_n( u32 n );
void imgui_row_cols( f32 row_h, u32 n );
void imgui_row2( f32 a, f32 b );
void imgui_row3( f32 a, f32 b, f32 c );
void imgui_row4( f32 a, f32 b, f32 c, f32 d );
void imgui_row_track( f32 row_h, const f32* cols );
void imgui_form( imgui_label_side_t side, f32 label_w );
void imgui_form_split( imgui_label_side_t side, f32 label, f32 control );
void imgui_field_split( imgui_label_side_t side, f32 label, f32 control );
void imgui_field_label_left( f32 width );
void imgui_field_label_right( f32 width );
void imgui_pad( imgui_pad_t region_pad );
void imgui_grid( imgui_layout_t desc );
void imgui_grid_cells( u32 ncols, u32 nrows );
void imgui_pack( imgui_pack_dir_t dir );
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

f32 imgui_line_h( void );
f32 imgui_text_w( const char* s );
f32 imgui_h_min( void );
f32 imgui_w_min( void );
f32 imgui_calc_row( f32 content_h );
f32 imgui_calc_col( f32 content_w );
imgui_vec2_t imgui_content_avail( void );
void imgui_set_window_drag( imgui_win_drag_t mode );

void imgui_push_id( const char* str );
void imgui_push_id_int( i32 i );
void imgui_pop_id( void );

void imgui_push_item_flag( imgui_item_flags_t flag, bool enable );
void imgui_pop_item_flag( void );
void imgui_next_item_flag( imgui_item_flags_t flag, bool enable );

void imgui_push_style_color( imgui_col_t slot, u32 abgr );
void imgui_pop_style_color( u32 count );
void imgui_next_style_color( imgui_col_t slot, u32 abgr );
void imgui_push_style_var( imgui_style_var_t var, f32 value );
void imgui_pop_style_var( u32 count );
void imgui_next_style_var( imgui_style_var_t var, f32 value );

void imgui_text( const char* str );
void imgui_textf( const char* fmt, ... );
void imgui_bullet_text( const char* str );
void imgui_label_text( const char* label, const char* value );
bool imgui_button( const char* label );
bool imgui_arrow_button( const char* id_str, imgui_dir_t dir );
bool imgui_checkbox( const char* label, bool* v );
bool imgui_radio_button( const char* label, i32* v, i32 value );
bool imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi );
bool imgui_input_text( const char* label, char* buf, u32 bufsz );
bool imgui_selectable( const char* label, bool* selected );
bool imgui_collapsing_header( const char* label );
bool imgui_tree_node( const char* label );
void imgui_tree_pop( void );
void imgui_indent( f32 w );
void imgui_unindent( f32 w );
void imgui_separator_text( const char* label );

void imgui_set_font      ( imgui_font_t font );
void imgui_set_bmp_scale ( u32 scale );
void imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr );
void imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str );
void imgui_push_clip( f32 x, f32 y, f32 w, f32 h );
void imgui_pop_clip( void );

void imgui_debug_set_layers( u32 layers );
u32  imgui_debug_get_layers( void );

/*============================================================================================*/
#endif    // IMGUI_HOST_H
