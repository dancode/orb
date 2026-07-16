#ifndef GUI_HOST_H
#define GUI_HOST_H
/*==============================================================================================

    runtime_service/gui/gui_host.h -- Host-only gui services.
    Includes gui_api.h.

    Usage:
        mod_static_load( "gui", gui_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( gui );

==============================================================================================*/

#include "runtime_service/gui/gui_api.h"

// clang-format off
/*==============================================================================================
    Module Descriptor
==============================================================================================*/

mod_desc_t* gui_get_mod_desc( void );

/*==============================================================================================
    Direct-call functions (host and sandbox use only)
==============================================================================================*/

void gui_init_config_back( gui_backend_caps_t caps );
void gui_init_config_front( gui_forward_caps_t caps );
bool gui_init( gui_builtin_font_t font );
void gui_shutdown( void );

gui_mem_stats_t gui_mem_stats( void );
void gui_print_mem_stats( void );

/* NOTE: the built-in perf/state overlays and the pipeline dashboard are internal now -- armed by
   gui_debug_enable( true ) and emitted behind hotkeys (P / O / F10); hosts no longer call them. */

/* font */
u32  gui_font_load( const char* path );

/* asset_path -- resolve a path relative to the engine's assets/ root (see gui_api.h) */
void gui_asset_path( const char* relative, char* out, int out_size );

/* frame -- frame_begin returns frame_dirty: emit the UI build only when true.  set_frame_hooks
   hands gui the OS clock / sleep / wait callbacks it cannot reach itself (typically
   sys_tick_seconds, sys_sleep_milliseconds, sys_wait_for_os_events_ms); frame_pace is the
   end-of-loop sleep / idle wait.  See the FRAME CONTRACT in gui_api.h. */
void gui_set_frame_hooks( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events );
bool gui_frame_begin( f32 dt );
void gui_frame_end( void );
void gui_frame_pace( i32 spin_sleep_ms, i32 anim_sleep_ms );   /* 0 opts that sleep out */
void gui_set_idle_skip( bool on );
bool gui_idle_skip( void );

void gui_window_anim_enable( bool on );
bool gui_window_anim_is_enabled( void );
void gui_ctx_begin( gui_ctx_id_t ctx );
void gui_ctx_end( void );
void gui_render( gui_vp_t vp, rhi_cmd_t cmd );

/* multi-context */
gui_ctx_id_t gui_ctx_create       ( const gui_ctx_config_t* cfg );
void         gui_ctx_destroy      ( gui_ctx_id_t ctx );
void         gui_ctx_bind         ( gui_ctx_id_t ctx );
void         gui_ctx_set_listening( gui_ctx_id_t ctx, bool listen );

/* viewport management */
gui_vp_t    gui_viewport_open  ( i32 win_id );
void       gui_viewport_close ( gui_vp_t vp );
void       gui_viewport_resize( gui_vp_t vp, i32 w, i32 h );
f32        gui_viewport_shell ( gui_vp_t vp, const char* title, gui_win_flags_t flags );
f32        gui_viewport_caption_h( gui_vp_t vp );

/* boot-tier host front end (gui_boot.c) */
gui_vp_t    gui_boot( const gui_boot_desc_t* desc );
bool       gui_frame_poll( f32* out_dt );
bool       gui_present_begin( rhi_cmd_t* out_cmd );
void       gui_present_end( void );

/* gui-owned floater surfaces (window + context owned by gui) */
gui_vp_t    gui_viewport_spawn          ( const char* title, i32 x, i32 y, i32 w, i32 h );
void        gui_viewport_update         ( void );
void        gui_viewport_render_floaters( void );

/* io */
bool gui_event( const app_event_t* ev );

/* window */
void gui_window_set_next_pos ( f32 x, f32 y, gui_cond_t cond );
void gui_window_set_next_size( f32 w, f32 h, gui_cond_t cond );
void gui_window_set_next_viewport( gui_vp_t vp );
void gui_window_set_next_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h );
bool gui_window_begin( const char* title, gui_win_flags_t flags );
void gui_window_end( void );
void gui_window_set_open( const char* title, bool open );
bool gui_window_is_open( const char* title );

/* docking */
void gui_dockspace_inset( gui_vp_t vp, f32 top );
gui_dock_id_t gui_dockspace_over_viewport( gui_vp_t vp, gui_dockspace_flags_t flags );
gui_dock_id_t gui_dock_split( gui_dock_id_t node, gui_dir_t dir, f32 ratio, gui_dock_id_t* out_remain );
gui_dock_id_t gui_dock_split_root( gui_vp_t vp, gui_dir_t dir, f32 ratio );
void gui_dock_window( const char* title, gui_dock_id_t node );
void gui_dock_undock( const char* title );
bool gui_window_is_docked( const char* title );
void gui_window_tab( const char* title, const char* onto_title );
u32  gui_dock_save( gui_vp_t vp, char* buf, u32 bufsz );
bool gui_dock_load( gui_vp_t vp, const char* text );
void gui_dock_clear( gui_vp_t vp );

/* popup */
void gui_popup_open( const char* id );
bool gui_popup_begin( const char* id, gui_win_flags_t flags );
bool gui_popup_modal_begin( const char* id, const char* title, gui_win_flags_t flags );
void gui_popup_end( void );
void gui_popup_close_current( void );
bool gui_popup_is_open( const char* id );
bool gui_popup_context_item_begin( const char* id );
bool gui_popup_context_window_begin( const char* id );
void gui_set_item_tooltip( const char* text );
bool gui_tooltip_begin( void );
void gui_tooltip_end( void );

/* drag and drop */
bool gui_drag_source_begin( gui_drag_flags_t flags );
void gui_drag_source_end( void );
bool gui_drag_payload_set( const char* type, const void* data, u32 size );
bool gui_drag_target_begin( void );
const gui_drag_payload_t* gui_drag_payload_accept( const char* type, gui_drag_flags_t flags );
void gui_drag_target_end( void );
bool gui_drag_active( void );
const gui_drag_payload_t* gui_drag_payload_peek( void );

/* menu */
bool gui_main_menu_bar_begin( void );
void gui_main_menu_bar_end( void );
f32  gui_main_menu_bar_h( void );
bool gui_menu_bar_begin( void );
void gui_menu_bar_end( void );
bool gui_menu_begin( const char* label );
void gui_menu_end( void );
bool gui_menu_item( const char* label, const char* shortcut, bool* selected );

/* toolbar */
bool gui_toolbar_begin( const char* str_id );
void gui_toolbar_end( void );
bool gui_toolbar_button( const char* id_str, gui_icon_id_t icon, const char* tooltip );
bool gui_toolbar_toggle( const char* id_str, gui_icon_id_t icon, bool* v, const char* tooltip );
bool gui_toolbar_dropdown_begin( const char* id_str, gui_icon_id_t icon, const char* tooltip );
void gui_toolbar_dropdown_end( void );
void gui_toolbar_separator( void );

/* child layout */
bool gui_child_begin( const char* id, f32 w, f32 h, gui_win_flags_t flags );
void gui_child_end( void );
void gui_push_layout( void );
void gui_push_layout_overlay( gui_rect_t rect );
void gui_pop_layout( void );

/* root region -- a fixed-rect layout primitive with no window chrome */
bool gui_region_begin( const char* id, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                       gui_win_flags_t flags );
void gui_region_end( void );

/* layout */
void gui_layout( gui_layout_t desc );
void gui_layout_default( void );

/* layout - stack */
void gui_stack( void );

/* layout - rows and columns */
void gui_row( f32 row_h );
void gui_cols( const f32* tracks );
void gui_cols_n( u32 n );
void gui_row_cols( f32 row_h, const f32* tracks );
void gui_row_cols_n( f32 row_h, u32 n );
void gui_row2( f32 a, f32 b );
void gui_row3( f32 a, f32 b, f32 c );
void gui_row4( f32 a, f32 b, f32 c, f32 d );

/* layout - split forms */
void gui_form( gui_label_side_t side, f32 label_w );
void gui_field_split( gui_label_side_t side, f32 label, f32 control );
void gui_field_label_left( f32 width );
void gui_field_label_right( f32 width );

/* layout - grids and packing */
void gui_pad( gui_pad_t region_pad );

/* layout - grid */
void gui_grid( gui_layout_t desc );
void gui_grid_cells( u32 ncols, u32 nrows );

/* layout - pack */ 
void gui_pack( gui_pack_dir_t dir );

/* layout bar + strip */
void gui_bar( void );
void gui_strip( void );

void gui_pack_size( f32 unit );
void gui_pack_nextline( void );

/* layout - save/restore the declared shape around a scoped header change */
void gui_push_layout_state( void );
void gui_pop_layout_state( void );

void gui_align( gui_align_t a );
void gui_next_item_fit( f32 unit );
void gui_same_line( f32 spacing );
void gui_stack_same_line( f32 spacing );
void gui_skip( void );
void gui_separator( void );

/* user substrate - blank space canvas (user/gui_canvas.c): reserve a rect to draw into */
gui_rect_t gui_canvas( f32 height );

/* sizing (sz_) - intent to px; grid-first first, content-fit escape hatches last */
f32 gui_sz_u( f32 n );
f32 gui_sz_row_gap( void );
f32 gui_sz_rows_h( u32 n );
f32 gui_sz_scale_row( gui_scale_t s );
f32 gui_sz_line_h( void );
f32 gui_sz_chars( f32 n );
f32 gui_sz_fit_row( f32 content_h );
f32 gui_sz_fit_col( f32 content_w );
gui_vec2_t gui_content_avail( void );
gui_vec2_t gui_cursor_screen_pos( void );
gui_rect_t gui_content_rect( void );
u32        gui_split( gui_rect_t area, gui_axis_t axis, const f32* sizes, f32 gap, gui_rect_t* out );
u32        gui_carve( const f32* form, gui_rect_t area, f32 gap, gui_rect_t* out, u32 max );
gui_rect_t gui_anchor( gui_rect_t parent, gui_anchor_t a );
gui_rect_t gui_empty( f32 w, f32 h );

/* user substrate - behavior on caller rects (user/gui_behavior.c): the shared interaction
   state machine run over a rect YOU derived.  item() reports the full state; invisible_button
   is its click bit.  A custom widget = rect (canvas/split/carve) + item() + draw_*. */
gui_item_state_t gui_item( const char* id_str, gui_rect_t r );
bool gui_invisible_button( const char* id_str, gui_rect_t r );
bool gui_is_mouse_hovering_rect( gui_rect_t r );
void gui_window_set_drag( gui_win_drag_t mode );
void gui_window_set_nav( const char* title );

/* id stack and item flags */
void gui_push_id( const char* str );
void gui_push_id_int( i32 i );
void gui_pop_id( void );

/* item flags */
void gui_push_item_flag( gui_item_flags_t flag, bool enable );
void gui_pop_item_flag( void );
void gui_next_item_flag( gui_item_flags_t flag, bool enable );
void gui_disabled_begin( bool disabled );
void gui_disabled_end( void );

/* style modifiers */
void gui_push_style_color( gui_col_t slot, u32 abgr );
void gui_pop_style_color( u32 count );
void gui_next_style_color( gui_col_t slot, u32 abgr );
void gui_push_style_var( gui_style_var_t var, f32 value );
void gui_pop_style_var( u32 count );
void gui_next_style_var( gui_style_var_t var, f32 value );
void gui_scale_push( gui_scale_t s );
void gui_scale_pop( void );
void gui_set_check_style( u8 style );
void gui_set_bullet_style( u8 style );
void gui_set_arrow_style( u8 style );

/* themes */
const gui_theme_t* gui_theme_list ( u32* count_out );
bool               gui_theme_set  ( const char* name );
const char*        gui_theme_get  ( void );
void               gui_theme_reset( void );

/* widgets */
void gui_text( const char* str );
void gui_textf( const char* fmt, ... );
void gui_bullet_text( const char* str );
void gui_text_colored( u32 abgr, const char* str );
void gui_text_disabled( const char* str );
void gui_text_wrapped( const char* str );
void gui_bullet( void );
void gui_new_line( f32 h );
void gui_label_text( const char* label, const char* value );
bool gui_button( const char* label );
bool gui_small_button( const char* label );
void gui_progress_bar( f32 fraction, const char* overlay );
bool gui_arrow_button( const char* label, gui_dir_t dir );
bool gui_checkbox( const char* label, bool* v );
bool gui_radio_button( const char* label, i32* v, i32 value );

/* widget - sliders */
bool gui_slider_float( const char* label, f32* v, f32 lo, f32 hi );
bool gui_slider_float_step( const char* label, f32* v, f32 lo, f32 hi, f32 step );
bool gui_slider_int( const char* label, i32* v, i32 lo, i32 hi );
/* Drag widgets: v_speed is value units per pixel dragged.  Pass v_speed <= 0 for the range-relative
   default -- the whole [v_min,v_max] span is swept over a fixed drag distance, so every bounded drag
   feels the same regardless of range (recommended; pass an explicit v_speed only for a specific
   per-pixel feel).  Alt drags finer, Shift faster, Ctrl+Click edits the value as text. */
bool gui_drag_int( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format );
bool gui_drag_float( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool gui_drag_float2( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool gui_drag_float3( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
bool gui_drag_float4( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );

bool gui_color_edit3( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags );
bool gui_color_edit4( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags );

/* widget - input boxes */
bool gui_input_text( const char* label, char* buf, u32 bufsz );
bool gui_input_text_ex( const char* label, char* buf, u32 bufsz, gui_text_cb_fn on_change, void* cb_user );
bool gui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz );
bool gui_input_int   ( const char* label, i32* v, i32 step, i32 step_fast );
bool gui_input_float ( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt );
bool gui_input_double( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt );
bool gui_input_float2( const char* label, f32* v, const char* fmt );
bool gui_input_float3( const char* label, f32* v, const char* fmt );
bool gui_input_float4( const char* label, f32* v, const char* fmt );

/* widget - combo and list box */
bool gui_selectable( const char* label, bool* selected );
bool gui_combo_begin( const char* label, const char* preview_value, gui_combo_flags_t flags );
void gui_combo_end( void );
bool gui_combo( const char* label, i32* current_item, const char* const items[], i32 count );
bool gui_listbox_begin( const char* label, f32 w, f32 h );
void gui_listbox_end( void );
bool gui_listbox( const char* label, i32* current_item, const char* const items[], i32 count, i32 height_in_items );

/* widget - collapsing items */
bool gui_collapsing_header( const char* label );
bool gui_tree_node( const char* label );
void gui_tree_pop( void );

/* widget - formatting */
void gui_indent( f32 w );
void gui_unindent( f32 w );
void gui_separator_text( const char* label );
void gui_help_marker( const char* text );

/* font */
bool gui_font_load_into     ( u32 id, const char* path );
void gui_font_use           ( u32 id );
void gui_push_font          ( u32 id );
void gui_pop_font           ( void );
u32  gui_font_active_id     ( void );

/* drawing */
void gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr );
void gui_draw_rects( const gui_rect_col_t* rects, u32 count );
void gui_draw_text( f32 x, f32 y, u32 abgr, const char* str );
gui_vec2_t gui_text_size( const char* s );
void gui_draw_text_in( gui_rect_t r, gui_align_t align, u32 col, const char* s );
void gui_draw_text_clipped( gui_rect_t r, gui_align_t align, u32 col, const char* s );

/* icons -- runtime icon atlas */
gui_icon_id_t gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage );
gui_icon_id_t gui_load_icon( const char* name, const char* path );
gui_icon_id_t gui_find_icon( const char* name );
gui_vec2_t gui_icon_size( gui_icon_id_t id );
void gui_image( gui_icon_id_t id, f32 w, f32 h, u32 col );
void gui_draw_icon_in( gui_rect_t r, gui_icon_id_t id, u32 col );

/* RGBA textures -- arbitrary bindless texture as a full-color quad (scene viewport) */
void gui_image_texture( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr );
void gui_draw_texture_in( gui_rect_t r, u32 bindless_idx, u32 tint_abgr );

/* font atlas access -- bindless index + pixel size for previewing a font's live GPU atlas */
u32 gui_font_atlas_idx( u32 font_id );
gui_vec2_t gui_font_atlas_size( u32 font_id );

/* symbol + shape render primitives -- Dear ImGui Render* / AddXxx family (normal pipeline, not the
   icon atlas).  Implemented in gui_symbol.c. */
void gui_draw_check_mark( gui_rect_t box, u32 col );
void gui_draw_arrow( gui_rect_t box, gui_dir_t dir, u32 col );
void gui_draw_bullet( f32 cx, f32 cy, f32 r, u32 col );
void gui_draw_close( gui_rect_t box, u32 col );
void gui_draw_arrow_pointing_at( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 col );
void gui_draw_chevron( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 col );
void gui_draw_plus_minus( gui_rect_t box, bool plus, f32 thickness, u32 col );
void gui_draw_frame( gui_rect_t box, u32 col_bg, u32 col_border, f32 border );
void gui_draw_round_rect( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl, bool filled, f32 thickness, u32 col );
void gui_draw_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col );
void gui_draw_circle( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
void gui_draw_arc( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col );
void gui_draw_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col );
void gui_draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
void gui_draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
void gui_draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
void gui_draw_hatch( gui_rect_t box, f32 spacing, f32 thickness, u32 col );
void gui_draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
void gui_draw_shadow( gui_rect_t box, f32 spread, u32 col );
void gui_draw_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
void gui_draw_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );
void gui_draw_grip( gui_rect_t box, u32 col );
void gui_draw_spinner( gui_rect_t box, f32 t, f32 thickness, u32 col );
void gui_draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );

/* draw -- paths */
void gui_draw_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr );
void gui_draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 abgr );
void gui_draw_polyline( const gui_vec2_t* pts, u32 count, f32 thickness, gui_stroke_align_t align, bool closed, u32 abgr );
void gui_path_clear( void );
void gui_path_line_to( f32 x, f32 y );
void gui_path_stroke( f32 thickness, gui_stroke_align_t align, bool closed, u32 abgr );

/* clipping */
void gui_push_clip( f32 x, f32 y, f32 w, f32 h );
void gui_pop_clip( void );

/* debug drawing */
void gui_debug_set_layers( u32 layers );
u32  gui_debug_get_layers( void );
void gui_debug_enable( bool enable );
bool gui_debug_is_enabled( void );

/* Reverse lookup for the id name registry (gui_debug_overlay.c): the source string an id was
   minted from (widget label, window/popup title, region/child/table id string), or NULL if
   unregistered.  Only Debug builds (GUI_DEBUG_OVERLAY) populate the registry, but the accessor
   itself is always callable -- Release just always gets NULL -- so callers need no #ifdef. */
const char* gui_debug_name( gui_id_t id );

/* input */
bool gui_want_capture_mouse( void );
bool gui_want_capture_keyboard( void );

/* last-item introspection */
bool        gui_is_item_hovered( void );
bool        gui_is_item_active( void );
bool        gui_is_item_clicked( void );
bool        gui_is_item_focused( void );
bool        gui_is_item_activated( void );
bool        gui_is_item_deactivated( void );
bool        gui_is_item_deactivated_after_edit( void );
bool        gui_is_item_visible( void );
gui_rect_t  gui_get_item_rect( void );

bool gui_is_key_down( app_key_t key );
bool gui_is_key_pressed( app_key_t key );
bool gui_is_key_pressed_repeat( app_key_t key );
bool gui_is_key_released( app_key_t key );
bool gui_is_mouse_down( app_mouse_button_t b );
bool gui_is_mouse_clicked( app_mouse_button_t b );
bool gui_is_mouse_released( app_mouse_button_t b );
bool gui_is_mouse_double_clicked( app_mouse_button_t b );
void gui_get_mouse_pos( f32* x, f32* y );
f32  gui_get_mouse_wheel( void );

/* cursor */
void         gui_set_mouse_cursor( app_cursor_t c );
app_cursor_t gui_get_mouse_cursor( void );

/* focus */
void gui_set_keyboard_focus( void );
void gui_set_edit_cursor_end( void );
void gui_set_edit_key_hook( gui_edit_key_fn fn, void* user );

/* time */
f32  gui_get_delta_time( void );
f64  gui_get_time( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_HOST_H
