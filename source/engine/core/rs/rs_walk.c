/*==============================================================================================

    core/rs/rs_walk.c - Reference walker.

    Iterates an instance's fields, identifies which carry pointers (directly or as inline
    arrays of pointers), and hands each pointer slot to the visitor. Recurses into nested
    structs and inline arrays of structs so the visitor sees buried references too.

    Cost model: O(total fields reachable from the type, counting nested structs).
    The walker reads only the metadata tables and the instance bytes; it never allocates.

==============================================================================================*/

void
rs_walk_refs( void* instance, uint16_t type_id, rs_ref_visitor_t visit, void* user )
{
    if ( !instance || !visit )
        return;

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t )
        return;

    /* Only aggregates have member fields to walk. Enums/prims have nothing to do here,
       and function-kind types describe signatures, not instances. */
    if ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION )
        return;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f      = &g_rs.fields[ t->field_index + i ];
        uint8_t*          addr   = (uint8_t*)instance + f->offset;

        uint8_t           s0     = RS_MOD_GET( f->mods, 0 );
        uint8_t           s1     = RS_MOD_GET( f->mods, 1 );
        rs_mod_op_t       op0    = RS_MOD_OP( s0 );
        rs_mod_op_t       op1    = RS_MOD_OP( s1 );

        /* Case 1: bare value of base type. Recurse if the base is an aggregate. */
        if ( op0 == RS_MOD_NONE )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk_refs( addr, f->type_id, visit, user );
            continue;
        }

        /* Case 2: single pointer T*. One visit. */
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_NONE )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* Case 3: inline array T[N]. Recurse per element if the base is an aggregate. */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_NONE )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
            {
                const rs_type_t* bt = rs_get_type( f->type_id );
                if ( bt && bt->size > 0 )
                {
                    for ( uint16_t k = 0; k < f->aux; k++ )
                        rs_walk_refs( addr + (size_t)k * bt->size, f->type_id, visit, user );
                }
            }
            continue;
        }

        /* Case 4: array of pointers T*[N]. Visit each pointer slot. */
        if ( op0 == RS_MOD_PTR && op1 == RS_MOD_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
            continue;
        }

        /* Case 5: pointer to array T(*)[N]. Single pointer slot. */
        if ( op0 == RS_MOD_ARRAY && op1 == RS_MOD_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* Case 6+: function pointers and deeper modifier chains are intentionally
           skipped. Function pointers don't trace as data references; deeper chains
           are not produced by gameplay code and would require a richer walker. */
    }
}

/*============================================================================================*/
