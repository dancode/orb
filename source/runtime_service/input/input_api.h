#ifndef INPUT_API_H
#define INPUT_API_H
/*==============================================================================================

    runtime_service/input/input_api.h -- Input action service API struct and gateway macro.

    Include this in DLL .c files that call the input service through the vtable.  Host
    executables and sandboxes include input_host.h instead.

    Function groups (all called through the input() vtable):
        Actions  : action_register / action_find / action_count / action_name
        Frame    : frame (host loop only, after cmd_pump)
        State    : down / pressed / released / value / value2
        Contexts : context_push / context_pop / context_active

==============================================================================================*/

#include "runtime_service/input/input.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct input_api_s
{
    /* Register an action; name is copied.  Idempotent by name: re-registering returns the
       existing id (so DLL reload() just registers again), with a warning if the type
       differs.  BUTTON actions auto-register '+name' / '-name' console commands, which is
       what makes 'bind w +forward' work with zero further glue.  context_mask picks the
       contexts the action is live in (see context_push); 0 = live in ALL contexts.
       Returns INPUT_ACTION_INVALID when the table or name is full/invalid. */
    input_action_t ( *action_register )( const char* name, input_action_type_t type, u32 context_mask );

    /* Look up an action id by name; INPUT_ACTION_INVALID if unknown. */
    input_action_t ( *action_find )( const char* name );

    u32            ( *action_count )( void );
    const char*    ( *action_name  )( input_action_t action );    /* NULL for invalid ids */

    /* Latch this frame's action state.  The HOST calls this once per frame, AFTER
       cmd_pump(), so +/- edges queued by this frame's binds land in this frame's counts.
       Also applies context gating: actions outside the active context are force-released
       (one released edge, then quiet).  dt is reserved for axis filtering (next phase). */
    void ( *frame )( f32 dt );

    /* Per-frame state, valid until the next frame() call.  All O(1) table reads.
       pressed/released are COUNTS, not flags -- a sub-frame tap (down+up between two
       frames) still reports pressed=1 released=1 with down=false, so no click is lost. */
    bool ( *down     )( input_action_t action );
    u32  ( *pressed  )( input_action_t action );
    u32  ( *released )( input_action_t action );

    /* Analog value (AXIS1 / AXIS2 actions; BUTTON reads as 0/1 on x).  Sources land in
       the next phase -- until then axes read 0. */
    f32  ( *value  )( input_action_t action );
    void ( *value2 )( input_action_t action, f32* out_x, f32* out_y );

    /* Context stack (max INPUT_CONTEXT_MAX).  The ACTIVE mask is the top of the stack;
       an empty stack means everything is active.  Push the UI context when a menu opens
       and gameplay actions with a disjoint mask force-release; pop to restore.  Masks are
       caller-defined bits -- the service only tests (action.context_mask & active). */
    void ( *context_push   )( u32 mask );
    void ( *context_pop    )( void );
    u32  ( *context_active )( void );

} input_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( INPUT_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
MOD_GATEWAY_STATIC( input_api_t, input )
    #define MOD_USE_INPUT     /* static: gateway returns pointer to global struct directly */
    #define MOD_FETCH_INPUT   true
#else
MOD_GATEWAY_DYNAMIC( input_api_t, input )
    #define MOD_USE_INPUT     MOD_DEFINE_API_PTR( input_api_t, input )
    #define MOD_FETCH_INPUT   MOD_FETCH_API( input_api_t, input )
#endif

// clang-format on
/*============================================================================================*/
#endif    // INPUT_API_H
