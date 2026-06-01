/* engine/ref/ref_walk.c - Reference walker and value walker.

   Two complementary traversals over a reflected struct instance:
     ref_walk_refs -- visits ONLY pointer-typed slots (T*, T*[N], T(*)[N]).
                    Used for pointer fixups after save-load, or for pointer-set tracking.
     ref_walk      -- visits EVERY field including bare values. Used for deep copy,
                    debug print, or any operation that needs to see all data.

   Union handling: a union's members overlap in memory, so walking all of them would touch
   storage that does not belong to the active member (and, for ref_walk_refs, would zero
   pointers that are not really live). Both walkers therefore descend into a union only when
   its discriminant is known: the union field carries @union_tag naming a sibling tag field,
   and each member carries @case giving the tag value that selects it. With those present the
   walker reads the tag from the instance and visits only the active member. Without them the
   union is skipped -- the safe default. */

// clang-format off

/*==============================================================================================
    Discriminant Helpers

    ref_finalize_frame resolves each union field's @union_tag attribute string to a direct
    tag_field_id index stored on ref_field_t. ref_resolve_union_member uses that index for
    an O(1) discriminant read at walk time -- no attribute scan, no string-based field lookup.
==============================================================================================*/

/* Read a small signed integer of the given byte width -- used to fetch a discriminant value
   regardless of whether the tag field is an enum (4 bytes), a byte, or a wider integer. */
static int32_t
ref_read_tag_value( const void* p, uint16_t size )
{
    switch ( size )
    {
        case 1:  return *(const int8_t* )p;
        case 2:  return *(const int16_t*)p;
        case 4:  return *(const int32_t*)p;
        case 8:
        {
            int64_t v = *(const int64_t*)p;
            assert( v == (int64_t)(int32_t)v && "ref: 64-bit discriminant out of int32_t range; use int32_t tag field" );
            return (int32_t)v;
        }
        default: return 0;
    }
}

/* Returns the active union member field, or NULL if the field is untagged (tag_field_id ==
   REF_FIELD_INVALID) or no @case matches the live discriminant value. */
static const ref_field_t*
ref_resolve_union_member( const void* parent, const ref_field_t* union_field )
{
    if ( union_field->aux == REF_FIELD_INVALID ) return NULL;
    const ref_field_t* tag_field = &g_ref.fields[ union_field->aux ];
    int32_t            tag_value = ref_read_tag_value( (const uint8_t*)parent + tag_field->offset, tag_field->size );
    return ref_union_case_field( union_field->type_id, tag_value );
}

/*==============================================================================================
    Reference Walker

    Visits only pointer slots (T*, T*[N], T(*)[N]). For each pointer slot found, the
    visitor receives the address of the pointer variable (a void**), the pointee type_id,
    and the field descriptor. The visitor can read or overwrite *slot.

    Recursion: bare value fields (T) and inline arrays (T[N]) are recursed into if the
    element type is an aggregate, because they may contain pointer fields deeper inside.
    Unions descend only into their active member (see file header). Function pointers are
    intentionally skipped -- they are code references, not data pointers, and should never
    be patched by generic fixup passes.
==============================================================================================*/

static void ref_walk_refs_fields( void* instance, const ref_type_t* t, ref_ref_visitor_t visit, void* user );

/* Process a single field for pointer slots. `instance` is the base of the field's owner;
   the field address is instance + f->offset. Factored out so the union active-member can be
   handled by recursing with the union type as the owner. */
static void
ref_walk_refs_one( void* instance, const ref_field_t* f, ref_ref_visitor_t visit, void* user )
{
    uint8_t* addr = (uint8_t*)instance + f->offset;

    /* T -- bare value; no pointer here. Recurse into structs to find deeper ones; for unions
       descend only into the active member, which is itself processed as a field. */
    if ( f->mods == REF_MODS_VALUE )
    {
        if ( f->kind == REF_KIND_STRUCT )
        {
            ref_walk_refs_fields( addr, ref_get_type( f->type_id ), visit, user );
        }
        else if ( f->kind == REF_KIND_UNION )
        {
            const ref_field_t* active = ref_resolve_union_member( instance, f );
            if ( active )
                ref_walk_refs_one( addr, active, visit, user );
        }
        return;
    }

    /* T*, const T*, T* const -- all three are single pointer slots containing a data address.
       const T* (PTR_TO_CONST) and T* const (CONST_PTR) carry live addresses that must be
       zeroed during serialization just like plain T*. The const qualifiers are C-source
       annotations; the in-memory representation is identical. */
    if ( f->mods == REF_MODS_PTR || f->mods == REF_MODS_PTR_TO_CONST || f->mods == REF_MODS_CONST_PTR )
    {
        visit( (void**)addr, f->type_id, f, user );
        return;
    }

    /* T[N] -- inline array; no pointer at this level, recurse per-element for structs.
       Union arrays are intentionally skipped: each element's discriminant (@union_tag) is
       a sibling field of the union in its enclosing struct, not something we can reach from
       inside the array loop, so there is no safe way to identify which member is active.
       The in-struct union case is handled via ref_resolve_union_member in the VALUE branch. */
    if ( f->mods == REF_MODS_ARRAY )
    {
        if ( f->kind == REF_KIND_STRUCT )
        {
            const ref_type_t* bt = ref_get_type( f->type_id );
            if ( bt && bt->size > 0 )
                for ( uint16_t k = 0; k < f->aux; k++ )
                    ref_walk_refs_fields( addr + (size_t)k * bt->size, bt, visit, user );
        }
        return;
    }

    /* T*[N] -- array of pointers; visit each individual pointer slot in the array. */
    if ( f->mods == REF_MODS_PTR_ARRAY )
    {
        for ( uint16_t k = 0; k < f->aux; k++ )
            visit( (void**)( addr + (size_t)k * sizeof( void* ) ), f->type_id, f, user );
        return;
    }

    /* T(*)[N] -- pointer to array; the pointer itself is a single slot, not the elements. */
    if ( f->mods == REF_MODS_ARRAY_PTR )
    {
        visit( (void**)addr, f->type_id, f, user );
        return;
    }

    /* Function pointers (REF_MODS_FUNCTION) and deeper chains (T**, const T*) are
       intentionally skipped -- they are not data pointers and must not be patched. */
}

static void
ref_walk_refs_fields( void* instance, const ref_type_t* t, ref_ref_visitor_t visit, void* user )
{
    if ( !t ) return;

    /* A struct iterates its fields; a union recursed into directly has no parent context to
       resolve a discriminant, so it cannot be walked safely from here. The only safe union
       entry point is through ref_walk_refs_one, which is reached from the enclosing struct. */
    if ( t->kind != REF_KIND_STRUCT ) return;

    for ( uint16_t i = 0; i < t->field_count; i++ )
        ref_walk_refs_one( instance, &g_ref.fields[ t->field_index + i ], visit, user );
}

void
ref_walk_refs( void* instance, uint16_t type_id, ref_ref_visitor_t visit, void* user )
{
    if ( !instance || !visit ) return;
    ref_walk_refs_fields( instance, ref_get_type( type_id ), visit, user );
}

/*==============================================================================================
    Generic Value Walker

    Visits every field in a struct instance, including bare values, pointers, and arrays.
    The visitor receives the field's memory address, its resolved base type_id, the field
    descriptor, and a user pointer. For aggregate values (nested structs), the visitor fires
    on the aggregate itself AND then ref_walk recurses into it, so the visitor sees both the
    container and its contents. A union value fires the visitor on the union itself and then
    recurses only into its active member (see file header).

    Contrast with ref_walk_refs: this walker is exhaustive (everything), ref_walk_refs is
    selective (pointers only). Use ref_walk for deep copy, diagnostics, or any operation
    that processes all data uniformly.
==============================================================================================*/

static void ref_walk_fields( void* instance, const ref_type_t* t, ref_visitor_t visit, void* user );

static void
ref_walk_one( void* instance, const ref_field_t* f, ref_visitor_t visit, void* user )
{
    uint8_t* addr = (uint8_t*)instance + f->offset;

    /* T -- bare value; visit the field, then recurse if it is itself an aggregate. */
    if ( f->mods == REF_MODS_VALUE )
    {
        visit( addr, f->type_id, f, user );
        if ( f->kind == REF_KIND_STRUCT )
        {
            ref_walk_fields( addr, ref_get_type( f->type_id ), visit, user );
        }
        else if ( f->kind == REF_KIND_UNION )
        {
            const ref_field_t* active = ref_resolve_union_member( instance, f );
            if ( active )
                ref_walk_one( addr, active, visit, user );
        }
        return;
    }

    /* T* -- single pointer; visit the pointer variable (not what it points to). */
    if ( f->mods == REF_MODS_PTR )
    {
        visit( addr, f->type_id, f, user );
        return;
    }

    /* T[N] -- inline array; visit and recurse each element in order. Hoist the struct
       check so the branch is not re-evaluated on every iteration of the element loop. */
    if ( f->mods == REF_MODS_ARRAY )
    {
        const ref_type_t* bt = ref_get_type( f->type_id );
        if ( bt && bt->size > 0 )
        {
            if ( f->kind == REF_KIND_STRUCT )
            {
                for ( uint16_t k = 0; k < f->aux; k++ )
                {
                    uint8_t* elem = addr + (size_t)k * bt->size;
                    visit( elem, f->type_id, f, user );
                    ref_walk_fields( elem, bt, visit, user );
                }
            }
            else
            {
                for ( uint16_t k = 0; k < f->aux; k++ )
                    visit( addr + (size_t)k * bt->size, f->type_id, f, user );
            }
        }
        return;
    }

    /* T*[N] -- array of pointers; visit each pointer slot (the pointer, not pointee). */
    if ( f->mods == REF_MODS_PTR_ARRAY )
    {
        for ( uint16_t k = 0; k < f->aux; k++ )
            visit( addr + (size_t)k * sizeof( void* ), f->type_id, f, user );
        return;
    }

    /* T(*)[N] -- pointer to array; visit the pointer variable. */
    if ( f->mods == REF_MODS_ARRAY_PTR )
    {
        visit( addr, f->type_id, f, user );
        return;
    }

    /* Function pointers and any other modifier chains fall through to an opaque visit. */
    visit( addr, f->type_id, f, user );
}

static void
ref_walk_fields( void* instance, const ref_type_t* t, ref_visitor_t visit, void* user )
{
    if ( !t || t->kind != REF_KIND_STRUCT ) return;
    for ( uint16_t i = 0; i < t->field_count; i++ )
        ref_walk_one( instance, &g_ref.fields[ t->field_index + i ], visit, user );
}

void
ref_walk( void* instance, uint16_t type_id, ref_visitor_t visit, void* user )
{
    if ( !instance || !visit ) return;
    ref_walk_fields( instance, ref_get_type( type_id ), visit, user );
}

// clang-format on
/*============================================================================================*/
