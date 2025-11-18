/*==============================================================================================

    Module

==============================================================================================*/

#include "orb.h"
#include "base.h"

/*============================================================================================*/
#if PLATFORM_WINDOWS

// #include <windows.h>

typedef void*         HMODULE;
typedef const char*   LPCSTR;
typedef void*         FARPROC;
typedef unsigned long DWORD;
typedef int           BOOL;

__declspec( dllimport ) HMODULE __stdcall LoadLibraryA( LPCSTR lpLibFileName );
__declspec( dllimport ) FARPROC __stdcall GetProcAddress( HMODULE hModule, LPCSTR lpProcName );
__declspec( dllimport ) DWORD __stdcall GetModuleFileNameA( HMODULE hModule, char* lpFilename, DWORD nSize );
__declspec( dllimport ) BOOL __stdcall FreeLibrary( HMODULE hModule );

#else

#    include <dlfcn.h>
#    include <unistd.h>
typedef void* HMODULE;
typedef void* FARPROC;

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