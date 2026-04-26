#ifndef MODULE_SYS_API_H
#define MODULE_SYS_API_H
/*==============================================================================================

    module_get_api.h

    Minimal module-system interface passed into every module's init() call.

    This is the ONLY module-system symbol a DLL ever touches — it never links
    against the exe.  At runtime the system passes a pointer to the single
    static instance of this struct so the module can resolve any registered API.

    Usage inside a DLL:
        core_api_t*   core   = sys->get_api("core");
        engine_api_t* engine = sys->get_api("engine");
        render_api_t* render = sys->get_api("render");

==============================================================================================*/

// typedef void* ( *get_api_fn )( const char* name );

typedef struct module_sys_api_s
{
    /* Returns the exported API pointer for a named, initialized module.
       Returns NULL if the module is not found or not yet initialized.
       The caller casts the result to the expected typed struct pointer. */
    void* ( *get_api )( const char* name );

} module_sys_api_t;

/*============================================================================================*/
#endif    // MODULE_SYS_API_H