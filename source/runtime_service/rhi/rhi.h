/*==============================================================================================

    runtime_service/rhi/rhi.h — Render Hardware Interface, public types and handles.

==============================================================================================*/
#ifndef RHI_H
#define RHI_H

#include "orb.h"

/*==============================================================================================
    Opaque handles  (defined privately inside the implementation)
==============================================================================================*/

typedef struct rhi_command_list_s* rhi_command_list_t;

/*==============================================================================================
    Render context pool

    Each render context wraps one platform window.  Contexts are identified by
    an i32 id; RHI_CTX_INVALID (-1) signals failure or unset.
    Up to RHI_CTX_MAX contexts may be live simultaneously.
==============================================================================================*/

#define RHI_CTX_INVALID  ( -1 )
#define RHI_CTX_MAX      4        /* matches APP_WIN_MAX */

/*==============================================================================================
    Value types
==============================================================================================*/

typedef struct rhi_color_s
{
    f32 r, g, b, a;

} rhi_color_t;

/*============================================================================================*/
#endif    // RHI_H
