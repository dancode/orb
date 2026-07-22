/*==============================================================================================

    runtime_service/gui/frame/gui_context.c -- The public multi-context lifecycle.

    Moved from core/gui_ctx.c at the R4 carve (GUI_SERVER_PLAN.md): creating and destroying a
    context is orchestrator work -- destruction tears down the context's GPU surfaces
    (viewport_destroy, a render-server call the interact server must never make).  The pool
    STORAGE and the allocation/bind verbs stay with the server (core/gui_ctx.c: s_ctx_pool,
    ctx_alloc_slot, ctx_bind); this file is the public policy over them.

    Included by gui.c in the frame group.

==============================================================================================*/
// clang-format off

/* Set whether a context listens for hover/click/nav input.  Call between frames.
   Multiple contexts may listen simultaneously; a deaf context renders but returns inert
   widget state.  The default context starts listening; secondary contexts start deaf. */
void
gui_ctx_set_listening( gui_ctx_id_t ctx, bool listen )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        s_ctx_pool[ ctx ]->listening = listen;
}

/* Allocate a fresh secondary context sized to `cfg` (NULL = the internal maxima).
   Each gets a unique id_salt so same-named widgets do not alias across contexts.
   Returns GUI_CTX_INVALID when the pool is full.  Call between frames. */
gui_ctx_id_t
gui_ctx_create( const gui_ctx_config_t* cfg )
{
    /* Resolve config: zero fields fall back to the internal caps.  max_dock_nodes == 0 in an
       EXPLICIT cfg is valid (disables docking); only a NULL cfg gets the dock default. */
    gui_ctx_config_t c = cfg ? *cfg
                             : ( gui_ctx_config_t ){ .max_dock_nodes = GUI_DEFAULT_DOCK_NODES };
    if ( !c.max_windows   ) c.max_windows   = GUI_DEFAULT_MAX_WINDOWS;
    if ( !c.state_slots   ) c.state_slots   = GUI_DEFAULT_STATE_SLOTS;
    if ( !c.popup_depth   ) c.popup_depth   = GUI_DEFAULT_POPUP_DEPTH;
    if ( !c.max_viewports ) c.max_viewports = GUI_MAX_VIEWPORTS;

    /* Counts are free of the old power-of-two rule (multiply-shift bucketing, gui_state.c);
       just floor so the small class (3/4 of this) keeps usable headroom. */
    u32 slots = c.state_slots;
    if ( slots < 16 ) slots = 16;

    /* Find a free pool slot (1..GUI_CTX_POOL_MAX-1). */
    i32 slot = -1;
    for ( i32 i = 1; i < GUI_CTX_POOL_MAX; ++i )
        if ( !s_ctx_pool[ i ] ) { slot = i; break; }
    if ( slot < 0 ) return GUI_CTX_INVALID;

    gui_context_t* ctx = ctx_alloc_slot( &c, slots, slot );
    if ( !ctx ) return GUI_CTX_INVALID;
    ctx->listening = false;   /* secondary contexts start deaf; caller opts in */

    s_ctx_pool[ slot ] = ctx;
    if ( (u32)slot >= s_ctx_pool_count ) s_ctx_pool_count = (u32)slot + 1u;
    return (gui_ctx_id_t)slot;
}

/* Free a secondary context; rebinds the default if this was current.  Never destroys slot 0. */
void
gui_ctx_destroy( gui_ctx_id_t ctx )
{
    if ( ctx <= 0 || ctx >= GUI_CTX_POOL_MAX || !s_ctx_pool[ ctx ] )
        return;
    gui_context_t* c = s_ctx_pool[ ctx ];
    if ( g_ctx == c ) ctx_bind( NULL );
    /* Destroy any GPU surfaces the context opened before releasing its memory block. */
    for ( u32 i = 0; i < c->vp.max; ++i )
        viewport_destroy( &c->vp.pool[ i ] );
    if ( c->_alloc ) free( c->_alloc );
    s_ctx_pool[ ctx ] = NULL;
}

/* Make ctx the current context.  GUI_CTX_DEFAULT (0) or an invalid handle rebinds the default. */
void
gui_ctx_bind( gui_ctx_id_t ctx )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        ctx_bind( s_ctx_pool[ ctx ] );
    else
        ctx_bind( NULL );
}

// clang-format on
/*============================================================================================*/
