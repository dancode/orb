/*==============================================================================================

    runtime_service/gui/frame/gui_context.c -- The public multi-context lifecycle.

    Moved from core/gui_ctx.c at the R4 carve (GUI_SERVER_PLAN.md): creating and destroying a
    context is orchestrator work -- destruction tears down the context's GPU surfaces.  The
    block ALLOCATION (ctx_alloc_slot) joined it at R11: the single-malloc layout sizes every
    unit's retained records with sizeof -- including chrome's (gui_popup_t, gui_dock_node_t),
    which the server holds opaque -- so only the orchestrator, which sees the whole stack, can
    compute it.  The pool STORAGE and the bind verb stay with the server (core/gui_ctx.c:
    s_ctx_pool, ctx_bind); this file is the policy over them.

    Included by gui.c in the frame group.

==============================================================================================*/
// clang-format off

/* Single-malloc layout for one context block.  The header (gui_context_t) sits at offset 0;
   all pool arrays follow at ALIGN8 boundaries.  Caller sets `listening` and wires s_ctx_pool. */
gui_context_t*
ctx_alloc_slot( const gui_ctx_config_t* c, u32 slots, i32 slot )
{
    /* Keyed-state class partition: tiny gets the full state_slots (the hot renters), small 3/4
       of it -- counts are free of the power-of-two rule (multiply-shift bucketing, gui_state.c). */
    u32 slots_small = ( slots / 4u ) * 3u;
    if ( slots_small == 0 ) slots_small = slots;

    #define ALIGN8( x ) ( ( ( x ) + 7u ) & ~7u )
    u32 sz_tiny  = slots               * (u32)sizeof( gui_state_tiny_slot_t );
    u32 sz_state = slots_small         * (u32)sizeof( gui_state_slot_t     );
    u32 sz_big   = GUI_STATE_BIG_SLOTS * (u32)sizeof( gui_state_big_slot_t );
    u32 sz_pop   = c->popup_depth      * (u32)sizeof( gui_popup_t          );
    u32 sz_win   = c->max_windows      * (u32)sizeof( gui_window_t         );
    u32 sz_vp    = c->max_viewports    * (u32)sizeof( gui_viewport_t       );
    u32 sz_dock  = c->max_dock_nodes   * (u32)sizeof( gui_dock_node_t      );

    u32 off_tiny  = ALIGN8( (u32)sizeof( gui_context_t ) );
    u32 off_state = ALIGN8( off_tiny  + sz_tiny  );
    u32 off_big   = ALIGN8( off_state + sz_state );
    u32 off_pop   = ALIGN8( off_big   + sz_big   );
    u32 off_win   = ALIGN8( off_pop   + sz_pop   );
    u32 off_vp    = ALIGN8( off_win   + sz_win   );
    u32 off_dock  = ALIGN8( off_vp    + sz_vp    );
    u32 total     = ALIGN8( off_dock  + sz_dock  );
    #undef ALIGN8

    char* blk = (char*)malloc( total );
    if ( !blk ) return NULL;
    memset( blk, 0, total );

    gui_context_t* ctx      = (gui_context_t*)blk;
    ctx->retained.state_tiny  = (gui_state_tiny_slot_t*)( blk + off_tiny );
    ctx->retained.tiny_count  = slots;
    ctx->retained.state       = (gui_state_slot_t*)( blk + off_state );
    ctx->retained.state_count = slots_small;
    ctx->retained.state_big   = (gui_state_big_slot_t*)( blk + off_big );
    ctx->retained.big_count   = GUI_STATE_BIG_SLOTS;
    ctx->retained.id_salt     = (u32)slot * 0x9e3779b9u;
    ctx->popup.open           = (gui_popup_t*)   ( blk + off_pop  );
    ctx->popup.depth          = c->popup_depth;
    ctx->win.pool             = (gui_window_t*)  ( blk + off_win  );
    ctx->win.max              = c->max_windows;
    ctx->vp.pool              = (gui_viewport_t*)( blk + off_vp   );
    ctx->vp.max               = c->max_viewports;
    ctx->dock.pool            = c->max_dock_nodes
                                ? (gui_dock_node_t*)( blk + off_dock ) : NULL;
    ctx->dock.max             = c->max_dock_nodes;
    ctx->_alloc               = blk;
    ctx->_alloc_size          = total;
    return ctx;
}

/* Allocate the default context (slot 0) at the internal maxima -- the compile-time caps the
   library is built with (GUI_STRESS_TEST scales these); no preset overrides them.
   Called once from gui_init (frame/gui_frame_loop.c). */
void
ctx_pool_init( void )
{
    gui_ctx_config_t c = {
        .max_windows    = GUI_DEFAULT_MAX_WINDOWS,
        .state_slots    = GUI_DEFAULT_STATE_SLOTS,
        .popup_depth    = GUI_DEFAULT_POPUP_DEPTH,
        .max_viewports  = GUI_MAX_VIEWPORTS,
        .max_dock_nodes = GUI_DEFAULT_DOCK_NODES,
    };
    gui_context_t* ctx = ctx_alloc_slot( &c, c.state_slots, 0 );
    ORB_ASSERT( ctx != NULL );   /* no gui without a default context */
    ctx->listening = true;       /* default context listens to input */

    s_ctx_pool[ 0 ]  = ctx;
    s_ctx_pool_count = 1;
    g_ctx            = ctx;
}

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
