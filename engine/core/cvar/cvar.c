/*==============================================================================================

    cvar.c

    System for managing console variables.

==============================================================================================*/

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "orb.h"
#include "cvar.h"

/* Case-insensitive string compare helper */

bool
str_icmp_eq( const char* a, const char* b )
{
    while ( *a && *b )
    {
        char ca = *a;
        if ( ca >= 'A' && ca <= 'Z' )
            ca = ca + ( 'a' - 'A' );

        char cb = *b;
        if ( cb >= 'A' && cb <= 'Z' )
            cb = cb + ( 'a' - 'A' );

        if ( ca != cb )
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// clang-format off
/*==============================================================================================

    Utility Functions

==============================================================================================*/

static u32
fnv1a_hash( const char* s )
{
    u32 h = 2166136261u;    // FNV offset basis
    while ( *s )
    {
        char c = *s++;
        if ( c >= 'A' && c <= 'Z' )
            c = c + ( 'a' - 'A' );

        h ^= ( unsigned char )c;
        h *= 16777619u;    // FNV prime
    }
    return h;
}

string_pool_t g_string_pool;

/*==============================================================================================

    Cvar System : Callbacks

    * Supports multiple callbacks per cvar with module tracking for hot reload.
    * Cvar callbacks are called on value changes.
    * Callbacks are refeneced by index in cvar_t.callback_id field.

==============================================================================================*/

#define MAX_CVAR_CALLBACKS      128       // Max global callbacks
#define MAX_CVAR_FUNCS_PER_CVAR 3         // Max callbacks per cvar
#define INVALID_ID              0xFFFF    // Invalid callback ID (no callback)

// Callback table linking cvars to functions.

typedef struct cvar_callback_s
{
    u16 function_id[ MAX_CVAR_FUNCS_PER_CVAR ];    // Function indices (INVALID_ID = empty)
    u16 module_id;                                 // Owning module (INVALID_ID = unused)

} cvar_callback_t;

// The function pointer array stores a pointer-sized encoded "next free index",
// instead of a function pointer when slot is free.

static cvar_callback_fn g_function_array[ MAX_CVAR_CALLBACKS ];    // function pointer array.
static cvar_callback_t  g_callback_table[ MAX_CVAR_CALLBACKS ];    // callback entry.
static uint16_t         g_callback_count = 0;                      // number of used callback slots
static uint16_t         g_free_head      = INVALID_ID;             // intrusive frreelist head

/*============================================================================================*/
/* Initialize callback system */

void
cvar_callbacks_init()
{
    g_callback_count = 0;
    memset( g_callback_table, INVALID_ID, sizeof( g_callback_table ) );

    /* Initialize intrusive free list in g_function_array */
    for ( uint16_t i = 0; i < MAX_CVAR_CALLBACKS - 1; i++ )
    {
        /* Encode next index as pointer value */
        g_function_array[ i ] = ( cvar_callback_fn )( uintptr_t )( i + 1 );
    }
    g_function_array[ MAX_CVAR_CALLBACKS - 1 ] = ( cvar_callback_fn )( uintptr_t )INVALID_ID;
    g_free_head                                = 0; /* Head points to first free slot */
}

/*============================================================================================*/
/* Allocate a free function slot (O(1)) */

static inline uint16_t
alloc_function_slot( cvar_callback_fn fn )
{
    if ( g_free_head == INVALID_ID )
        return INVALID_ID; /* No free slots */

    uint16_t slot = g_free_head;

    /* Pop from free list */
    g_free_head = ( uint16_t )( uintptr_t )g_function_array[ slot ];

    /* Assign actual function */
    g_function_array[ slot ] = fn;

    return slot;
}

/* Free a function slot (O(1)) */

static inline void
free_function_slot( uint16_t slot )
{
    if ( slot >= MAX_CVAR_CALLBACKS )
        return;

    /* Push this slot back into the free list */
    g_function_array[ slot ] = ( cvar_callback_fn )( uintptr_t )g_free_head;
    g_free_head              = slot;
}

/*============================================================================================*/
/* Register callback for cvar */

uint16_t
cvar_callback_register( cvar_t* cv, cvar_callback_fn fn, i32 module_id )
{
    if ( !cv || !fn )
        return INVALID_ID;

    /* Allocate callback slot if needed */
    if ( cv->callback_id == INVALID_ID )
    {
        if ( g_callback_count >= MAX_CVAR_CALLBACKS )
        {
            fprintf( stderr, "cvar: callback table full\n" );
            return INVALID_ID;
        }

        cv->callback_id = g_callback_count++;
        cv->flag |= CVAR_CALLBACK;

        cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];
        cb->module_id       = ( u16 )module_id;

        for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ ) cb->function_id[ i ] = INVALID_ID;
    }

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    /* Find empty function slot */
    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        if ( cb->function_id[ i ] == INVALID_ID )
        {
            uint16_t slot = alloc_function_slot( fn );
            if ( slot == INVALID_ID )
            {
                fprintf( stderr, "cvar: no free callback slots available\n" );
                return INVALID_ID;
            }

            cb->function_id[ i ] = slot;
            return cv->callback_id;
        }
    }

    fprintf( stderr, "cvar: no room for more callbacks on '%s'\n", cvar_get_name( cv ) );
    return INVALID_ID;
}

/*============================================================================================*/
/* Clear all callbacks associated with a single cvar */

void
cvar_callback_unregister( cvar_t* cv )
{
    if ( !cv || cv->callback_id == INVALID_ID )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        cu16 fid = cb->function_id[ i ];
        if ( fid != INVALID_ID )
        {
            free_function_slot( fid );
            cb->function_id[ i ] = INVALID_ID;
        }
    }

    cb->module_id   = INVALID_ID;
    cv->callback_id = INVALID_ID;
    cv->flag &= ~CVAR_CALLBACK;
}

/*============================================================================================*/
/* Invoke all callbacks for cvar */

void
cvar_callback_invoke( cvar_t* cv )
{
    if ( !cv || cv->callback_id == INVALID_ID )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        u16 fid = cb->function_id[ i ];
        if ( fid != INVALID_ID && g_function_array[ fid ] )
        {
            g_function_array[ fid ]( cv );
        }
    }
}

/*============================================================================================*/
/* Remove all callbacks for a module (for hot reload) */

void
cvar_callback_unregister_by_module( i32 module_id )
{
    int total_cvars = cvar_get_count(); /* You must provide this helper */

    for ( int i = 0; i < total_cvars; i++ )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || cv->callback_id == INVALID_ID )
            continue;

        cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

        /* If this cvar's callbacks belong to the module, clear it */
        if ( cb->module_id == ( u16 )module_id )
        {
            cvar_callback_unregister( cv );
        }
    }
}

/*==============================================================================================

    Cvar Hash Table - Open Addressing with Linear Probing

    - Hash is initialized to HASH_EMPTY.
    - uses u16 indexes into the cvar instance pool.
    - HASH_TOMBSTONE is used for deletion.
    - insert: linear probe to next empty slot (or first tombstone).
    - find: linear probe until empty slot or match.

==============================================================================================*/

#define MAX_CVARS      256                  // Maximum registered cvars
#define HASH_SIZE      512                  // Hash table size (power of 2)
#define HASH_MASK      ( HASH_SIZE - 1 )    // Bit mask for hash

#define HASH_EMPTY     ( ( u16 )0xFFFF )    // Empty slot sentinel
#define HASH_TOMBSTONE ( ( u16 )0xFFFE )    // Deleted slot sentinel

static u32    g_cvar_count = 0;                 // Number of registered cvars
static cvar_t g_cvar_pool[ MAX_CVARS ];         // Fixed cvar array
static u16    global_cvar_hash[ HASH_SIZE ];    // Hash table of cvar indices

/*============================================================================================*/
/* Initialize hash table */

static void
cvar_hash_init()
{
    for ( u32 i = 0; i < HASH_SIZE; ++i ) global_cvar_hash[ i ] = HASH_EMPTY;
}

/*============================================================================================*/
/* Find cvar by name using linear probing (returns NULL if not found) */

static cvar_t*
cvar_hash_find( const char* name )
{
    if ( !name )
        return NULL;

    u32  hash  = fnv1a_hash( name ) & HASH_MASK;
    cu32 start = hash;

    while ( true )
    {
        cu16 idx = global_cvar_hash[ hash ];
        if ( idx == HASH_EMPTY )
        {
            return NULL;    // Not found
        }

        if ( idx != HASH_TOMBSTONE )
        {
            cvar_t*     cv      = &g_cvar_pool[ idx ];
            const char* cv_name = string_pool_get( &g_string_pool, cv->name );
            if ( str_icmp_eq( cv_name, name ) )
            {
                return cv;    // Found cvar!
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            return NULL;    // Full loop
        }
    }
}

/*============================================================================================*/
/* Insert cvar into hash table by its cvar array id */

static void
cvar_hash_insert( u32 cvar_index )
{
    cvar_t*     cv         = &g_cvar_pool[ cvar_index ];
    const char* name       = string_pool_get( &g_string_pool, cv->name );

    u32         hash       = fnv1a_hash( name ) & HASH_MASK;
    u32         start      = hash;
    u32         first_tomb = ( u32 )-1;    // first free found if adding after not found.

    while ( true )
    {
        u16 slot = global_cvar_hash[ hash ];

        if ( slot == HASH_EMPTY )
        {
            if ( first_tomb != -1 )
            {
                // new entry goes into first tombstone found.
                global_cvar_hash[ first_tomb ] = ( u16 )cvar_index;
            }
            else
            {
                // current slot is new entry.
                global_cvar_hash[ hash ] = ( u16 )cvar_index;
            }
            return;
        }
        else if ( slot == HASH_TOMBSTONE )
        {
            if ( first_tomb == -1 )
                first_tomb = ( int )hash;
        }
        else
        {
            /* If duplicate insertion we do nothing */

            cvar_t* other = &g_cvar_pool[ slot ];
            if ( str_icmp_eq( string_pool_get( &g_string_pool, other->name ), name ) )
            {
                /* Duplicate found - shouldn't happen */
                return;
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            fprintf( stderr, "hash table full while inserting cvar\n" );
            exit( 1 );
        }
    }
}

/*==============================================================================================

    System Initialization

==============================================================================================*/

/* The user string pool is now a `user_string_pool_t` instance exported from string_pool.c */
extern user_string_pool_t g_user_string_pool;

void
cvar_system_init( void )
{
    string_pool_init( &g_string_pool );
    user_string_pool_init( &g_user_string_pool );

    cvar_hash_init();
    cvar_callbacks_init();
    g_cvar_count = 0;
}

void
cvar_system_exit( void )
{
    string_pool_exit( &g_string_pool );
    user_string_pool_exit( &g_user_string_pool );
    g_cvar_count = 0;
}

void
cvar_compact_user_pool( void )
{
    user_string_pool_t new_pool = { 0 };
    user_string_pool_init( &new_pool );

    for ( u32 i = 0; i < g_cvar_count; i++ )
    {
        cvar_t* cv = &g_cvar_pool[ i ];
        if ( ( cv->type & CVAR_TYPE_MASK ) == CVAR_USR )
        {
            const char* str = user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
            if ( str )
            {
                u16 new_bucket;
                u16 new_offset     = user_string_pool_alloc( &new_pool, str, &new_bucket );
                cv->u.value_offset = new_offset;
                cv->u.bucket_index = new_bucket;
            }
        }
    }

    user_string_pool_exit( &g_user_string_pool );
    g_user_string_pool = new_pool;
}

/*==============================================================================================

    User value promotion

    If a cvar was created as a user variable (CVAR_USR) but is now being registered
    as a built-in variable, we need to promote its value from the user string pool 
    to the main cvar system. Value promotion occurs during registration of built-in cvars.

==============================================================================================*/

static u16 g_user_off  = USER_STRING_INVALID_OFFSET;
static u16 g_user_buck = USER_STRING_INVALID_LIST;

static void
cvar_cache_user_value( cvar_t* cv )
{
    // Cache user value info for later promotion since it is about to be removed
    // and we need to preserve the string value whos offset as stored in the cvar data.

    if ( cv->type & CVAR_USR )
    {
        g_user_off  = cv->u.value_offset;
        g_user_buck = cv->u.bucket_index;
    }
}

static void
cvar_promote_user_value( cvar_t* cv )
{
    if ( !( cv->type & CVAR_USR ) )
        return;

    // Remove user-created flag
    cv->type &= ~CVAR_USR;

    if ( g_user_off == USER_STRING_INVALID_OFFSET || g_user_buck == USER_STRING_INVALID_LIST )
    {
        fprintf( stderr, "we expected a user value to be cached\n" );
        return;
    }

    const u16   off     = g_user_off;
    const u16   buck    = g_user_buck;

    const char* val_str = user_string_pool_get( &g_user_string_pool, off );
    if ( val_str && val_str[ 0 ] )
    {
        // Use central value parser to assign correctly
        cvar_set_value( string_pool_get( &g_string_pool, cv->name ), val_str );
    }

    // Free user-pool allocation
    user_string_pool_free( &g_user_string_pool, off, buck );

    g_user_off  = USER_STRING_INVALID_OFFSET;
    g_user_buck = USER_STRING_INVALID_LIST;
}

/*==============================================================================================

    Register Functions

==============================================================================================*/
/* Create the cvar entry and place in hash lookup */

cvar_t*
cvar_register_base( const char* name, const char* desc, u32 type )
{
    // find existing cvar and return it (dll reload case)
    cvar_t* existing = cvar_hash_find( name );
    if ( existing )
    {
        // If a user var already existed, merge and clear user-created type
        if ( existing->type & CVAR_USR )
        {
            // Update metadata
            existing->desc = ( u16 )string_pool_push( &g_string_pool, desc ? desc : "" );
            existing->type = ( existing->type | type );
            existing->flag &= ~( CVAR_MODIFIED | CVAR_LATCHED );
            cvar_cache_user_value( existing );
        }
        return existing;
    }

    if ( g_cvar_count >= MAX_CVARS )
    {
        fprintf( stderr, "cvar: pool overflow (max %d)\n", MAX_CVARS );
        exit( 1 );
    }

    cvar_t* cv = &g_cvar_pool[ g_cvar_count ];
    memset( cv, 0, sizeof( cvar_t ) );

    cv->name        = ( u16 )string_pool_push( &g_string_pool, name );
    cv->desc        = ( u16 )string_pool_push( &g_string_pool, desc );
    cv->type        = type;
    cv->flag        = 0;
    cv->callback_id = INVALID_ID;

    /* CVAR_USR must be initialized to valid 'empty' values */
    if ( type & CVAR_USR )
    {
        cv->u.value_offset = USER_STRING_INVALID_OFFSET;
        cv->u.bucket_index = USER_STRING_INVALID_LIST;
        cv->flag           = CVAR_USER_CREATED;
    }

    cvar_hash_insert( g_cvar_count );
    ++g_cvar_count;

    return cv;
}

/* Register a boolean cvar */

cvar_t*
cvar_register_b( const char* name, const char* desc, bool value, u32 type )
{
    cvar_t* cv  = cvar_register_base( name, desc, type | CVAR_BOOL );
    cv->b.value = value;
    cv->b.latch = value;
    cv->b.reset = value;
    cvar_promote_user_value( cv );

    return cv;
}

/* Register an integer cvar with optional min/max bounds (set max=0 for no bounds) */

cvar_t*
cvar_register_i( const char* name, const char* desc, i32 value, i32 min, i32 max, u32 type )
{
    cvar_t* cv  = cvar_register_base( name, desc, type | CVAR_INT );
    cv->i.value = value;
    cv->i.min   = min;
    cv->i.max   = max;
    cv->i.latch = value;
    cv->i.reset = value;
    cvar_promote_user_value( cv );
    return cv;
}

/* Register a float cvar with optional min/max bounds (set max=0 for no bounds) */

cvar_t*
cvar_register_f( const char* name, const char* desc, f32 value, f32 min, f32 max, u32 type )
{
    cvar_t* cv  = cvar_register_base( name, desc, type | CVAR_FLOAT );
    cv->f.value = value;
    cv->f.min   = min;
    cv->f.max   = max;
    cv->f.latch = value;
    cv->f.reset = value;
    cvar_promote_user_value( cv );
    return cv;
}

/* Register a string list cvar (select from predefined options by index) */

cvar_t*
cvar_register_s( const char* name, const char* desc, const char** values, u32 count, u32 def_index, u32 type )
{
    cvar_t* cv = cvar_register_base( name, desc, type | CVAR_STR );

    if ( !values || count == 0 )
        return cv;

    if ( def_index >= count )
        def_index = 0;

    /* Find maximum string length */
    u32 maxlen = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        u32 len = ( u32 )strlen( values[ i ] ) + 1;
        if ( len > maxlen )
            maxlen = len;
    }
    maxlen = string_pool_align_up( maxlen );

    /* Reserve contiguous space for all strings */
    u32 total_bytes = maxlen * count;
    u32 base_off    = string_pool_reserve( &g_string_pool, total_bytes );

    /* Copy strings into fixed-width slots */
    for ( u32 i = 0; i < count; ++i )
    {
        char* dst = g_string_pool.data + base_off + ( i * maxlen );
        strncpy( dst, values[ i ], maxlen - 1 );
        dst[ maxlen - 1 ] = '\0';
    }

    cv->s.base  = ( u16 )base_off;
    cv->s.width = ( u16 )maxlen;
    cv->s.count = ( u16 )count;
    cv->s.value = ( u16 )def_index;
    cv->s.latch = ( u16 )def_index;
    cv->s.reset = ( u16 )def_index;

    cvar_promote_user_value( cv );
    return cv;
}

/* Register a writable string buffer cvar with fixed size */

cvar_t*
cvar_register_w( const char* name, const char* desc, const char* reset, u32 size, u32 type )
{
    ci32    align_size = string_pool_align_up( size );
    cvar_t* cv         = cvar_register_base( name, desc, type | CVAR_BUF );

    cv->w.reset        = ( u16 )string_pool_push( &g_string_pool, reset );
    cv->w.size         = ( u16 )align_size;
    cv->w.buf          = ( u16 )string_pool_reserve( &g_string_pool, cv->w.size );
    string_pool_write( &g_string_pool, cv->w.buf, reset, cv->w.size );

    cvar_promote_user_value( cv );

    return cv;
}

/* Register a read-only string reference cvar */

cvar_t*
cvar_register_r( const char* name, const char* desc, const char* value, u32 type )
{
    cvar_t* cv  = cvar_register_base( name, desc, type | CVAR_REF );

    cv->r.value = ( u16 )string_pool_push( &g_string_pool, value );

    cvar_promote_user_value( cv );
    return cv;
}

/* Register a read-only string reference cvar */

cvar_t*
cvar_register_u( const char* name, const char* value )
{
    cvar_t* cv = cvar_register_base( name, NULL, CVAR_USR );
    cvar_set_value( name, value );
    return cv;
}

/*==============================================================================================

    Lookup Functions

==============================================================================================*/

cvar_t*
cvar_find( const char* name )
{
    return cvar_hash_find( name );
}

cvar_t*
cvar_get_by_index( u32 index )
{
    if ( index >= g_cvar_count )
        return NULL;

    return &g_cvar_pool[ index ];
}

u32
cvar_get_count( void )
{
    return g_cvar_count;
}

/*==============================================================================================

    Type Query Functions

==============================================================================================*/

bool cvar_is_bool   ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_BOOL ));  }
bool cvar_is_int    ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_INT ));   }
bool cvar_is_float  ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_FLOAT )); }
bool cvar_is_str    ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_STR ));   }
bool cvar_is_buf    ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_BUF ));   }
bool cvar_is_ref    ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_REF ));   }
bool cvar_is_user   ( const cvar_t* cv ) { return ( cv && ( cv->type & CVAR_USR ));   }

/*==============================================================================================

    Value Access Functions

==============================================================================================*/

static const char*
_cvar_pool_string( u16 offset )
{
    if ( offset >= g_string_pool.used )
        return "<bad offset>";
    return g_string_pool.data + offset;
}

const char*
cvar_get_name( const cvar_t* cv )
{
    if ( !cv )
        return "<null>";
    return _cvar_pool_string( cv->name );
}

const char*
cvar_get_desc( const cvar_t* cv )
{
    if ( !cv )
        return "<null>";
    return _cvar_pool_string( cv->desc );
}

bool
cvar_get_bool( const cvar_t* cv )
{
    assert( cvar_is_bool( cv ) );
    return cv->b.value;
}

i32
cvar_get_int( const cvar_t* cv )
{
    assert( cvar_is_int( cv ) );
    return cv->i.value;
}

f32
cvar_get_float( const cvar_t* cv )
{
    assert( cvar_is_float( cv ) );
    return cv->f.value;
}

const char*
cvar_get_string_from_id( const cvar_t* cv, i32 value_id )
{
    if ( !cv || !( cv->type & CVAR_STR ) )
        return "";

    if ( cv->s.count == 0 )
        return "";

    // String set strings are in the main pool
    return g_string_pool.data + cv->s.base + ( value_id * cv->s.width );
}

const char*
cvar_get_string( const cvar_t* cv )
{
    if ( !cv )
        return "";

    switch ( cv->type & CVAR_TYPE_MASK )
    {
        case CVAR_STR: return cvar_get_string_from_id( cv, cv->s.value );
        case CVAR_BUF: return string_pool_get( &g_string_pool, cv->w.buf );
        case CVAR_REF: return string_pool_get( &g_string_pool, cv->r.value );
        case CVAR_USR: return user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
        default: return "";
    }
}

/*==============================================================================================

    Value Modification

==============================================================================================*/

/* Reset cvar to default value */

void
cvar_reset( cvar_t* cv )
{
    if ( !cv )
        return;

    switch ( cv->type & CVAR_TYPE_MASK )
    {
        case CVAR_BOOL:
            cv->b.value = cv->b.reset;
            cv->b.latch = cv->b.reset;
            break;

        case CVAR_INT:
            cv->i.value = cv->i.reset;
            cv->i.latch = cv->i.reset;
            break;

        case CVAR_FLOAT:
            cv->f.value = cv->f.reset;
            cv->f.latch = cv->f.reset;
            break;

        case CVAR_STR:
            cv->s.value = cv->s.reset;
            cv->s.latch = cv->s.reset;
            break;

        case CVAR_BUF:
            string_pool_write( &g_string_pool, cv->w.buf, g_string_pool.data + cv->w.reset, cv->w.size );
            break;

            // CVAR_USR has no "reset" value. Freeing its current value is
            // the equivalent of "resetting" it to an empty string.

        case CVAR_USR:
            user_string_pool_free( &g_user_string_pool, cv->u.value_offset, cv->u.bucket_index );
            cv->u.value_offset = USER_STRING_INVALID_OFFSET;
            cv->u.bucket_index = USER_STRING_INVALID_LIST;
            break;

        default: break;
    }

    if ( cv->flag & CVAR_CALLBACK )
        cvar_callback_invoke( cv );

    cv->flag &= ~( CVAR_MODIFIED | CVAR_LATCHED );
}

/* Reset all cvars to default values */

void
cvar_reset_all( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        cvar_t* cv = &g_cvar_pool[ i ];

        /* Skip CVAR_NORESTART variables */
        if ( cv->type & CVAR_NORESTART )
            continue;

        cvar_reset( cv );
    }
}

/*============================================================================================*/

/* Apply all latched cvar values */

void
cvar_apply_latched( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        cvar_t* cv = &g_cvar_pool[ i ];

        if ( !( cv->flag & CVAR_LATCHED ))
            continue;

        switch ( cv->type & CVAR_TYPE_MASK )
        {
            case CVAR_BOOL:     cv->b.value = cv->b.latch; break;
            case CVAR_INT:      cv->i.value = cv->i.latch; break;
            case CVAR_FLOAT:    cv->f.value = cv->f.latch; break;
            case CVAR_STR:      cv->s.value = cv->s.latch; break;

            default: break;
        }

        cv->flag &= ~CVAR_LATCHED;
        cv->flag |= CVAR_MODIFIED;

        if ( cv->flag & CVAR_CALLBACK )
            cvar_callback_invoke( cv );
    }
}

/* Clear all CVAR_MODIFIED flags */

void
cvar_clear_modified( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        g_cvar_pool[ i ].flag &= ~CVAR_MODIFIED;
    }
}

/*============================================================================================*/
/* Internal function that contains all the 'set' logic. */

static bool
cvar_set_value_internal( cvar_t* cv, const char* value )
{
    /* Check protection flags */
    if ( cv->type & CVAR_ROM )
    {
        fprintf( stderr, "cvar: '%s' is read-only\n", cvar_get_name( cv ) );
        return false;
    }

    /* TODO: Check CVAR_INIT, CVAR_CHEAT flags based on system state */

    cu32 type    = cv->type & CVAR_TYPE_MASK;
    bool changed = false;
    bool success = true;

    switch ( type )
    {
        case CVAR_BOOL:
        {
            /* Parse: 1/0, true/false, on/off, yes/no */

            bool new_value = false;
            bool parsed = false;

            // Fast single-character check
            if ( value[ 0 ] && !value[ 1 ] )
            {
                if      ( value[ 0 ] == '1' ) { new_value = true;  parsed = true; }
                else if ( value[ 0 ] == '0' ) { new_value = false; parsed = true; }
            }
            else
            {
                if      ( str_icmp_eq( value, "true" )  || str_icmp_eq( value, "on" )  || str_icmp_eq( value, "yes" ) ) { new_value = true;  parsed = true; }
                else if ( str_icmp_eq( value, "false" ) || str_icmp_eq( value, "off" ) || str_icmp_eq( value, "no" ) )  { new_value = false; parsed = true; }
            }

            if ( !parsed ) { break; } // Invalid bool string

            /* Apply new value, handle latch/modify logic */

            const bool is_latched = ( cv->type & CVAR_LATCH );
            bool* target = is_latched ? &cv->b.latch : &cv->b.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->flag |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_INT:
        {
            char* endptr = NULL;
            long  new_value = strtol( value, &endptr, 0 );

            // Check for conversion error.
            if ( endptr == value || *endptr != '\0' ) {
                changed = false;
                break;
            }

            if ( cv->i.min != cv->i.max )
            {
                if ( new_value < cv->i.min ) new_value = cv->i.min;
                if ( new_value > cv->i.max ) new_value = cv->i.max;
            }
            
            const bool is_latched = ( cv->type & CVAR_LATCH );
            i32* target = is_latched ? &cv->i.latch : &cv->i.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->flag |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_FLOAT:
        {
            char* endptr = NULL;
            float new_value = strtof( value, &endptr );

            // Check for conversion error.
            if ( endptr == value || *endptr != '\0' ) {
                changed = false;
                break;
            }

            if ( cv->f.min != cv->f.max )
            {
                if ( new_value < cv->f.min ) new_value = cv->f.min;
                if ( new_value > cv->f.max ) new_value = cv->f.max;
            }

            const bool is_latched = ( cv->type & CVAR_LATCH );
            f32* target = is_latched ? &cv->f.latch : &cv->f.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->flag |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }        
        case CVAR_STR:
        {        
            u16 new_value = 0xFFFF;

            /* Accept numeric index or string match */
            if ( isdigit( ( unsigned char )value[ 0 ] ) )
            {
                u32 idx = ( u32 )strtoul( value, NULL, 10 );
                if ( idx < cv->s.count )
                    new_value = ( u16 )idx;
            }
            else
            {
                /* TODO: find best match for convenience */
                /* Find matching string (case-insensitive) */
                for ( u32 i = 0; i < cv->s.count; ++i )
                {
                    const char* s = g_string_pool.data + cv->s.base + i * cv->s.width;
                    if ( str_icmp_eq( s, value ) )
                    {
                        new_value = ( u16 )i;
                    }
                }
            }

            if ( new_value != 0xFFFF && cv->s.value != new_value )
            {
                const bool is_latched = ( cv->type & CVAR_LATCH );
                u16* target = is_latched ? &cv->s.latch : &cv->s.value;
                if ( *target != new_value )
                {
                    *target = new_value;
                    cv->flag |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                    changed = true;
                }
            }
            break;
        }
        case CVAR_BUF:
        {
            string_pool_write( &g_string_pool, cv->w.buf, value, cv->w.size );
            cv->flag |= CVAR_MODIFIED;
            changed = true;
            break;
        }
        case CVAR_REF:
        {
            /* read-only reference - cannot set */
            break;
        }
        case CVAR_USR:
        {
            /*: Use the new destructive user string pool */
            
            /* 1. Free the old string's buffer, if it has one */

            user_string_pool_free( &g_user_string_pool, cv->u.value_offset, cv->u.bucket_index );

            /* 2. Allocate a new buffer from the correct size pool */

            u16 new_bucket;
            u16 new_offset = user_string_pool_alloc( &g_user_string_pool, value, &new_bucket );

            /* 3. Store the new handle (offset and bucket) */

            if ( cv->u.value_offset != new_offset )
            {
                cv->u.value_offset = new_offset;
                cv->u.bucket_index = new_bucket;
                cv->flag |= CVAR_MODIFIED;
                changed = true;
            }

            /* Check if string content changed even if offset is same */

            else if ( strcmp( user_string_pool_get( &g_user_string_pool, new_offset ), value) != 0 )
            {
                /* This case is unlikely (getting same offset for different string) */
                /* but we set the flags just in case. */
                cv->flag |= CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
    }

    // Invoke callbacks if value changed
    if ( changed && ( cv->flag & CVAR_CALLBACK ) )
    {
        cvar_callback_invoke( cv );
    }

    return success;
}

/*============================================================================================*/
/* Set cvar value by name with string value (returns true if changed) */
/* This is the implementation of the new 'non-creating' set function */
/* Used fpr "var value" style assignemt */

bool
cvar_set_value( const char* name, const char* value )
{
    if ( !name || !value )
        return false;

    cvar_t* cv = cvar_find( name );
    if ( !cv )
        return false; /* Does not create, just returns false */

    return cvar_set_value_internal( cv, value );
}

/*============================================================================================*/
/* Get cvar value as string by name */

const char*
cvar_get_value( const char* name )
{
    cvar_t* cv = cvar_find( name );
    if ( !cv )
        return "";

    // Use a round-robin buffer for thread-safe-ish string conversions.
    // This allows multiple calls within a single printf, for example.
    static char bufs[ 4 ][ 32 ];
    static int  buf_idx = 0;
    char*       buf     = bufs[ buf_idx++ & 3 ];

    switch ( cv->type & CVAR_TYPE_MASK )
    {
        case CVAR_BOOL:     return ( cv->b.value ? "1" : "0" );
        case CVAR_INT:      snprintf( buf, sizeof( bufs[ 0 ] ), "%d", cv->i.value ); return buf;
        case CVAR_FLOAT:    snprintf( buf, sizeof( bufs[ 0 ] ), "%g", cv->f.value ); return buf;
        case CVAR_STR:      return cvar_get_string_from_id( cv, cv->s.value );
        case CVAR_BUF:      return string_pool_get( &g_string_pool, cv->w.buf );
        case CVAR_REF:      return string_pool_get( &g_string_pool, cv->r.value );
        case CVAR_USR:      return user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
        default: return "";
    }
}

/*============================================================================================*/
/* Print cvar value with type info */

void
cvar_print_value( const cvar_t* cv )
{
    if ( !cv )
        return;

    const char* name  = cvar_get_name( cv );
    const char* value = cvar_get_value( name );

    printf( "  \"%s\" is: \"%s\"", name, value );

    /* Show latched value if present */
    if ( cv->flag & CVAR_LATCHED )
    {
        printf( " (latched)" );
    }

    /* Show type info */
    switch ( cv->type & CVAR_TYPE_MASK )
    {
        case CVAR_BOOL: printf( " [bool]" ); break;
        case CVAR_INT:
            if ( cv->i.min != cv->i.max ) 
                    printf( " [int: %d..%d]", cv->i.min, cv->i.max );
            else    printf( " [int]" );
            break;

        case CVAR_FLOAT:
            if ( cv->f.min != cv->f.max) 
                    printf( " [float: %.2f..%.2f]", cv->f.min, cv->f.max );
            else    printf( " [float]" );
            break;

        case CVAR_STR: printf( " [choice: %u of %u]", cv->s.value, cv->s.count ); break;
        case CVAR_BUF: printf( " [string]" ); break;
        case CVAR_REF: printf( " [readonly]" ); break;
        case CVAR_USR: printf( " [user]" ); break;
    }

    printf( "\n" );
}

/*============================================================================================*/
/* Print cvar flags */

void
cvar_print_flags( const cvar_t* cv )
{
    if ( !cv )
        return;

    printf( "  Type:" );

    if ( cv->type & CVAR_ROM )        printf( " ROM" );
    if ( cv->type & CVAR_INIT )       printf( " INIT" );
    if ( cv->type & CVAR_LATCH )      printf( " LATCH" );
    if ( cv->type & CVAR_CHEAT )      printf( " CHEAT" );

    if ( cv->type & CVAR_RUNTIME )    printf( " RUNTIME" );
    if ( cv->type & CVAR_NORESTART )  printf( " NORESTART" );

    if ( cv->type & CVAR_ARCHIVE )    printf( " ARCHIVE" );

    if ( cv->type & CVAR_DEVONLY )    printf( " DEVONLY" );
    if ( cv->type & CVAR_HIDDEN )     printf( " HIDDEN" );

    if ( cv->type & CVAR_NETSYNC )    printf( " NETSYNC" );
    if ( cv->type & CVAR_USERINFO )   printf( " USERINFO" );
    if ( cv->type & CVAR_SERVERINFO ) printf( " SERVERINFO" );
    if ( cv->type & CVAR_SYSTEMINFO ) printf( " SYSTEMINFO" );
    
    printf( "\n" );
}

/*============================================================================================*/
// export internal data for debug api for NatVis

core_debug_api_t g_core_debug_api = {
    .string_pool      = &g_string_pool,
    .user_string_pool = &g_user_string_pool.pool,
};

core_debug_api_t*
core_debug_get_api( void )
{
    return &g_core_debug_api;
}

/*============================================================================================*/
// clang-format on