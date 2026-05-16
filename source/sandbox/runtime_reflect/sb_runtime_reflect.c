/*==============================================================================================

    sandbox/sb_runtime_reflect.c - Testbed for loading a reflected module.

    Boots sys + core, wires the rs_ system to the mod DLL load/unload callbacks,
    then loads example_reflect. Reflection registration is automatic:

        rs_init(rs_sid_intern, rs_sid_cstr) + rs_install_builtins()
        mod_set_dll_load_cb  -> rs_load_module_dll  (sym-looks up "rs_register")
        mod_set_dll_unload_cb -> rs_unload_module_dll (pops the frame by name)

        mod_load( example_reflect )  <-- triggers on_load, registers types

    After that, exercises the full feature surface against the registered types.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys.h"
#include "engine/core/core.h"
#include "engine/core/sid/sid.h"
#include "engine/rs/rs.h"

#include "runtime_modules/example_reflect/example_reflect.h"

MOD_DEFINE_API_PTR( example_reflect_api_t, example_reflect );

/* Adapters that satisfy rs_intern_fn / rs_cstr_fn using the engine's sid system. */
static rs_name_t    rs_sid_intern( const char* s ) { return sid_intern_cstr( s ).off; }
static const char*  rs_sid_cstr  ( rs_name_t id )  { return sid_cstr( ( sid_t ){ id } ); }

/*==============================================================================================
    DLL callbacks wired to the rs_ system
==============================================================================================*/

static void
on_dll_load( const char* name, lib_handle_t dll, void* user )
{
    UNUSED( user );
    uint16_t frame = rs_load_module_dll( name, dll );
    if ( frame != RS_FRAME_INVALID )
        printf( "[host] reflect: loaded frame %u for '%s'\n", frame, name );
}

static void
on_dll_unload( const char* name, lib_handle_t dll, void* user )
{
    UNUSED( dll );
    UNUSED( user );
    rs_unload_module_dll( name );
}

/*==============================================================================================
    Iteration callbacks for the feature tour
==============================================================================================*/

typedef struct
{
    uint16_t frame_id;
} list_ctx_t;

static void
list_type_cb( uint16_t type_id, const rs_type_t* t, void* user )
{
    list_ctx_t* ctx = ( list_ctx_t* )user;
    if ( t->frame_id != ctx->frame_id ) return;

    const char* kind = "?";
    switch ( t->kind )
    {
        case RS_KIND_STRUCT: kind = "struct"; break;
        case RS_KIND_ENUM:   kind = "enum";   break;
        case RS_KIND_BITSET: kind = "bitset"; break;
        case RS_KIND_UNION:  kind = "union";  break;
        default: break;
    }
    printf( "  [%3u] %-7s %-20s size=%-3u align=%-2u fields=%u\n",
            type_id, kind, sid_cstr( ( sid_t ){ t->name_id } ),
            (unsigned)t->size, (unsigned)t->align, (unsigned)t->field_count );
}

static void
print_field_cb( uint16_t field_id, const rs_field_t* f, void* user )
{
    UNUSED( field_id );
    UNUSED( user );

    char desc[ 128 ];
    rs_field_describe( f, desc, sizeof desc );

    printf( "      %-20s : %-28s offset=%-3u size=%-3u",
            sid_cstr( ( sid_t ){ f->name_id } ), desc, (unsigned)f->offset, (unsigned)f->size );
    if ( f->attr_count > 0 ) printf( "  attrs=%u", (unsigned)f->attr_count );
    printf( "\n" );
}

static void
print_enum_cb( uint16_t enum_id, const rs_enum_t* e, void* user )
{
    UNUSED( enum_id );
    UNUSED( user );
    printf( "      %-20s = %lld\n", sid_cstr( ( sid_t ){ e->name_id } ), (long long)e->value );
}

static void
ref_visit( void** slot, uint16_t pointee_type_id, const rs_field_t* field, void* user )
{
    UNUSED( user );
    const char* fname = sid_cstr( ( sid_t ){ field->name_id } );
    const char* tname = "?";
    const rs_type_t* pt = rs_get_type( pointee_type_id );
    if ( pt ) tname = sid_cstr( ( sid_t ){ pt->name_id } );
    printf( "    ref slot '%s' -> %s* %s\n", fname, tname,
            *slot ? "(non-null)" : "(NULL)" );
}

/*==============================================================================================
    Feature tour
==============================================================================================*/

static void
exercise_reflection( void )
{
    const example_reflect_api_t* mod = example_reflect_api();

    uint16_t entity_tid = rs_find_type_by_name( "ex_entity_t" );
    uint16_t facing_tid = rs_find_type_by_name( "ex_facing_t" );
    uint16_t caps_tid   = rs_find_type_by_name( "ex_caps_t" );

    /* Frame iteration: ex_entity_t.frame_id identifies the module's frame. */
    list_ctx_t lc = { .frame_id = rs_get_type( entity_tid )->frame_id };
    printf( "\n=== Types in frame %u ===\n", lc.frame_id );
    rs_each_type( list_type_cb, &lc );

    /* Fields */
    const rs_type_t* etype = rs_get_type( entity_tid );
    if ( etype )
    {
        printf( "\n=== Fields of %s ===\n", sid_cstr( ( sid_t ){ etype->name_id } ) );
        rs_each_field( entity_tid, print_field_cb, NULL );

        const rs_attrib_t* tip = rs_type_get_attr( entity_tid, "tooltip" );
        if ( tip && tip->type == RS_ATTR_STRING )
            printf( "  tooltip = \"%s\"\n", sid_cstr( ( sid_t ){ tip->value.str } ) );

        const rs_field_t* health = rs_find_field( entity_tid, "health" );
        if ( health )
            printf( "  field 'health' attr_count=%u (expect 2 from range=0,100)\n",
                    (unsigned)health->attr_count );
    }

    /* Enum + bitset */
    const rs_type_t* ftype = rs_get_type( facing_tid );
    if ( ftype )
    {
        printf( "\n=== Enumerators of %s ===\n", sid_cstr( ( sid_t ){ ftype->name_id } ) );
        rs_each_enumerator( facing_tid, print_enum_cb, NULL );
    }
    const rs_type_t* ctype = rs_get_type( caps_tid );
    if ( ctype )
    {
        printf( "\n=== Bitset %s ===\n", sid_cstr( ( sid_t ){ ctype->name_id } ) );
        rs_each_enumerator( caps_tid, print_enum_cb, NULL );

        char buf[ 128 ];
        const ex_entity_t* demo = mod->demo_entity();
        rs_bitset_describe( caps_tid, (int64_t)demo->caps, buf, sizeof buf );
        printf( "  describe(0x%x) = %s\n", (unsigned)demo->caps, buf );
    }

    /* Reference walker */
    if ( entity_tid != RS_TYPE_INVALID )
    {
        printf( "\n=== rs_walk_refs over demo entity ===\n" );
        ex_entity_t local = *mod->demo_entity();
        rs_walk_refs( &local, entity_tid, ref_visit, NULL );
    }

    /* Serialization round-trip */
    if ( entity_tid != RS_TYPE_INVALID )
    {
        printf( "\n=== Serialization round-trip ===\n" );
        const ex_entity_t* demo = mod->demo_entity();

        uint8_t buf[ 1024 ];
        size_t  n = rs_write( demo, entity_tid, buf, sizeof buf );
        printf( "  wrote %zu bytes\n", n );

        uint32_t tag = rs_peek_type_hash( buf, n );
        printf( "  peek type_hash = 0x%08x  (expected 0x%08x)\n",
                tag, rs_hash_str( "ex_entity_t" ) );

        ex_entity_t restored = { 0 };
        size_t consumed = 0;
        rs_io_status_t st = rs_read( &restored, entity_tid, buf, n, &consumed );
        printf( "  read status = %d, consumed=%zu\n", (int)st, consumed );

        if ( st == RS_IO_OK )
        {
            printf( "  id=%d  facing=%d  caps=0x%x  health=%.2f\n",
                    restored.id, (int)restored.facing,
                    (unsigned)restored.caps, restored.health );
        }
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

    /* Boot sys + core first so core_init() calls sid_init() before rs_ uses it. */
    if ( !mod_static_load( "sys",  sys_get_mod_api() ) )  return 1;
    if ( !mod_static_load( "core", core_get_mod_api() ) ) return 1;

    if ( !mod_init_all() )
    {
        fprintf( stderr, "init: %s\n", mod_last_error() );
        return 1;
    }

    /* sid is live (core initialized it). Now set up reflection. */
    rs_init( rs_sid_intern, rs_sid_cstr );
    rs_install_builtins();

    /* Wire rs_ to mod DLL events, then load the DLL (fires on_dll_load immediately). */
    mod_set_dll_load_cb  ( on_dll_load,   NULL );
    mod_set_dll_unload_cb( on_dll_unload, NULL );

    if ( !mod_load( example_reflect ) )                   return 1;  /* fires on_dll_load */

    /* mod_init_all skips already-initialized modules; only example_reflect runs init. */
    if ( !mod_init_all() )
    {
        fprintf( stderr, "init: %s\n", mod_last_error() );
        return 1;
    }

    HOST_FETCH_API( example_reflect_api_t, example_reflect );

    exercise_reflection();

    rs_exit();
    mod_system_exit();  /* core_exit() calls sid_exit() */
    return 0;
}

/*============================================================================================*/
