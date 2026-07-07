#ifndef INPUT_H
#define INPUT_H
/*==============================================================================================

    runtime_service/input/input.h -- Input action service, public types.

    The input service sits ABOVE core and app.  It turns raw sources (keys, mouse buttons,
    pad buttons -- and in later phases axes) into named ACTIONS a game reads by dense id:
    "did +attack fire this frame", "is +forward held", "what is the move vector".

    Layering: app supplies device state and posts key edges; core/cmd supplies the Quake
    digital transport ('bind w +forward' queues "+forward <key>" / "-forward <key>" text);
    this service registers those +name/-name commands, accumulates the edges, and latches
    them into a per-frame state block when the host calls input()->frame( dt ) AFTER
    cmd_pump().  That ordering is the contract: edges queued by this frame's binds resolve
    into this frame's pressed/released counts.

    Pure types only -- no function declarations, no vtable.  Callers include input_api.h
    (DLL modules) or input_host.h (host exes and sandboxes).

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define INPUT_ACTION_MAX      128    // max registered actions
#define INPUT_ACTION_NAME_LEN 24     // bytes per action name incl. NUL ("+name" must fit CMD_NAME_LEN)
#define INPUT_CONTEXT_MAX     8      // context stack depth
#define INPUT_HELD_MAX        4      // simultaneous sources holding one button action
#define INPUT_AXIS_BIND_MAX   64     // max axis bind table entries (bindaxis)

/*==============================================================================================
    Types
==============================================================================================*/

/* Dense action id, assigned at registration in first-come order and stable for the run of
   the process.  Registration is idempotent by name, so a hot-reloaded DLL re-registering
   its actions in reload() gets the same ids back.  INPUT_ACTION_INVALID = not found/full. */
typedef i32 input_action_t;

#define INPUT_ACTION_INVALID ( ( input_action_t )-1 )

/* What kind of value the action carries.  BUTTON is the digital kind and rides the cmd
   transport (+name/-name auto-registered).  AXIS1/AXIS2 reserve the analog kinds; their
   sources (bindaxis, mouse, sticks, digital composites) land in the next phase -- the
   state block and value queries already exist so game code written now does not change. */
typedef enum input_action_type_e
{
    INPUT_ACTION_BUTTON = 0,    // digital: down / pressed / released
    INPUT_ACTION_AXIS1,         // scalar: value in -1..1 (or 0..1 for triggers)
    INPUT_ACTION_AXIS2,         // vector: value2, e.g. move / look

} input_action_type_t;

/*============================================================================================*/
#endif    // INPUT_H
