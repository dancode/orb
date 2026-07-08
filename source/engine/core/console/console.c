/*==============================================================================================

    engine/core/console/console.c

    Developer console view: scrollback, input history, completion.  Dispatch lives in the
    cmd backend (cmd.c) -- con_exec forwards there.  See console.h for the design contract.
    Compiled inside the core unity build (core_cvar.c) after cvar.c and cmd.c, so the
    cvar_* / cmd_* declarations are visible.

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
        else
        {
            // Soft-wrap at the line-length limit instead of dropping the overflow, so
            // scrollback never loses text that stdout still shows in full.
            if ( s_con_partial_len >= CON_LINE_LEN - 1 )
                con_commit_line();
            s_con_partial[ s_con_partial_len++ ] = *p;
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

u32
con_line_total( void )
{
    return s_con_line_total;
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

    Input submission

    The console is a front end: echo the line, record it, hand it to the backend.

==============================================================================================*/

/* Shared ingest: trim, echo, record.  Returns NULL for blank input (no echo, no history). */

static const char*
con_ingest( const char* line )
{
    if ( !line )
        return NULL;

    const char* scan = line;
    while ( *scan && isspace( ( unsigned char )*scan ) ) scan++;
    if ( !*scan )
        return NULL;

    /* Flush any partial output so the echo starts on its own line. */
    if ( s_con_partial_len > 0 )
        con_print( "\n" );

    con_printf( "] %s\n", scan );
    con_history_push( scan );
    return scan;
}

bool
con_exec( const char* line )
{
    // Bypasses the deferred command buffer entirely for a registered command or cvar
    // get/set -- runs right here.  An alias or "exec" is the one exception: its body
    // still goes through cmd_queue_front, so it only runs once something later calls
    // cmd_pump.  See the con_exec contract note in console.h before relying on this
    // for anything beyond a single command or cvar.
    const char* scan = con_ingest( line );
    return scan ? cmd_execute_string( scan ) : false;
}

void
con_submit( const char* line )
{
    const char* scan = con_ingest( line );
    if ( scan )
        cmd_queue( scan );
}

/*==============================================================================================

    Completion

==============================================================================================*/

u32
con_complete( const char* prefix, const char** out_names, u32 max )
{
    if ( !prefix || !prefix[ 0 ] || !out_names || max == 0 )
        return 0;

    u32 n = 0;

    const u32 cmd_total = cmd_count();
    for ( u32 i = 0; i < cmd_total && n < max; ++i )
    {
        if ( cvar_str_icmp_prefix( cmd_name( i ), prefix ) )
            out_names[ n++ ] = cmd_name( i );
    }

    const u32 cvar_total = cvar_get_count();
    for ( u32 i = 0; i < cvar_total && n < max; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || ( cv->flags & CVAR_HIDDEN ) )
            continue;

        const char* name = cvar_get_name( cv );
        if ( cvar_str_icmp_prefix( name, prefix ) )
            out_names[ n++ ] = name;
    }

    const u32 alias_total = cmd_alias_count();
    for ( u32 i = 0; i < alias_total && n < max; ++i )
    {
        const char* name = cmd_alias_name( i );
        if ( cvar_str_icmp_prefix( name, prefix ) )
            out_names[ n++ ] = name;
    }

    return n;
}

/*==============================================================================================

    Console-owned commands (view concerns: they register into the backend like any other)

==============================================================================================*/

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
    cmd_register( "clear",   con_cmd_clear,   "Clear console scrollback" );
    cmd_register( "history", con_cmd_history, "Show input history" );
}

void
con_exit( void )
{
    s_con_line_total    = 0;
    s_con_partial_len   = 0;
    s_con_history_total = 0;
}

/*============================================================================================*/
