#ifndef EXAMPLE_API_H
#define EXAMPLE_API_H
/*==============================================================================================

    renderer_api.h

    Public API exported by the renderer module (typically a hot-reloadable DLL).

    Consumers include this header and call renderer_api()->begin_frame() etc.
    The call cost is:
        BUILD_STATIC: direct address load (LTO-devirtualizable)
        dynamic:      one pointer load from g_renderer_api_ptr (cached in consumer's init)

    Consumer usage — call site is identical in all build variants:
        example_api()->example_function_1();
        example_api()->example_function_2( 42 );

    How the gateway resolves per build:
        BUILD_STATIC:
            example_api() → &g_example_api_struct       (direct address, LTO-devirtualizable)
        dynamic:
            example_api() → g_example_api_ptr           (one pointer load, cached in init)

==============================================================================================*/

#include "module/module_api.h"

/*==============================================================================================
    Renderer API struct
==============================================================================================*/

typedef struct example_api_s
{
    void ( *example_function_1 )( void );
    void ( *example_function_2 )( int value );

} example_api_t;

/*============================================================================================*/

MODULE_GATEWAY( example_api_t, example );

/* expands to:

    extern const example_api_t* g_example_api_ptr;

    static inline const example_api_t*
    example_api( void )
    {
        return g_example_api_ptr;
    }

*/

/*============================================================================================*/
#endif    // EXAMPLE_API_H
