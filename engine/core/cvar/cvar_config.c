// clang-format off
/*==============================================================================================

    cvar_config.c - Cvar Configuration File Management

    - cvar_write_config   : Writes all CVAR_ARCHIVE variables to a file.
    - cvar_exec_config    : Executes a configuration file.

==============================================================================================*/

/*============================================================================================*/
/* Write all archived cvars to a config file */

bool
cvar_write_config( const char* filename, u32 type_filter )
{
    if ( !filename )
        return false;

    FILE* f = fopen( filename, "w" );
    if ( !f )
    {
        fprintf( stderr, "config: could not write to '%s'\n", filename );
        return false;
    }

    fprintf( f, "// Generated config file\n" );
    fprintf( f, "// Do not modify while game is running\n\n" );

    u32 count = cvar_get_count();
    u32 written = 0;

    for ( u32 i = 0; i < count; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );

        if ( !cv )                      continue;
        if ( cv->type & CVAR_ROM )      continue;
        if ( cv->type & CVAR_INIT )     continue;
        if ( cv->type & CVAR_RUNTIME )  continue;
        if ( cv->type & type_filter )
        {
            const char* name  = cvar_get_name( cv );
            const char* value = cvar_get_value( name );
            fprintf( f, "seta %s \"%s\"\n", name, value );
            written++;
        }
    }

    fclose( f );
    printf( "cvar: %u cvars written to %s\n", written, filename );

    return true;
}

// clang-format on

/*============================================================================================*/
/* Execute a config file (loads and runs commands) */

#define MAX_ARGS     16
#define MAX_LINE_LEN 1024

bool
cvar_exec_config( const char* filename )
{
    if ( !filename )
        return false;

    FILE* f = fopen( filename, "r" );
    if ( !f )
    {
        return false;   // Not an error, may not exist on first run.
    }

    /**************************************************************/

    char  line_buf[ MAX_LINE_LEN ];
    char* argv[ MAX_ARGS ];
    int   argc;

    while ( fgets( line_buf, sizeof( line_buf ), f ) )
    {
        char* p = line_buf;
        argc    = 0;

        // Strip trailing newline
        line_buf[ strcspn( line_buf, "\r\n" ) ] = 0;

        // Skip leading whitespace
        while ( *p && isspace( ( unsigned char )*p ) ) p++;

        // Skip comments and empty lines
        if ( *p == '\0' || ( *p == '/' && p[ 1 ] == '/' ) )
            continue;

        // Tokenize line
        while ( *p && argc < MAX_ARGS )
        {
            if ( *p == '"' )    // handle quoted string
            {
                p++;
                argv[ argc++ ] = p;
                while ( *p && *p != '"' ) p++;
            }
            else    // handle unquoted token
            {
                argv[ argc++ ] = p;
                while ( *p && !isspace( ( unsigned char )*p ) ) p++;
            }
            if ( *p )
                *p++ = '\0';    // null-terminate token

            // skip whitespace to next token
            while ( *p && isspace( ( unsigned char )*p ) ) p++;
        }

        if ( argc == 0 )
            continue;

        // Dispatch command
        if ( str_icmp_eq( argv[ 0 ], "seta" ) )
        {
            cmd_seta( argc, argv );
        }
        else if ( str_icmp_eq( argv[ 0 ], "set" ) )
        {
            cmd_set( argc, argv );
        }
        // else: other commands could be handled here by a real command system
        // TODO: create command system to handle more commands
    }

    fclose( f );
    return true;
}


/*============================================================================================*/
/* Load default config sequence -- Loads: default.cfg -> config.cfg -> autoexec.cfg */

void
cvar_load_defaults( void )
{
    printf( "\n" );
    printf( "====================================================================\n" );
    printf( "Loading configuration files\n" );
    printf( "====================================================================\n" );

    /* Load default.cfg - engine defaults */
    if ( cvar_exec_config( "default.cfg" ) )
    {
        printf( "Loaded default configuration\n" );
    }
    else
    {
        printf( "Warning: default.cfg not found\n" );
    }

    /* Load config.cfg - user settings */
    if ( cvar_exec_config( "config.cfg" ) )
    {
        printf( "Loaded user configuration\n" );
    }
    else
    {
        printf( "Warning: config.cfg not found (will be created on exit)\n" );
    }

    /* Load autoexec.cfg - user startup commands */
    if ( cvar_exec_config( "autoexec.cfg" ) )
    {
        printf( "Loaded autoexec configuration\n" );
    }
    else
    {
        printf( "Info: autoexec.cfg not found (optional)\n" );
    }

    printf( "====================================================================\n" );
    printf( "\n" );
}

/*============================================================================================*/
/* Save user config -- Writes all archived cvars to config.cfg */

void
cvar_save_config( void )
{
    printf( "Saving configuration...\n" );
    cvar_write_config( "config.cfg", CVAR_ARCHIVE );
}


/*============================================================================================*/
