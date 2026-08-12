#ifndef GUI_HOST_H
#define GUI_HOST_H
/*==============================================================================================

    runtime_service/gui/gui_host.h -- Host-only gui services.
    Includes gui_api.h.

    Most engine code only ever sees gui.h (types) and gui_api.h (the gui() function table) --
    that pair is all a DLL module needs. This third header adds a small extra set of functions
    that only make sense for the executable actually hosting the GUI module: loading the module
    itself, one-time boot/shutdown, memory and render stats, and a handful of other calls a
    sandbox or host exe reaches for directly instead of through gui(). Nothing in here is meant
    to be called from inside a hot-reloadable module.

    Usage:
        mod_static_load( "gui", gui_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( gui );

    Sections below follow the same order as gui_api.h, so the two headers read side by side:
    FRAME / DRAW / CORE / SURFACE / RECT / FLOW / STYLE / STOCK / CHROME / DEBUG.

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

/*===============================================  GUI_FRAME  ===============================================*/

/* diagnostics sink -- install before gui_init() to capture the init-path messages; NULL
   restores the default printf sink.  Declared in log/gui_log.h; repeated here because this
   header is the host's index of the direct-call surface.  See gui_api.h for the contract. */

void gui_log_set_fn( gui_log_fn fn, void* user );

bool gui_init( gui_builtin_font_t font );
void gui_shutdown( void );

gui_mem_stats_t gui_mem_stats( void );
void gui_print_mem_stats( void );

/* per-frame geometry / batch counts for the LAST completed frame (one-frame lag) */

gui_render_stats_t gui_render_stats( void );

/* NOTE: the built-in perf/state overlays and the pipeline dashboard are internal now -- armed by
   gui_debug_enable( true ) and emitted behind hotkeys (P / O / F10); hosts no longer call them. */


/* font lifecycle (load-into-registry half lives with GUI_DRAW below) */

u32  gui_font_load( const char* path );
u32  gui_font_load_builtin( gui_builtin_font_t font );

/* DPI response -- monitor-scale font retargeting (see dpi_set in gui_api.h) */

void           gui_dpi_set  ( gui_dpi_mode_t mode, f32 scale );
gui_dpi_mode_t gui_dpi_mode ( void );
f32            gui_dpi_scale( void );

/* asset_path -- resolve a path relative to the engine's assets/ root (see gui_api.h) */

void gui_asset_path( const char* relative, char* out, int out_size );

/* frame -- frame_begin returns frame_dirty: emit the UI build only when true.  frame_set_hooks
   hands gui the OS clock / sleep / wait callbacks it cannot reach itself (typically
   sys_tick_seconds, sys_sleep_milliseconds, sys_wait_for_os_events_ms), which power the perf
   clocks and boot_pace's sleeps.  End-of-loop pacing is host policy -- see boot_pace below and
   the FRAME CONTRACT in gui_api.h. */

void gui_frame_set_hooks( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events );
bool gui_frame_begin( f32 dt );
void gui_frame_end( void );
void gui_set_idle_skip( bool on );
bool gui_idle_skip( void );
void gui_render( i32 vp, rhi_cmd_t cmd );

/* multi-context */

i32  gui_ctx_create       ( const gui_ctx_config_t* cfg );
void gui_ctx_destroy      ( i32 ctx );
void gui_ctx_bind         ( i32 ctx );
void gui_ctx_set_listening( i32 ctx, bool listen );
void gui_ctx_begin        ( i32 ctx );
void gui_ctx_end          ( void );

/* viewport management */

i32  gui_viewport_open       ( i32 win_id );
void gui_viewport_close      ( i32 vp );
void gui_viewport_resize     ( i32 vp, i32 w, i32 h );
f32  gui_viewport_shell      ( i32 vp, const char* title, gui_win_flags_t flags );
f32  gui_viewport_caption_h  ( i32 vp );
void gui_viewport_size       ( i32 vp, i32* out_w, i32* out_h );
f32  gui_viewport_content_y  ( i32 vp );

/* boot path (gui_boot.c) -- the alternative to the runtime host: gui owns the main surface AND
   the loop shape.  A host on the runtime path (run_host_main) calls none of these; it calls the
   frame verbs above and drives its own window, pump, and present.  See BOOT PATH in gui_api.h. */

i32  gui_boot                ( const gui_boot_desc_t* desc );
bool gui_boot_poll           ( f32* out_dt );
bool gui_boot_present_begin  ( rhi_cmd_t* out_cmd );
void gui_boot_present_end    ( void );
void gui_boot_pace           ( i32 spin_sleep_ms, i32 anim_sleep_ms );  /* 0 opts that sleep out */

/* gui-owned floater surfaces (window + context owned by gui) */

i32  gui_viewport_spawn          ( const char* title, i32 x, i32 y, i32 w, i32 h );
void gui_viewport_update         ( void );
void gui_viewport_render_floaters( void );

/* event routing -- the host drains the app event ring and forwards each event */
app_event_result_t gui_event( const app_event_t* ev );

/*===============================================  GUI_DRAW  ================================================*/

/* font registry */
bool gui_font_load_into     ( u32 id, const char* path );
void gui_font_use           ( u32 id );
void gui_push_font          ( u32 id );
void gui_pop_font           ( void );
u32  gui_font_active_id     ( void );

/* drawing */
void gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr );
void gui_draw_rects( const gui_rect_col_t* rects, u32 count );
void gui_draw_text( f32 x, f32 y, u32 abgr, const char* str );
void gui_draw_text_xf( f32 x, f32 y, u32 abgr, const char* str, f32 scale, f32 rot );
gui_vec2_t gui_text_size( const char* str );
void gui_draw_text_in( gui_rect_t r, gui_align_t align, u32 col, const char* str );
void gui_draw_text_clipped( gui_rect_t r, gui_align_t align, u32 col, const char* str );

/* volatile blocks (per-frame retessellated custom draws) */
void gui_volatile_begin( void );
void gui_volatile_cb( const char* label, gui_volatile_fn fn );
void gui_volatile_end( void );

/* icons -- runtime icon atlas */
gui_icon_id_t gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage );
gui_icon_id_t gui_load_icon( const char* name, const char* path );
gui_icon_id_t gui_register_icon_sdf( const char* name, u32 w, u32 h, const u8* coverage,
                                     u32 out_max );
gui_icon_id_t gui_load_icon_sdf( const char* name, const char* path, u32 out_max );
gui_icon_id_t gui_find_icon( const char* name );
gui_vec2_t gui_icon_size( gui_icon_id_t id );
void gui_image( gui_icon_id_t id, f32 w, f32 h, u32 col );
void gui_draw_icon_in( gui_rect_t r, gui_icon_id_t id, u32 col );
void gui_draw_icon_xf( gui_rect_t r, gui_icon_id_t id, u32 col, f32 rot );

/* RGBA textures -- arbitrary bindless texture as a full-color quad (scene viewport) */
void gui_image_texture( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr );
void gui_draw_texture_in( gui_rect_t r, u32 bindless_idx, u32 tint_abgr );
void gui_draw_texture_xf( gui_rect_t r, u32 bindless_idx, u32 tint_abgr, f32 rot );

/* sprites -- authored RGBA art + the nine-slice that lets it fill any rect */
gui_sprite_id_t gui_register_sprite( const char* name, u32 w, u32 h, const u8* rgba );
gui_sprite_id_t gui_load_sprite( const char* name, const char* path );
gui_sprite_id_t gui_find_sprite( const char* name );
bool gui_sprite_set_slice( gui_sprite_id_t id, gui_pad_t slice );
gui_pad_t gui_sprite_slice( gui_sprite_id_t id );
gui_vec2_t gui_sprite_size( gui_sprite_id_t id );
void gui_image_sprite( gui_sprite_id_t id, f32 w, f32 h, u32 tint_abgr );
void gui_draw_sprite_in( gui_rect_t r, gui_sprite_id_t id, u32 tint_abgr );

/* the widened paint floor -- fill a rect with a brush (solid / gradient / sprite / nine-slice) */
void gui_draw_brush( gui_rect_t r, const gui_brush_t* brush );
void gui_draw_set_rounding( f32 r );
f32  gui_draw_rounding( void );

/* ambient second colour outside the glyph edge -- outlined / shadowed SDF text from one quad */
void gui_draw_set_text_edge( f32 width, u32 abgr );
u32  gui_draw_text_edge( void );
void gui_draw_set_text_edge_raw( u32 edge );

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
void gui_draw_arc_dashed( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, f32 dash, f32 gap, u32 col );
void gui_draw_arc_gradient( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col_a, u32 col_b );
void gui_draw_box_xf( gui_rect_t box, f32 rounding, f32 feather, f32 rot, u32 col );
void gui_draw_round_rect_shadow( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl, f32 feather, u32 col );
void gui_draw_bezier_quad( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
void gui_draw_bezier_cubic( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
void gui_draw_checker( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
void gui_draw_grid( gui_rect_t box, f32 cell, f32 thickness, f32 origin_x, f32 origin_y, u32 col );
void gui_draw_hatch( gui_rect_t box, f32 spacing, f32 thickness, u32 col );
void gui_draw_gradient( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
void gui_draw_shadow( gui_rect_t box, f32 spread, u32 col );
void gui_draw_pulse( gui_rect_t box, f32 rate, f32 depth, u32 col );
void gui_draw_text_outline( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
void gui_draw_text_shadow( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );
void gui_draw_grip( gui_rect_t box, u32 col );
void gui_draw_spinner( gui_rect_t box, f32 t, f32 thickness, u32 col );
void gui_draw_progress_arc( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );

/* lines + paths */
void gui_draw_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr );
void gui_draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 abgr );
void gui_draw_polyline( const gui_vec2_t* pts, u32 count, f32 thickness, gui_stroke_align_t align, bool closed, u32 abgr );
void gui_path_clear( void );
void gui_path_line_to( f32 x, f32 y );
void gui_path_stroke( f32 thickness, gui_stroke_align_t align, bool closed, u32 abgr );

/* clipping */
void gui_push_clip( f32 x, f32 y, f32 w, f32 h );
void gui_pop_clip( void );

/*===============================================  GUI_CORE  ================================================*/

/* behavior on caller rects (core/gui_item.c): the shared interaction
   state machine run over a rect YOU derived.  item() reports the full state; invisible_button
   is its click bit.  A custom widget = rect (canvas/split/carve) + item() + draw_*. */
gui_item_state_t gui_item( const char* id_str, gui_rect_t r );
bool gui_invisible_button( const char* id_str, gui_rect_t r );
bool gui_is_mouse_hovering_rect( gui_rect_t r );

/* id stack */
void gui_push_id( const char* id_str );
void gui_push_id_int( i32 i );
void gui_pop_id( void );

/* item flags */
void gui_push_item_flag( gui_item_flags_t flag, bool enable );
void gui_pop_item_flag( void );
void gui_next_item_flag( gui_item_flags_t flag, bool enable );
void gui_disabled_begin( bool disabled );
void gui_disabled_end( void );

/* drag and drop */
bool gui_drag_source_begin( gui_drag_flags_t flags );
void gui_drag_source_end( void );
bool gui_drag_payload_set( const char* type, const void* data, u32 size );
bool gui_drag_target_begin( void );
const gui_drag_payload_t* gui_drag_payload_accept( const char* type, gui_drag_flags_t flags );
void gui_drag_target_end( void );
bool gui_drag_active( void );
const gui_drag_payload_t* gui_drag_payload_peek( void );

/* multi-select protocol (interact/gui_msel.c) -- scope bracket + row feed resolving clicks /
   modifiers / keyboard into one index-range action for caller-owned selection storage; apply
   is the dense bool-array application.  Stock row: gui_msel_item (chrome). */
void       gui_msel_begin( const char* id_str, i32 count );
void       gui_msel_feed( i32 index, gui_item_state_t st );
gui_msel_t gui_msel_end( void );
void       gui_msel_apply( gui_msel_t act, bool* sel, i32 count );

/* input capture fences */
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

/* io snapshot */
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
void         gui_cursor_set( app_cursor_t c );
app_cursor_t gui_get_mouse_cursor( void );

/* focus */
void gui_set_keyboard_focus( void );
void gui_set_edit_cursor_end( void );
void gui_set_edit_key_hook( gui_edit_key_fn fn, void* user );

/* animation service -- keyed value stepping (dampers + fixed-duration tweens); see gui_api.h */
f32        gui_anim_f32  ( gui_id_t id, f32 target, f32 speed );
void       gui_anim_start( gui_id_t id, f32 secs );
f32        gui_anim_ease ( gui_id_t id, gui_ease_t ease, bool* out_active );
u32        gui_anim_color( gui_id_t id, u32 target_abgr, f32 speed );
gui_vec2_t gui_anim_vec2 ( gui_id_t id, gui_vec2_t target, f32 speed );
gui_rect_t gui_anim_rect ( gui_id_t id, gui_rect_t target, f32 speed );

/* time */
f32  gui_get_delta_time( void );
f64  gui_get_time( void );

void gui_request_redraw( void );   /* one-shot next-frame dirty (see gui_api.h GUI_CORE queries) */

/* redraw / dirty queries (frame/gui_frame_loop.c) -- the rest of the GUI_CORE redraw family; the
   implementations live in the frame unit, so their prototypes belong here on its public face. */
bool gui_wants_redraw( void );
bool gui_frame_dirty( void );
bool gui_volatile_live( void );
void gui_set_force_redraw( bool on );
bool gui_force_redraw( void );

/*==============================================  GUI_SURFACE  ==============================================*/

/* pane -- the minimal top-level surface occupant: identity + hover/z contest + base clip */
gui_pane_t gui_pane_begin( const char* id_str, gui_rect_t r, gui_region_tier_t tier,
                           i32 vp, gui_win_flags_t flags );
void       gui_pane_end( void );

/* root region -- a fixed-rect layout primitive with no window chrome */
bool gui_region_begin( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                       i32 vp, gui_win_flags_t flags );
void gui_region_end( void );
void gui_scroll_by( f32 dx, f32 dy );

/* feat_* kit -- window features as freestanding id-keyed mechanisms */
bool gui_feat_move( gui_id_t id, gui_rect_t handle, f32* x, f32* y );
u8   gui_feat_resize( gui_id_t id, gui_rect_t* r, u8 edges, f32 min_w, f32 min_h );
f32  gui_feat_collapse( gui_id_t id, bool open, f32 head_h, f32 full_h );
void gui_feat_maximize( gui_id_t id, bool maximized, gui_rect_t* r, gui_rect_t* restore,
                        gui_rect_t work );
void gui_feat_clamp( gui_rect_t* r, gui_rect_t work, f32 margin );

/*===============================================  GUI_RECT  ================================================*/

gui_rect_t gui_content_rect( void );
u32        gui_split( gui_rect_t area, gui_axis_t axis, const f32* sizes, f32 gap, gui_rect_t* out );
u32        gui_carve( const f32* form, gui_rect_t area, f32 gap, gui_rect_t* out, u32 max );
gui_rect_t gui_anchor( gui_rect_t parent, gui_anchor_t a );

/*===============================================  GUI_FLOW  ================================================*/

/* child regions + sub-layouts */
bool gui_child_begin( const char* id_str, f32 w, f32 h, gui_win_flags_t flags );
void gui_child_end( void );
void gui_push_layout( void );
void gui_push_layout_overlay( gui_rect_t rect );
void gui_pop_layout( void );

/* rect <-> flow seam pair (see gui_api.h GUI_FLOW section) */
void       gui_flow_begin( gui_rect_t rect );
gui_rect_t gui_flow_cell( f32 w, f32 h );
void       gui_flow_end( void );

/* layout */
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

/* the ambient label layout -- set once, every labeled widget's own label aligns to it (gui_field_t).
   gui_field_row (gui()->field_row) is the seam a widget routes its label through; skip_label
   drops it for the next widget. */
void         gui_field_set( const gui_field_t* f );
gui_field_t* gui_field_get( void );
void         gui_skip_label( void );
void         gui_field_row( const char* label );

/* layout - grid */
void gui_grid( gui_grid_t desc );
void gui_grid_cells( u32 ncols, u32 nrows );

/* layout - bar + strip pack runs */
void gui_bar( void );
void gui_strip( void );
void gui_pack_size( f32 unit );
void gui_pack_nextline( void );
void gui_pack_wrap( void );

/* layout - side-by-side split panels (flow/gui_split.c) */
void gui_split_begin( const char* id_str, f32 right_w );
void gui_split_next( void );
void gui_split_end( void );

/* layout - save/restore the declared shape around a scoped header change */
void gui_push_layout_state( void );
void gui_pop_layout_state( void );

void gui_align( gui_align_t a );
void gui_next_item_fit( f32 unit );
void gui_next_item_h( f32 unit );
void gui_next_item_rect( gui_rect_t r );
void gui_next_item_align( gui_align_t a );
void gui_same_line( f32 spacing );
void gui_stack_same_line( f32 spacing );
void gui_skip( void );
void gui_new_line( f32 h );

/* canvas -- reserve a full-width drawing area in the layout (draw/gui_canvas.c) */
gui_rect_t gui_canvas( f32 height );

/* sizing (sz_) - intent to px; grid-first first, content-fit escape hatches last */
f32 gui_sz_u( f32 n );
f32 gui_sz_row_gap( void );
f32 gui_sz_rows_h( u32 n );
f32 gui_sz_child_rows_h( u32 n );
f32 gui_sz_scale_row( gui_scale_t s );
f32 gui_sz_line_h( void );
f32 gui_sz_chars( f32 n );
f32 gui_sz_fit_row( f32 content_h );
f32 gui_sz_fit_col( f32 content_w );

/* placement queries + reservation */
gui_vec2_t gui_content_avail( void );
gui_vec2_t gui_view_avail( void );
gui_span_t gui_rows_clip( i32 count, f32 row_h );
void       gui_rows_clip_end( void );
gui_vec2_t gui_cursor_screen_pos( void );
gui_rect_t gui_empty( f32 w, f32 h );

/*===============================================  GUI_STYLE  ===============================================*/

/* base style access -- style_get marks the theme anonymous, style_peek does not */
gui_style_t*       gui_style_get ( void );
const gui_style_t* gui_style_peek( void );
void               gui_style_apply( void );

void gui_style_source_set( gui_style_source_fn fn, void* user );

gui_style_set_t gui_style_set_create ( gui_style_source_fn fn, void* user );
void            gui_style_set_push   ( gui_style_set_t set );
void            gui_style_set_pop    ( void );
gui_style_set_t gui_style_set_current( void );
void gui_push_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr );
void gui_pop_style_color( u32 count );
void gui_next_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr );
void gui_push_style_var( gui_style_var_t var, f32 value );
void gui_pop_style_var( u32 count );
void gui_next_style_var( gui_style_var_t var, f32 value );
void gui_push_style_seed( gui_style_seed_t seed, u32 abgr );
void gui_pop_style_seed( u32 count );
void gui_push_style_color_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look, u32 abgr );
void gui_next_style_color_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look, u32 abgr );

/* the resolved reads: the phase mapping + the grid cell every render picks a face with, plus
   the installed-style door a kit writes a look through and the axis name tables */
gui_style_phase_t gui_item_phase       ( gui_item_state_t st );
/* the FACE plane -- art installed on a style cell (see gui_api.h's GUI_STYLE band) */
gui_style_face_t gui_style_brush_add( const gui_brush_t* brush );
void gui_push_style_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face );
void gui_pop_style_face ( u32 count );
void gui_next_style_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face );
void gui_push_style_face_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look, gui_style_face_t face );
const gui_brush_t* gui_style_face     ( gui_style_role_t role, gui_style_phase_t phase );
const gui_brush_t* gui_style_face_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look );
void gui_draw_face     ( gui_rect_t r, gui_style_role_t role, gui_style_phase_t phase );
void gui_draw_face_look( gui_rect_t r, gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look );
void gui_draw_face_item( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected );
void gui_draw_face_mix ( gui_rect_t r, gui_style_role_t role, gui_style_mix_t mix );

gui_style_mix_t gui_style_mix      ( gui_id_t id, gui_item_state_t st, bool selected );
u32             gui_style_color_mix( gui_style_role_t role, gui_style_mix_t mix );

u32               gui_style_color      ( gui_style_role_t role, gui_style_phase_t phase );
u32               gui_style_color_look ( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look );
gui_style_t*      gui_style_edit      ( void );
const char*       gui_style_role_name ( gui_style_role_t role );
const char*       gui_style_phase_name( gui_style_phase_t phase );
const char*       gui_style_look_name ( gui_style_look_t look );
const char*       gui_style_seed_name ( gui_style_seed_t seed );
const char*       gui_style_ramp_name ( gui_style_ramp_t ramp );
const char*       gui_style_var_name  ( gui_style_var_t var );
gui_style_class_t gui_style_var_class ( gui_style_var_t var );
const char*       gui_style_class_name( gui_style_class_t cls );

void gui_scale_push( gui_scale_t s );
void gui_scale_pop( void );

/*===============================================  GUI_STOCK  ===============================================*/

/* component (widget logic, no paint -- component/) + its stock_* reference render
   (stock/gui_stock_widgets.c).  A widget of your own is the stock render's sibling. */
gui_comp_slider_t     gui_comp_slider     ( const char* id_str, gui_rect_t rect, f32* v, f32 lo, f32 hi );
gui_comp_slider_t     gui_comp_slider_ex  ( const gui_comp_slider_desc_t* desc );
bool                  gui_stock_slider    ( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi );
gui_comp_button_t     gui_comp_button     ( const char* id_str, gui_rect_t rect );
bool                  gui_stock_button    ( gui_rect_t r, const char* label );
gui_comp_check_t      gui_comp_check      ( const char* id_str, gui_rect_t rect, bool* v );
bool                  gui_stock_check     ( gui_rect_t r, const char* id_str, bool* v );
gui_comp_cycle_t      gui_comp_cycle      ( const char* id_str, gui_rect_t rect, i32* idx, i32 count );
bool                  gui_stock_cycle     ( gui_rect_t r, const char* id_str, i32* idx, const char* const* items, i32 count );
gui_comp_selectable_t gui_comp_selectable ( const char* id_str, gui_rect_t rect, bool* selected );
bool                  gui_stock_selectable( gui_rect_t r, const char* label, bool* selected );
gui_comp_input_t      gui_comp_input      ( const char* id_str, gui_rect_t rect, f32 pad, char* buf, u32 bufsz );
bool                  gui_stock_input     ( gui_rect_t r, const char* id_str, char* buf, u32 bufsz );

/* the inert three -- no component (no interaction to extract), render-only by design */
void gui_stock_panel( gui_rect_t r );
void gui_stock_label( gui_rect_t r, gui_align_t align, const char* str );
void gui_stock_meter( gui_rect_t r, f32 frac, u32 fill_abgr );

/*==============================================  GUI_CHROME  ===============================================*/

/* window */
void gui_window_set_next_pos ( f32 x, f32 y, gui_cond_t cond );
void gui_window_set_next_size( f32 w, f32 h, gui_cond_t cond );
void gui_window_set_next_viewport( i32 vp );
void gui_window_set_next_size_constraints( f32 min_w, f32 min_h, f32 max_w, f32 max_h );
bool gui_window_begin( const char* title, gui_win_flags_t flags );
void gui_window_end( void );
void gui_window_set_open( const char* title, bool open );
bool gui_window_is_open( const char* title );
void gui_window_anim_enable( bool on );
bool gui_window_anim_is_enabled( void );
void gui_window_set_drag( gui_win_drag_t mode );
void gui_window_set_nav( const char* title );

/* docking */
void gui_dockspace_inset( i32 vp, f32 top );
gui_dock_id_t gui_dockspace_over_viewport( i32 vp, gui_dockspace_flags_t flags );
gui_dock_id_t gui_dock_split( gui_dock_id_t node, gui_dir_t dir, f32 ratio, gui_dock_id_t* out_remain );
gui_dock_id_t gui_dock_split_root( i32 vp, gui_dir_t dir, f32 ratio );
void gui_dock_window( const char* title, gui_dock_id_t node );
void gui_dock_undock( const char* title );
bool gui_window_is_docked( const char* title );
void gui_dock_window_maximize( const char* title, bool on );
bool gui_window_is_dock_maximized( const char* title );
void gui_window_tab( const char* title, const char* onto_title );
u32  gui_dock_save( i32 vp, char* buf, u32 bufsz );
bool gui_dock_load( i32 vp, const char* text );
void gui_dock_clear( i32 vp );

/* popup + tooltip */
void gui_popup_open( const char* id_str );
bool gui_popup_begin( const char* id_str, gui_win_flags_t flags );
bool gui_popup_modal_begin( const char* id_str, const char* title, gui_win_flags_t flags );
void gui_popup_end( void );
void gui_popup_close_current( void );
bool gui_popup_is_open( const char* id_str );
bool gui_popup_context_item_begin( const char* id_str );
bool gui_popup_context_window_begin( const char* id_str );
void gui_set_item_tooltip( const char* str );
bool gui_tooltip_begin( void );
void gui_tooltip_end( void );

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
bool gui_toolbar_begin( const char* id_str );
void gui_toolbar_end( void );
bool gui_toolbar_button( const char* id_str, gui_icon_id_t icon, const char* tooltip );
bool gui_toolbar_toggle( const char* id_str, gui_icon_id_t icon, bool* v, const char* tooltip );
bool gui_toolbar_dropdown_begin( const char* id_str, gui_icon_id_t icon, const char* tooltip );
void gui_toolbar_dropdown_end( void );
void gui_toolbar_separator( void );

/* widget - text + buttons */
void gui_text( const char* str );
void gui_textf( const char* fmt, ... );
void gui_bullet_text( const char* str );
void gui_text_colored( u32 abgr, const char* str );
void gui_text_disabled( const char* str );
void gui_text_wrapped( const char* str );
void gui_bullet( void );
void gui_separator( void );
void gui_label_text( const char* label, const char* value );
bool gui_button( const char* label );
bool gui_small_button( const char* label );
bool gui_button_fill( const char* label );
f32  gui_button_width( const char* label );
void gui_progress_bar( f32 fraction, const char* overlay );
void gui_plot_lines( const char* label, const f32* values, i32 count, i32 offset,
                     const char* overlay, f32 scale_min, f32 scale_max, f32 h );
void gui_plot_histogram( const char* label, const f32* values, i32 count, i32 offset,
                         const char* overlay, f32 scale_min, f32 scale_max, f32 h );
bool gui_arrow_button( const char* id_str, gui_dir_t dir );
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
bool gui_color_picker3( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags );
bool gui_color_picker4( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags );
void gui_next_input_filter( gui_input_filter_t filter );

/* widget - input boxes */
bool gui_input_text( const char* label, char* buf, u32 bufsz );
bool gui_input_text_ex( const char* label, char* buf, u32 bufsz, gui_text_cb_fn on_change, void* cb_user );
bool gui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz );
bool gui_input_text_multiline( const char* label, char* buf, u32 bufsz, f32 h );
bool gui_input_int   ( const char* label, i32* v, i32 step, i32 step_fast );
bool gui_input_float ( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt );
bool gui_input_double( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt );
bool gui_input_float2( const char* label, f32* v, const char* fmt );
bool gui_input_float3( const char* label, f32* v, const char* fmt );
bool gui_input_float4( const char* label, f32* v, const char* fmt );

/* widget - combo and list box */
bool gui_selectable( const char* label, bool* selected );
bool gui_msel_item( const char* label, i32 index, bool selected );
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

/* widget - tab bar */
bool gui_tab_bar_begin( const char* id_str, gui_tab_bar_flags_t flags );
void gui_tab_bar_end( void );
bool gui_tab_item_begin( const char* label, bool* p_open, gui_tab_item_flags_t flags );
void gui_tab_item_end( void );

/* widget - formatting */
void gui_indent( f32 w );
void gui_unindent( f32 w );
void gui_box_begin( const char* label, gui_style_role_t role );
void gui_box_end( void );
void gui_separator_text( const char* label );
void gui_help_marker( const char* str );

/* tables (chrome/table/gui_table.c, chrome unit) */
bool gui_table_begin( const char* id_str, i32 ncols, gui_table_flags_t flags, f32 height );
void gui_table_end( void );
bool gui_table_next_column( void );
void gui_table_next_row( f32 min_h );
gui_span_t gui_table_rows_clip( i32 count, f32 min_h );
void gui_table_headers_row( void );
void gui_table_setup_column( const char* label, gui_table_col_flags_t flags, f32 width );
void gui_table_set_bg_color( gui_table_bg_target_t target, u32 abgr );
bool gui_table_set_column_index( i32 col );
i32  gui_table_get_column_count( void );
i32  gui_table_get_column_index( void );
i32  gui_table_get_row_index( void );
bool gui_table_get_sort_specs( gui_table_sort_specs_t* out );
bool gui_table_sort_order( i32* order, i32 count, gui_table_sort_value_fn val_fn,
                           gui_table_sort_cmp_fn cmp_fn, void* user );
bool gui_table_is_column_visible( i32 col );
void gui_table_set_column_visible( i32 col, bool visible );
i32  gui_table_get_hovered_column( void );
void gui_table_fit_column( i32 col );
void gui_table_reset_columns( void );

/* themes -- chrome's named style presets (style kit) */
const gui_theme_t* gui_theme_list ( u32* count_out );
bool               gui_theme_set  ( const char* name );
const char*        gui_theme_get  ( void );
void               gui_theme_reset( void );

/*===============================================  GUI_DEBUG  ===============================================*/

void gui_debug_set_layers( u32 layers );
u32  gui_debug_get_layers( void );
void gui_debug_enable( bool enable );
bool gui_debug_is_enabled( void );
bool gui_debug_hotkeys_armed( void );

/* render mode (live in every build) + the retained-cache levers */
void              gui_debug_set_render_mode( gui_render_mode_t mode );
gui_render_mode_t gui_debug_get_render_mode( void );
void              gui_debug_dump_geometry( void );
void              gui_set_retained_skip( bool on );
bool              gui_retained_skip( void );

/* Reverse lookup for the id name registry (gui_debug_overlay.c): the source string an id was
   minted from (widget label, window/popup title, region/child/table id string), or NULL if
   unregistered.  Only Debug builds (GUI_DEBUG_OVERLAY) populate the registry, but the accessor
   itself is always callable -- Release just always gets NULL -- so callers need no #ifdef. */
const char* gui_debug_name( gui_id_t id );

// clang-format on
/*============================================================================================*/
#endif    // GUI_HOST_H
