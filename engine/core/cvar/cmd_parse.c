/*==============================================================================================

    cmd_parse.c

==============================================================================================*/

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "orb.h"
#include "cvar.h"

/*============================================================================================*/

#define MAX_CONFIG_LINE   1024
#define MAX_CONFIG_TOKENS 16

// typedef struct
// {
//     char* tokens[ MAX_CONFIG_TOKENS ];
//     int   count;
// 
// } token_list_t;

/*==============================================================================================

    command parsing utility

==============================================================================================*/

/* Check if character is a quote */
// static bool
// is_quote( char c )
// {
//     return c == '"' || c == '\'';
// }
// 
// /* Check if character is whitespace */
// static bool
// is_space( char c )
// {
//     return isspace( ( unsigned char )c );
// }
// 
// /* Skip whitespace */
// static const char*
// skip_whitespace( const char* s )
// {
//     while ( *s && is_space( *s ) ) s++;
//     return s;
// }

/*============================================================================================*/
/* Parse a line into tokens -- supports quoted strings and handles escapes.*/

// static token_list_t
// parse_line( char* line )
// {
//     token_list_t result = { .count = 0 };
//     char*        p      = line;
// 
//     while ( *p && result.count < MAX_CONFIG_TOKENS )
//     {
//         p = ( char* )skip_whitespace( p );
// 
//         /* End of line or comment */
//         if ( *p == '\0' || *p == '/' )
//             break;
// 
//         /* Handle quoted strings */
//         if ( is_quote( *p ) )
//         {
//             char  quote_char = *p++;
//             char* start      = p;
// 
//             /* Find closing quote */
//             while ( *p && *p != quote_char )
//             {
//                 /* Handle escape sequences */
//                 if ( *p == '\\' && p[ 1 ] != '\0' )
//                     p++;
//                 p++;
//             }
// 
//             if ( *p == quote_char )
//                 *p++ = '\0';
// 
//             result.tokens[ result.count++ ] = start;
//         }
//         else
//         {
//             /* Unquoted token */
//             char* start = p;
// 
//             while ( *p && !is_space( *p ) && *p != '/' ) p++;
// 
//             if ( *p )
//                 *p++ = '\0';    // null-terminate token
// 
//             result.tokens[ result.count++ ] = start;
//         }
//     }
// 
//     return result;
// }

/*============================================================================================*/
/* Execute a parsed command line.*/

// static void
// exec_command( token_list_t* tokens )
// {
//     if ( tokens->count == 0 )
//         return;
// 
//     const char* cmd = tokens->tokens[ 0 ];
// 
//     /* Handle 'set' command */
//     if ( strcmp( cmd, "set" ) == 0 )
//     {
//         if ( tokens->count < 3 )
//         {
//             fprintf( stderr, "config: 'set' requires 2 arguments\n" );
//             return;
//         }
// 
//         const char* name  = tokens->tokens[ 1 ];
//         const char* value = tokens->tokens[ 2 ];
// 
//         cvar_t*     cv    = cvar_find( name );
// 
//         /* Create user variable if it doesn't exist */
//         if ( !cv )
//         {
//             cv = cvar_register_base( name, "user created", CVAR_USR );
//         }
// 
//         if ( cv )
//         {
//             cvar_set_value( name, value );
//         }
//     }
//     /* Handle 'seta' command (set + archive) */
//     else if ( strcmp( cmd, "seta" ) == 0 )
//     {
//         if ( tokens->count < 3 )
//         {
//             fprintf( stderr, "config: 'seta' requires 2 arguments\n" );
//             return;
//         }
// 
//         const char* name  = tokens->tokens[ 1 ];
//         const char* value = tokens->tokens[ 2 ];
// 
//         cvar_t*     cv    = cvar_find( name );
// 
//         /* Create archived user variable if it doesn't exist */
//         if ( !cv )
//         {
//             cv = cvar_register_base( name, "user created", CVAR_USR | CVAR_ARCHIVE );
//         }
//         else
//         {
//             /* Mark existing cvar for archiving */
//             cv->type |= CVAR_ARCHIVE;
//         }
// 
//         if ( cv )
//         {
//             cvar_set_value( name, value );
//         }
//     }
//     else
//     {
//         /* Unknown command - could be forwarded to command system */
//         fprintf( stderr, "config: unknown command '%s'\n", cmd );
//     }
// }

/*============================================================================================*/
/* Execute a single line from config file */

// static void
// exec_config_line( char* line )
// {
//     /* Skip empty lines and comments */
//     const char* p = skip_whitespace( line );
//     if ( *p == '\0' || *p == '/' )
//         return;
// 
//     /* Parse and execute */
//     token_list_t tokens = parse_line( line );
//     exec_command( &tokens );
// }

/*============================================================================================*/
/* Load and execute a config file -- Returns true on success */

// bool
// cvar_exec_config( const char* filename )
// {
//     if ( !filename )
//     {
//         fprintf( stderr, "config: null filename\n" );
//         return false;
//     }
// 
//     FILE* fp = fopen( filename, "r" );
//     if ( !fp )
//     {
//         fprintf( stderr, "config: couldn't open '%s'\n", filename );
//         return false;
//     }
// 
//     printf( "Executing config: %s\n", filename );
// 
//     char line[ MAX_CONFIG_LINE ];
//     int  line_num = 0;
// 
//     while ( fgets( line, sizeof( line ), fp ) )
//     {
//         line_num++;
// 
//         /* Remove newline */
//         size_t len = strlen( line );
//         if ( len > 0 && line[ len - 1 ] == '\n' )
//             line[ len - 1 ] = '\0';
//         if ( len > 1 && line[ len - 2 ] == '\r' )
//             line[ len - 2 ] = '\0';
// 
//         exec_config_line( line );
//     }
// 
//     fclose( fp );
//     printf( "Executed %d lines from %s\n", line_num, filename );
// 
//     return true;
// }

/*============================================================================================*/