/* engine/rs/rs_walk.c - Reference walker and value walker. See rs.md for supported shapes. */

/*==============================================================================================
    Reference Walker

    Visits only pointer slots (T*, T*[N], etc.). Used for fixups and pointer tracking.
==============================================================================================*/

void
rs_walk_refs( void* instance, uint16_t type_id, rs_ref_visitor_t visit, void* user )
{
    if ( !instance || !visit ) return;

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION ) ) return;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f    = &g_rs.fields[ t->field_index + i ];
        uint8_t*          addr = (uint8_t*)instance + f->offset;

        /* T — bare value; recurse if aggregate */
        if ( f->mods == RS_MODS_VALUE )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk_refs( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer */
        if ( f->mods == RS_MODS_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; recurse per element if aggregate */
        if ( f->mods == RS_MODS_ARRAY )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
            {
                const rs_type_t* bt = rs_get_type( f->type_id );
                if ( bt && bt->size > 0 )
                    for ( uint16_t k = 0; k < f->aux; k++ )
                        rs_walk_refs( addr + (size_t)k * bt->size, f->type_id, visit, user );
            }
            continue;
        }

        /* T*[N] — array of pointers; visit each slot */
        if ( f->mods == RS_MODS_PTR_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; single visit */
        if ( f->mods == RS_MODS_ARRAY_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and deeper chains are intentionally skipped. */
    }
}

/*==============================================================================================
    Generic Value Walker

    Traverses all fields recursively, including bare values and arrays.
==============================================================================================*/

void
rs_walk( void* instance, uint16_t type_id, rs_visitor_t visit, void* user )
{
    if ( !instance || !visit ) return;

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION ) ) return;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f    = &g_rs.fields[ t->field_index + i ];
        uint8_t*          addr = (uint8_t*)instance + f->offset;

        /* T — bare value; visit then recurse if aggregate */
        if ( f->mods == RS_MODS_VALUE )
        {
            visit( addr, f->type_id, f, user );
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer; visit the pointer variable */
        if ( f->mods == RS_MODS_PTR )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; visit and recurse each element */
        if ( f->mods == RS_MODS_ARRAY )
        {
            const rs_type_t* bt = rs_get_type( f->type_id );
            if ( bt && bt->size > 0 )
            {
                for ( uint16_t k = 0; k < f->aux; k++ )
                {
                    uint8_t* elem = addr + (size_t)k * bt->size;
                    visit( elem, f->type_id, f, user );
                    if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                        rs_walk( elem, f->type_id, visit, user );
                }
            }
            continue;
        }

        /* T*[N] — array of pointers; visit each slot */
        if ( f->mods == RS_MODS_PTR_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( addr + (size_t)k * sizeof( void* ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; visit the pointer */
        if ( f->mods == RS_MODS_ARRAY_PTR )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and deeper chains visited as opaque slots. */
        visit( addr, f->type_id, f, user );
    }
}

/*============================================================================================*/
