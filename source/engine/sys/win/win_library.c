/*==============================================================================================

    win library.c : dynamic library loading

==============================================================================================*/
#if OS_WINDOWS

lib_handle_t
sys_library_load( const char* path )
{
    return ( void* )LoadLibraryA( path );
}

void*
sys_library_get_symbol( lib_handle_t module, const char* name )
{
    return ( void* )GetProcAddress( ( HMODULE )module, name );
}

void
sys_library_unload( lib_handle_t module )
{
    if ( module )
        FreeLibrary( ( HMODULE )module );
}

#else

    #include <dlfcn.h>
    #include <unistd.h>
typedef void* HMODULE;
typedef void* FARPROC;

lib_handle_t
sys_library_load( const char* path )
{
    return dlopen( path, RTLD_NOW );
}

void*
sys_library_get_symbol( lib_handle_t h, const char* s )
{
    return dlsym( h, s );
}

void
sys_library_unload( lib_handle_t module )
{
    dlclose( module );
}

#endif
/*============================================================================================*/