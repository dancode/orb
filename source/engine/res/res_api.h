/*==============================================================================================

    engine/res/res_api.h : res_api_t function-pointer struct and module accessor macros.

==============================================================================================*/
#ifndef RES_API_H
#define RES_API_H

#include "engine/res/res.h"
#include "engine/mod/mod_import.h"

// clang-format off

/*==============================================================================================
    Resource Catalogue Runtime API

    Hashing a literal needs no vtable (RID() is inline).  The vtable is for everything that
    touches the registry: turning an id back into its name, checking membership, and
    feeding names in from cooked content at load time.
==============================================================================================*/

typedef struct res_api_s
{
    /* Lookup */
    const char*  ( *name           )( rid_t id );                   /* NULL if unregistered */
    bool         ( *exists         )( rid_t id );
    u32          ( *count          )( void );
    void         ( *each           )( res_each_fn fn, void* user );

    /* Registration (content feed) */
    rid_t        ( *register_name  )( const char* name );           /* RID_INVALID on failure */
    rid_t        ( *register_id    )( rid_t id, const char* name );   /* id must hash from name */
    u32          ( *register_table )( const res_table_t* table );   /* names now registered */

    /* Diagnostics */
    const char*  ( *last_error     )( void );

} res_api_t;

/*============================================================================================*/
/* res is always statically linked into the host -- RES_STATIC is set by the build globally. */

#if defined( BUILD_STATIC ) || defined( RES_STATIC )
    MOD_GATEWAY_STATIC( res_api_t, res )
    #define MOD_USE_RES    /* static build -- no pointer needed */
    #define MOD_FETCH_RES  true
#else
    MOD_GATEWAY_DYNAMIC( res_api_t, res )
    #define MOD_USE_RES    MOD_DEFINE_API_PTR( res_api_t, res )
    #define MOD_FETCH_RES  MOD_FETCH_API( res_api_t, res )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RES_API_H
