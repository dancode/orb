// clang-format off
/*==============================================================================================

    cvar_config.c - Cvar Configuration File Management

    - cvar_write_config   : Writes all CVAR_ARCHIVE variables to a file.

    Config EXECUTION has no bespoke parser: the "exec" command (cmd.c) queues raw file text
    into the command buffer, so every registered command works inside config files.

==============================================================================================*/

/*============================================================================================*/
/* Extra config-section writers -- services (input axis binds, etc.) hook in here so one
   writeconfig round-trips everything.  Small fixed table; slots free on remove. */

#define CVAR_CONFIG_WRITER_MAX 8

static cvar_config_writer_fn s_config_writers[ CVAR_CONFIG_WRITER_MAX ];

bool
cvar_config_writer_add( cvar_config_writer_fn fn )
{
    if ( !fn )
        return false;

    for ( u32 i = 0; i < CVAR_CONFIG_WRITER_MAX; ++i )
        if ( s_config_writers[ i ] == fn )
            return true;    // already registered (service re-init)

    for ( u32 i = 0; i < CVAR_CONFIG_WRITER_MAX; ++i )
    {
        if ( !s_config_writers[ i ] )
        {
            s_config_writers[ i ] = fn;
            return true;
        }
    }
    return false;
}

void
cvar_config_writer_remove( cvar_config_writer_fn fn )
{
    for ( u32 i = 0; i < CVAR_CONFIG_WRITER_MAX; ++i )
        if ( s_config_writers[ i ] == fn )
            s_config_writers[ i ] = NULL;
}

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
        if ( cv->flags & CVAR_ROM )     continue;
        if ( cv->flags & CVAR_INIT )    continue;
        if ( cv->flags & CVAR_RUNTIME ) continue;
        if ( cv->flags & type_filter )
        {
            const char* name  = cvar_get_name( cv );
            const char* value = cvar_value_string( cv );
            fprintf( f, "seta %s \"", name );
            cmd_write_quoted( f, value );
            fprintf( f, "\"\n" );
            written++;
        }
    }

    /* Key binds persist alongside the cvars (unbindall + bind lines). */
    cmd_bind_write_config( f );

    /* Aliases persist alongside the cvars (alias lines). */
    cmd_alias_write_config( f );

    /* Service-registered sections (input axis binds, etc.). */
    for ( u32 i = 0; i < CVAR_CONFIG_WRITER_MAX; ++i )
        if ( s_config_writers[ i ] )
            s_config_writers[ i ]( f );

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
