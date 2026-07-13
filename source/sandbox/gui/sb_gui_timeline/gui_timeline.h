/*==============================================================================================

    sandbox/gui/sb_gui_timeline/gui_timeline.h - profiler timeline overlay (flame graph).

    A live Tracy-style zone timeline over engine/prof, built entirely on the public gui
    custom-draw surface. Self-contained on purpose: this pair of files transplants into an
    official home (gui debug/ or a developer service) without edits.

    Host contract, once per frame:

        gui_timeline_update();                  // drain the prof rings (see one-consumer rule)
        if ( gui()->frame_begin( dt ) ) { ... gui_timeline_window(); ... }
        gui()->set_force_redraw( gui_timeline_is_live() );   // live view animates every frame

==============================================================================================*/
#ifndef GUI_TIMELINE_H
#define GUI_TIMELINE_H

#include "orb.h"

// clang-format off

/* Drain the prof rings into the timeline's history. Call once per frame, AFTER the frame's
   workload so its zones appear immediately. The timeline is the single drain consumer while
   it runs; it stands down automatically when a Chrome-trace dump or an armed hitch monitor
   owns the drain (the window reports which). While PAUSED it drains nothing -- capture stops
   so the frozen view cannot be overwritten mid-investigation; resume discards the stale ring
   backlog and continues from now. */
void gui_timeline_update  ( void );

/* Emit the "Profiler Timeline" window. Call between ctx_begin/ctx_end on emit frames. */
void gui_timeline_window  ( void );

/* True while the view follows the newest event (the host's force-redraw pin). */
bool gui_timeline_is_live ( void );

/* Drop all captured history and start over. */
void gui_timeline_clear   ( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_TIMELINE_H
