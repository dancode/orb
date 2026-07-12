/*==============================================================================================

    engine/prof/prof_host.h - Host-only profiler API: lifecycle, direct capture calls, and
    the drain surface. Includes prof_api.h.

==============================================================================================*/
#ifndef PROF_HOST_H
#define PROF_HOST_H

#include "engine/prof/prof_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

// clang-format off

/* INFO: The three-header pattern, and who includes what.

       prof.h       types + constants only   -- safe for ANY header to include
       prof_api.h   vtable + capture macros  -- what DLL module .c files include
       prof_host.h  direct declarations      -- what host exes and sandboxes include

   The split keeps the dependency direction honest: a DLL can only see the vtable surface,
   so it physically cannot link host internals; the host gets these direct declarations
   (plus the lifecycle entries below) because prof is statically linked into it. The same
   functions appear twice on purpose -- once as pointers in prof_api_t, once as direct
   decls here -- and prof_api.c is where both surfaces are proven to be the same code.     */
/*==============================================================================================
    Lifecycle

    Self-bootstraps on first capture call -- hosts need not call prof_init explicitly.
    prof_init is idempotent; prof_exit tears the registry down for a clean re-init and
    must only run when no other thread is still emitting events. Capture requires
    sys_tick_init() to have run (events are stamped with sys_tick_nanoseconds).
==============================================================================================*/

void            prof_init            ( void );
void            prof_exit            ( void );

/*==============================================================================================
    Zones
==============================================================================================*/

void            prof_zone_begin      ( u32 id );
void            prof_zone_begin_name ( const char* name );
void            prof_zone_end        ( void );
u32             prof_name_register   ( const char* name );
const char*     prof_name_lookup     ( u32 id );

/*==============================================================================================
    Frame + Counters
==============================================================================================*/

u64             prof_frame_mark      ( void );
u64             prof_frame_number    ( void );
void            prof_counter_set     ( u32 id, i64 value );
void            prof_counter_add     ( u32 id, i64 delta );
u32             prof_counters        ( prof_counter_t* out, u32 max );

/*==============================================================================================
    Capture Control
==============================================================================================*/

void            prof_set_enabled     ( bool enabled );
bool            prof_is_enabled      ( void );
void            prof_thread_name     ( const char* name );
void            prof_thread_release  ( void );

/*==============================================================================================
    Drain -- single consumer, once per frame
==============================================================================================*/

u32             prof_thread_count    ( void );
const char*     prof_thread_label    ( u32 thread_index );
u32             prof_thread_dropped  ( u32 thread_index );
u32             prof_drain           ( u32 thread_index, prof_event_t* out, u32 max );

/*==============================================================================================
    Chrome-trace dump -- consumer side; the dump IS the single drain consumer while active
==============================================================================================*/

bool            prof_dump_begin      ( const char* path );
u32             prof_dump_flush      ( void );
void            prof_dump_end        ( void );
bool            prof_dump_active     ( void );

/*==============================================================================================
    Module Descriptor

    Used by the host to register the prof module:
        mod_static_load( "prof", prof_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_static( prof );
==============================================================================================*/

mod_desc_t*     prof_get_mod_desc    ( void );

// clang-format on
/*============================================================================================*/
#endif    // PROF_HOST_H
