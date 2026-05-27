/*==============================================================================================

    core.h

==============================================================================================*/
#pragma once
#include "orb.h"

// #include "cvar/cvar.h"
#include "sid/sid.h"

/*==============================================================================================

    engine/core/core_log — Logging subsystem types.

    Five severity levels. Compile-time stripping via LOG_COMPILE_MIN (see log.h).
    Entries flow through a fixed ring buffer and a small sink list.

==============================================================================================*/

RS_ENUM( tooltip = "Severity level for a log message." )
typedef enum log_level_e
{
    LOG_LEVEL_TRACE = ORB_LOG_TRACE,    // per-frame spam; stripped in release by default
    LOG_LEVEL_DEBUG = ORB_LOG_DEBUG,    // development diagnostics
    LOG_LEVEL_INFO  = ORB_LOG_INFO,     // significant one-time events
    LOG_LEVEL_WARN  = ORB_LOG_WARN,     // recoverable issues
    LOG_LEVEL_ERROR = ORB_LOG_ERROR,    // non-fatal errors

} log_level_t;

#define LOG_LEVEL_COUNT   5
#define LOG_RING_CAPACITY 4096          // ring slot count; must be a power of 2

/* Single log record written into the ring buffer and passed to every sink. */

typedef struct log_entry_s      /* 256 bytes */
{
    u32          seq;           /* monotonic write counter */
    log_level_t  level;         /* severity level logged */
    const char*  channel;       /* points to a static string; always valid */
    char         msg[ 240 ];    /* pre-formatted, null-terminated */

} log_entry_t;

/* Sink callback — called once per log_write that passes the runtime level filter. */
typedef void ( *log_sink_fn )( const log_entry_t* entry, void* userdata );

/*==============================================================================================

    engine/core/core_memory.c

==============================================================================================*/

RS_ENUM( tooltip = "Built-in memory tag IDs reserved by the engine." )
typedef enum memtag_kind_e
{
    MEMTAG_UNKNOWN = 0,    // unknown / untagged
    MEMTAG_CORE,           // core engine systems
    MEMTAG_ENGINE,         // engine systems
    MEMTAG_COUNT           // total number of built-in tags

} memtag_kind_t;

typedef uint16_t memtag_t;    // runtime-generated tag ID

/*============================================================================================*/
