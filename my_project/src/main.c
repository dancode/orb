#include <stdio.h>
#include "orb.h"

#include "engine/sys/sys.h"
#include "engine/sys/sys_api.h"

f64 sys_tick_seconds( void );

int main( void )
{
    printf( "my_project: hello from a standalone ORB project\n" );

    f64 second = sys_tick_seconds();

    UNUSED( second ); 
    return 0;
}
