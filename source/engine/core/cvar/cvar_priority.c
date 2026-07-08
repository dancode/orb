/*==============================================================================================

    cvar_priority.c

    Priority Guard

    Enabled by default: a lower-priority "set" can't stomp a value a higher-priority one
    already set (see cvar_set_value_internal in cvar.c's Named Access section for the
    actual check).

    Also owns the ambient priority applied to "set"/"seta"/bare-name sets that don't specify
    one explicitly (defaults to CVAR_PRI_USER). cvar_load_defaults pushes CVAR_PRI_DEFAULT/
    CONFIG/AUTOEXEC around each boot config file's queued text; pop restores the previous
    ambient value. Small fixed stack -- push/pop must balance within CVAR_PRI_STACK_DEPTH.

==============================================================================================*/
// clang-format off

#define CVAR_PRI_STACK_DEPTH 8

static cvar_priority_t s_cvar_pri_stack[ CVAR_PRI_STACK_DEPTH ];
static u32             s_cvar_pri_depth   = 0;
static cvar_priority_t s_cvar_pri_current = CVAR_PRI_USER;

/* Priority guard: on by default, a lower-priority source can't stomp a value that was set
   by a higher-priority one. Off, every set applies immediately (last write wins) -- useful
   for debugging which code path is overwriting a cvar. */

static bool g_cvar_priority_guard = true;

void
cvar_set_priority_guard( bool enabled )
{
    g_cvar_priority_guard = enabled;
}

bool
cvar_get_priority_guard( void )
{
    return g_cvar_priority_guard;
}

/*============================================================================================*/
/* Ambient source priority: what an unspecified "set" (console line, set/seta, bare "name value")
   is tagged with. Defaults to CVAR_PRI_USER. cvar_load_defaults pushes CVAR_PRI_DEFAULT/CONFIG/
   AUTOEXEC as hidden commands bracketing each boot config file's queued text (see cvar_config.c)
   so everything that file sets -- including a nested "exec" it runs -- inherits that tier; an
   "exec" run at the console or from another already-leveled file just inherits the ambient value
   already in effect, since it doesn't push its own. */

void
cvar_source_priority_push( cvar_priority_t priority )
{
    if ( s_cvar_pri_depth >= CVAR_PRI_STACK_DEPTH )
    {
        con_printf( "cvar: priority stack overflow, ignoring push\n" );
        return;
    }
    s_cvar_pri_stack[ s_cvar_pri_depth++ ] = s_cvar_pri_current;
    s_cvar_pri_current = priority;
}

void
cvar_source_priority_pop( void )
{
    if ( s_cvar_pri_depth == 0 )
    {
        con_printf( "cvar: priority stack underflow, ignoring pop\n" );
        return;
    }
    s_cvar_pri_current = s_cvar_pri_stack[ --s_cvar_pri_depth ];
}

cvar_priority_t
cvar_source_priority( void )
{
    return s_cvar_pri_current;
}

/*============================================================================================*/
/* Reset ambient priority state; called by cvar_system_init so a re-init (hot reload, sandbox
   re-run) doesn't inherit a stale push depth from a previous session. */

static void
cvar_priority_reset( void )
{
    s_cvar_pri_depth   = 0;
    s_cvar_pri_current = CVAR_PRI_USER;
}

// clang-format on
