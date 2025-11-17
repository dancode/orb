/*==============================================================================================

    cvar_system.c
    
==============================================================================================*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "orb.h"

#include "cvar/string_pool.h"
#include "cvar/string_pool.c"

#include "cvar/cvar.h"

#include "cvar/cmd.c"
#include "cvar/cmd_buffer.c"
#include "cvar/cmd_parse.c"

#include "cvar/cvar.c"
#include "cvar/cvar_cmd.c"
#include "cvar/cvar_config.c"

#include "test/test_core_cvar.c"

/*============================================================================================*/