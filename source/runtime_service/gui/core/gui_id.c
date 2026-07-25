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

    Included by gui_core.c after core/gui_ctx.c, which defines g_ctx (for the per-context id
    salt) and the id-stack storage (s_id_stack[], s_id_sp) the verbs below operate on.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    id_hash -- FNV-1a 32-bit hash of a NUL-terminated string
==============================================================================================*/

gui_id_t                       /* non-static: a cross-unit seam (core/gui_core.h) */
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

/*==============================================================================================
    id_combine -- mix a scope seed with a local key into one id (boost-style hash_combine).

    The single rule for how an id is namespaced: every sub-id (a leaf widget under a region, a
    child region under its parent, a window's chrome control) is id_combine(scope, key).  Unlike
    a bare XOR it avalanches and is order-dependent, so distinct (scope, key) pairs stay distinct.
==============================================================================================*/

gui_id_t
id_combine( gui_id_t seed, u32 key )
{
    u32 h = seed ^ ( key + 0x9E3779B9u + ( seed << 6 ) + ( seed >> 2 ) );
    return h ? h : 1u;    /* never return GUI_ID_NONE (0) */
}

/*==============================================================================================
    Id-scope stack verbs -- over the storage (s_id_stack[], s_id_sp) and under the contract both
    declared in core/gui_ctx.c.  The top of the stack is the seed every widget id combines
    against; over-deep pushes alias the top slot, and id_seed clamps its read to match.
==============================================================================================*/

/* Current scope seed -- top of the stack, or NONE when empty (a bare top-level widget). */
gui_id_t
id_seed( void )
{
    if ( s_id_sp == 0 ) return GUI_ID_NONE;
    u32  i = s_id_sp - 1;
    if ( i >= GUI_ID_STACK_DEPTH ) i = GUI_ID_STACK_DEPTH - 1;
    return s_id_stack[ i ];
}

void
id_push( gui_id_t id )
{
    if ( s_id_sp < GUI_ID_STACK_DEPTH )
        s_id_stack[ s_id_sp ] = id;
    ++s_id_sp;    /* count truthfully so push/pop stay paired even past the cap */
}

void
id_pop( void )
{
    if ( s_id_sp ) --s_id_sp;
}

/*==============================================================================================

    Widget label grammar  (Dear ImGui style) -- the id half (a label's id is
    identity derivation).

        "Text"        -> display "Text",  id = hash("Text")
        "Text##key"   -> display "Text",  id = hash("Text##key")   distinct ids, same visible text
        "pre###key"   -> display "pre",   id = hash("###key")      id ignores a dynamic prefix

    The visible span ends at the first "##".  A "###" additionally re-roots the id hash at that
    "###", so a label whose visible part changes every frame (a counter, a name) keeps one stable
    id.  Every labeled widget routes its display through label_width / draw_label (draw unit) and
    its id through item_id, so the grammar is honored uniformly in one place.

==============================================================================================*/

/* Visible byte count: up to the first "##" marker, or the whole string.  Non-static: a
   cross-unit seam (core/gui_core.h) -- the stock unit's stock_button and the draw unit's label
   painters honor the same label grammar, so the rule stays authored in one place. */
u32
label_vis_len( const char* s )
{
    u32 i = 0;
    while ( s[ i ] )
    {
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' )    /* s[i+1] is at worst the NUL: safe */
            break;
        ++i;
    }
    return i;
}

/* The substring hashed for the id: the whole label, unless a "###" tail re-roots it there. */
const char*
label_id_str( const char* s )
{
    for ( u32 i = 0; s[ i ]; ++i )
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' && s[ i + 2 ] == '#' )    /* reads stop at NUL */
            return s + i;
    return s;
}

/* The id for a labeled widget: the active scope seed combined with the label's id key. */
gui_id_t
item_id( const char* label )
{
    gui_id_t id = id_combine( id_seed(), id_hash( label_id_str( label ) ) );
    DBG_NAME( id, label );
    return id;
}

// clang-format on
/*============================================================================================*/
