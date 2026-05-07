/*==============================================================================================

    win library.c : dynamic library loading

==============================================================================================*/
#if OS_WINDOWS

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

#    include <dlfcn.h>
#    include <unistd.h>
typedef void* HMODULE;
typedef void* FARPROC;

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