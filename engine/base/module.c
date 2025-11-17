/*==============================================================================================

    Module

==============================================================================================*/

#include "orb.h"
#include "base.h"

/*============================================================================================*/

#if PLATFORM_WINDOWS
#include <windows.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

/*============================================================================================*/

#if PLATFORM_WINDOWS

lib_handle_t
library_load( const char* path )
{
    return LoadLibraryA( path );
}
void*
library_get_symbol( lib_handle_t h, const char* s )
{
    return (void*)GetProcAddress( h, s );
}
int
library_unload( lib_handle_t module )
{
    return FreeLibrary( module );
}

#else

lib_handle_t
library_load( const char* path )
{
    return dlopen( path, RTLD_NOW );
}
void*
library_get_symbol( lib_handle_t h, const char* s )
{
    return dlsym( h, s );
}
int
library_unload( lib_handle_t module )
{
    return dlclose( module );
}

#endif
/*============================================================================================*/
