/*==============================================================================================

    cmd_buffer.c

==============================================================================================*/

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "orb.h"
#include "cvar.h"

/*============================================================================================*/

#define MAX_CONFIG_LINE   1024
#define MAX_CONFIG_TOKENS 16

typedef struct
{
    char* tokens[ MAX_CONFIG_TOKENS ];
    int   count;

} token_list_t;

/*==============================================================================================

    command parsing utility

==============================================================================================*/

/* Check if character is a quote */
static bool
is_quote( char c )
{
    return c == '"' || c == '\'';
}

/* Check if character is whitespace */
static bool
is_space( char c )
{
    return isspace( ( unsigned char )c );
}

/* Skip whitespace */
static const char*
skip_whitespace( const char* s )
{
    while ( *s && is_space( *s ) ) s++;
    return s;
}

/*============================================================================================*/
/* Parse a line into tokens -- supports quoted strings and handles escapes.*/

static token_list_t
parse_line( char* line )
{
    token_list_t result = { .count = 0 };
    char*        p      = line;

    while ( *p && result.count < MAX_CONFIG_TOKENS )
    {
        p = ( char* )skip_whitespace( p );

        /* End of line or comment */
        if ( *p == '\0' || *p == '/' )
            break;

        /* Handle quoted strings */
        if ( is_quote( *p ) )
        {
            char  quote_char = *p++;
            char* start      = p;

            /* Find closing quote */
            while ( *p && *p != quote_char )
            {
                /* Handle escape sequences */
                if ( *p == '\\' && p[ 1 ] != '\0' )
                    p++;
                p++;
            }

            if ( *p == quote_char )
                *p++ = '\0';

            result.tokens[ result.count++ ] = start;
        }
        else
        {
            /* Unquoted token */
            char* start = p;

            while ( *p && !is_space( *p ) && *p != '/' ) p++;

            if ( *p )
                *p++ = '\0';

            result.tokens[ result.count++ ] = start;
        }
    }

    return result;
}

/*============================================================================================*/
/* Execute a parsed command line.*/

static void
exec_command( token_list_t* tokens )
{
    if ( tokens->count == 0 )
        return;

    const char* cmd = tokens->tokens[ 0 ];

    /* Handle 'set' command */
    if ( strcmp( cmd, "set" ) == 0 )
    {
        if ( tokens->count < 3 )
        {
            fprintf( stderr, "config: 'set' requires 2 arguments\n" );
            return;
        }

        const char* name  = tokens->tokens[ 1 ];
        const char* value = tokens->tokens[ 2 ];

        cvar_t*     cv    = cvar_find( name );

        /* Create user variable if it doesn't exist */
        if ( !cv )
        {
            cv = cvar_register_base( name, "user created", CVAR_USR );
        }

        if ( cv )
        {
            cvar_set_value( name, value );
        }
    }
    /* Handle 'seta' command (set + archive) */
    else if ( strcmp( cmd, "seta" ) == 0 )
    {
        if ( tokens->count < 3 )
        {
            fprintf( stderr, "config: 'seta' requires 2 arguments\n" );
            return;
        }

        const char* name  = tokens->tokens[ 1 ];
        const char* value = tokens->tokens[ 2 ];

        cvar_t*     cv    = cvar_find( name );

        /* Create archived user variable if it doesn't exist */
        if ( !cv )
        {
            cv = cvar_register_base( name, "user created", CVAR_USR | CVAR_ARCHIVE );
        }
        else
        {
            /* Mark existing cvar for archiving */
            cv->type |= CVAR_ARCHIVE;
        }

        if ( cv )
        {
            cvar_set_value( name, value );
        }
    }
    else
    {
        /* Unknown command - could be forwarded to command system */
        fprintf( stderr, "config: unknown command '%s'\n", cmd );
    }
}

/*============================================================================================*/
/* Execute a single line from config file */

static void
exec_config_line( char* line )
{
    /* Skip empty lines and comments */
    const char* p = skip_whitespace( line );
    if ( *p == '\0' || *p == '/' )
        return;

    /* Parse and execute */
    token_list_t tokens = parse_line( line );
    exec_command( &tokens );
}

/*============================================================================================*/
/* Load and execute a config file -- Returns true on success */

bool
cvar_exec_config( const char* filename )
{
    if ( !filename )
    {
        fprintf( stderr, "config: null filename\n" );
        return false;
    }

    FILE* fp = fopen( filename, "r" );
    if ( !fp )
    {
        fprintf( stderr, "config: couldn't open '%s'\n", filename );
        return false;
    }

    printf( "Executing config: %s\n", filename );

    char line[ MAX_CONFIG_LINE ];
    int  line_num = 0;

    while ( fgets( line, sizeof( line ), fp ) )
    {
        line_num++;

        /* Remove newline */
        size_t len = strlen( line );
        if ( len > 0 && line[ len - 1 ] == '\n' )
            line[ len - 1 ] = '\0';
        if ( len > 1 && line[ len - 2 ] == '\r' )
            line[ len - 2 ] = '\0';

        exec_config_line( line );
    }

    fclose( fp );
    printf( "Executed %d lines from %s\n", line_num, filename );

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

/*==============================================================================================
    Command-line Argument Processing
==============================================================================================*/
/*
 * Process command-line arguments for cvar settings
 * Handles: +set name value, +seta name value
 * Returns number of arguments consumed
 */

int
cvar_process_args( int argc, char** argv, int start_index )
{
    int consumed = 0;

    for ( int i = start_index; i < argc; )
    {
        const char* arg = argv[ i ];

        /* Check for +set or +seta */
        if ( strcmp( arg, "+set" ) == 0 || strcmp( arg, "+seta" ) == 0 )
        {
            bool archive = ( strcmp( arg, "+seta" ) == 0 );

            if ( i + 2 >= argc )
            {
                fprintf( stderr, "Error: %s requires 2 arguments\n", arg );
                i++;
                consumed++;
                continue;
            }

            const char* name  = argv[ i + 1 ];
            const char* value = argv[ i + 2 ];

            cvar_t*     cv    = cvar_find( name );

            if ( !cv )
            {
                u32 flags = CVAR_USR | CVAR_INIT;
                if ( archive )
                    flags |= CVAR_ARCHIVE;

                cv = cvar_register_base( name, "command-line", flags );
            }

            if ( cv )
            {
                if ( archive )
                    cv->type |= CVAR_ARCHIVE;

                cvar_set_value( name, value );
                printf( "Command-line: set %s = %s\n", name, value );
            }

            i += 3;
            consumed += 3;
        }
        else
        {
            /* Not a cvar argument */
            break;
        }
    }

    return consumed;
}

/*==============================================================================================
    Console Commands for Config System
==============================================================================================*/
/*
 * cmd_exec - Execute a config file
 * Usage: exec <filename>
 */

void
cmd_exec( int argc, char** argv )
{
    if ( argc < 2 )
    {
        printf( "Usage: exec <filename>\n" );
        printf( "  Executes a configuration file.\n" );
        return;
    }

    const char* filename = argv[ 1 ];
    cvar_exec_config( filename );
}


/*============================================================================================*/