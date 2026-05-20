/* engine/rs/rs_walk.c - Reference walker and value walker.

   Two complementary traversals over a reflected struct instance:
     rs_walk_refs -- visits ONLY pointer-typed slots (T*, T*[N], T(*)[N]).
                    Used for pointer fixups after save-load, or for pointer-set tracking.
     rs_walk      -- visits EVERY field including bare values. Used for deep copy,
                    debug print, or any operation that needs to see all data. */

/*==============================================================================================
    Reference Walker

    Visits only pointer slots (T*, T*[N], T(*)[N]). For each pointer slot found, the
    visitor receives the address of the pointer variable (a void**), the pointee type_id,
    and the field descriptor. The visitor can read or overwrite *slot.

    Recursion: bare value fields (T) and inline arrays (T[N]) are recursed into if the
    element type is an aggregate, because they may contain pointer fields deeper inside.
    Function pointers are intentionally skipped -- they are code references, not data
    pointers, and should never be patched by generic fixup passes.
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

        /* T -- bare value; no pointer here, but recurse into aggregates to find deeper ones. */
        if ( f->mods == RS_MODS_VALUE )
        {
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk_refs( addr, f->type_id, visit, user );
            continue;
        }

        /* T* -- single pointer slot; visit and let the callback read/write *slot. */
        if ( f->mods == RS_MODS_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* T[N] -- inline array; no pointer at this level, recurse per-element for aggregates. */
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

        /* T*[N] -- array of pointers; visit each individual pointer slot in the array. */
        if ( f->mods == RS_MODS_PTR_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] -- pointer to array; the pointer itself is a single slot, not the elements. */
        if ( f->mods == RS_MODS_ARRAY_PTR )
        {
            visit( (void**)addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers (RS_MODS_FUNCTION) and deeper chains (T**, const T*) are
           intentionally skipped -- they are not data pointers and must not be patched. */
    }
}

/*==============================================================================================
    Generic Value Walker

    Visits every field in a struct instance, including bare values, pointers, and arrays.
    The visitor receives the field's memory address, its resolved base type_id, the field
    descriptor, and a user pointer. For aggregate values (nested structs/unions), the
    visitor fires on the aggregate itself AND then rs_walk recurses into it, so the
    visitor sees both the container and its contents.

    Contrast with rs_walk_refs: this walker is exhaustive (everything), rs_walk_refs is
    selective (pointers only). Use rs_walk for deep copy, diagnostics, or any operation
    that processes all data uniformly.
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

        /* T -- bare value; visit the field, then recurse if it is itself an aggregate. */
        if ( f->mods == RS_MODS_VALUE )
        {
            visit( addr, f->type_id, f, user );
            if ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION )
                rs_walk( addr, f->type_id, visit, user );
            continue;
        }

        /* T* -- single pointer; visit the pointer variable (not what it points to). */
        if ( f->mods == RS_MODS_PTR )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* T[N] -- inline array; visit and recurse each element in order. */
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

        /* T*[N] -- array of pointers; visit each pointer slot (the pointer, not pointee). */
        if ( f->mods == RS_MODS_PTR_ARRAY )
        {
            for ( uint16_t k = 0; k < f->aux; k++ )
                visit( addr + (size_t)k * sizeof( void* ), f->type_id, f, user );
            continue;
        }

        /* T(*)[N] -- pointer to array; visit the pointer variable. */
        if ( f->mods == RS_MODS_ARRAY_PTR )
        {
            visit( addr, f->type_id, f, user );
            continue;
        }

        /* Function pointers and any other modifier chains fall through to an opaque visit. */
        visit( addr, f->type_id, f, user );
    }
}

/*============================================================================================*/
