/*==============================================================================================

    cvar_cmd.c - Console Variable Commands

    User-facing console commands for cvar manipulation:
    - set/seta   : Set variable value (seta archives to config)
    - toggle     : Toggle boolean variable
    - reset      : Reset variable to default
    - cvarlist   : List all cvars with filtering
    - cvarinfo   : Show detailed info about a cvar

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "cvar.h"
#include "string_pool.h"

// clang-format on

/*==============================================================================================

    Command Registration

==============================================================================================*/

void
cvar_register_commands( void )
{
    // NOTE: must init your command system before registering commands

    /* Note: These are example function signatures.
     * Replace with your actual command registration API */

    // cmd_register( "set", cmd_set, "Set a console variable value" );
    // cmd_register( "seta", cmd_seta, "Set and archive a console variable" );
    // cmd_register( "toggle", cmd_toggle, "Toggle a boolean variable" );
    // cmd_register( "reset", cmd_reset, "Reset a variable to default" );
    // cmd_register( "reset_all", cmd_reset_all, "Reset all variables to defaults" );
    // cmd_register( "cvarlist", cmd_cvarlist, "List all console variables" );
    // cmd_register( "cvarinfo", cmd_cvarinfo, "Show detailed cvar information" );
    // cmd_register( "apply_latched", cmd_apply_latched, "Apply latched cvar changes" );
    // cmd_register( "cvar_modified", cmd_cvar_modified, "List modified cvars" );

    /* Note: Replace with your actual command registration API */
    // cmd_register( "exec", cmd_exec, "Execute a config file" );
    // cmd_register( "writeconfig", cmd_writeconfig, "Write config file" );
}

/*==============================================================================================
    Internal 'set' command logic
    Usage:
            var value   : set only if var exists
    set     var value   : set or create a user var if not found
    seta    var value   : CVAR_ARCHIVE saved to config
    setu    var vlaue   : CVAR_USERINFO	sent to server in userinfo
    sets    var value   : CVAR_SERVERINFO advertised to clients by server
==============================================================================================*/

static void
cmd_set_internal( const char* name, const char* value, u32 internal_flags )
{
    /* Create user variable if doesn't exist */
    cvar_t* cv = cvar_find( name );
    if ( !cv )
    {
        // Keep track of what variables were originally user created vs engine defined.
        cv = cvar_register_base( name, "user created", CVAR_USR | internal_flags );
        if ( !cv )
        {
            printf( "Error: Failed to create variable '%s'\n", name );
            return;
        }
    }
    else
    {
        /* Mark existing cvar for archiving */
        cv->type |= internal_flags;
    }

    /* Set the value */
    if ( cvar_set_value( name, value ) )
    {
        cvar_print_value( cv );
    }
    else
    {
        printf( "Failed to set '%s' to '%s'\n", name, value );
    }
}

/*============================================================================================*/
/* Set a cvar value or create a user cvar if not found */
/* Usage: seta <name> <value> */

void
cmd_set( int argc, char** argv )
{
    if ( argc < 3 )
    {
        printf( "Usage: set <variable> <value>\n" );
        printf( "  Sets a console variable to the specified value.\n" );
        return;
    }

    const char* name  = argv[ 1 ];
    const char* value = argv[ 2 ];

    cmd_set_internal( name, value, 0 );
}

/* Set a cvar value and mark for archiving */
/* Usage: seta <name> <value> */

void
cmd_seta( int argc, char** argv )
{
    if ( argc < 3 )
    {
        printf( "Usage: seta <variable> <value>\n" );
        printf( "  Sets a console variable and marks it to be saved to config.\n" );
        return;
    }

    const char* name  = argv[ 1 ];
    const char* value = argv[ 2 ];

    cmd_set_internal( name, value, CVAR_ARCHIVE );
}

/*============================================================================================*/
/* cmd_toggle - Toggle a boolean cvar */
/* Usage: toggle <name> */

void
cmd_toggle( int argc, char** argv )
{
    if ( argc < 2 )
    {
        printf( "Usage: toggle <variable>\n" );
        printf( "  Toggles a boolean variable between 0 and 1.\n" );
        return;
    }

    const char* name = argv[ 1 ];
    cvar_t*     cv   = cvar_find( name );

    if ( !cv )
    {
        printf( "Error: Variable '%s' not found\n", name );
        return;
    }

    if ( !cvar_is_int( cv ) && !( cv->type & CVAR_BOOL ) )
    {
        printf( "Error: Variable '%s' is not a boolean\n", name );
        return;
    }

    /* Toggle the value */
    const char* new_value = ( cv->b.value ) ? "0" : "1";
    cvar_set_value( name, new_value );
    cvar_print_value( cv );
}

/*============================================================================================*/
/* cmd_reset - Reset cvar to default value */
/* Usage : reset <name> */

void
cmd_reset( int argc, char** argv )
{
    if ( argc < 2 )
    {
        printf( "Usage: reset <variable>\n" );
        printf( "  Resets a console variable to its default value.\n" );
        return;
    }

    const char* name = argv[ 1 ];
    cvar_t*     cv   = cvar_find( name );

    if ( !cv )
    {
        printf( "Error: Variable '%s' not found\n", name );
        return;
    }

    cvar_reset( cv );

    printf( "Reset '%s' to default value\n", name );
    cvar_print_value( cv );
}

/*============================================================================================*/
/* cmd_reset_all - Reset all cvars to defaults */
/* Usage : reset_all */

void
cmd_reset_all( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "Resetting all cvars to default values...\n" );
    cvar_reset_all();
    printf( "Done.\n" );
}

/*============================================================================================*/
/* cmd_apply_latched - Apply all latched cvar changes */
/* Usage : apply_latched */

void
cmd_apply_latched( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    u32 count = 0;
    u32 total = cvar_get_count();

    /* Count latched cvars */
    for ( u32 i = 0; i < total; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( cv && ( cv->flag & CVAR_LATCHED ) )
            count++;
    }

    if ( count == 0 )
    {
        printf( "No latched cvars to apply.\n" );
        return;
    }

    printf( "Applying %u latched cvar(s)...\n", count );
    cvar_apply_latched();
    printf( "Done.\n" );
}

/*============================================================================================*/
/*  cmd_cvar_modified - List all modified cvars */
/* Usage : cvar_modified */

void
cmd_cvar_modified( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    u32 count = 0;
    u32 total = cvar_get_count();

    printf( "\nModified cvars:\n" );
    printf( "--------------------------------------------------------------------\n" );

    for ( u32 i = 0; i < total; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || !( cv->flag & CVAR_MODIFIED ) )
            continue;

        const char* name  = cvar_get_name( cv );
        const char* value = cvar_get_value( name );
        printf( "  %-24s = %s\n", name, value );
        count++;
    }

    printf( "--------------------------------------------------------------------\n" );
    printf( "%u modified cvar(s)\n\n", count );
}

/*============================================================================================*/
/* cmd_cvarinfo - Display detailed info about a cvar */
/* Usage: cvarinfo <name> */

// clang-format off

void
cmd_cvarinfo( int argc, char** argv )
{
    if ( argc < 2 )
    {
        printf( "Usage: cvarinfo <variable>\n" );
        printf( "  Displays detailed information about a console variable.\n" );
        return;
    }

    const char* name = argv[ 1 ];
    cvar_t*     cv   = cvar_find( name );

    if ( !cv )
    {
        printf( "Error: Variable '%s' not found\n", name );
        return;
    }

    printf( "\nVariable: %s\n", cvar_get_name( cv ) );
    printf( "Description: %s\n", cvar_get_desc( cv ) );

    cvar_print_value( cv );
    cvar_print_flags( cv );

    /* Show default value */
    switch ( cv->type & CVAR_TYPE_MASK )
    {
        case CVAR_BOOL:     printf( "  Default: %d\n", cv->b.reset ); break;
        case CVAR_INT:      printf( "  Default: %d\n", cv->i.reset ); break;
        case CVAR_FLOAT:    printf( "  Default: %g\n", cv->f.reset ); break;
        case CVAR_STR:      printf( "  Default: index %u\n", cv->s.reset ); break;
        default: break;
    }

    /* Show string list options for CVAR_STR */
    if ( cv->type & CVAR_STR )
    {
        printf( "  Options:\n" );
        for ( u32 i = 0; i < cv->s.count; ++i )
        {
            const char* str = cvar_get_string_from_id( cv, i );
            printf( "    [%u] %s%s\n", i, str, ( i == cv->s.value ) ? " (current)" : "" );
        }
    }

    printf( "\n" );
}

/*============================================================================================*/
/* cmd_cvarlist - List all cvars with optional filtering */
/* Usage: cvarlist [filter] */

void
cmd_cvarlist( int argc, char** argv )
{
    const char* filter = ( argc > 1 ) ? argv[ 1 ] : NULL;
    u32         count  = 0;
    u32         total  = cvar_get_count();

    printf( "\n" );
    printf( "%-24s %-12s %-10s %s\n", "Name", "Value", "Type", "Flags" );
    printf( "--------------------------------------------------------------------\n" );

    for ( u32 i = 0; i < total; ++i )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv )
            continue;

        const char* name = cvar_get_name( cv );

        /* Apply filter */
        if ( filter && strstr( name, filter ) == NULL )
            continue;

        /* Skip hidden variables in release builds */
        if ( cv->type & CVAR_HIDDEN )
            continue;
        if ( cv->type & CVAR_DEVONLY )
            continue;

        const char* value    = cvar_get_value( name );
        const char* type_str = "unknown";

        switch ( cv->type & CVAR_TYPE_MASK )
        {
            case CVAR_BOOL:     type_str = "bool"; break;
            case CVAR_INT:      type_str = "int"; break;
            case CVAR_FLOAT:    type_str = "float"; break;
            case CVAR_STR:      type_str = "choice"; break;
            case CVAR_BUF:      type_str = "string"; break;
            case CVAR_REF:      type_str = "ref"; break;
            case CVAR_USR:      type_str = "user"; break;
        }

        /* Build flags string */
        char flags[ 64 ] = "";
        if ( cv->type & CVAR_ROM )          strcat( flags, "R" );
        if ( cv->type & CVAR_ARCHIVE )      strcat( flags, "A" );
        if ( cv->type & CVAR_LATCH )        strcat( flags, "L" );
        if ( cv->type & CVAR_CHEAT )        strcat( flags, "C" );
        if ( cv->type & CVAR_USERINFO )     strcat( flags, "U" );
        if ( cv->type & CVAR_SERVERINFO )   strcat( flags, "S" );
        if ( cv->flag & CVAR_MODIFIED )     strcat( flags, "*" );

        printf( "%-24s %-12s %-10s %s\n", name, value, type_str, flags );
        count++;
    }

    printf( "--------------------------------------------------------------------\n" );
    printf( "%u cvars", count );
    if ( filter )
        printf( " (filtered from %u total)", total );
    printf( "\n\n" );

    printf( "Flag legend:\n" );
    printf( "  R = Read-only    A = Archived    L = Latched    C = Cheat\n" );
    printf( "  U = UserInfo     S = ServerInfo  * = Modified\n" );
    printf( "\n" );
}

/*============================================================================================*/
/* cmd_writeconfig - Write current config to file */
/* Usage: writeconfig [filename] */

void
cmd_writeconfig( int argc, char** argv )
{
    const char* filename = ( argc > 1 ) ? argv[ 1 ] : "config.cfg";

    if ( cvar_write_config( filename, CVAR_ARCHIVE ) )
    {
        printf( "Configuration saved to '%s'\n", filename );
    }
    else
    {
        printf( "Failed to write configuration to '%s'\n", filename );
    }
}

/*============================================================================================*/
// clang-format on
// clang-format off
