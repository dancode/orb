/*==============================================================================================

    engine/core/cmd/cmd_alias.c

    Aliases: name -> command string (Quake/GoldSrc model).  "alias foo bar" makes typing "foo"
    queue "bar" through the buffer, so multi-statement bodies (';'-joined) and further nested
    aliases/commands all work exactly like typed input.

    Dispatch order in cmd_execute_string is command -> cvar -> alias -> unknown, so an alias
    can never shadow a real command or cvar; cmd_cmd_alias refuses to create one that would.
    Expansion is queued (cmd_queue_front), never re-entered inline, so a self-referential alias
    is bounded by CMD_PUMP_BUDGET like any other statement -- no separate loop guard needed.

    Compiled inside the core unity build (core_cvar.c) before cmd.c, which calls
    cmd_alias_value() as the final resolution step.

==============================================================================================*/

typedef struct cmd_alias_s
{
    char name[ CMD_NAME_LEN ];    // copied at creation
    u16  value_off;               // offset into s_alias_pool

} cmd_alias_t;

static cmd_alias_t   s_aliases[ CMD_ALIAS_CAP ];
static u32           s_alias_count = 0;
static string_pool_t s_alias_pool;

static cmd_alias_t*
alias_find( const char* name )
{
    for ( u32 i = 0; i < s_alias_count; ++i )
    {
        if ( cvar_str_icmp_eq( s_aliases[ i ].name, name ) )
            return &s_aliases[ i ];
    }
    return NULL;
}

bool
cmd_alias_exists( const char* name )
{
    return alias_find( name ) != NULL;
}

const char*
cmd_alias_value( const char* name )
{
    cmd_alias_t* a = alias_find( name );
    return a ? string_pool_get( &s_alias_pool, a->value_off ) : NULL;
}

/*============================================================================================*/
/* Write "alias <name> <value>" for every alias; called by cvar_write_config after the binds. */

void
cmd_alias_write_config( void* file )
{
    FILE* f = ( FILE* )file;
    if ( s_alias_count == 0 )
        return;

    fprintf( f, "\n" );
    for ( u32 i = 0; i < s_alias_count; ++i )
    {
        fprintf( f, "alias %s \"", s_aliases[ i ].name );
        cmd_write_quoted( f, string_pool_get( &s_alias_pool, s_aliases[ i ].value_off ) );
        fprintf( f, "\"\n" );
    }
}

/*==============================================================================================

    Commands: alias / unalias / unaliasall

==============================================================================================*/

static void
cmd_cmd_alias( int argc, char** argv )
{
    /* No args: list every alias. */
    if ( argc == 1 )
    {
        if ( s_alias_count == 0 )
        {
            con_printf( "No aliases defined\n" );
            return;
        }

        con_printf( "\nAliases:\n" );
        for ( u32 i = 0; i < s_alias_count; ++i )
        {
            con_printf( "  %-16s \"%s\"\n", s_aliases[ i ].name,
                        string_pool_get( &s_alias_pool, s_aliases[ i ].value_off ) );
        }
        con_printf( "\n" );
        return;
    }

    const char* name = argv[ 1 ];

    /* Query form: show the current alias body. */
    if ( argc == 2 )
    {
        cmd_alias_t* a = alias_find( name );
        if ( a )
            con_printf( "\"%s\" = \"%s\"\n", name, string_pool_get( &s_alias_pool, a->value_off ) );
        else
            con_printf( "\"%s\" is not aliased\n", name );
        return;
    }

    /* Creating/redefining: refuse to shadow a real command or cvar. */
    if ( cmd_exists( name ) )
    {
        con_printf( "alias: \"%s\" is already a command\n", name );
        return;
    }
    if ( cvar_find( name ) )
    {
        con_printf( "alias: \"%s\" is already a cvar\n", name );
        return;
    }

    /* Join remaining args back into one command string (shared with cmd_cmd_bind). */
    char line[ CMD_LINE_LEN ];
    cmd_join_args( line, sizeof( line ), argv, 2, argc );

    cmd_alias_t* a = alias_find( name );
    if ( !a )
    {
        if ( s_alias_count >= CMD_ALIAS_CAP )
        {
            con_printf( "alias: registry full (max %d)\n", CMD_ALIAS_CAP );
            return;
        }
        a = &s_aliases[ s_alias_count++ ];
        snprintf( a->name, sizeof( a->name ), "%s", name );
    }

    a->value_off = ( u16 )string_pool_push( &s_alias_pool, line );    // old value orphaned; rebind is rare
}

static void
cmd_cmd_unalias( int argc, char** argv )
{
    if ( argc < 2 )
    {
        con_printf( "Usage: unalias <name>\n" );
        return;
    }

    cmd_alias_t* a = alias_find( argv[ 1 ] );
    if ( !a )
    {
        con_printf( "unalias: \"%s\" is not aliased\n", argv[ 1 ] );
        return;
    }

    *a = s_aliases[ --s_alias_count ];    // swap tail down; order is not contractual
}

static void
cmd_cmd_unaliasall( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    s_alias_count = 0;
    string_pool_init( &s_alias_pool );    // reclaim orphaned strings
}

/*==============================================================================================

    Lifetime (called by cmd_system_init/exit)

==============================================================================================*/

static void
cmd_alias_init( void )
{
    s_alias_count = 0;
    string_pool_init( &s_alias_pool );

    cmd_register( "alias",      cmd_cmd_alias,      "Create, query, or list command aliases" );
    cmd_register( "unalias",    cmd_cmd_unalias,    "Remove a command alias" );
    cmd_register( "unaliasall", cmd_cmd_unaliasall, "Remove all command aliases" );
}

static void
cmd_alias_exit( void )
{
    s_alias_count = 0;
    string_pool_exit( &s_alias_pool );
}

/*============================================================================================*/
