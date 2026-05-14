/*==============================================================================================

    core.h

==============================================================================================*/
#pragma once
#include "orb.h"

// #include "cvar/cvar.h"
// #include "sid/sid.h"

/*==============================================================================================

    engine/core/core_log.h — Logging subsystem, public interface.

    Three severity levels, printf-style formatting, automatic newline.
    Info goes to stdout; warn and error to stderr. A min-level filter lets release
    builds suppress info traffic without touching call sites.

==============================================================================================*/

typedef enum log_level_e
{
    LOG_LEVEL_INFO  = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_ERROR = 2,

} log_level_t;

/*==============================================================================================

    engine/core/core_memory.c

==============================================================================================*/

enum
{
    MEMTAG_UNKNOWN = 0,    // unknown / untagged
    MEMTAG_CORE,           // core engine systems
    MEMTAG_ENGINE,         // engine systems
    MEMTAG_COUNT           // total number of built-in tags
};

typedef uint16_t memtag_t;    // runtime-generated tag ID

/*==============================================================================================

    Export the API struct and gateway macro for fetching the API.

==============================================================================================*/

#define CORE_DECLARED
#include "engine/core/core_api.h"

/*============================================================================================*/
