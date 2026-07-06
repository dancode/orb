/*==============================================================================================

    engine/core/console/console.c

    Developer console: scrollback, command registry, line execution, history, completion.
    See console.h for the design contract.  Compiled inside the core unity build
    (core_cvar.c) after cvar.h, so the cvar_* declarations are visible for dispatch.

==============================================================================================*/

/*==============================================================================================

    Scrollback

    A ring of fixed-width lines plus a partial-line accumulator: cvar_print_value builds one
    output line from several con_printf calls, so text only becomes a line on '\n'.  Every
    write also echoes to stdout -- the console is fully usable with no front end attached.

==============================================================================================*/

static char s_con_lines[ CON_LINE_CAP ][ CON_LINE_LEN ];    // scrollback ring storage
static u32  s_con_line_total = 0;                           // lines committed since startup
static char s_con_partial[ CON_LINE_LEN ];                  // pending text awaiting '\n'
static u32  s_con_partial_len = 0;                          // bytes accumulated in partial

/* Commit the accumulated partial as one scrollback line. */

static void
con_commit_line( void )
{
    char* dst = s_con_lines[ s_con_line_total % CON_LINE_CAP ];
    memcpy( dst, s_con_partial, s_con_partial_len );
    dst[ s_con_partial_len ] = '\0';
    s_con_line_total++;
    s_con_partial_len = 0;
}

void
con_print( const char* text )
{
    if ( !text )
        return;

    fputs( text, stdout );    // headless echo: console works with no front end

    for ( const char* p = text; *p; ++p )
    {
        if ( *p == '\n' )
        {
            con_commit_line();
        }
        else if ( s_con_partial_len < CON_LINE_LEN - 1 )
        {
            s_con_partial[ s_con_partial_len++ ] = *p;    // overlong lines truncate
        }
    }
}

void
con_printf( const char* fmt, ... )
{
    if ( !fmt )
        return;

    char    buf[ 1024 ];
    va_list args;
    va_start( args, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );

    con_print( buf );
}

void
con_clear( void )
{
    s_con_line_total  = 0;
    s_con_partial_len = 0;
}

u32
con_line_count( void )
{
    return ( s_con_line_total < CON_LINE_CAP ) ? s_con_line_total : CON_LINE_CAP;
}

const char*
con_line_get( u32 index )
{
    const u32 count = con_line_count();
    if ( index >= count )
        return "";

    const u32 first = s_con_line_total - count;    // oldest retained line
    return s_con_lines[ ( first + index ) % CON_LINE_CAP ];
}

/*==============================================================================================

    Command registry

    Compact array with copied names -- unregistering swaps the tail entry down, so lookup is
    a linear scan over live entries only.  64 commands x linear scan is well under any budget.

==============================================================================================*/

typedef struct con_cmd_s
{
    char       name[ CON_CMD_NAME_LEN ];    // copied at registration (hot-reload safe)
    char       desc[ CON_CMD_DESC_LEN ];    // copied at registration
    con_cmd_fn fn;                          // handler

} con_cmd_t;

static con_cmd_t s_con_cmds[ CON_CMD_CAP ];
static u32       s_con_cmd_count = 0;

static con_cmd_t*
con_cmd_find( const char* name )
{
    for ( u32 i = 0; i < s_con_cmd_count; ++i )
    {
        if ( cvar_str_icmp_eq( s_con_cmds[ i ].name, name ) )
            return &s_con_cmds[ i ];
    }
    return NULL;
}

bool
con_cmd_register( const char* name, con_cmd_fn fn, const char* desc )
{
    if ( !name || !name[ 0 ] || !fn )
        return false;

    if ( con_cmd_find( name ) )
    {
        con_printf( "console: command \"%s\" already registered\n", name );
        return false;
    }

    if ( s_con_cmd_count >= CON_CMD_CAP )
    {
        con_printf( "console: command registry full (max %d)\n", CON_CMD_CAP );
        return false;
    }

    con_cmd_t* cmd = &s_con_cmds[ s_con_cmd_count++ ];
    snprintf( cmd->name, sizeof( cmd->name ), "%s", name );
    snprintf( cmd->desc, sizeof( cmd->desc ), "%s", desc ? desc : "" );
    cmd->fn = fn;
    return true;
}

void
con_cmd_unregister( const char* name )
{
    con_cmd_t* cmd = con_cmd_find( name );
    if ( !cmd )
        return;

    *cmd = s_con_cmds[ --s_con_cmd_count ];    // swap tail down; order is not contractual
}

u32
con_cmd_count( void )
{
    return s_con_cmd_count;
}

const char*
con_cmd_name( u32 index )
{
    return ( index < s_con_cmd_count ) ? s_con_cmds[ index ].name : "";
}

const char*
con_cmd_desc( u32 index )
{
    return ( index < s_con_cmd_count ) ? s_con_cmds[ index ].desc : "";
}

/*==============================================================================================

    History

==============================================================================================*/

static char s_con_history[ CON_HISTORY_CAP ][ CON_HISTORY_LEN ];
static u32  s_con_history_total = 0;

static void
con_history_push( const char* line )
{
    /* Skip immediate duplicates: retrying the same line should not flood recall. */
    if ( s_con_history_total > 0 )
    {
        const char* last = s_con_history[ ( s_con_history_total - 1 ) % CON_HISTORY_CAP ];
        if ( strcmp( last, line ) == 0 )
            return;
    }

    snprintf( s_con_history[ s_con_history_total % CON_HISTORY_CAP ], CON_HISTORY_LEN, "%s", line );
    s_con_history_total++;
}

u32
con_history_count( void )
{
    return ( s_con_history_total < CON_HISTORY_CAP ) ? s_con_history_total : CON_HISTORY_CAP;
}

const char*
con_history_get( u32 index )
{
    const u32 count = con_history_count();
    if ( index >= count )
        return "";

    const u32 first = s_con_history_total - count;
    return s_con_history[ ( first + index ) % CON_HISTORY_CAP ];
}

/*==============================================================================================

    Execution

    Tokenize one line in place (double-quoted strings form one token), then dispatch:
        1. registered command        -> fn( argc, argv )
        2. cvar name alone           -> print current value
        3. cvar name + value         -> set + print
        4. otherwise                 -> unknown

==============================================================================================*/

static int
con_tokenize( char* text, char** argv, int max_args )
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
            argv[ argc++ ] = text;
            while ( *text && *text != '"' ) text++;
        }
        else
        {
            argv[ argc++ ] = text;
            while ( *text && !isspace( ( unsigned char )*text ) ) text++;
        }

        if ( *text )
            *text++ = '\0';
    }

    return argc;
}

bool
con_exec( const char* line )
{
    if ( !line )
        return false;

    /* Ignore blank input entirely -- no echo, no history. */
    const char* scan = line;
    while ( *scan && isspace( ( unsigned char )*scan ) ) scan++;
    if ( !*scan )
        return false;

    /* Flush any partial output so the echo starts on its own line. */
    if ( s_con_partial_len > 0 )
        con_print( "\n" );

    con_printf( "] %s\n", scan );
    con_history_push( scan );

    char  buf[ CON_HISTORY_LEN * 2 ];
    char* argv[ CON_ARG_MAX ];
    snprintf( buf, sizeof( buf ), "%s", scan );
    int argc = con_tokenize( buf, argv, CON_ARG_MAX );
    if ( argc == 0 )
        return false;

    /* 1. Registered command. */
    con_cmd_t* cmd = con_cmd_find( argv[ 0 ] );
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
        else if ( cvar_set_value( cvar_get_name( cv ), argv[ 1 ] ) )
        {
            cvar_print_value( cv );
        }
        else
        {
            con_printf( "Failed to set \"%s\" to \"%s\"\n", argv[ 0 ], argv[ 1 ] );
        }
        return true;
    }

    con_printf( "Unknown command \"%s\"\n", argv[ 0 ] );
    return false;
}

/*==============================================================================================

    Completion

==============================================================================================*/

static bool
con_prefix_match( const char* name, const char* prefix )
{
    while ( *prefix )
    {
        char cn = *name, cp = *prefix;
        if ( cn >= 'A' && cn <= 'Z' ) cn = cn + ( 'a' - 'A' );
        if ( cp >= 'A' && cp <= 'Z' ) cp = cp + ( 'a' - 'A' );
        if ( cn != cp )
            return false;
        ++name;
        ++prefix;
    }
    return true;
}

u32
con_complete( const char* prefix, const char** out_names, u32 max )
{
    if ( !prefix || !prefix[ 0 ] || !out_names || max == 0 )
        return 0;

    u32 n = 0;

    for ( u32 i = 0; i < s_con_cmd_count && n < max; ++i )
    {
        if ( con_prefix_match( s_con_cmds[ i ].name, prefix ) )
            out_names[ n++ ] = s_con_cmds[ i ].name;
    }

    const u32 cvar_total = cvar_get_count();
    for ( u32 i = 0; i < cvar_total && n < max; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || ( cv->type & CVAR_HIDDEN ) )
            continue;

        const char* name = cvar_get_name( cv );
        if ( con_prefix_match( name, prefix ) )
            out_names[ n++ ] = name;
    }

    return n;
}

/*==============================================================================================

    Built-in commands

==============================================================================================*/

static void
con_cmd_help( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    con_printf( "\nCommands:\n" );
    for ( u32 i = 0; i < s_con_cmd_count; ++i )
    {
        con_printf( "  %-16s %s\n", s_con_cmds[ i ].name, s_con_cmds[ i ].desc );
    }
    con_printf( "\nType a cvar name to print it, \"<name> <value>\" to set it.\n\n" );
}

static void
con_cmd_echo( int argc, char** argv )
{
    for ( int i = 1; i < argc; ++i )
    {
        con_printf( "%s%s", ( i > 1 ) ? " " : "", argv[ i ] );
    }
    con_printf( "\n" );
}

static void
con_cmd_clear( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    con_clear();
}

static void
con_cmd_history( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    const u32 count = con_history_count();
    for ( u32 i = 0; i < count; ++i )
    {
        con_printf( "  %2u  %s\n", i, con_history_get( i ) );
    }
}

/*==============================================================================================

    Lifetime

==============================================================================================*/

void
con_init( void )
{
    con_cmd_register( "help",    con_cmd_help,    "List all console commands" );
    con_cmd_register( "cmdlist", con_cmd_help,    "List all console commands" );
    con_cmd_register( "echo",    con_cmd_echo,    "Print arguments to the console" );
    con_cmd_register( "clear",   con_cmd_clear,   "Clear console scrollback" );
    con_cmd_register( "history", con_cmd_history, "Show input history" );
}

void
con_exit( void )
{
    s_con_line_total    = 0;
    s_con_partial_len   = 0;
    s_con_cmd_count     = 0;
    s_con_history_total = 0;
}

/*============================================================================================*/
