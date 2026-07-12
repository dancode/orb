/*==============================================================================================

    sandbox/gui/sb_gui_timeline/tl_workload.h - synthetic capture workload for the timeline.

    A suite of toggleable busy-work scenarios that exercise every shape the timeline must
    draw: nested zone trees, worker-thread tracks, micro-zone floods (the LOD path), frame
    spikes (the frame strip), and counter + memory-scope churn.

==============================================================================================*/
#ifndef TL_WORKLOAD_H
#define TL_WORKLOAD_H

#include "orb.h"

// clang-format off

void tl_workload_init   ( void );    // spawn the worker pool (idle until enabled)
void tl_workload_tick   ( void );    // once per frame on the main thread: run enabled scenarios
void tl_workload_window ( void );    // the "Workload" control window (inside ctx scope)
void tl_workload_exit   ( void );    // stop + join the workers

// clang-format on
/*============================================================================================*/
#endif    // TL_WORKLOAD_H
