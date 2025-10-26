/*==============================================================================================

    cmd_buffer.c

==============================================================================================*/

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "orb.h"
#include "cvar.h"

/*==============================================================================================
    Command-line Argument Processing
==============================================================================================*/
/*
 * Process command-line arguments for cvar settings
 * Handles: +set name value, +seta name value
 * Returns number of arguments consumed
 */
// 
// int
// cvar_process_args( int argc, char** argv, int start_index )
// {
//     int consumed = 0;
// 
//     for ( int i = start_index; i < argc; )
//     {
//         const char* arg = argv[ i ];
// 
//         /* Check for +set or +seta */
//         if ( strcmp( arg, "+set" ) == 0 || strcmp( arg, "+seta" ) == 0 )
//         {
//             bool archive = ( strcmp( arg, "+seta" ) == 0 );
// 
//             if ( i + 2 >= argc )
//             {
//                 fprintf( stderr, "Error: %s requires 2 arguments\n", arg );
//                 i++;
//                 consumed++;
//                 continue;
//             }
// 
//             const char* name  = argv[ i + 1 ];
//             const char* value = argv[ i + 2 ];
// 
//             cvar_t*     cv    = cvar_find( name );
//             if ( !cv )
//             {
//                 u32 flags = CVAR_USR | CVAR_INIT;
//                 if ( archive )
//                     flags |= CVAR_ARCHIVE;
// 
//                 cv = cvar_register_base( name, "command-line", flags );
//             }
// 
//             if ( cv )
//             {
//                 if ( archive )
//                     cv->type |= CVAR_ARCHIVE;
// 
//                 cvar_set_value( name, value );
//                 printf( "Command-line: set %s = %s\n", name, value );
//             }
// 
//             i += 3;
//             consumed += 3;
//         }
//         else
//         {
//             /* Not a cvar argument */
//             break;
//         }
//     }
// 
//     return consumed;
// }

/*==============================================================================================
    Console Commands for Config System
==============================================================================================*/
/*
 * cmd_exec - Execute a config file
 * Usage: exec <filename>
 */

// void
// cmd_exec( int argc, char** argv )
// {
//     if ( argc < 2 )
//     {
//         printf( "Usage: exec <filename>\n" );
//         printf( "  Executes a configuration file.\n" );
//         return;
//     }
//
//     const char* filename = argv[ 1 ];
//     cvar_exec_config( filename );
// }


/*============================================================================================*/