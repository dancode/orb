/*==============================================================================================

    runtime_service/input/input_actions.c -- Action registry, cmd transport, frame latch.

    Data flow for a BUTTON action (the Quake hybrid):

        bind w +forward                                (cmd_bind, core)
        key edge -> cmd_bind_event queues "+forward 17" / "-forward 17"
        cmd_pump() dispatches to input_cmd_plus/minus  (registered here per action)
        edges accumulate: held-source list + pending press/release counts
        input()->frame( dt )  latches pending -> the frame-visible state block

    The held-source list (not a bool) is why overlapping holds work: W down, A down,
    W up must leave +forward held by A.  Each +/- carries its source key number, so
    release removes exactly that source.  A console-typed "+forward" (no key argument)
    uses a sentinel source and pairs with a typed "-forward" the same way.

    All state is file-scope -- the service is a STATIC lib registered via mod_static,
    exactly one instance per process.

==============================================================================================*/

/*==============================================================================================
    Registry state
==============================================================================================*/

#define INPUT_KEY_NONE -2    // "no source" sentinel for console-typed +name (key -1 = list-empty)

typedef struct input_action_rec_s
{
    char name[ INPUT_ACTION_NAME_LEN ];
    u8   type;            // input_action_type_t
    u32  context_mask;    // 0 = live in all contexts

    /* digital transport accumulator (BUTTON) -- mutated by cmd handlers during cmd_pump */
    i32 held[ INPUT_HELD_MAX ];    // source key numbers currently holding; -1 = free slot
    u32 held_count;
    u32 pending_press;      // 0->held transitions since the last frame() latch
    u32 pending_release;    // held->0 transitions since the last frame() latch

    /* frame-visible block -- latched by input_frame, read by the state queries */
    bool frame_down;
    u32  frame_pressed;
    u32  frame_released;
    f32  value[ 2 ];    // axis values; sources land next phase, 0 until then

} input_action_rec_t;

static input_action_rec_t s_actions[ INPUT_ACTION_MAX ];
static u32                s_action_count = 0;

static u32 s_ctx_stack[ INPUT_CONTEXT_MAX ];
static u32 s_ctx_depth = 0;    // empty stack -> everything active

/*============================================================================================*/
/* Case-insensitive name compare (action names ride command names, which match that way). */

static bool
input_name_eq( const char* a, const char* b )
{
    while ( *a && *b )
    {
        char ca = ( *a >= 'A' && *a <= 'Z' ) ? ( char )( *a + 32 ) : *a;
        char cb = ( *b >= 'A' && *b <= 'Z' ) ? ( char )( *b + 32 ) : *b;
        if ( ca != cb )
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static input_action_rec_t*
input_rec_find( const char* name, input_action_t* out_id )
{
    for ( u32 i = 0; i < s_action_count; ++i )
    {
        if ( input_name_eq( s_actions[ i ].name, name ) )
        {
            if ( out_id )
                *out_id = ( input_action_t )i;
            return &s_actions[ i ];
        }
    }
    return NULL;
}

/*==============================================================================================
    Cmd transport: shared '+name' / '-name' handlers
==============================================================================================*/

/* argv[0] is the full command name ("+forward"); argv[1] is the source key number the bind
   system appended (absent when typed in the console).  The handlers only touch the pending
   accumulator -- the frame-visible block changes at frame() only. */

static i32
input_cmd_source( int argc, char** argv )
{
    return ( argc > 1 ) ? ( i32 )strtol( argv[ 1 ], NULL, 10 ) : INPUT_KEY_NONE;
}

static void
input_cmd_plus( int argc, char** argv )
{
    input_action_rec_t* rec = input_rec_find( argv[ 0 ] + 1, NULL );
    if ( !rec )
        return;

    const i32 src = input_cmd_source( argc, argv );

    /* Already holding from this source (auto-repeat leak / double +): ignore. */
    for ( u32 i = 0; i < INPUT_HELD_MAX; ++i )
        if ( rec->held[ i ] == src )
            return;

    for ( u32 i = 0; i < INPUT_HELD_MAX; ++i )
    {
        if ( rec->held[ i ] == -1 )
        {
            rec->held[ i ] = src;
            if ( rec->held_count++ == 0 )
                rec->pending_press++;
            return;
        }
    }
    /* More than INPUT_HELD_MAX simultaneous sources: drop the extra hold. */
}

static void
input_cmd_minus( int argc, char** argv )
{
    input_action_rec_t* rec = input_rec_find( argv[ 0 ] + 1, NULL );
    if ( !rec )
        return;

    const i32 src = input_cmd_source( argc, argv );

    for ( u32 i = 0; i < INPUT_HELD_MAX; ++i )
    {
        if ( rec->held[ i ] == src )
        {
            rec->held[ i ] = -1;
            if ( rec->held_count && --rec->held_count == 0 )
                rec->pending_release++;
            return;
        }
    }
    /* Source not in the list (already force-released by a context switch): ignore. */
}

/*============================================================================================*/
/* Drop every hold on an action and emit the release edge -- context gating / teardown. */

static void
input_rec_force_release( input_action_rec_t* rec )
{
    if ( rec->held_count == 0 )
        return;

    for ( u32 i = 0; i < INPUT_HELD_MAX; ++i )
        rec->held[ i ] = -1;
    rec->held_count = 0;
    rec->pending_release++;
}

/*==============================================================================================
    API: registration
==============================================================================================*/

static input_action_t
input_action_register( const char* name, input_action_type_t type, u32 context_mask )
{
    if ( !name || !name[ 0 ] || strlen( name ) >= INPUT_ACTION_NAME_LEN )
    {
        LOG_WARN( "[input] action name invalid or too long: \"%s\"", name ? name : "(null)" );
        return INPUT_ACTION_INVALID;
    }

    /* Idempotent: a DLL re-registering in reload() gets its id back. */
    input_action_t id = INPUT_ACTION_INVALID;

    input_action_rec_t* rec = input_rec_find( name, &id );
    if ( rec )
    {
        if ( rec->type != ( u8 )type )
            LOG_WARN( "[input] action \"%s\" re-registered with a different type", name );
        rec->context_mask = context_mask;
        return id;
    }

    if ( s_action_count >= INPUT_ACTION_MAX )
    {
        LOG_WARN( "[input] action table full (%d), \"%s\" dropped", INPUT_ACTION_MAX, name );
        return INPUT_ACTION_INVALID;
    }

    id  = ( input_action_t )s_action_count++;
    rec = &s_actions[ id ];

    memset( rec, 0, sizeof( *rec ) );
    strncpy( rec->name, name, INPUT_ACTION_NAME_LEN - 1 );
    rec->type         = ( u8 )type;
    rec->context_mask = context_mask;
    for ( u32 i = 0; i < INPUT_HELD_MAX; ++i )
        rec->held[ i ] = -1;

    /* The cmd glue: '+name' / '-name' become real console commands, so binds, config
       files, and typed console lines all drive this action with no further wiring. */
    if ( type == INPUT_ACTION_BUTTON )
    {
        char cmd_name[ INPUT_ACTION_NAME_LEN + 1 ];

        snprintf( cmd_name, sizeof( cmd_name ), "+%s", rec->name );
        core()->cmd_register( cmd_name, input_cmd_plus, "Action press (input service)" );

        cmd_name[ 0 ] = '-';
        core()->cmd_register( cmd_name, input_cmd_minus, "Action release (input service)" );
    }

    return id;
}

static input_action_t
input_action_find( const char* name )
{
    input_action_t id = INPUT_ACTION_INVALID;
    if ( name )
        input_rec_find( name, &id );
    return id;
}

static u32
input_action_count( void )
{
    return s_action_count;
}

static const char*
input_action_name( input_action_t action )
{
    return ( action >= 0 && ( u32 )action < s_action_count ) ? s_actions[ action ].name : NULL;
}

/*==============================================================================================
    API: contexts
==============================================================================================*/

static u32
input_context_active( void )
{
    return s_ctx_depth ? s_ctx_stack[ s_ctx_depth - 1 ] : 0xFFFFFFFFu;
}

static void
input_context_push( u32 mask )
{
    if ( s_ctx_depth >= INPUT_CONTEXT_MAX )
    {
        LOG_WARN( "[input] context stack overflow, push dropped" );
        return;
    }
    s_ctx_stack[ s_ctx_depth++ ] = mask;
}

static void
input_context_pop( void )
{
    if ( s_ctx_depth )
        s_ctx_depth--;
}

/*==============================================================================================
    API: frame latch
==============================================================================================*/

/* Host contract: call once per frame AFTER cmd_pump().  Everything the pump accumulated
   this frame (bind edges, typed +/-, exec'd configs) resolves into this frame's state. */

static void
input_frame( f32 dt )
{
    UNUSED( dt );    // reserved: axis filtering / mouse scaling (next phase)

    const u32 active = input_context_active();

    for ( u32 i = 0; i < s_action_count; ++i )
    {
        input_action_rec_t* rec = &s_actions[ i ];

        /* Context gate: an action whose mask is disjoint from the active context is
           force-released -- the game sees one clean released edge, never a stuck key. */
        if ( rec->context_mask && !( rec->context_mask & active ) )
            input_rec_force_release( rec );

        rec->frame_down     = rec->held_count > 0;
        rec->frame_pressed  = rec->pending_press;
        rec->frame_released = rec->pending_release;
        rec->pending_press  = 0;
        rec->pending_release = 0;
    }
}

/*==============================================================================================
    API: per-frame state queries
==============================================================================================*/

static bool
input_down( input_action_t action )
{
    return action >= 0 && ( u32 )action < s_action_count && s_actions[ action ].frame_down;
}

static u32
input_pressed( input_action_t action )
{
    return ( action >= 0 && ( u32 )action < s_action_count ) ? s_actions[ action ].frame_pressed : 0;
}

static u32
input_released( input_action_t action )
{
    return ( action >= 0 && ( u32 )action < s_action_count ) ? s_actions[ action ].frame_released : 0;
}

static f32
input_value( input_action_t action )
{
    if ( action < 0 || ( u32 )action >= s_action_count )
        return 0.0f;

    const input_action_rec_t* rec = &s_actions[ action ];
    return ( rec->type == INPUT_ACTION_BUTTON ) ? ( rec->frame_down ? 1.0f : 0.0f ) : rec->value[ 0 ];
}

static void
input_value2( input_action_t action, f32* out_x, f32* out_y )
{
    f32 x = 0.0f, y = 0.0f;

    if ( action >= 0 && ( u32 )action < s_action_count )
    {
        const input_action_rec_t* rec = &s_actions[ action ];
        if ( rec->type == INPUT_ACTION_BUTTON )
            x = rec->frame_down ? 1.0f : 0.0f;
        else
        {
            x = rec->value[ 0 ];
            y = rec->value[ 1 ];
        }
    }

    if ( out_x ) *out_x = x;
    if ( out_y ) *out_y = y;
}

/*==============================================================================================
    Console command: actionlist
==============================================================================================*/

static void
input_cmd_actionlist( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    static const char* type_names[] = { "button", "axis1", "axis2" };

    for ( u32 i = 0; i < s_action_count; ++i )
    {
        const input_action_rec_t* rec = &s_actions[ i ];
        core()->con_printf( "  %-20s %-6s ctx=%08x %s\n", rec->name, type_names[ rec->type ],
                            rec->context_mask, rec->frame_down ? "[down]" : "" );
    }
    core()->con_printf( "%u action(s)\n", s_action_count );
}

/*==============================================================================================
    Lifetime (called from input_api.c's mod hooks)
==============================================================================================*/

static void
input_system_init( void )
{
    memset( s_actions, 0, sizeof( s_actions ) );
    s_action_count = 0;
    s_ctx_depth    = 0;

    core()->cmd_register( "actionlist", input_cmd_actionlist, "List registered input actions" );
}

static void
input_system_exit( void )
{
    /* Unregister the per-action commands so a clean core outlives the service. */
    for ( u32 i = 0; i < s_action_count; ++i )
    {
        if ( s_actions[ i ].type == INPUT_ACTION_BUTTON )
        {
            char cmd_name[ INPUT_ACTION_NAME_LEN + 1 ];

            snprintf( cmd_name, sizeof( cmd_name ), "+%s", s_actions[ i ].name );
            core()->cmd_unregister( cmd_name );

            cmd_name[ 0 ] = '-';
            core()->cmd_unregister( cmd_name );
        }
    }
    core()->cmd_unregister( "actionlist" );

    s_action_count = 0;
    s_ctx_depth    = 0;
}

/*============================================================================================*/
