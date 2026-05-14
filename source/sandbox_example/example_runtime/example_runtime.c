/*==============================================================================================

    sandbox/sandbox_example/example_runtime/example_runtime.c — example runtime host.

    This is a starting point for new runtime hosts: A template used to test runtime 
    functionality such as services and modules.

    This should be able to test enigne modules, our runtime services, and any modules.

    Loop:  RT_LOOP_RUN
    Flags: RT_HOST_CONSOLE | RT_HOST_HOT_RELOAD
    
==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/mod/mod.h"

#include "runtime/host/host.h"

/*==============================================================================================
    Host callbacks
==============================================================================================*/

int
main( int argc, char** argv )
{
    // return rt_host_main( &k_desc, argc, argv );
}
