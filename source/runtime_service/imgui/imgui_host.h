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

void imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h );
void imgui_end_window( void );
void imgui_set_window_drag( imgui_win_drag_t mode );

void imgui_text( const char* str );
void imgui_textf( const char* fmt, ... );
bool imgui_button( const char* label );
bool imgui_checkbox( const char* label, bool* v );
bool imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi );
bool imgui_input_text( const char* label, char* buf, u32 bufsz );

void imgui_set_font      ( imgui_font_t font );
void imgui_set_bmp_scale ( u32 scale );
void imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr );
void imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str );
void imgui_push_clip( f32 x, f32 y, f32 w, f32 h );
void imgui_pop_clip( void );

/*============================================================================================*/
#endif    // IMGUI_HOST_H
