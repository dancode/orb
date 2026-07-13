/*==============================================================================================

    engine/core/core_log.c — Logging subsystem.

    Single ring buffer + small sink list + per-channel filter registry.  Single-writer
    assumption on log_write.  The ring provides the read path for a future in-engine
    log console.

==============================================================================================*/

#define LOG_MAX_SINKS 4

// clang-format off
/*==============================================================================================
    State
==============================================================================================*/

typedef struct
{
    log_sink_fn         fn;             // called on every log_write that passes level filter
    void*               userdata;       // passed to fn; can be used for stateful sinks 
                                        // (e.g. file handle, network connection)
} log_sink_slot_t;

/* current mutable logging level */

static log_level_t      g_min_level = LOG_LEVEL_INFO;

/* monotonic sequence number for log entries; 
   used to compute ring slot index and detect dropped entries */

static u32              g_ring_seq = 0;

/* fixed-size ring buffer of recent log entries;
   indexed by seq % LOG_RING_CAPACITY */

static log_entry_t      g_ring[ LOG_RING_CAPACITY ];

/* simple fixed-size sink list; no duplicates, no ordering guarantees */

static log_sink_slot_t  g_sinks[ LOG_MAX_SINKS ];
static int              g_sink_count = 0;

/*==============================================================================================

    Channel registry

    Channels auto-register on the first log_write that names them (or via log_channel_set
    before any write, so configs can tune a channel that has not logged yet).  Names are
    COPIED into this table: ring entries and sinks point at the canonical copy, so scrollback
    read later stays valid after the hot-reloaded DLL that supplied the name string unloads.
    Same single-writer assumption as log_write.

==============================================================================================*/

typedef struct
{
    char    name[ LOG_CHANNEL_NAME_LEN ];   // canonical copy; ring entries point here
    u32     hash;                           // case-folded fnv1a of the stored name
    u8      override;                       // log_level_t filter override, or LOG_LEVEL_INHERIT

} log_channel_t;

static log_channel_t    g_channels[ LOG_MAX_CHANNELS ];
static u32              g_channel_count = 0;

/* Case-folded fnv1a over at most LOG_CHANNEL_NAME_LEN-1 chars -- capped to match the stored
   (possibly truncated) copy so an over-long name still finds its own slot. */

static u32
log_channel_hash( const char* s )
{
    u32 h = 2166136261u;
    for ( u32 i = 0; s[ i ] && i < LOG_CHANNEL_NAME_LEN - 1; i++ )
    {
        u32 c = ( u8 )s[ i ];
        if ( c >= 'A' && c <= 'Z' )
            c += 32;
        h = ( h ^ c ) * 16777619u;
    }
    return h;
}

/* Case-insensitive equality, bounded like the hash (console conventions match cvar lookup). */

static bool
log_channel_name_eq( const char* a, const char* b )
{
    for ( u32 i = 0; i < LOG_CHANNEL_NAME_LEN - 1; i++ )
    {
        u32 ca = ( u8 )a[ i ];
        u32 cb = ( u8 )b[ i ];
        if ( ca >= 'A' && ca <= 'Z' ) ca += 32;
        if ( cb >= 'A' && cb <= 'Z' ) cb += 32;
        if ( ca != cb )
            return false;
        if ( ca == 0 )
            return true;
    }
    return true;    /* both identical up to the storage bound */
}

static log_channel_t*
log_channel_find( const char* name )
{
    u32 hash = log_channel_hash( name );
    for ( u32 i = 0; i < g_channel_count; i++ )
    {
        if ( g_channels[ i ].hash == hash && log_channel_name_eq( g_channels[ i ].name, name ) )
            return &g_channels[ i ];
    }
    return NULL;
}

static log_channel_t*
log_channel_intern( const char* name )
{
    log_channel_t* ch = log_channel_find( name );
    if ( ch )
        return ch;
    if ( g_channel_count >= LOG_MAX_CHANNELS )
        return NULL;    /* table full: caller falls back to the global floor */

    ch = &g_channels[ g_channel_count++ ];

    u32 i = 0;
    for ( ; name[ i ] && i < LOG_CHANNEL_NAME_LEN - 1; i++ )
        ch->name[ i ] = name[ i ];
    ch->name[ i ] = '\0';

    ch->hash     = log_channel_hash( ch->name );
    ch->override = LOG_LEVEL_INHERIT;
    return ch;
}

/*==============================================================================================
    Console sink
==============================================================================================*/

static const char* s_prefixes[ LOG_LEVEL_COUNT ] = {
    "[trace] ", "[debug] ", "[info ] ", "[warn ] ", "[error] ", "[fatal] "
};

static const char s_separator[] = "------------------------------------------------";

static void
log_console_sink( const log_entry_t* entry, void* userdata )
{
    UNUSED( userdata );
    if ( entry->level == LOG_LEVEL_LINE )
    {
        fprintf( stdout, "%s\n", s_separator );
        return;
    }
    if ( (int)entry->level >= LOG_LEVEL_COUNT ) /* guard: level must index s_prefixes[] */
        return;

    FILE* stream = ( entry->level >= LOG_LEVEL_WARN ) ? stderr : stdout;
    if ( entry->channel == NULL ) /* overflow continuation: no prefix */
    {
        fprintf( stream, "%s\n", entry->msg );
        return;
    }
    fprintf( stream, "%s[%s] %s\n", s_prefixes[ entry->level ], entry->channel, entry->msg );
}

/*==============================================================================================
    Sink management
==============================================================================================*/

static void
log_add_sink( log_sink_fn fn, void* userdata )
{
    if ( g_sink_count >= LOG_MAX_SINKS )
        return;
    g_sinks[ g_sink_count ].fn       = fn;
    g_sinks[ g_sink_count ].userdata = userdata;
    g_sink_count++;
}

static void
log_remove_sink( log_sink_fn fn )
{
    for ( int i = 0; i < g_sink_count; i++ )
    {
        if ( g_sinks[ i ].fn == fn )
        {
            g_sinks[ i ] = g_sinks[ --g_sink_count ];
            return;
        }
    }
}

/*==============================================================================================
    Configuration
==============================================================================================*/

static void
log_set_min_level( log_level_t level )
{
    g_min_level = level;
}

/* Set one channel's override: LOG_LEVEL_TRACE..ERROR, LOG_LEVEL_OFF (mute all but FATAL),
   or LOG_LEVEL_INHERIT to clear.  Creates the channel if it has not logged yet. */

static void
log_channel_set( const char* name, log_level_t level )
{
    if ( !name || !name[ 0 ] )
        return;
    if ( level > LOG_LEVEL_ERROR && level != LOG_LEVEL_OFF && level != LOG_LEVEL_INHERIT )
        return;

    log_channel_t* ch = log_channel_intern( name );
    if ( ch )
        ch->override = ( u8 )level;
}

/* Clear one channel's override back to inherit; NULL clears every channel. */

static void
log_channel_reset( const char* name )
{
    if ( !name )
    {
        for ( u32 i = 0; i < g_channel_count; i++ )
            g_channels[ i ].override = LOG_LEVEL_INHERIT;
        return;
    }
    log_channel_t* ch = log_channel_find( name );
    if ( ch )
        ch->override = LOG_LEVEL_INHERIT;
}

/*==============================================================================================
    Channel enumeration  (read path for the `log` command and editor/tools)
==============================================================================================*/

static u32
log_channel_count( void )
{
    return g_channel_count;
}

static bool
log_channel_get( u32 index, const char** name, log_level_t* override_level, log_level_t* effective )
{
    if ( index >= g_channel_count )
        return false;

    const log_channel_t* ch = &g_channels[ index ];
    if ( name )
        *name = ch->name;
    if ( override_level )
        *override_level = ( log_level_t )ch->override;
    if ( effective )
        *effective = ( ch->override != LOG_LEVEL_INHERIT ) ? ( log_level_t )ch->override
                                                           : g_min_level;
    return true;
}

/*==============================================================================================
    Ring buffer access  (read path for editor/tools)
==============================================================================================*/

static const log_entry_t*
log_ring_entries( void )
{
    return g_ring;
}

static u32
log_ring_capacity( void )
{
    return LOG_RING_CAPACITY;
}

static u32
log_ring_seq( void )
{
    return g_ring_seq;
}

/*==============================================================================================
    Core write path
==============================================================================================*/

static void
log_write( log_level_t level, const char* channel, const char* fmt, ... )
{
    /* Intern even when filtered so muted channels still show up in `log list`. */
    log_channel_t* ch = log_channel_intern( channel ? channel : "?" );

    /* FATAL bypasses all filtering: it must reach the sinks and must terminate. */
    if ( level != LOG_LEVEL_FATAL )
    {
        log_level_t filter = ( level == LOG_LEVEL_LINE ) ? LOG_LEVEL_INFO : level;
        log_level_t min    = ( ch && ch->override != LOG_LEVEL_INHERIT )
                                 ? ( log_level_t )ch->override
                                 : g_min_level;
        if ( filter < min )
            return;
    }

    /* Format into a large scratch buffer; emit in msg[]-sized chunks if it overflows. */

    char full[ 4096 ];

    va_list ap; 
    va_start( ap, fmt );
    u32 full_len = vsnprintf( full, sizeof( full ), fmt, ap );
    va_end( ap );

    if ( full_len < 0 )     full_len = 0;
    if ( full_len >= 4096 ) full_len = 4096 - 1;
 
    u32 offset = 0;
    do
    {
        u32 seq            = g_ring_seq++;
        log_entry_t* entry = &g_ring[ seq & ( LOG_RING_CAPACITY - 1 ) ];

        entry->seq     = seq;
        entry->level   = level;
        entry->channel = ( offset == 0 ) ? ( ch ? ch->name : ( channel ? channel : "?" ) ) : NULL;

        u32  to_copy = full_len - offset;
        if ( to_copy >= sizeof( entry->msg ))  
             to_copy  = sizeof( entry->msg ) - 1;

        memcpy( entry->msg, full + offset, (size_t)to_copy );
        entry->msg[ to_copy ] = '\0';
        offset += to_copy;

        for ( int i = 0; i < g_sink_count; i++ )
            g_sinks[ i ].fn( entry, g_sinks[ i ].userdata );
    }
    while ( offset < full_len );

    if ( level == LOG_LEVEL_FATAL )
    {
        assert( 0 );
        exit( 1 );
    }
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static void
log_init( void )
{
    g_min_level     = LOG_LEVEL_INFO;
    g_ring_seq      = 0;
    g_sink_count    = 0;
    g_channel_count = 0;
    log_add_sink( log_console_sink, NULL );
}

static void
log_exit( void )
{
    g_sink_count = 0;
}

/*==============================================================================================
    Log sink adapter : public

    log_fn_t-compatible bridge. Routes pre-formatted messages from sys/mod/app
    (which cannot call core() directly) into the core write path.
    Pass to mod_set_log_fn() and app_set_log_fn() after mod_init_all().
==============================================================================================*/

void
core_log_fn( int level, const char* tag, const char* msg )
{
    log_write( ( log_level_t )level, tag, "%s", msg );
}

// clang-format on
/*============================================================================================*/
