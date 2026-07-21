#ifndef GUI_DEBUG_INTERNAL_H
#define GUI_DEBUG_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/debug/gui_debug.h -- server introspection (the debug unit).

    The pipeline dashboard + command stepper: ordinary debug-band windows over the render
    server's capture snapshots.  Emitted by debug_overlays_emit (gui_frame_overlay.c, the
    frame unit).  Severable: a build without the debug unit stubs these three.

==============================================================================================*/

// clang-format off

void gui_pipeline_dashboard( bool* open );      /* debug unit: F10 dashboard (stub w/o feature) */
void gui_step_window       ( bool* open );      /* debug unit: F8 command stepper window        */
u32  gui_debug_unit_mem_bytes( void );          /* debug unit: its fixed statics, for mem stats */

// clang-format on
/*============================================================================================*/
#endif    // GUI_DEBUG_INTERNAL_H
