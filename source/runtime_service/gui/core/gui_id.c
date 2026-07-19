/*==============================================================================================

    runtime_service/gui/core/gui_id.c -- Identity service: ID hashing, combining, scope stack.

    The naming substrate every interaction service keys on.  Three utilities the rest of the
    widget tree is built on:

        id_hash    -- FNV-1a 32-bit hash of a NUL-terminated string, salted per context
                      so identical labels in different contexts never collide.
        id_combine -- mix a scope seed + a local key into one stable id (boost hash_combine).
        id stack   -- the push/pop scope stack that widget ids combine against; regions seed
                      it automatically, push_id/pop_id add temporary levels for repeated rows.

    It knows nothing about widgets, rects, or input: strings and scopes in, stable ids out.
    The keyed per-widget state pool that these ids address is the companion service in
    core/gui_state.c, included immediately after this file.

    Included by gui.c after core/gui_ctx.c, which defines g_ctx (needed for g_ctx->retained /
    gui_context_t) and the id-stack variables (s_id_stack[], s_id_sp) referenced below.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    id_hash -- FNV-1a 32-bit hash of a NUL-terminated string
----------------------------------------------------------------------------------------------*/

static gui_id_t
id_hash( const char* str )
{
    /* Seed FNV-1a with the context's id salt so the same string hashes to a distinct id per context.
       g_ctx->retained.id_salt is 0 for the default context -> the standard 0x811C9DC5 basis -> ids are
       byte-identical to the unsalted hash, so single-context behavior is unchanged. */
    u32 h = 0x811C9DC5u ^ g_ctx->retained.id_salt;
    for ( ; *str; ++str )
        h = ( h ^ (u8)*str ) * 0x01000193u;
    return h ? h : 1u;    /* never return GUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    id_combine -- mix a scope seed with a local key into one id (boost-style hash_combine).

    The single rule for how an id is namespaced: every sub-id (a leaf widget under a region, a
    child region under its parent, a window's chrome control) is id_combine(scope, key).  Unlike
    a bare XOR it avalanches and is order-dependent, so distinct (scope, key) pairs stay distinct.
----------------------------------------------------------------------------------------------*/

static gui_id_t
id_combine( gui_id_t seed, u32 key )
{
    u32 h = seed ^ ( key + 0x9E3779B9u + ( seed << 6 ) + ( seed >> 2 ) );
    return h ? h : 1u;    /* never return GUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    Id-scope stack functions

    id_seed/push/pop operate on the s_id_stack[] / s_id_sp variables declared in gui_ctx.c.
    The top of the stack is the seed every widget id combines against; regions seed it
    automatically, and push_id / pop_id add temporary levels for repeated widgets in one region.
----------------------------------------------------------------------------------------------*/

/* Current scope seed -- top of the stack, or NONE when empty (a bare top-level widget). */
static gui_id_t
id_seed( void )
{
    if ( s_id_sp == 0 ) return GUI_ID_NONE;
    u32  i = s_id_sp - 1;
    if ( i >= GUI_ID_STACK_DEPTH ) i = GUI_ID_STACK_DEPTH - 1;
    return s_id_stack[ i ];
}

static void
id_push( gui_id_t id )
{
    if ( s_id_sp < GUI_ID_STACK_DEPTH )
        s_id_stack[ s_id_sp ] = id;
    ++s_id_sp;    /* count truthfully so push/pop stay paired even past the cap */
}

static void
id_pop( void )
{
    if ( s_id_sp ) --s_id_sp;
}

/* The keyed state pool (gui_state_get / gui_state_peek / GUI_STATE) is the companion tracking
   service in core/gui_state.c, included next. */

// clang-format on
/*============================================================================================*/
