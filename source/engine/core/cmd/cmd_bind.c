/*==============================================================================================

    engine/core/cmd/cmd_bind.c

    Key binds: key -> command string, executed through the command buffer (Quake model).

    Core sits below app and cannot see app_key_t, so keys are opaque indexes here.  The
    host wires the human-readable name table at boot (cmd_bind_wire_names with app's
    key-name array); without it, keys read/write as bare numbers.

    - Storage: one u16 offset per key into a private string pool.  Offset 0 is the pool's
      reserved empty string, so a zeroed table means "all unbound".  Rebinding pushes a new
      pool string (the old one is orphaned); unbindall reinits the pool to reclaim.
    - '+command' binds queue "+cmd <key>" on key down and "-cmd <key>" on key up -- the
      key number argument lets overlapping holds release the right action.
    - Plain binds queue on key down only.  Auto-repeat is filtered by the caller.

    Compiled inside the core unity build (core_cvar.c) before cmd.c.

==============================================================================================*/

#define CMD_BIND_KEY_MAX 192    // covers the unified source space app_src_t (~162) with headroom

static string_pool_t      s_bind_pool;                        // bind command strings
static u16                s_bind_off[ CMD_BIND_KEY_MAX ];     // 0 = unbound (pool empty string)
static const char* const* s_bind_names      = NULL;           // host-wired key-name table
static u32                s_bind_name_count = 0;

/*============================================================================================*/
/* Wire the key-name table (index-matched to the platform key enum).  Host calls once at
   boot; the pointers must outlive the bind system (app's table is static data). */

void
cmd_bind_wire_names( const char* const* names, u32 count )
{
    s_bind_names      = names;
    s_bind_name_count = ( count < CMD_BIND_KEY_MAX ) ? count : CMD_BIND_KEY_MAX;
}

/*============================================================================================*/
/* Key <-> name helpers.  Unnamed keys fall back to bare numbers so binds survive a host
   that never wired a table. */

static const char*
bind_key_name( u32 key, char* num_buf, u32 num_size )
{
    if ( key < s_bind_name_count && s_bind_names[ key ] )
        return s_bind_names[ key ];

    snprintf( num_buf, num_size, "%u", key );
    return num_buf;
}

static i32
bind_key_from_name( const char* name )
{
    for ( u32 i = 0; i < s_bind_name_count; ++i )
    {
        if ( s_bind_names[ i ] && cvar_str_icmp_eq( s_bind_names[ i ], name ) )
            return ( i32 )i;
    }

    /* Bare number fallback ("bind 33 ..." / unnamed keys). */
    if ( isdigit( ( unsigned char )name[ 0 ] ) )
    {
        char* end = NULL;
        long  key = strtol( name, &end, 10 );
        if ( *end == '\0' && key >= 0 && key < CMD_BIND_KEY_MAX )
            return ( i32 )key;
    }

    return -1;
}

/*============================================================================================*/
/* Key event entry: queue the bound command.  down=false only matters for '+' binds. */

void
cmd_bind_event( u32 key, bool down )
{
    if ( key >= CMD_BIND_KEY_MAX || !s_bind_off[ key ] )
        return;

    const char* str = string_pool_get( &s_bind_pool, s_bind_off[ key ] );

    if ( str[ 0 ] == '+' )
    {
        /* "+forward" -> down: "+forward <key>", up: "-forward <key>". */
        char line[ CMD_LINE_LEN ];
        snprintf( line, sizeof( line ), "%s %u", str, key );
        if ( !down )
            line[ 0 ] = '-';
        cmd_queue( line );
    }
    else if ( down )
    {
        cmd_queue( str );
    }
}

/*============================================================================================*/
/* Write "bind" lines for every bound key; called by cvar_write_config after the cvars.
   void* keeps FILE out of the public header (same convention as sid_print_stats). */

void
cmd_bind_write_config( void* file )
{
    FILE* f = ( FILE* )file;
    char  num[ 16 ];

    fprintf( f, "\nunbindall\n" );

    for ( u32 key = 0; key < CMD_BIND_KEY_MAX; ++key )
    {
        if ( !s_bind_off[ key ] )
            continue;

        const char* str = string_pool_get( &s_bind_pool, s_bind_off[ key ] );
        fprintf( f, "bind %s \"", bind_key_name( key, num, sizeof( num ) ) );
        cmd_write_quoted( f, str );
        fprintf( f, "\"\n" );
    }
}

/*==============================================================================================

    Commands: bind / unbind / unbindall / bindlist

==============================================================================================*/

static void
cmd_cmd_bind( int argc, char** argv )
{
    if ( argc < 2 )
    {
        con_printf( "Usage: bind <key> [command]\n" );
        return;
    }

    const i32 key = bind_key_from_name( argv[ 1 ] );
    if ( key < 0 )
    {
        con_printf( "bind: unknown key \"%s\"\n", argv[ 1 ] );
        return;
    }

    /* Query form: show the current binding. */
    if ( argc == 2 )
    {
        if ( s_bind_off[ key ] )
            con_printf( "\"%s\" = \"%s\"\n", argv[ 1 ], string_pool_get( &s_bind_pool, s_bind_off[ key ] ) );
        else
            con_printf( "\"%s\" is not bound\n", argv[ 1 ] );
        return;
    }

    /* Join remaining args back into one command string ("bind f toggle r_wireframe"). */
    char line[ CMD_LINE_LEN ];
    u32  len = 0;
    for ( int i = 2; i < argc; ++i )
    {
        const u32 alen = ( u32 )strlen( argv[ i ] );
        if ( len + alen + 2 >= sizeof( line ) )
            break;
        if ( len )
            line[ len++ ] = ' ';
        memcpy( line + len, argv[ i ], alen );
        len += alen;
    }
    line[ len ] = '\0';

    s_bind_off[ key ] = ( u16 )string_pool_push( &s_bind_pool, line );
}

static void
cmd_cmd_unbind( int argc, char** argv )
{
    if ( argc < 2 )
    {
        con_printf( "Usage: unbind <key>\n" );
        return;
    }

    const i32 key = bind_key_from_name( argv[ 1 ] );
    if ( key < 0 )
    {
        con_printf( "unbind: unknown key \"%s\"\n", argv[ 1 ] );
        return;
    }

    s_bind_off[ key ] = 0;
}

static void
cmd_cmd_unbindall( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    memset( s_bind_off, 0, sizeof( s_bind_off ) );
    string_pool_init( &s_bind_pool );    // reclaim orphaned strings
}

static void
cmd_cmd_bindlist( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    u32  count = 0;
    char num[ 16 ];

    for ( u32 key = 0; key < CMD_BIND_KEY_MAX; ++key )
    {
        if ( !s_bind_off[ key ] )
            continue;

        con_printf( "  %-12s \"%s\"\n", bind_key_name( key, num, sizeof( num ) ),
                    string_pool_get( &s_bind_pool, s_bind_off[ key ] ) );
        count++;
    }

    con_printf( "%u bind(s)\n", count );
}

/*==============================================================================================

    Lifetime (called by cmd_system_init/exit)

==============================================================================================*/

static void
cmd_bind_init( void )
{
    memset( s_bind_off, 0, sizeof( s_bind_off ) );
    string_pool_init( &s_bind_pool );

    cmd_register( "bind",      cmd_cmd_bind,      "Bind a key to a command" );
    cmd_register( "unbind",    cmd_cmd_unbind,    "Remove a key binding" );
    cmd_register( "unbindall", cmd_cmd_unbindall, "Remove all key bindings" );
    cmd_register( "bindlist",  cmd_cmd_bindlist,  "List all key bindings" );
}

static void
cmd_bind_exit( void )
{
    memset( s_bind_off, 0, sizeof( s_bind_off ) );
    string_pool_exit( &s_bind_pool );
}

/*============================================================================================*/
