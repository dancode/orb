/*==============================================================================================

    sandbox/engine/sb_res/sb_res.c -- Test sandbox for the engine/res resource catalogue.

    Proves the properties the rest of the design leans on:
      - identity:      the same name yields the same rid_t from any site, with or without
                       the registry; canonical folding makes case and slash direction moot
      - idempotency:   re-registering a name (a hot-reload swap) changes nothing
      - ownership:     a name registered from a buffer that is then overwritten reads back
                       intact, because the pool copies
      - collisions:    two names arriving under one id are refused and both are reported
      - misses:        an unknown or invalid id is a clean NULL/false, never a crash
      - mod lifecycle: a module descriptor carrying a res_table gets registered by the
                       pre_init hook when mod_init_all runs, and again on re-registration
                       without growing the catalogue

    Exit code is the number of failed checks, so it can gate a build step.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/res/res_host.h"

/*==============================================================================================
    Check helper
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

/*==============================================================================================
    Identity
==============================================================================================*/

static void
test_identity( void )
{
    printf( "  identity\n" );
    res_init();

    /* Two sites spelling the same literal agree without touching the registry. */
    rid_t a = RID( "ui/icon/save" );
    rid_t b = RID( "ui/icon/save" );
    sb_check( a == b, "same literal -> same id" );
    sb_check( a != RID_INVALID, "id is never RID_INVALID" );
    sb_check( a != RID( "ui/icon/load" ), "different names -> different ids" );

    /* Canonical form: case and separator direction are not identity. */
    sb_check( RID( "UI/Icon/Save" ) == a, "case folds" );
    sb_check( RID( "ui\\icon\\save" ) == a, "backslash folds to slash" );
    sb_check( RID( "ui/icon/save/" ) != a, "trailing slash is a different name" );

    /* Registration returns the same id the macro computes, and the stored name is canonical. */
    rid_t r = res_register( "UI\\Icon\\Save" );
    sb_check( r == a, "register returns the hashed id" );
    sb_check( res_exists( a ), "registered id exists" );
    sb_check( res_name( a ) && strcmp( res_name( a ), "ui/icon/save" ) == 0,
              "name reads back canonical" );
    sb_check( res_count() == 1, "one entry" );

    char canon[ 64 ];
    u32  len = res_canon( "Font/Cascadia_Mono\\16", canon, sizeof( canon ) );
    sb_check( len == 21 && strcmp( canon, "font/cascadia_mono/16" ) == 0, "res_canon folds in place" );
    sb_check( res_canon( "abc", canon, 3 ) == 0, "res_canon rejects a name that does not fit" );
}

/*==============================================================================================
    Idempotency (hot-reload shape)
==============================================================================================*/

static void
test_idempotent( void )
{
    printf( "  idempotency\n" );
    res_init();

    rid_t first = res_register( "font/cascadia_mono/16" );
    u32   n     = res_count();

    /* A reloaded DLL registers its table again: same ids, no growth. */
    for ( int i = 0; i < 3; ++i )
    {
        rid_t again = res_register( "font/cascadia_mono/16" );
        sb_check( again == first, "re-register returns the same id" );
    }
    sb_check( res_count() == n, "re-register does not grow the catalogue" );

    /* An id taken before the swap still resolves after it. */
    rid_t held = RID( "font/cascadia_mono/16" );
    sb_check( res_name( held ) != NULL, "id held across re-registration still resolves" );
}

/*==============================================================================================
    Ownership (pool copies)
==============================================================================================*/

static void
test_pool_copies( void )
{
    printf( "  ownership\n" );
    res_init();

    char buf[ 64 ];
    strcpy( buf, "shader/gui_quad" );
    rid_t id = res_register( buf );

    /* Simulate the referencing image going away: scribble the source buffer. */
    memset( buf, 'X', sizeof( buf ) - 1 );
    buf[ sizeof( buf ) - 1 ] = 0;

    const char* name         = res_name( id );
    sb_check( name != NULL, "name survives source buffer death" );
    sb_check( name && strcmp( name, "shader/gui_quad" ) == 0,
              "name text is the original, not the scribble" );
    sb_check( name != buf, "name pointer is not the caller's buffer" );
}

/*==============================================================================================
    Collisions
==============================================================================================*/

static void
test_collision( void )
{
    printf( "  collisions\n" );
    res_init();

    /* Force two names under one id through the precomputed-id feed. */
    rid_t forced = ( rid_t )0x01234567u;
    rid_t r1     = res_register_id( forced, "level/alpha" );
    rid_t r2     = res_register_id( forced, "level/beta" );

    sb_check( r1 == forced, "first registration under an id succeeds" );
    sb_check( r2 == RID_INVALID, "second name under the same id is refused" );
    sb_check( res_count() == 1, "the collision did not add an entry" );
    sb_check( res_name( forced ) && strcmp( res_name( forced ), "level/alpha" ) == 0,
              "first registration stands" );

    const char* err = res_last_error();
    sb_check( strstr( err, "level/alpha" ) != NULL, "error names the registered name" );
    sb_check( strstr( err, "level/beta" ) != NULL, "error names the rejected name" );
    sb_check( strstr( err, "collision" ) != NULL, "error says collision" );

    /* Same id, same name is not a collision. */
    rid_t r3 = res_register_id( forced, "LEVEL/ALPHA" );
    sb_check( r3 == forced, "same name under the same id is an idempotent hit" );
}

/*==============================================================================================
    Misses and bad input
==============================================================================================*/

static void
test_misses( void )
{
    printf( "  misses\n" );
    res_init();

    res_register( "ui/icon/save" );

    sb_check( res_name( RID( "ui/icon/never" ) ) == NULL, "unknown id -> NULL name" );
    sb_check( !res_exists( RID( "ui/icon/never" ) ), "unknown id -> not exists" );
    sb_check( res_name( RID_INVALID ) == NULL, "RID_INVALID -> NULL name" );
    sb_check( !res_exists( RID_INVALID ), "RID_INVALID -> not exists" );

    sb_check( res_register( "" ) == RID_INVALID, "empty name refused" );
    sb_check( res_register( NULL ) == RID_INVALID, "NULL name refused" );
    sb_check( res_register_id( RID_INVALID, "x" ) == RID_INVALID, "RID_INVALID as id refused" );

    char too_long[ RES_NAME_MAX + 2 ];
    memset( too_long, 'a', sizeof( too_long ) - 1 );
    too_long[ sizeof( too_long ) - 1 ] = 0;
    sb_check( res_register( too_long ) == RID_INVALID, "over-long name refused" );
    sb_check( res_count() == 1, "refused names add nothing" );
}

/*==============================================================================================
    Mod lifecycle: a descriptor's res_table registers through the pre_init hook

    Stands in for the generated per-module table Phase 1 emits.
==============================================================================================*/

static const res_entry_t s_fake_entries[] = {
    { "font/cascadia_mono/16" },
    { "ui/icon/save" },
    { "ui/icon/load" },
    { "UI/ICON/SAVE" }, /* duplicate spelled differently: must not add an entry */
};

static const res_table_t g_fake_res_table = {
    .entries = s_fake_entries,
    .count   = ARRAY_COUNT( s_fake_entries ),
};

/* The mod system requires a non-empty, fully populated api struct; one no-op suffices. */
typedef struct fake_api_s
{
    void ( *noop )( void );
} fake_api_t;

static void
fake_noop( void )
{}

static const fake_api_t g_fake_api_struct = { .noop = fake_noop };

static bool
fake_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    return true;
}

static mod_desc_t*
fake_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( fake_api_t ),
        .func_api      = &g_fake_api_struct,
        .deps          = { "res" },
        .dep_count     = 1,
        .init          = fake_init,
        .exit          = NULL,
        .reload        = NULL,
        .res_table     = MOD_RES_TABLE( fake ),
    };
    return &desc;
}

static void
count_visitor( rid_t id, const char* name, void* user )
{
    UNUSED( id );
    UNUSED( name );
    ( *( u32* )user )++;
}

static void
test_mod_lifecycle( void )
{
    printf( "  mod lifecycle\n" );
    res_init();

    mod_system_init();
    res_wire_mod_callbacks();
    res_wire_mod_callbacks(); /* second wire is a no-op, not a double subscription */

    bool ok = mod_static_load( "sys", sys_get_mod_desc() ) /* mod itself depends on sys */
              && mod_static_load( "res", res_get_mod_desc() ) &&
              mod_static_load( "fake", fake_get_mod_desc() );
    sb_check( ok, "static loads succeed" );
    sb_check( res_count() == 0, "load is passive: nothing registered before init_all" );

    ok = mod_init_all();
    sb_check( ok, "mod_init_all succeeds" );
    sb_check( res_count() == 3, "pre_init registered the table (3 distinct names)" );
    sb_check( res_exists( RID( "ui/icon/load" ) ), "table name resolvable via RID" );
    sb_check( res_name( RID( "font/cascadia_mono/16" ) ) != NULL, "font name present" );

    /* Hot-reload shape: the same table arrives again. */
    u32 again = res_register_table( &g_fake_res_table );
    sb_check( again == 4, "every table entry reports registered" );
    sb_check( res_count() == 3, "re-registration did not grow the catalogue" );

    u32 visited = 0;
    res_each( count_visitor, &visited );
    sb_check( visited == 3, "res_each visits each entry once" );

    /* Unloading the module leaves the catalogue intact: it is cumulative. */
    mod_system_exit();
    sb_check( res_count() == 3, "catalogue survives module exit" );
    sb_check( res_name( RID( "ui/icon/save" ) ) != NULL, "names survive module exit" );
}

/*==============================================================================================
    Entry
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "sb_res: resource catalogue\n" );

    test_identity();
    test_idempotent();
    test_pool_copies();
    test_collision();
    test_misses();
    test_mod_lifecycle();

    printf( "sb_res: %d checks, %d failed\n", s_checks, s_fails );
    return s_fails;
}

/*============================================================================================*/
