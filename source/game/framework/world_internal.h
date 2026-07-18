#ifndef WORLD_INTERNAL_H
#define WORLD_INTERNAL_H
/*==============================================================================================

    game/framework/world_internal.h -- seams between world.c / comp.c / pool.c.

    Compiled only through game/framework/framework.c (the unity unit); nothing here is
    part of the public surface in world.h.

==============================================================================================*/

#include "game/framework/world.h"

/*============================================================================================*/

#define WORLD_SPARSE_NONE  0xFFFFFFFFu    // sparse entry: entity has no slot in this pool
#define WORLD_OFF_NONE     0xFFFFFFFFu    // pool_carve: arena exhausted

/* world.c -- bound-slot table */
world_t*      world_resolve     ( world_id_t id );                     // NULL when unbound/invalid
world_comp_t* world_comp_resolve( world_t* w, comp_id_t c );           // NULL when out of range

/* pool.c -- offset-addressed sparse-set primitives over world_t.pool_mem */
u32   pool_carve ( world_t* w, u32 bytes );                            // 16-aligned; WORLD_OFF_NONE on OOM
u32*  pool_sparse( world_t* w, world_comp_t* c );
u32*  pool_dense ( world_t* w, world_comp_t* c );
void* pool_data  ( world_t* w, world_comp_t* c, u32 slot );
void* pool_attach( world_t* w, world_comp_t* c, u32 ent_index );       // zeroed slot; NULL when full
bool  pool_detach( world_t* w, world_comp_t* c, u32 ent_index );       // swap-remove; false if absent
void* pool_get   ( world_t* w, world_comp_t* c, u32 ent_index );
void  pool_clear ( world_t* w, world_comp_t* c );                      // count = 0, sparse reset

/*============================================================================================*/
#endif    // WORLD_INTERNAL_H
