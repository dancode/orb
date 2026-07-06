// clang-format off
/*==============================================================================================

    cvar_config.c - Cvar Configuration File Management

    - cvar_write_config   : Writes all CVAR_ARCHIVE variables to a file.

    Config EXECUTION has no bespoke parser: the "exec" command (cmd.c) queues raw file text
    into the command buffer, so every registered command works inside config files.

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
        con_printf( "config: could not write to '%s'\n", filename );
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
    con_printf( "cvar: %u cvars written to %s\n", written, filename );

    return true;
}

// clang-format on

/*============================================================================================*/
/* Load default config sequence -- default.cfg -> config.cfg -> autoexec.cfg.  Queued, not
   executed: the files run through the command buffer on the next pump, in order (each exec
   inserts its file's text ahead of the next queued exec).  Missing files are reported by
   the exec command itself. */

void
cvar_load_defaults( void )
{
    cmd_queue( "exec default.cfg" );
    cmd_queue( "exec config.cfg" );
    cmd_queue( "exec autoexec.cfg" );
}

/*============================================================================================*/
/* Save user config -- Writes all archived cvars to config.cfg */

void
cvar_save_config( void )
{
    con_printf( "Saving configuration...\n" );
    cvar_write_config( "config.cfg", CVAR_ARCHIVE );
}


/*============================================================================================*/
