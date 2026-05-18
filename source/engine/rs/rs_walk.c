/* engine/rs/rs_walk.c - Reference walker and value walker. See rs.md for supported shapes. */

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

        uint8_t     s0  = RS_MOD_GET( f->mods, 0 );
        uint8_t     s1  = RS_MOD_GET( f->mods, 1 );
        rs_mod_op_t op0 = RS_MOD_OP( s0 );
        rs_mod_op_t op1 = RS_MOD_OP( s1 );

        /* T — bare value; recurse if aggregate */
        if ( op0 == RS_MOD_NONE )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk_refs( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer */
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_NONE )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; recurse per element if aggregate */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_NONE )
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
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; single visit */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and deeper chains are intentionally skipped. */
    }
}

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

        uint8_t     s0  = RS_MOD_GET( f->mods, 0 );
        uint8_t     s1  = RS_MOD_GET( f->mods, 1 );
        rs_mod_op_t op0 = RS_MOD_OP( s0 );
        rs_mod_op_t op1 = RS_MOD_OP( s1 );

        /* T — bare value; visit then recurse if aggregate */
        if ( op0 == RS_MOD_NONE )
        {
            visit( addr, f->type_id, f, user );
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk( addr, f->type_id, visit, user );
            continue;
        }

        /* T* — single pointer; visit the pointer variable */
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_NONE )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* T[N] — inline array; visit and recurse each element */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_NONE )
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
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( addr + (size_t)k * sizeof( void* ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] — pointer to array; visit the pointer */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_PTR )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and deeper chains visited as opaque slots. */
        visit( addr, f->type_id, f, user );
    }
}

/*============================================================================================*/
