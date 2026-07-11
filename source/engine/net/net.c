/*==============================================================================================

    net.c -- Unity build entry for the net module.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/mod/mod_export.h"
#include "engine/sys/sys_host.h"    /* direct socket + tick calls; net is always host-static */
#include "engine/net/net_host.h"
#include "engine/net/net_internal.h"

/*==============================================================================================
    Unity build  (dependency order: primitives -> channels -> connections -> peer)
==============================================================================================*/

#include "engine/net/net_packet.c"
#include "engine/net/net_channel.c"
#include "engine/net/net_conn.c"
#include "engine/net/net_peer.c"

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef NET_API_C_PRELUDE
#include "engine/net/net_api.c"
#endif

/*============================================================================================*/
