/*==============================================================================================

    sandbox/sb_runtime_reflect.c - How to use the rs_ reflection system.

    This file is a guided tour. Read it top to bottom. Each section shows one concept.

    The reflection system (rs_) lets you inspect C structs and enums at runtime — their
    field names, types, offsets, and attributes — without writing any parsing code.
    You annotate your types with RS_STRUCT / RS_PROP / RS_ENUM, run the build_reflect
    tool, and it generates the registration code automatically.

    BOOT ORDER
    ----------
    Before you can query anything:

        1. mod_system_init()                 -- module registry online
        2. rs_wire_mod_callbacks()           -- install the load/unload hooks; nothing
                                                fires yet. Safe to call before any module
                                                loads because the rs registry self-
                                                bootstraps on first touch.
        3. mod_static_load / mod_load(...)   -- PASSIVE. Each call only registers the
                                                descriptor. No callbacks fire here.
        4. mod_init_all()                    -- pass 1 fires the load callback for every
                                                newly-loaded module in dep order (rs reads
                                                desc->rs_register and pushes a frame);
                                                pass 2 runs init() in the same order.

    After step 4 the registry reflects the dep graph: deps are below their dependents
    on the rs frame stack, and every init() saw its own types already registered.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_api.h"
#include "engine/rs/rs_host.h"

#include "runtime_modules/example_reflect/example_reflect_api.h"

MOD_USE_EXAMPLE_REFLECT;

/*==============================================================================================
    Callbacks used by the iteration and walker examples below.

    rs_ uses callbacks for iteration — you give it a function, it calls it once per item.
    Each callback receives the item and a void* user pointer for your own context.
==============================================================================================*/

/* Used to filter types by frame when listing all types in a module. */
typedef struct
{
    uint16_t frame_id;
} list_ctx_t;

/* Called once per type in the registry. We filter to only print types from our module's frame. */
static void
list_type_cb( uint16_t type_id, const rs_type_t* t, void* user )
{
    list_ctx_t* ctx = ( list_ctx_t* )user;
    if ( t->frame_id != ctx->frame_id )
        return;

    const char* kind = "?";
    switch ( t->kind )
    {
        case RS_KIND_STRUCT: kind = "struct"; break;
        case RS_KIND_ENUM: kind = "enum"; break;
        case RS_KIND_BITSET: kind = "bitset"; break;
        case RS_KIND_UNION: kind = "union"; break;
        default: break;
    }
    printf( "  [%3u] %-7s %-20s size=%-3u align=%-2u fields=%u\n", type_id, kind, rs_cstr( t->name_id ),
            ( unsigned )t->size, ( unsigned )t->align, ( unsigned )t->field_count );
}

/* Called once per field on a type. rs_field_describe builds a human-readable type string
   like "const vec3_t*[8]" from the packed modifier chain stored in the field. */
static void
print_field_cb( uint16_t field_id, const rs_field_t* f, void* user )
{
    UNUSED( field_id );
    UNUSED( user );

    char desc[ 128 ];
    rs_field_describe( f, desc, sizeof desc );

    printf( "      %-20s : %-28s offset=%-3u size=%-3u", rs_cstr( f->name_id ), desc, ( unsigned )f->offset,
            ( unsigned )f->size );
    if ( f->attr_count > 0 )
        printf( "  attrs=%u", ( unsigned )f->attr_count );
    printf( "\n" );
}

/* Called once per enumerator on an enum or bitset type. */
static void
print_enum_cb( uint16_t enum_id, const rs_enum_t* e, void* user )
{
    UNUSED( enum_id );
    UNUSED( user );
    printf( "      %-20s = %lld\n", rs_cstr( e->name_id ), ( long long )e->value );
}

/*----------------------------------------------------------------------------------------------
    value_visit  -  callback for rs_walk (the full value walker).

    rs_walk visits every field of a live struct instance, recursing into nested structs
    automatically. This callback is called once per field visit with:

        addr     - pointer to the field's bytes inside the instance
        type_id  - the base type of the field (e.g. RS_PRIM_F32 for float)
        field    - the field's metadata (name, offset, size, mods, attributes)
        user     - whatever you passed to rs_walk (unused here)

    field->mods tells you the shape: compare directly against rs_mods_t values (RS_MODS_PTR etc.).
----------------------------------------------------------------------------------------------*/

static void
value_visit( void* addr, uint16_t type_id, const rs_field_t* field, void* user )
{
    UNUSED( user );

    const rs_type_t* t     = rs_get_type( type_id );
    const char*      tname = t ? rs_cstr( t->name_id ) : "?";
    const char*      fname = rs_cstr( field->name_id );

    /* Pointer field (T*, T**, T* const, T*[N]): addr is the pointer variable itself. */
    bool is_ptr_shape = ( field->mods == RS_MODS_PTR         || field->mods == RS_MODS_PTR_PTR  ||
                          field->mods == RS_MODS_CONST_PTR   || field->mods == RS_MODS_PTR_ARRAY ||
                          field->mods == RS_MODS_PTR_TO_CONST );
    if ( is_ptr_shape )
    {
        printf( "    %-20s %-14s %s\n", fname, tname, *( void** )addr ? "(non-null)" : "(null)" );
        return;
    }

    /* Struct or union field: rs_walk will recurse into it automatically after this call,
       so just print an opening marker. Sub-fields will appear on the lines that follow. */
    if ( !t || ( t->kind == RS_KIND_STRUCT || t->kind == RS_KIND_UNION ) )
    {
        printf( "    %-20s %s {\n", fname, tname );
        return;
    }

    /* Enum or bitset: read the integer value, then look up its name in the registry. */
    if ( t->kind == RS_KIND_ENUM || t->kind == RS_KIND_BITSET )
    {
        int64_t val = 0;
        switch ( t->size )
        {
            case 1: val = *( int8_t* )addr; break;
            case 2: val = *( int16_t* )addr; break;
            case 4: val = *( int32_t* )addr; break;
            case 8: val = *( int64_t* )addr; break;
        }
        const rs_enum_t* e = rs_enum_find_by_value( type_id, val );
        printf( "    %-20s %-14s %s (%lld)\n", fname, tname, e ? rs_cstr( e->name_id ) : "?", ( long long )val );
        return;
    }

    /* Inline char arrays (e.g. char name[64]) generate one visit per element.
       Skip the trailing null bytes so we only see the actual string content. */
    if ( field->mods == RS_MODS_ARRAY && type_id == RS_PRIM_CHAR && *( char* )addr == '\0' )
        return;

    /* Primitive value: cast addr to the correct type and print. */
    printf( "    %-20s %-14s ", fname, tname );
    switch ( type_id )
    {
        case RS_PRIM_BOOL: printf( "%s", *( bool* )addr ? "true" : "false" ); break;
        case RS_PRIM_CHAR: printf( "'%c'", *( char* )addr ); break;
        case RS_PRIM_I8: printf( "%d", *( int8_t* )addr ); break;
        case RS_PRIM_U8: printf( "%u", *( uint8_t* )addr ); break;
        case RS_PRIM_I16: printf( "%d", *( int16_t* )addr ); break;
        case RS_PRIM_U16: printf( "%u", *( uint16_t* )addr ); break;
        case RS_PRIM_I32: printf( "%d", *( int32_t* )addr ); break;
        case RS_PRIM_U32: printf( "%u", *( uint32_t* )addr ); break;
        case RS_PRIM_I64: printf( "%lld", *( int64_t* )addr ); break;
        case RS_PRIM_U64: printf( "%llu", *( uint64_t* )addr ); break;
        case RS_PRIM_F32: printf( "%.3f", *( float* )addr ); break;
        case RS_PRIM_F64: printf( "%.3f", *( double* )addr ); break;
        default: printf( "?" ); break;
    }
    printf( "\n" );
}

/*----------------------------------------------------------------------------------------------
    ref_visit  -  callback for rs_walk_refs (the pointer-only walker).

    Unlike rs_walk, rs_walk_refs only calls you for fields that hold pointers.
    This is useful for GC, relocation, or anything that only cares about references.

        slot             - address of the pointer variable (void**); write to it to relocate
        pointee_type_id  - the type the pointer points to
        field            - field metadata
----------------------------------------------------------------------------------------------*/

static void
ref_visit( void** slot, uint16_t pointee_type_id, const rs_field_t* field, void* user )
{
    UNUSED( user );
    const char*      fname = rs_cstr( field->name_id );
    const char*      tname = "?";
    const rs_type_t* pt    = rs_get_type( pointee_type_id );
    if ( pt )
        tname = rs_cstr( pt->name_id );
    printf( "    ref slot '%s' -> %s* %s\n", fname, tname, *slot ? "(non-null)" : "(NULL)" );
}

/*==============================================================================================
    Feature tour
==============================================================================================*/

static void
exercise_reflection( void )
{
    const example_reflect_api_t* mod = example_reflect();

    /* STEP 1: Look up types by name.
       Every type registered from a reflected module gets a stable numeric ID (type_id).
       rs_find_type_by_name does a hash lookup — O(1), no string scanning at runtime. */

    u16 entity_tid = rs_find_type_by_name( "ex_entity_t" );
    u16 facing_tid = rs_find_type_by_name( "ex_facing_t" );
    u16 caps_tid   = rs_find_type_by_name( "ex_caps_t" );

    /* STEP 2: List all types registered by a module.
       Each module owns a "frame" in the registry. When the module unloads, its frame
       is popped and all its types disappear — no cleanup code needed. */

    list_ctx_t lc = { .frame_id = rs_get_type( entity_tid )->frame_id };

    printf( "\n=== Types in frame %u ===\n", lc.frame_id );
    rs_each_type( list_type_cb, &lc );

    /* STEP 3: Inspect a struct's fields.
       rs_each_field calls your callback once per field with the full field descriptor:
       name, type, byte offset, size, and the packed modifier chain (pointer/array/const). */
    const rs_type_t* etype = rs_get_type( entity_tid );
    if ( etype )
    {
        printf( "\n=== Fields of %s ===\n", rs_cstr( etype->name_id ) );
        rs_each_field( entity_tid, print_field_cb, NULL );

        /* STEP 4: Read attributes.
           Attributes are metadata tags you write in the RS_PROP() annotation.
           e.g. RS_PROP( tooltip="An entity" ) or RS_PROP( range=0, 100 )
           They are stored flat in the registry alongside the field or type. */
        const rs_attrib_t* tip = rs_type_get_attr( entity_tid, "tooltip" );
        if ( tip && tip->type == RS_ATTR_STRING )
            printf( "  tooltip = \"%s\"\n", rs_cstr( tip->value.str ) );

        /* rs_find_field looks up a specific field by name on a type. */
        const rs_field_t* health = rs_find_field( entity_tid, "health" );
        if ( health )
            printf( "  field 'health' attr_count=%u (expect 2 from range=0,100)\n", ( unsigned )health->attr_count );
    }

    /* STEP 5: Enums and bitsets.
       Enums are registered with their enumerator names and integer values.
       Bitsets are flag-style enums where values OR together.
       rs_bitset_describe turns a bitmask back into a readable "FLAG_A | FLAG_B" string. */
    const rs_type_t* ftype = rs_get_type( facing_tid );
    if ( ftype )
    {
        printf( "\n=== Enumerators of %s ===\n", rs_cstr( ftype->name_id ) );
        rs_each_enumerator( facing_tid, print_enum_cb, NULL );
    }
    const rs_type_t* ctype = rs_get_type( caps_tid );
    if ( ctype )
    {
        printf( "\n=== Bitset %s ===\n", rs_cstr( ctype->name_id ) );
        rs_each_enumerator( caps_tid, print_enum_cb, NULL );

        char               buf[ 128 ];
        const ex_entity_t* demo = mod->demo_entity();
        rs_bitset_describe( caps_tid, ( int64_t )demo->caps, buf, sizeof buf );
        printf( "  describe(0x%x) = %s\n", ( unsigned )demo->caps, buf );
    }

    /* STEP 6: Pointer walker (rs_walk_refs).
       Scans a live instance and calls your visitor only for fields that hold pointers.
       Useful for garbage collection, memory relocation, or serialization pre-passes.
       Recurses into nested structs automatically so buried pointers are found too. */
    if ( entity_tid != RS_TYPE_INVALID )
    {
        printf( "\n=== rs_walk_refs over demo entity ===\n" );
        ex_entity_t local = *mod->demo_entity();
        rs_walk_refs( &local, entity_tid, ref_visit, NULL );
    }

    /* STEP 7: Full value walker (rs_walk).
       Visits every field of a live instance — primitives, enums, structs, and pointers.
       Recurses into nested structs automatically. Use this for inspection UIs, diffing,
       copying, or any operation that needs to see every value in a struct tree.
       The visitor receives the address of each field so you can read or write values. */
    if ( entity_tid != RS_TYPE_INVALID )
    {
        printf( "\n=== rs_walk over demo entity ===\n" );
        ex_entity_t local = *mod->demo_entity();
        rs_walk( &local, entity_tid, value_visit, NULL );
    }

    /* STEP 8: Serialization.
       rs_write saves a struct to a flat byte buffer. It zeroes pointer fields and any
       field marked @transient so saved data is self-contained.
       rs_read restores it, but only if the schema hash matches — meaning the struct layout
       hasn't changed since it was saved. This catches hot-reload ABI breaks automatically. */
    if ( entity_tid != RS_TYPE_INVALID )
    {
        printf( "\n=== Serialization round-trip ===\n" );
        const ex_entity_t* demo = mod->demo_entity();

        uint8_t            buf[ 1024 ];
        size_t             n = rs_write( demo, entity_tid, buf, sizeof buf );
        printf( "  wrote %zu bytes\n", n );

        /* rs_peek_type_hash reads the type identity from the saved header without
           fully deserializing — handy for routing saved blobs to the right handler. */
        uint32_t tag = rs_peek_type_hash( buf, n );
        printf( "  peek type_hash = 0x%08x  (expected 0x%08x)\n", tag, rs_hash_str( "ex_entity_t" ) );

        ex_entity_t    restored = { 0 };
        size_t         consumed = 0;
        rs_io_status_t st       = rs_read( &restored, entity_tid, buf, n, &consumed );
        printf( "  read status = %d, consumed=%zu\n", ( int )st, consumed );

        if ( st == RS_IO_OK )
        {
            printf( "  id=%d  facing=%d  caps=0x%x  health=%.2f\n", restored.id, ( int )restored.facing,
                    ( unsigned )restored.caps, restored.health );
        }
    }

    /* STEP 9: Tagged union.
       RS_UNION() types are reflected identically to structs: each variant is a named field
       with its own offset and size.  For a plain union all offsets are 0 (they all alias
       the same bytes).  The discriminant lives in the enclosing struct (ex_event_t.kind)
       and is a separate reflected field — the reflection system records the shape, not the
       semantics of which variant is live.
       Tools (inspectors, serializers) use the discriminant field to decide which member to
       show or serialize; rs_walk visits every member when walking blindly. */
    uint16_t payload_tid = rs_find_type_by_name( "ex_event_payload_t" );
    uint16_t event_tid   = rs_find_type_by_name( "ex_event_t" );

    if ( payload_tid != RS_TYPE_INVALID )
    {
        const rs_type_t* pt = rs_get_type( payload_tid );
        printf( "\n=== Union %s  (kind=%u, size=%u, %u variants) ===\n", rs_cstr( pt->name_id ),
                ( unsigned )pt->kind, ( unsigned )pt->size, ( unsigned )pt->field_count );
        rs_each_field( payload_tid, print_field_cb, NULL );

        const rs_attrib_t* tip = rs_type_get_attr( payload_tid, "tooltip" );
        if ( tip && tip->type == RS_ATTR_STRING )
            printf( "  tooltip = \"%s\"\n", rs_cstr( tip->value.str ) );
    }

    if ( event_tid != RS_TYPE_INVALID )
    {
        /* Construct a damage event locally — no module API needed.
           rs_walk visits all fields including the nested union variants. */
        ex_event_t ev               = { 0 };
        ev.id                       = 42;
        ev.kind                     = EX_EVENT_DAMAGE;
        ev.payload.damage.amount    = 75;
        ev.payload.damage.source_id = 1001;

        printf( "\n=== rs_walk over ex_event_t (damage variant) ===\n" );
        rs_walk( &ev, event_tid, value_visit, NULL );
    }
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "=== sb_runtime_reflect ===\n" );

    mod_system_init();

    /* Wire reflection into the module lifecycle. The rs registry self-bootstraps on
       first reflected load, so this call is safe before any module's mod_init runs. */
    rs_wire_mod_callbacks();

    /* Register engine modules. Order here just declares them; mod_init_all() below
       resolves the real init order from declared dependencies. */
    mod_static( rs );
    mod_static( sys );
    mod_static( core );

    /* Load example_reflect — the load callback above fires and registers its types. */
    if ( !mod_load( example_reflect ) )
        return 1;

    /* Single dep-ordered init pass for everything. */
    if ( !mod_init_all() )
    {
        fprintf( stderr, "init: %s\n", mod_last_error() );
        return 1;
    }

    /* Cache the example_reflect API pointer so we can call into the module. */
    MOD_HOST_FETCH_API( example_reflect_api_t, example_reflect );

    exercise_reflection();

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
