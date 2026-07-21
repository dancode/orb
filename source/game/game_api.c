/*==============================================================================================

    game_api.c -- game module wiring.
    Implements the game_api_t vtable struct and the mod_desc_t lifecycle descriptor.

    The RUNNER: the standard driver of the project contract (runtime/run_project.h).  Owns
    the play session so hosts don't have to: the state machine (stopped/playing/paused),
    the fixed-step accumulator, and the per-frame phase drive of the bound project DLL.

    Hosts bind the runtime-loaded project once at on_ready, control the session with
    play/stop/pause/step, and call tick( dt, view ) every frame.  See game_api.h for the
    host-side picture.

    Shutdown: hosts must stop() before quitting (close-request / quit paths).  exit()
    deliberately never calls into the project -- module exit order is not guaranteed, so
    the project DLL may already be gone; the project's own exit() is its backstop.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

MOD_USE_CORE;

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

/* Stall guard: never run more than this many sim steps in one tick -- excess accumulated
   time is dropped (the sim slows down instead of spiraling: more steps -> longer frame ->
   more steps).  4 steps absorbs a 4x frame spike at the configured rate. */
#define GAME_MAX_SIM_STEPS 4

typedef struct game_state_s
{
    i32  play_state;      /* game_play_state_t                        */
    f32  acc;             /* fixed-step accumulator, seconds          */
    char project[ 64 ];   /* bound project module name; "" = unbound  */

    /* The project's stable api slot -- the mod system rewrites its contents on every
       hot-reload, so this pointer stays live across project AND framework reloads. */
    const run_project_api_t* proj;

} game_state_t;

static game_state_t* g_game_state  = NULL;
static get_api_fn    g_get_api     = NULL;   /* host lookup fn -- re-stowed every init/reload */
static cvar_t*       g_cv_fixed_hz = NULL;

/* The fixed step is read per tick so a cvar change lands on the next frame; the cvar's
   min bound keeps the division safe. */
static f32
game_fixed_dt( void )
{
    return 1.0f / ( f32 )core()->cvar_get_int( g_cv_fixed_hz );
}

/*==============================================================================================
    Session control
==============================================================================================*/

static bool
game_project_bind( const char* name )
{
    game_state_t* s = g_game_state;
    if ( !s || !name || !name[ 0 ] )
        return false;

    if ( s->play_state != GAME_STOPPED )
    {
        LOG_WARN( "project_bind( '%s' ) while a session is active -- stop() first", name );
        return false;
    }

    /* game project api struct is the runtime api */
    const run_project_api_t* proj = ( const run_project_api_t* )g_get_api( name );
    if ( !proj )
    {
        LOG_WARN( "project '%s' is not loaded", name );
        return false;
    }

    snprintf( s->project, sizeof( s->project ), "%s", name );
    s->proj = proj;

    LOG_INFO( "project '%s' bound", s->project );
    return true;
}

static void
game_play( void )
{
    game_state_t* s = g_game_state;
    if ( !s || !s->proj || s->play_state != GAME_STOPPED )
        return;

    s->acc        = 0.0f;
    s->play_state = GAME_PLAYING;
    s->proj->on_start();

    LOG_INFO( "play '%s'", s->project );
}

static void
game_stop( void )
{
    game_state_t* s = g_game_state;
    if ( !s || !s->proj || s->play_state == GAME_STOPPED )
        return;

    s->proj->on_stop();
    s->play_state = GAME_STOPPED;

    LOG_INFO( "stop '%s'", s->project );
}

static void
game_pause( bool paused )
{
    game_state_t* s = g_game_state;
    if ( !s )
        return;

    if ( paused && s->play_state == GAME_PLAYING )
        s->play_state = GAME_PAUSED;
    else if ( !paused && s->play_state == GAME_PAUSED )
        s->play_state = GAME_PLAYING;
}

static void
game_step( void )
{
    game_state_t* s = g_game_state;
    if ( !s || !s->proj || s->play_state != GAME_PAUSED )
        return;

    s->proj->on_sim( game_fixed_dt() );
}

static i32
game_state( void )
{
    return g_game_state ? g_game_state->play_state : GAME_STOPPED;
}

/*==============================================================================================
    Frame drive
==============================================================================================*/

static void
game_tick( f32 dt, const run_view_t* view )
{
    game_state_t* s = g_game_state;
    if ( !s || !s->proj || s->play_state == GAME_STOPPED )
        return;

    f32 fixed_dt = game_fixed_dt();

    if ( s->play_state == GAME_PLAYING )
    {
        s->acc += dt;

        f32 cap = fixed_dt * GAME_MAX_SIM_STEPS;
        if ( s->acc > cap )
            s->acc = cap;

        while ( s->acc >= fixed_dt )
        {
            s->proj->on_sim( fixed_dt );
            s->acc -= fixed_dt;
        }
    }

    /* Paused keeps drawing: the sim is frozen but the scene stays live (and the alpha
       stays frozen with the accumulator, so interpolated draws hold their pose). */
    s->proj->on_frame( dt, view );
    s->proj->on_draw( s->acc / fixed_dt, view );
}

/* The gui-bracket phase: hosts call this from on_gui, so the project's widget emission
   lands inside the gui frame the tick-phase drive can never see.  Paused keeps the HUD
   up for the same reason paused keeps drawing. */
static void
game_hud( f32 dt, const run_view_t* view )
{
    game_state_t* s = g_game_state;
    if ( !s || !s->proj || s->play_state == GAME_STOPPED )
        return;

    s->proj->on_hud( dt, view );
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const game_api_t g_game_api_struct = {
    .project_bind = game_project_bind,
    .play         = game_play,
    .stop         = game_stop,
    .pause        = game_pause,
    .step         = game_step,
    .state        = game_state,
    .tick         = game_tick,
    .hud          = game_hud,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

/* Shared by init and reload: stow the lookup fn, register/refresh the cvar, and re-fetch
   the bound project's slot (same address either way -- the slot is stable). */
static bool
game_wire( game_state_t* gs, get_api_fn get_api )
{
    g_game_state = gs;
    g_get_api = get_api;

    if ( !MOD_FETCH_CORE )
        return false;

    g_cv_fixed_hz = core()->cvar_register_i(
        "game_fixed_hz", "Fixed simulation step rate (Hz)", 60, 10, 480, CVAR_NONE );

    if ( gs->project[ 0 ] )
        gs->proj = ( const run_project_api_t* )g_get_api( gs->project );

    return true;
}

static bool
game_init( void* raw_state, get_api_fn get_api )
{
    if ( !game_wire( ( game_state_t* )raw_state, get_api ) )
        return false;

    LOG_INFO( "init (runner ready)" );
    return true;
}

static bool
game_reload( void* raw_state, get_api_fn get_api )
{
    if ( !game_wire( ( game_state_t* )raw_state, get_api ) )
        return false;

    LOG_INFO( "reloaded (state=%d project='%s')", g_game_state->play_state, g_game_state->project );
    return true;
}

static void
game_exit( void* raw_state )
{
    game_state_t* s = ( game_state_t* )raw_state;
    if ( s && s->play_state != GAME_STOPPED )
        LOG_WARN( "exit with session active -- host should stop() first; project exit() is the backstop" );

    LOG_INFO( "exit" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
game_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( game_state_t ),
        .func_api_size = sizeof( game_api_t ),
        .deps          = { "core" },
        .dep_count     = 1,
        .func_api      = &g_game_api_struct,
        .init          = game_init,
        .exit          = game_exit,
        .reload        = game_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( game );

/*============================================================================================*/
