#pragma once
/*
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
5. runtime host for exe / editor / tool

     — picks which layers to include 
     — owns main(), the game loop, startup sequence

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
4. APP: platform app window, raw input, gpu surface

     — depends on : base, platform sys, core
     — registered as a service at runtime
     
     platform app — registered as a service after core. The actual window, input, GPU surface.

    app/

        app_api.h    app_api_t
        win32_window.c
        win32_input.c
        win32_gpu_surface.c
        linux_*.c
        app_module.c  module_api_t wrapper

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 
3. MOD: module system registry, hot-reload, dep sort

     — depends on : base, platform sys only
     — promoted to core alloc / log after core inits

    module system — built on base and platform sys. 
    ** Peer to core, not inside it **

    module_sys/

        module_api.h       contract every module implements
        module_sys_api.h   sys->get_api() handle passed into init()
        module_sys.h       public interface
        module_sys.c       implementation (uses sys directly)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
3. CORE: sid, log, alloc, cvar, assert 

    — depends on : base, platform sys only
    — registered as first service

    registered as the first service. Built on base and platform sys.

    core/

        core_api.h     core_api_t — the exported typed struct
        core.h         single include for internal use
        sid.h / .c     interned string ids (uses arena from base)
        log.h / .c     logging (uses sys file_io for output)
        alloc.h / .c   allocators built on top of arena
        cvar.h / .c    config vars
        core_module.c  module_api_t wrapper, service_register() call

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 
2. SYS: platform sys dll load, file watch, clock, file io, thread primitives

     — depends on : base only
     — no registration, no module api
     — statically linked into whatever needs it

    statically compiled, two or three .c files per platform. 
    No module API, no registration, no logging through core 
    (uses printf directly if it must log during bootstrap).

    sys/

        sys.h     single include — all functions below

        library.h          library_load, library_unload, library_get_symbol
        library_win32.c
        library_linux.c

        file_watch.h       file_watch_init, file_watch_poll, file_watch_shutdown
        file_watch_win32.c
        file_watch_linux.c

        clock.h            clock_now_ms, clock_now_ns
        clock_win32.c
        clock_linux.c

        file_io.h          sync file read/write, path utils, dir walk
        file_io_win32.c
        file_io_linux.c

        thread.h           thread, mutex, semaphore, atomic primitives
        thread_win32.c
        thread_linux.c

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 
1. STD: base types, math, strings, arenas, containers, compiler macros

     — zero dependencies
     — unity built into every translation unit
     - unity build, no .c files, pure headers.

    base/

    types.h        u8, u32, bool, byte, size_t aliases
    macros.h       assert, unused, likely, force_inline
    math.h         vec2/3/4, mat4, quat, scalar utils
    str.h          string view, small string, c-string utils
    arena.h        arena allocator, scratch arenas
    containers.h   array, hashmap, ring buffer, pool
    base.h         single include that pulls all of the above

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/