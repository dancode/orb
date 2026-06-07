/*==============================================================================================

    engine/core/core_log.c — Logging subsystem.

    Single ring buffer + small sink list. Single-writer assumption on log_write.
    The ring provides the read path for a future in-engine log console.

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
    log_level_t filter = ( level == LOG_LEVEL_LINE ) ? LOG_LEVEL_INFO : level;
    if ( filter < g_min_level )
        return;

    /* Format into a large scratch buffer; emit in msg[]-sized chunks if it overflows. */

    char full[ 4096 ];

    va_list ap; 
    va_start( ap, fmt );
    int full_len = vsnprintf( full, sizeof( full ), fmt, ap );
    va_end( ap );

    if ( full_len < 0 )     full_len = 0;
    if ( full_len >= 4096 ) full_len = 4096 - 1;
 
    const int chunk_max = (int)sizeof( g_ring[ 0 ].msg ) - 1;
    int offset = 0;
    do
    {
        u32 seq            = g_ring_seq++;
        log_entry_t* entry = &g_ring[ seq & ( LOG_RING_CAPACITY - 1 ) ];

        entry->seq     = seq;
        entry->level   = level;
        entry->channel = ( offset == 0 ) ? ( channel ? channel : "?" ) : NULL;

        int  to_copy = full_len - offset;
        if ( to_copy > chunk_max ) to_copy = chunk_max;
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
    g_min_level  = LOG_LEVEL_INFO;
    g_ring_seq   = 0;
    g_sink_count = 0;
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
