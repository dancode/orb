/*==============================================================================================

    engine/core/cmd/cmd.c

    Command registry + immediate executor.  See cmd.h for the design contract.
    Compiled inside the core unity build (core_cvar.c) after cvar.c, so cvar_* and the
    shared cvar_str_icmp_eq helper are visible for dispatch.

==============================================================================================*/

/*==============================================================================================

    Registry

    Compact array with copied names -- unregistering swaps the tail entry down, so lookup is
    a linear scan over live entries only.  64 commands x linear scan is well under any budget.

==============================================================================================*/

typedef struct cmd_entry_s
{
    char   name[ CMD_NAME_LEN ];    // copied at registration (hot-reload safe)
    char   desc[ CMD_DESC_LEN ];    // copied at registration
    cmd_fn fn;                      // handler

} cmd_entry_t;

static cmd_entry_t s_cmds[ CMD_CAP ];
static u32         s_cmd_count = 0;

static cmd_entry_t*
cmd_find( const char* name )
{
    for ( u32 i = 0; i < s_cmd_count; ++i )
    {
        if ( cvar_str_icmp_eq( s_cmds[ i ].name, name ) )
            return &s_cmds[ i ];
    }
    return NULL;
}

bool
cmd_exists( const char* name )
{
    return cmd_find( name ) != NULL;
}

bool
cmd_register( const char* name, cmd_fn fn, const char* desc )
{
    if ( !name || !name[ 0 ] || !fn )
        return false;

    if ( cmd_find( name ) )
    {
        con_printf( "cmd: command \"%s\" already registered\n", name );
        return false;
    }

    if ( cvar_find( name ) )
    {
        con_printf( "cmd: \"%s\" is already a cvar, refusing to register command\n", name );
        return false;
    }

    if ( s_cmd_count >= CMD_CAP )
    {
        con_printf( "cmd: command registry full (max %d)\n", CMD_CAP );
        return false;
    }

    cmd_entry_t* cmd = &s_cmds[ s_cmd_count++ ];
    snprintf( cmd->name, sizeof( cmd->name ), "%s", name );
    snprintf( cmd->desc, sizeof( cmd->desc ), "%s", desc ? desc : "" );
    cmd->fn = fn;
    return true;
}

void
cmd_unregister( const char* name )
{
    cmd_entry_t* cmd = cmd_find( name );
    if ( !cmd )
        return;

    *cmd = s_cmds[ --s_cmd_count ];    // swap tail down; order is not contractual
}

u32
cmd_count( void )
{
    return s_cmd_count;
}

const char*
cmd_name( u32 index )
{
    return ( index < s_cmd_count ) ? s_cmds[ index ].name : "";
}

const char*
cmd_desc( u32 index )
{
    return ( index < s_cmd_count ) ? s_cmds[ index ].desc : "";
}

/*==============================================================================================

    Execution

    Tokenize one statement in place (double-quoted strings form one token), then dispatch:
        1. registered command        -> fn( argc, argv )
        2. cvar name alone           -> print current value
        3. cvar name + value         -> set + print
        4. otherwise                 -> unknown

==============================================================================================*/

static int
cmd_tokenize( char* text, char** argv, int max_args )
{
    int argc = 0;

    while ( *text && argc < max_args )
    {
        while ( *text && isspace( ( unsigned char )*text ) ) text++;
        if ( !*text )
            break;

        if ( *text == '"' )
        {
            text++;
            char* dst = text;
            argv[ argc++ ] = dst;

            /* Unescape \" and \\ in place; token may end up shorter than its quoted span. */
            while ( *text && *text != '"' )
            {
                if ( text[ 0 ] == '\\' && ( text[ 1 ] == '"' || text[ 1 ] == '\\' ) )
                {
                    *dst++ = text[ 1 ];
                    text += 2;
                }
                else
                {
                    *dst++ = *text++;
                }
            }
            *dst = '\0';
        }
        else
        {
            argv[ argc++ ] = text;
            while ( *text && !isspace( ( unsigned char )*text ) ) text++;
        }

        if ( *text )
            *text++ = '\0';
    }

    /* Hit the arg cap with real text still unparsed (not just trailing whitespace)? Warn --
       those tokens are silently dropped rather than passed to the handler. */
    while ( *text && isspace( ( unsigned char )*text ) ) text++;
    if ( *text )
        con_printf( "cmd: statement has more than %d tokens, extra arguments dropped\n", max_args );

    return argc;
}

/*============================================================================================*/
/* Write side of cmd_tokenize's quote handling -- escapes '"' and '\\' so the value round-trips
   through a config file exactly. Caller writes the surrounding quotes. */

void
cmd_write_quoted( void* file, const char* str )
{
    FILE* f = ( FILE* )file;

    if ( !str )
        return;

    for ( const char* p = str; *p; ++p )
    {
        if ( *p == '"' || *p == '\\' )
            fputc( '\\', f );
        fputc( *p, f );
    }
}

/*============================================================================================*/
/* Join argv[start..argc) into one space-separated line, truncating rather than overflowing
   out_cap. Shared by cmd_cmd_alias and cmd_cmd_bind, which both rebuild a command string from
   the trailing arguments of "alias <name> ..." / "bind <key> ...". */

u32
cmd_join_args( char* out, u32 out_cap, char** argv, int start, int argc )
{
    u32 len = 0;
    for ( int i = start; i < argc; ++i )
    {
        const u32 alen = ( u32 )strlen( argv[ i ] );
        if ( len + alen + 2 >= out_cap )
            break;
        if ( len )
            out[ len++ ] = ' ';
        memcpy( out + len, argv[ i ], alen );
        len += alen;
    }
    out[ len ] = '\0';
    return len;
}

bool
cmd_execute_string( const char* text )
{
    if ( !text )
        return false;

    char  buf[ CMD_LINE_LEN ];
    char* argv[ CMD_ARG_MAX ];
    snprintf( buf, sizeof( buf ), "%s", text );
    int argc = cmd_tokenize( buf, argv, CMD_ARG_MAX );
    if ( argc == 0 )
        return false;

    /* 1. Registered command. */
    cmd_entry_t* cmd = cmd_find( argv[ 0 ] );
    if ( cmd )
    {
        cmd->fn( argc, argv );
        return true;
    }

    /* 2/3. Cvar: bare name prints, name + value sets. */
    cvar_t* cv = cvar_find( argv[ 0 ] );
    if ( cv )
    {
        if ( argc == 1 )
        {
            cvar_print_value( cv );
        }
        else if ( cvar_set( cv, argv[ 1 ] ) )
        {
            cvar_print_value( cv );
        }
        else
        {
            con_printf( "Failed to set \"%s\" to \"%s\"\n", argv[ 0 ], argv[ 1 ] );
        }
        return true;
    }

    /* 4. Alias: queue its body ahead of any pending buffer text (exec-style insertion), so a
       ';'-joined multi-statement body executes as its own group before the rest resumes. */
    const char* alias_value = cmd_alias_value( argv[ 0 ] );
    if ( alias_value )
    {
        cmd_queue_front( alias_value );
        return true;
    }

    con_printf( "Unknown command \"%s\"\n", argv[ 0 ] );
    return false;
}

/*==============================================================================================

    Built-in commands

==============================================================================================*/

static void
cmd_list( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    con_printf( "\nCommands:\n" );
    for ( u32 i = 0; i < s_cmd_count; ++i )
    {
        con_printf( "  %-16s %s\n", s_cmds[ i ].name, s_cmds[ i ].desc );
    }
    con_printf( "\nType a cvar name to print it, \"<name> <value>\" to set it.\n\n" );
}

static void
cmd_echo( int argc, char** argv )
{
    for ( int i = 1; i < argc; ++i )
    {
        con_printf( "%s%s", ( i > 1 ) ? " " : "", argv[ i ] );
    }
    con_printf( "\n" );
}

/* exec: read a config file and insert its contents ahead of pending buffered text, so the
   file runs before the rest of the statement group that triggered it (Quake exec semantics).
   The pump's statement extractor handles '\n', ';' and '//' comments, so raw file text
   queues directly -- and every registered command works inside configs. */

static void
cmd_exec( int argc, char** argv )
{
    if ( argc < 2 )
    {
        con_printf( "Usage: exec <filename>\n" );
        return;
    }

    if ( !cmd_exec_budget_take() )
    {
        con_printf( "exec: exceeded %d execs this frame, refusing '%s' (self-referential config?)\n",
                    CMD_EXEC_MAX_PER_PUMP, argv[ 1 ] );
        return;
    }

    FILE* f = fopen( argv[ 1 ], "rb" );
    if ( !f )
    {
        con_printf( "exec: could not open '%s'\n", argv[ 1 ] );
        return;
    }

    char   text[ CMD_BUF_CAP ];
    size_t n    = fread( text, 1, sizeof( text ) - 1, f );
    bool   over = ( fgetc( f ) != EOF );
    fclose( f );

    if ( over )
    {
        con_printf( "exec: '%s' exceeds %d bytes, ignored\n", argv[ 1 ], CMD_BUF_CAP - 1 );
        return;
    }

    text[ n ] = '\0';
    con_printf( "exec: %s\n", argv[ 1 ] );
    cmd_queue_front( text );
}

/*==============================================================================================

    Lifetime

==============================================================================================*/

void
cmd_system_init( void )
{
    s_cmd_count = 0;
    cmd_register( "cmdlist", cmd_list,  "List all console commands" );
    cmd_register( "echo",    cmd_echo,  "Print arguments to the console" );
    cmd_register( "exec",    cmd_exec,  "Execute a config file" );

    cmd_buffer_init();    /* buffer state + "wait" */
    cmd_bind_init();      /* bind table + bind/unbind/unbindall/bindlist */
    cmd_alias_init();     /* alias table + alias/unalias/unaliasall */
}

void
cmd_system_exit( void )
{
    s_cmd_count = 0;
    cmd_buffer_exit();
    cmd_bind_exit();
    cmd_alias_exit();
}

/*============================================================================================*/
