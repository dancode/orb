/*==============================================================================================

    base/fmt.c -- the single engine-wide stb_sprintf implementation, compiled into base.lib.

    Consumers include base/fmt.h (declarations + the fmt_ aliases) and link base; nothing else
    ever defines STB_SPRINTF_IMPLEMENTATION.  Included by base.c (the base unity unit).

    The vendored file is not warning-clean at this project's /W4 (/WX) / -Wall -Werror levels,
    so the implementation include is bracketed in suppressions.

==============================================================================================*/

#define STB_SPRINTF_IMPLEMENTATION

#if defined( _MSC_VER )
    #pragma warning( push, 0 )
#elif defined( __GNUC__ ) || defined( __clang__ )
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "vendor/stb_sprintf.h"

#if defined( _MSC_VER )
    #pragma warning( pop )
#elif defined( __GNUC__ ) || defined( __clang__ )
    #pragma GCC diagnostic pop
#endif

/*============================================================================================*/
