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
        if ( rs_mods_is_value( f->mods ) )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk_refs( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer */
        if ( rs_mods_is_ptr( f->mods ) )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; recurse per element if aggregate */
        if ( rs_mods_is_array( f->mods ) )
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
        if ( rs_mods_is_ptr_array( f->mods ) )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; single visit */
        if ( rs_mods_is_array_ptr( f->mods ) )
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
        if ( rs_mods_is_value( f->mods ) )
        {
            visit( addr, f->type_id, f, user );
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer; visit the pointer variable */
        if ( rs_mods_is_ptr( f->mods ) )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; visit and recurse each element */
        if ( rs_mods_is_array( f->mods ) )
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
        if ( rs_mods_is_ptr_array( f->mods ) )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( addr + (size_t)k * sizeof( void* ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; visit the pointer */
        if ( rs_mods_is_array_ptr( f->mods ) )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and deeper chains visited as opaque slots. */
        visit( addr, f->type_id, f, user );
    }
}

/*============================================================================================*/
