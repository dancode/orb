/*==============================================================================================

    platform.h

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

typedef void ( *platform_get_tick_fn )( void );


// The struct passed to every module
typedef struct platform_api_t
{
    /* platform api */
    platform_get_tick_fn func;


} core_api_t;

// engine.module.get_api()
// engine.module.reg_api()

// engine.get_time()
// engine.get_tick()


// engine.mem.alloc()
// engine.log.print()
// engine.var.register()
// engine.job.create()
// engine.fs.load_file()
// engine.render.draw_mesh()
// engine.input.is_key_down(


/*============================================================================================*/