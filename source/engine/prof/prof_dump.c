/*==============================================================================================

    engine/prof/prof_dump.c - Chrome-trace JSON dump consumer.

    Writes the classic Chrome trace event format ({"traceEvents":[...]}) -- loadable in
    chrome://tracing and https://ui.perfetto.dev. BEGIN/END events map 1:1 onto "B"/"E"
    phases (already time-ordered per ring, which is exactly the per-tid ordering the
    format wants), frame marks become global instants, counters become "C" step samples,
    and ring labels land as thread_name metadata at dump_end.

    Consumer-side only: while a dump is active it IS the single drain consumer -- nothing
    else may call prof_drain. All state is plain statics touched by that one thread.

    Included by prof.c (unity) after the core implementation: reads the file-scope ring
    and counter tables directly.

==============================================================================================*/

// clang-format off

#define PROF_DUMP_CHUNK  512    /* events per drain chunk (8 KB scratch)               */
#define PROF_DUMP_STACK  32     /* tracked zone depth per thread for "E" event names   */

typedef struct prof_dump_s
{
    FILE* file;                                            // open trace file; NULL = idle
    bool  first;                                           // no comma before the first event
    u32   depth[ PROF_MAX_THREADS ];                       // live zone depth per ring
    u32   stack[ PROF_MAX_THREADS ][ PROF_DUMP_STACK ];    // zone ids, for names on "E"

} prof_dump_t;

static prof_dump_t  g_prof_dump;
static prof_event_t g_prof_dump_buf[ PROF_DUMP_CHUNK ];

// clang-format on

/* Comma-separate events; the first one just opens the line. */
static void
prof_dump_sep( void )
{
    if ( g_prof_dump.first )
    {
        fputc( '\n', g_prof_dump.file );
        g_prof_dump.first = false;
    }
    else
        fputs( ",\n", g_prof_dump.file );
}

/* Write a zone/counter name as a JSON string body -- escapes the two structural
   characters and squashes control bytes; names are engine-authored ASCII literals, so
   this is defensive, not a full JSON escaper. Unregistered ids print as zone_<hash>. */
static void
prof_dump_name( u32 id )
{
    FILE*       f    = g_prof_dump.file;
    const char* name = prof_name_lookup( id );

    if ( !name )
    {
        fprintf( f, "zone_%08x", id );
        return;
    }

    for ( const char* c = name; *c; ++c )
    {
        if ( *c == '"' || *c == '\\' )
            fputc( '\\', f );
        fputc( ( unsigned char )*c < 32 ? ' ' : *c, f );
    }
}

/* One drained event -> one trace line. tid is the ring index; ts is microseconds. */
static void
prof_dump_event( u32 tid, const prof_event_t* e )
{
    FILE* f  = g_prof_dump.file;
    f64   ts = ( f64 )e->tick_ns / 1000.0;

    switch ( e->type )
    {
        case PROF_EV_BEGIN:
        {
            prof_dump_sep();
            fprintf( f, "{\"ph\":\"B\",\"pid\":0,\"tid\":%u,\"ts\":%.3f,\"name\":\"", tid, ts );
            prof_dump_name( e->id );
            fputs( "\"}", f );

            u32 d = g_prof_dump.depth[ tid ]++;
            if ( d < PROF_DUMP_STACK )
                g_prof_dump.stack[ tid ][ d ] = e->id;
            break;
        }

        case PROF_EV_END:
        {
            /* Name the close from the tracked stack when we have it -- viewers accept a
               bare "E", but matched names survive partial captures better. An END with
               no tracked BEGIN (overflow orphan, capture started mid-zone) goes bare. */
            u32 d = g_prof_dump.depth[ tid ];

            prof_dump_sep();
            if ( d > 0 && d <= PROF_DUMP_STACK )
            {
                fprintf( f, "{\"ph\":\"E\",\"pid\":0,\"tid\":%u,\"ts\":%.3f,\"name\":\"", tid, ts );
                prof_dump_name( g_prof_dump.stack[ tid ][ d - 1 ] );
                fputs( "\"}", f );
            }
            else
                fprintf( f, "{\"ph\":\"E\",\"pid\":0,\"tid\":%u,\"ts\":%.3f}", tid, ts );

            if ( d > 0 )
                g_prof_dump.depth[ tid ] = d - 1;
            break;
        }

        case PROF_EV_FRAME:
        {
            prof_dump_sep();
            fprintf( f,
                     "{\"ph\":\"i\",\"pid\":0,\"tid\":%u,\"ts\":%.3f,\"name\":\"frame\","
                     "\"s\":\"g\",\"args\":{\"n\":%u}}",
                     tid, ts, e->id );
            break;
        }

        default:
            break;
    }
}

/*==============================================================================================
    Public surface
==============================================================================================*/

bool
prof_dump_begin( const char* path )
{
    prof_ensure_init();

    if ( g_prof_dump.file )
        return false;    /* one capture at a time */

    FILE* f = fopen( path && path[ 0 ] ? path : "prof_dump.json", "wb" );
    if ( !f )
        return false;

    fputs( "{\"traceEvents\":[", f );

    memset( &g_prof_dump, 0, sizeof( g_prof_dump ) );
    g_prof_dump.file  = f;
    g_prof_dump.first = true;
    return true;
}

/* Drain every ring into the trace, then sample the counters as "C" steps. Returns the
   number of trace events written this flush. */
u32
prof_dump_flush( void )
{
    if ( !g_prof_dump.file )
        return 0;

    u32 written = 0;
    u32 threads = prof_thread_count();

    for ( u32 t = 0; t < threads; ++t )
    {
        u32 n;
        while ( ( n = prof_drain( t, g_prof_dump_buf, PROF_DUMP_CHUNK ) ) != 0 )
        {
            for ( u32 i = 0; i < n; ++i )
                prof_dump_event( t, &g_prof_dump_buf[ i ] );
            written += n;
        }
    }

    prof_counter_t snap[ PROF_MAX_COUNTERS ];
    u32            cn = prof_counters( snap, PROF_MAX_COUNTERS );
    if ( cn )
    {
        f64 now = ( f64 )sys_tick_nanoseconds() / 1000.0;
        for ( u32 i = 0; i < cn; ++i )
        {
            prof_dump_sep();
            fprintf( g_prof_dump.file, "{\"ph\":\"C\",\"pid\":0,\"tid\":0,\"ts\":%.3f,\"name\":\"", now );
            prof_dump_name( snap[ i ].id );
            fprintf( g_prof_dump.file, "\",\"args\":{\"value\":%lld}}", ( long long )snap[ i ].value );
        }
        written += cn;
    }

    return written;
}

void
prof_dump_end( void )
{
    if ( !g_prof_dump.file )
        return;

    prof_dump_flush();    /* final drain */

    /* Thread metadata last -- labels can change any time before this (threads claim and
       name rings mid-capture); viewers apply "M" records regardless of position. */
    u32 threads = prof_thread_count();
    for ( u32 t = 0; t < threads; ++t )
    {
        const char* label = prof_thread_label( t );
        if ( !label || !label[ 0 ] )
            continue;
        prof_dump_sep();
        fprintf( g_prof_dump.file,
                 "{\"ph\":\"M\",\"pid\":0,\"tid\":%u,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}",
                 t, label );
    }

    fputs( "\n]}\n", g_prof_dump.file );
    fclose( g_prof_dump.file );
    memset( &g_prof_dump, 0, sizeof( g_prof_dump ) );
}

bool
prof_dump_active( void )
{
    return g_prof_dump.file != NULL;
}

/*============================================================================================*/
