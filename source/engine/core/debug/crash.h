/*==============================================================================================

    engine/core/debug/crash.h

    Crash reporting policy over the sys crash mechanism (sys_crash_* in sys_host.h).

    On a fatal unhandled exception the handler writes, next to each other:
        <dir>/crash_YYYYMMDD_HHMMSS_<pid>.dmp   -- minidump (open in Visual Studio)
        <dir>/crash_YYYYMMDD_HHMMSS_<pid>.txt   -- symbolized backtrace report
    and mirrors the report to stderr and the debugger output pane, then lets the
    process die (no WER dialog). Host-only; DLL modules crash into the host's handler.

    Included by core_host.h. Do not include directly.

==============================================================================================*/
#pragma once

#ifndef CORE_HOST_H
    #error "crash.h must not be included directly; include core_host.h"
#endif

//   Install the process crash handler. `report_dir` is where .dmp/.txt land;
//   NULL = executable directory. Call once, early in host boot.
void core_crash_install( const char* report_dir );

//   Non-fatal snapshot: write the same .dmp/.txt pair for the CURRENT callstack and
//   continue running. For "this should never happen but did" telemetry.
void core_crash_report_now( const char* reason );

/*============================================================================================*/
