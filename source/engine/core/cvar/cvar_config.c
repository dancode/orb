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

    /* Log channel overrides persist alongside the cvars (log reset + log lines). */
    log_channel_write_config( f );

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
/* Hidden commands used only by cvar_load_defaults, below, to bracket a boot config file's
   queued text with its priority tier (CVAR_PRI_DEFAULT/CONFIG/AUTOEXEC) so every cvar it sets
   -- including through a nested "exec" the file itself runs -- is tagged with that tier. Not
   meant to be typed by a user; a plain "exec" run at the console or from within one of these
   files doesn't push, so it just inherits whatever tier is already ambient. Registered lazily
   so cvar_config.c stays self-contained -- no other init call site needs to know about it. */

static void
cvar_cmd_pri_push( int argc, char** argv )
{
    if ( argc < 2 )
        return;
    cvar_source_priority_push( ( cvar_priority_t )atoi( argv[ 1 ] ) );
}

static void
cvar_cmd_pri_pop( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    cvar_source_priority_pop();
}

/* Queue "exec <filename>" bracketed by a priority push/pop, appended after whatever's already
   pending (each call's block runs in order, once earlier blocks -- and whatever they exec --
   have finished). */

static void
cvar_queue_leveled_exec( cvar_priority_t priority, const char* filename )
{
    if ( !cmd_exists( "cvar_pri_push" ) )
    {
        cmd_register( "cvar_pri_push", cvar_cmd_pri_push, "(internal) push ambient cvar set priority" );
        cmd_register( "cvar_pri_pop",  cvar_cmd_pri_pop,  "(internal) pop ambient cvar set priority" );
    }

    char line[ 128 ];
    snprintf( line, sizeof( line ), "cvar_pri_push %d\nexec %s\ncvar_pri_pop\n", ( int )priority, filename );
    cmd_queue( line );
}

/*============================================================================================*/
/* Load default config sequence -- default.cfg -> config.cfg -> autoexec.cfg.  Queued, not
   executed: the files run through the command buffer on the next pump, in order (each exec
   inserts its file's text ahead of the next queued exec).  Missing files are reported by
   the exec command itself.  Each file's tier is pushed/popped around its exec so cvar sets
   inside it are guard-protected against a lower-tier file overwriting them later, regardless
   of pump ordering (see cvar_source_priority_push). */

void
cvar_load_defaults( void )
{
    cvar_queue_leveled_exec( CVAR_PRI_DEFAULT,  "default.cfg" );
    cvar_queue_leveled_exec( CVAR_PRI_CONFIG,   "config.cfg" );
    cvar_queue_leveled_exec( CVAR_PRI_AUTOEXEC, "autoexec.cfg" );
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
