/*==============================================================================================

    engine/res/res_host.h - Host-only resource catalogue API: lifecycle, registration, lookup,
    iteration, and the mod lifecycle hook.  Includes res_api.h.

    Boot sequence in a host:

        mod_system_init();
        res_wire_mod_callbacks();                    // install the pre_init hook; nothing fires
        mod_static_load( "res", res_get_mod_desc() );
        ... load the rest ...
        mod_init_all();                              // pre_init registers every module's table

    The registry is valid from program start: zero-initialised statics ARE the empty
    catalogue, so res_init is only needed to reset it (tests).

==============================================================================================*/
#ifndef RES_HOST_H
#define RES_HOST_H

#include "engine/res/res_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    Lifecycle

    Both reset the catalogue to empty and release its storage; the next registration allocates
    again.  Neither is required for normal use -- the catalogue is meant to live as long as the
    program, and the res module's own exit deliberately leaves it standing.
==============================================================================================*/

void            res_init             ( void );
void            res_exit             ( void );

/*==============================================================================================
    Registration

    Idempotent: registering a name already present returns its id and changes nothing, which
    is what makes a hot-reload swap safe -- the new DLL's table simply re-finds its ids.

    The catalogue is cumulative.  Nothing unregisters: a rid outlives the module that first
    mentioned it, so ids held elsewhere stay resolvable across module unloads.

    res_register_id takes a caller-supplied id for feeds that carry precomputed ids beside
    their names (cooked content headers).  The id is authoritative; the name is the label.
    Two different names arriving under one id is a collision: the call fails, both names
    are reported through res_last_error(), and the first registration stands.

    The name text is COPIED into the pool.  Callers may pass stack buffers or literals in a
    DLL image that is about to be unloaded.
==============================================================================================*/

rid_t           res_register         ( const char* name );              // RID_INVALID on failure
rid_t           res_register_id      ( rid_t id, const char* name );    // RID_INVALID on failure
u32             res_register_table   ( const res_table_t* table );      // names now registered

/*==============================================================================================
    Lookup

    res_name points into the name pool, which moves when a later registration grows it (the
    same rule sid_cstr follows).  Print it, copy it, or hold it across nothing -- do not stash
    it in a struct.  Ids are the durable handle; the text is a view.
==============================================================================================*/

const char*     res_name             ( rid_t id );                      // canonical; NULL if unknown
bool            res_exists           ( rid_t id );
u32             res_count            ( void );
void            res_each             ( res_each_fn fn, void* user );

/* Canonical form of `name` into out[cap]. Returns the length written (excluding the NUL),
   or 0 when the name is empty or does not fit -- the same rule registration applies. */
u32             res_canon            ( const char* name, char* out, u32 cap );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

const char*     res_last_error       ( void );                             /* "" when none */

/*==============================================================================================
    Mod Callbacks

    Installs the pre_init hook that registers each module's res_table as it comes online
    (first init and every hot-reload swap).  There is deliberately no post_exit hook: the
    catalogue is cumulative.  Safe to call before mod_init_all.
==============================================================================================*/

static inline void
res_host_on_pre_init( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( name );
    UNUSED( user );
    if ( desc && desc->res_table )
        res_register_table( ( const res_table_t* )desc->res_table );
}

static inline void
res_wire_mod_callbacks( void )
{
    mod_add_pre_init_cb( res_host_on_pre_init, NULL );
}

/*==============================================================================================
    Module Descriptor

        mod_static_load( "res", res_get_mod_desc() );    or    mod_static( res );
==============================================================================================*/

mod_desc_t*     res_get_mod_desc     ( void );

// clang-format on
/*============================================================================================*/
#endif    // RES_HOST_H
