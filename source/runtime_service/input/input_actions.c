/*==============================================================================================

    runtime_service/input/input_actions.c -- Action registry, cmd transport, axis binds,
    frame latch.

    Data flow for a BUTTON action (the Quake hybrid):

        bind w +forward                                (cmd_bind, core)
        key edge -> cmd_bind_event queues "+forward 17" / "-forward 17"
        cmd_pump() dispatches to input_cmd_plus/minus  (registered here per action)
        edges accumulate: held-source list + pending press/release counts
        input()->frame( dt )  latches pending -> the frame-visible state block

    Data flow for an AXIS action (the modern half -- no command strings on the hot path):

        bindaxis pad_lstick move / bindaxis w move 0 1   (writes the axis bind TABLE)
        input()->frame( dt )  walks the table, samples app() devices directly
        (sticks: radial deadzone + curve; mouse: raw delta * sens; digital: scale vector)
        bounded sources sum and clamp to -1..1; mouse deltas add on top unclamped

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

/*==============================================================================================
    Axis binds -- direct source -> action table, evaluated in frame().  Analog data never
    rides command strings; bindaxis/unbindaxis only edit this table.
==============================================================================================*/

/* What an axis bind samples. */
typedef enum axsrc_kind_e
{
    AXSRC_DIGITAL = 0,    // any digital source (app_src_t code): down -> scale vector
    AXSRC_MOUSE,          // raw mouse delta (unbounded; * in_mouse_sens; +y up)
    AXSRC_STICK_L,        // pad left stick pair (radial deadzone + curve)
    AXSRC_STICK_R,        // pad right stick pair
    AXSRC_PAD_AXIS,       // single pad axis (app_pad_axis_t code; triggers skip deadzone)

} axsrc_kind_t;

typedef struct axis_bind_s
{
    u8  kind;    // axsrc_kind_t
    u16 code;    // AXSRC_DIGITAL: app_src_t; AXSRC_PAD_AXIS: app_pad_axis_t

    /* Bound by NAME and resolved lazily, so config files can bindaxis before a game DLL
       registers the action (exec order independence); ids never change once assigned. */
    char           action[ INPUT_ACTION_NAME_LEN ];
    input_action_t cached;    // INPUT_ACTION_INVALID until resolved

    f32 sx, sy;    // per-bind scale applied to the sampled (x,y)

} axis_bind_t;

static axis_bind_t s_axis_binds[ INPUT_AXIS_BIND_MAX ];
static u32         s_axis_bind_count = 0;

/* Filtering policy cvars -- registered at init, read every frame (direct value access). */
static cvar_t* s_cv_mouse_sens;      // mouse delta multiplier
static cvar_t* s_cv_mouse_invert;    // flip mouse y
static cvar_t* s_cv_pad_deadzone;    // stick deadzone 0..1 (radial for pairs)
static cvar_t* s_cv_pad_curve;       // response exponent on rescaled stick magnitude

/* Named analog sources ("mouse", "pad_lstick", ...).  Digital sources resolve through
   app()->key_names, so every bindable key name works in bindaxis too. */
typedef struct axsrc_name_s
{
    const char* name;
    u8          kind;
    u16         code;

} axsrc_name_t;

static const axsrc_name_t k_axsrc_names[] = {
    { "mouse",      AXSRC_MOUSE,    0               },
    { "pad_lstick", AXSRC_STICK_L,  0               },
    { "pad_rstick", AXSRC_STICK_R,  0               },
    { "pad_lx",     AXSRC_PAD_AXIS, APP_PAD_AXIS_LX },
    { "pad_ly",     AXSRC_PAD_AXIS, APP_PAD_AXIS_LY },
    { "pad_rx",     AXSRC_PAD_AXIS, APP_PAD_AXIS_RX },
    { "pad_ry",     AXSRC_PAD_AXIS, APP_PAD_AXIS_RY },
    { "pad_lt",     AXSRC_PAD_AXIS, APP_PAD_AXIS_LT },
    { "pad_rt",     AXSRC_PAD_AXIS, APP_PAD_AXIS_RT },
};

/*============================================================================================*/
/* Source name <-> (kind, code).  Analog names first, then the app digital name table. */

static bool
axsrc_from_name( const char* name, u8* out_kind, u16* out_code )
{
    for ( u32 i = 0; i < ARRAY_COUNT( k_axsrc_names ); ++i )
    {
        if ( input_name_eq( k_axsrc_names[ i ].name, name ) )
        {
            *out_kind = k_axsrc_names[ i ].kind;
            *out_code = k_axsrc_names[ i ].code;
            return true;
        }
    }

    u32                count = 0;
    const char* const* names = app()->key_names( &count );
    for ( u32 i = 0; i < count; ++i )
    {
        if ( names[ i ] && input_name_eq( names[ i ], name ) )
        {
            *out_kind = AXSRC_DIGITAL;
            *out_code = ( u16 )i;
            return true;
        }
    }
    return false;
}

static const char*
axsrc_to_name( u8 kind, u16 code )
{
    if ( kind == AXSRC_DIGITAL )
    {
        u32                count = 0;
        const char* const* names = app()->key_names( &count );
        return ( code < count && names[ code ] ) ? names[ code ] : "?";
    }

    for ( u32 i = 0; i < ARRAY_COUNT( k_axsrc_names ); ++i )
        if ( k_axsrc_names[ i ].kind == kind && k_axsrc_names[ i ].code == code )
            return k_axsrc_names[ i ].name;
    return "?";
}

/*============================================================================================*/
/* Sampling helpers. */

/* Digital sources: keyboard + pad buttons share the key snapshot; mouse buttons live in
   their own snapshot fields, so translate the APP_SRC_MOUSE* block. */
static bool
axis_digital_down( u16 code )
{
    if ( code >= APP_SRC_MOUSE1 && code <= APP_SRC_MOUSE5 )
        return app()->mouse_button_down( ( app_mouse_button_t )( code - APP_SRC_MOUSE1 ) );
    return app()->key_down( ( app_key_t )code );
}

/* Rescale past the deadzone so output is continuous from 0, then apply the response
   curve: linear at in_pad_curve 1, finer center control above 1. */
static f32
axis_shape( f32 mag, f32 dz, f32 curve )
{
    if ( mag <= dz )
        return 0.0f;
    f32 t = ( mag - dz ) / ( 1.0f - dz );
    if ( t > 1.0f )
        t = 1.0f;
    return ( curve != 1.0f ) ? powf( t, curve ) : t;
}

/* Stick pair with RADIAL deadzone (magnitude-based, so diagonals are not clipped square)
   summed across connected pads. */
static void
axis_sample_stick( bool right, f32* out_x, f32* out_y )
{
    const f32 dz    = s_cv_pad_deadzone->f.value;
    const f32 curve = s_cv_pad_curve->f.value;

    f32 sx = 0.0f, sy = 0.0f;

    for ( i32 pad = 0; pad < APP_PAD_MAX; ++pad )
    {
        if ( !app()->pad_connected( pad ) )
            continue;

        const f32 x   = app()->pad_axis( pad, right ? APP_PAD_AXIS_RX : APP_PAD_AXIS_LX );
        const f32 y   = app()->pad_axis( pad, right ? APP_PAD_AXIS_RY : APP_PAD_AXIS_LY );
        const f32 mag = sqrtf( x * x + y * y );
        const f32 out = axis_shape( mag, dz, curve );

        if ( out > 0.0f )
        {
            sx += x * ( out / mag );
            sy += y * ( out / mag );
        }
    }

    *out_x = sx;
    *out_y = sy;
}

/* Single pad axis summed across connected pads.  Stick components get the scalar
   deadzone + curve; triggers are already clean 0..1 ramps and pass through. */
static f32
axis_sample_pad_axis( u16 code )
{
    const bool is_stick = ( code <= APP_PAD_AXIS_RY );
    const f32  dz       = s_cv_pad_deadzone->f.value;
    const f32  curve    = s_cv_pad_curve->f.value;

    f32 sum = 0.0f;

    for ( i32 pad = 0; pad < APP_PAD_MAX; ++pad )
    {
        if ( !app()->pad_connected( pad ) )
            continue;

        const f32 v = app()->pad_axis( pad, ( app_pad_axis_t )code );
        if ( is_stick )
        {
            const f32 out = axis_shape( ( v < 0.0f ) ? -v : v, dz, curve );
            sum += ( v < 0.0f ) ? -out : out;
        }
        else
        {
            sum += v;
        }
    }
    return sum;
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
    UNUSED( dt );    // reserved: per-axis smoothing

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

    /* ---- axis evaluation ------------------------------------------------ */
    /*
       Bounded sources (sticks, digital composites, single axes) accumulate separately
       from mouse deltas: the bounded sum clamps to -1..1 (stick + WASD both held cannot
       double speed) and the unbounded mouse delta adds on top unclamped (a look delta
       must never saturate).  Mouse y is +up to match the stick convention; in_mouse_invert
       flips it.
    */

    static f32 bnd[ INPUT_ACTION_MAX ][ 2 ];    // bounded accumulation per action
    static f32 unb[ INPUT_ACTION_MAX ][ 2 ];    // unbounded (mouse) accumulation

    for ( u32 i = 0; i < s_action_count; ++i )
    {
        bnd[ i ][ 0 ] = bnd[ i ][ 1 ] = 0.0f;
        unb[ i ][ 0 ] = unb[ i ][ 1 ] = 0.0f;
    }

    for ( u32 b = 0; b < s_axis_bind_count; ++b )
    {
        axis_bind_t* ab = &s_axis_binds[ b ];

        /* Lazy action resolve: a config can bindaxis before the action registers. */
        if ( ab->cached == INPUT_ACTION_INVALID )
            input_rec_find( ab->action, &ab->cached );
        if ( ab->cached == INPUT_ACTION_INVALID )
            continue;

        const input_action_rec_t* rec = &s_actions[ ab->cached ];
        if ( rec->context_mask && !( rec->context_mask & active ) )
            continue;

        f32  x = 0.0f, y = 0.0f;
        bool unbounded = false;

        switch ( ab->kind )
        {
            case AXSRC_DIGITAL:
                if ( axis_digital_down( ab->code ) )
                    x = y = 1.0f;
                break;

            case AXSRC_MOUSE:
            {
                f32 dx = 0.0f, dy = 0.0f;
                app()->mouse_raw_delta( &dx, &dy );

                const f32 sens = s_cv_mouse_sens->f.value;
                x = dx * sens;
                y = ( s_cv_mouse_invert->b.value ? dy : -dy ) * sens;    // raw dy is +down
                unbounded = true;
                break;
            }

            case AXSRC_STICK_L: axis_sample_stick( false, &x, &y ); break;
            case AXSRC_STICK_R: axis_sample_stick( true, &x, &y ); break;

            case AXSRC_PAD_AXIS:
                x = y = axis_sample_pad_axis( ab->code );
                break;
        }

        f32* acc = unbounded ? unb[ ab->cached ] : bnd[ ab->cached ];
        acc[ 0 ] += x * ab->sx;
        acc[ 1 ] += y * ab->sy;
    }

    for ( u32 i = 0; i < s_action_count; ++i )
    {
        input_action_rec_t* rec = &s_actions[ i ];
        if ( rec->type == INPUT_ACTION_BUTTON )
            continue;

        f32 vx = bnd[ i ][ 0 ], vy = bnd[ i ][ 1 ];
        vx = ( vx > 1.0f ) ? 1.0f : ( vx < -1.0f ) ? -1.0f : vx;
        vy = ( vy > 1.0f ) ? 1.0f : ( vy < -1.0f ) ? -1.0f : vy;

        rec->value[ 0 ] = vx + unb[ i ][ 0 ];
        rec->value[ 1 ] = vy + unb[ i ][ 1 ];
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
    Console commands: bindaxis / unbindaxis / unbindaxisall / axislist
==============================================================================================*/

/* bindaxis <source> <action> [x-scale] [y-scale]
   Source is an analog name (mouse, pad_lstick, pad_rstick, pad_lx..pad_rt) or any digital
   key name ("w", "pad_up", "mouse4") for composites.  Default scale: analog pairs (1,1),
   everything else (1,0) -- so "bindaxis w move 0 1" is the WASD pattern. */

static void
input_cmd_bindaxis( int argc, char** argv )
{
    if ( argc < 3 )
    {
        core()->con_printf( "Usage: bindaxis <source> <action> [x-scale] [y-scale]\n" );
        return;
    }

    u8  kind = 0;
    u16 code = 0;
    if ( !axsrc_from_name( argv[ 1 ], &kind, &code ) )
    {
        core()->con_printf( "bindaxis: unknown source \"%s\"\n", argv[ 1 ] );
        return;
    }

    if ( strlen( argv[ 2 ] ) >= INPUT_ACTION_NAME_LEN )
    {
        core()->con_printf( "bindaxis: action name too long \"%s\"\n", argv[ 2 ] );
        return;
    }

    const bool pair = ( kind == AXSRC_MOUSE || kind == AXSRC_STICK_L || kind == AXSRC_STICK_R );

    f32 sx = ( argc > 3 ) ? ( f32 )atof( argv[ 3 ] ) : 1.0f;
    f32 sy = ( argc > 4 ) ? ( f32 )atof( argv[ 4 ] ) : ( ( argc > 3 ) ? 0.0f : ( pair ? 1.0f : 0.0f ) );

    /* Rebinding the same (source, action) updates the scales in place. */
    for ( u32 i = 0; i < s_axis_bind_count; ++i )
    {
        axis_bind_t* ab = &s_axis_binds[ i ];
        if ( ab->kind == kind && ab->code == code && input_name_eq( ab->action, argv[ 2 ] ) )
        {
            ab->sx = sx;
            ab->sy = sy;
            return;
        }
    }

    if ( s_axis_bind_count >= INPUT_AXIS_BIND_MAX )
    {
        core()->con_printf( "bindaxis: table full (%d)\n", INPUT_AXIS_BIND_MAX );
        return;
    }

    axis_bind_t* ab = &s_axis_binds[ s_axis_bind_count++ ];
    ab->kind        = kind;
    ab->code        = code;
    ab->cached      = INPUT_ACTION_INVALID;    // resolved lazily in frame()
    ab->sx          = sx;
    ab->sy          = sy;
    memset( ab->action, 0, sizeof( ab->action ) );
    strncpy( ab->action, argv[ 2 ], INPUT_ACTION_NAME_LEN - 1 );
}

static void
input_cmd_unbindaxis( int argc, char** argv )
{
    if ( argc < 2 )
    {
        core()->con_printf( "Usage: unbindaxis <source> [action]\n" );
        return;
    }

    u8  kind = 0;
    u16 code = 0;
    if ( !axsrc_from_name( argv[ 1 ], &kind, &code ) )
    {
        core()->con_printf( "unbindaxis: unknown source \"%s\"\n", argv[ 1 ] );
        return;
    }

    for ( u32 i = 0; i < s_axis_bind_count; )
    {
        axis_bind_t* ab = &s_axis_binds[ i ];
        if ( ab->kind == kind && ab->code == code &&
             ( argc < 3 || input_name_eq( ab->action, argv[ 2 ] ) ) )
        {
            s_axis_binds[ i ] = s_axis_binds[ --s_axis_bind_count ];    // swap-remove
        }
        else
        {
            ++i;
        }
    }
}

static void
input_cmd_unbindaxisall( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    s_axis_bind_count = 0;
}

static void
input_cmd_axislist( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    for ( u32 i = 0; i < s_axis_bind_count; ++i )
    {
        const axis_bind_t* ab = &s_axis_binds[ i ];
        core()->con_printf( "  %-12s -> %-16s scale (%g, %g)\n",
                            axsrc_to_name( ab->kind, ab->code ), ab->action, ab->sx, ab->sy );
    }
    core()->con_printf( "%u axis bind(s)\n", s_axis_bind_count );
}

/*============================================================================================*/
/* Config section writer -- hooked into core's writeconfig so axis binds persist in the
   same file as cvars and key binds, restored through normal exec. */

static void
input_write_config( void* file )
{
    FILE* f = ( FILE* )file;

    fprintf( f, "\nunbindaxisall\n" );

    for ( u32 i = 0; i < s_axis_bind_count; ++i )
    {
        const axis_bind_t* ab = &s_axis_binds[ i ];
        fprintf( f, "bindaxis %s %s %g %g\n",
                 axsrc_to_name( ab->kind, ab->code ), ab->action, ab->sx, ab->sy );
    }
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
    memset( s_axis_binds, 0, sizeof( s_axis_binds ) );
    s_action_count    = 0;
    s_axis_bind_count = 0;
    s_ctx_depth       = 0;

    /* Filtering policy -- archived, so tuned feel persists via writeconfig. */
    s_cv_mouse_sens   = core()->cvar_register_f( "in_mouse_sens", "Mouse look sensitivity",
                                                 1.0f, 0.0f, 1000.0f, CVAR_ARCHIVE );
    s_cv_mouse_invert = core()->cvar_register_b( "in_mouse_invert", "Invert mouse look y",
                                                 false, CVAR_ARCHIVE );
    s_cv_pad_deadzone = core()->cvar_register_f( "in_pad_deadzone", "Stick deadzone (radial for pairs)",
                                                 0.24f, 0.0f, 0.9f, CVAR_ARCHIVE );
    s_cv_pad_curve    = core()->cvar_register_f( "in_pad_curve", "Stick response exponent (1 = linear)",
                                                 1.0f, 0.25f, 4.0f, CVAR_ARCHIVE );

    core()->cmd_register( "actionlist",    input_cmd_actionlist,    "List registered input actions" );
    core()->cmd_register( "bindaxis",      input_cmd_bindaxis,      "Bind an axis source to an action" );
    core()->cmd_register( "unbindaxis",    input_cmd_unbindaxis,    "Remove axis bind(s) for a source" );
    core()->cmd_register( "unbindaxisall", input_cmd_unbindaxisall, "Remove all axis binds" );
    core()->cmd_register( "axislist",      input_cmd_axislist,      "List all axis binds" );

    /* Axis binds persist through the shared writeconfig path. */
    core()->config_writer_add( input_write_config );
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
    core()->cmd_unregister( "bindaxis" );
    core()->cmd_unregister( "unbindaxis" );
    core()->cmd_unregister( "unbindaxisall" );
    core()->cmd_unregister( "axislist" );
    core()->config_writer_remove( input_write_config );

    s_action_count    = 0;
    s_axis_bind_count = 0;
    s_ctx_depth       = 0;
}

/*============================================================================================*/
