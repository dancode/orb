/*==============================================================================================

    sandbox/gui_editor/ed_engine.c -- Synthetic engine stub.

    Fakes just enough engine to exercise the editor shell: a demo scene, a play-mode tick that
    animates entity transforms, entity pool operations for the Hierarchy context menu, a
    console log ring the whole editor writes into, and a static asset database for the Assets
    panel.  Included by sb_gui_editor.c (unity build).

==============================================================================================*/
// clang-format off

ed_state_t g_ed;

/*==============================================================================================
    Console log
==============================================================================================*/

void
ed_logf( ed_log_level_t level, const char* fmt, ... )
{
    ed_log_entry_t* e = &g_ed.log[ g_ed.log_count % ED_LOG_MAX ];
    e->level = (u8)level;
    e->time  = (f32)( sys_tick_seconds() - g_ed.start_time );

    va_list args;
    va_start( args, fmt );
    vsnprintf( e->msg, sizeof( e->msg ), fmt, args );
    va_end( args );

    g_ed.log_count++;
}

/*==============================================================================================
    Entity pool
==============================================================================================*/

static const char* s_kind_names[ ED_KIND_COUNT ] = { "Mesh", "Light", "Camera" };

i32
ed_entity_add( ed_kind_t kind )
{
    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        ed_entity_t* e = &g_ed.entities[ i ];
        if ( e->used )
            continue;

        memset( e, 0, sizeof( *e ) );
        e->used   = true;
        e->active = true;
        e->kind   = (i32)kind;
        snprintf( e->name, sizeof( e->name ), "%s %d", s_kind_names[ kind ], i );
        e->scale[ 0 ] = e->scale[ 1 ] = e->scale[ 2 ] = 1.0f;
        e->color[ 0 ] = 0.7f; e->color[ 1 ] = 0.7f; e->color[ 2 ] = 0.75f; e->color[ 3 ] = 1.0f;
        e->spin_speed = 45.0f;
        e->intensity  = 1.0f;
        e->fov        = 60.0f;

        ed_logf( ED_LOG_INFO, "Created entity '%s'", e->name );
        return i;
    }
    ed_logf( ED_LOG_ERROR, "Entity pool full (%d)", ED_MAX_ENTITIES );
    return -1;
}

void
ed_entity_delete( i32 idx )
{
    if ( idx < 0 || idx >= ED_MAX_ENTITIES || !g_ed.entities[ idx ].used )
        return;
    ed_logf( ED_LOG_INFO, "Deleted entity '%s'", g_ed.entities[ idx ].name );
    g_ed.entities[ idx ].used = false;
    if ( g_ed.selected == idx )
        g_ed.selected = -1;
}

i32
ed_entity_duplicate( i32 idx )
{
    if ( idx < 0 || idx >= ED_MAX_ENTITIES || !g_ed.entities[ idx ].used )
        return -1;

    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        if ( g_ed.entities[ i ].used )
            continue;
        g_ed.entities[ i ] = g_ed.entities[ idx ];
        snprintf( g_ed.entities[ i ].name, ED_NAME_MAX, "%.56s copy", g_ed.entities[ idx ].name );
        g_ed.entities[ i ].pos[ 0 ] += 1.0f;   /* nudge so the copy is visible beside the source */
        ed_logf( ED_LOG_INFO, "Duplicated '%s'", g_ed.entities[ idx ].name );
        return i;
    }
    ed_logf( ED_LOG_ERROR, "Entity pool full (%d)", ED_MAX_ENTITIES );
    return -1;
}

/*==============================================================================================
    Play mode -- snapshot on Play, restore on Stop, so edit-time state survives a session.
==============================================================================================*/

void
ed_play( void )
{
    if ( g_ed.mode == ED_MODE_EDIT )
    {
        memcpy( g_ed.snapshot, g_ed.entities, sizeof( g_ed.entities ) );
        g_ed.sim_time = 0.0;
        ed_logf( ED_LOG_INFO, "Play" );
    }
    else if ( g_ed.mode == ED_MODE_PAUSE )
    {
        ed_logf( ED_LOG_INFO, "Resume" );
    }
    g_ed.mode = ED_MODE_PLAY;
}

void
ed_pause( void )
{
    if ( g_ed.mode != ED_MODE_PLAY )
        return;
    g_ed.mode = ED_MODE_PAUSE;
    ed_logf( ED_LOG_INFO, "Pause" );
}

void
ed_stop( void )
{
    if ( g_ed.mode == ED_MODE_EDIT )
        return;
    memcpy( g_ed.entities, g_ed.snapshot, sizeof( g_ed.entities ) );
    g_ed.mode = ED_MODE_EDIT;
    ed_logf( ED_LOG_INFO, "Stop -- scene restored" );
}

/*==============================================================================================
    Tick -- the "simulation": meshes spin and orbit while playing.
==============================================================================================*/

void
ed_tick( f32 dt )
{
    if ( g_ed.mode != ED_MODE_PLAY )
        return;

    g_ed.sim_time += dt;

    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        ed_entity_t* e = &g_ed.entities[ i ];
        if ( !e->used || !e->active || e->kind != ED_KIND_MESH )
            continue;

        if ( e->spin )
        {
            e->rot[ 1 ] += e->spin_speed * dt;
            if ( e->rot[ 1 ] > 360.0f ) e->rot[ 1 ] -= 360.0f;
        }
        if ( e->orbit_radius > 0.0f )
        {
            /* phase from the entity index so orbiters spread out */
            f32 a = (f32)g_ed.sim_time * 0.8f + (f32)i * 1.3f;
            e->pos[ 0 ] = cosf( a ) * e->orbit_radius;
            e->pos[ 2 ] = sinf( a ) * e->orbit_radius;
        }
    }
}

/*==============================================================================================
    Assets -- a static fake database; enough rows to make sorting/scrolling meaningful.
==============================================================================================*/

const ed_asset_t ed_assets[] =
{
    { "player_character",   "Mesh",     2048.0f },
    { "crate_wooden",       "Mesh",      256.0f },
    { "crate_metal",        "Mesh",      312.0f },
    { "terrain_chunk_00",   "Mesh",     8192.0f },
    { "rock_small_a",       "Mesh",      128.0f },
    { "rock_small_b",       "Mesh",      144.0f },
    { "tree_pine",          "Mesh",     1024.0f },
    { "grass_clump",        "Mesh",       64.0f },
    { "albedo_dirt",        "Texture",  4096.0f },
    { "albedo_stone",       "Texture",  4096.0f },
    { "normal_stone",       "Texture",  4096.0f },
    { "skybox_day",         "Texture", 16384.0f },
    { "ui_icons",           "Texture",   512.0f },
    { "footstep_grass",     "Sound",      96.0f },
    { "footstep_stone",     "Sound",      88.0f },
    { "ambient_wind",       "Sound",    2400.0f },
    { "music_theme",        "Sound",    9600.0f },
    { "standard_lit",       "Shader",     24.0f },
    { "standard_unlit",     "Shader",     12.0f },
    { "water_surface",      "Shader",     48.0f },
    { "level_intro",        "Scene",     640.0f },
    { "level_arena",        "Scene",     720.0f },
    { "player_controller",  "Script",     18.0f },
    { "camera_rig",         "Script",     11.0f },
    { "spawner",            "Script",      9.0f },
};

const i32 ed_asset_count = (i32)( sizeof( ed_assets ) / sizeof( ed_assets[ 0 ] ) );

/*==============================================================================================
    Init -- build the demo scene.
==============================================================================================*/

void
ed_engine_init( void )
{
    memset( &g_ed, 0, sizeof( g_ed ) );
    g_ed.selected   = -1;
    g_ed.mode       = ED_MODE_EDIT;
    g_ed.start_time = sys_tick_seconds();

    g_ed.log_show[ ED_LOG_INFO ] = g_ed.log_show[ ED_LOG_WARN ] = g_ed.log_show[ ED_LOG_ERROR ] = true;

    g_ed.show_hierarchy = g_ed.show_inspector = g_ed.show_console
                        = g_ed.show_assets    = g_ed.show_viewport = true;

    g_ed.cam.yaw     = 0.7f;
    g_ed.cam.pitch   = 0.5f;
    g_ed.cam.dist    = 14.0f;
    g_ed.cam.fov_deg = 55.0f;

    /* Demo scene: a ground slab, a ring of orbiting crates, a spinning centerpiece, lights. */
    struct { ed_kind_t kind; const char* name; f32 x, y, z; f32 s; f32 r, g, b;
             bool spin; f32 orbit; } spec[] =
    {
        { ED_KIND_MESH,   "Centerpiece", 0.0f, 1.2f, 0.0f, 1.6f, 0.85f, 0.55f, 0.20f, true,  0.0f },
        { ED_KIND_MESH,   "Crate A",     4.0f, 0.5f, 0.0f, 1.0f, 0.30f, 0.65f, 0.90f, true,  4.0f },
        { ED_KIND_MESH,   "Crate B",    -4.0f, 0.5f, 0.0f, 1.0f, 0.35f, 0.80f, 0.40f, true,  4.0f },
        { ED_KIND_MESH,   "Crate C",     0.0f, 0.5f, 4.0f, 1.0f, 0.80f, 0.35f, 0.55f, true,  6.0f },
        { ED_KIND_MESH,   "Pillar N",    0.0f, 1.5f, -6.0f, 0.8f, 0.55f, 0.55f, 0.60f, false, 0.0f },
        { ED_KIND_MESH,   "Pillar S",    0.0f, 1.5f,  6.0f, 0.8f, 0.55f, 0.55f, 0.60f, false, 0.0f },
        { ED_KIND_LIGHT,  "Sun",         6.0f, 6.0f,  3.0f, 0.4f, 1.00f, 0.95f, 0.70f, false, 0.0f },
        { ED_KIND_LIGHT,  "Fill",       -5.0f, 4.0f, -4.0f, 0.3f, 0.50f, 0.60f, 1.00f, false, 0.0f },
        { ED_KIND_CAMERA, "Main Camera", 8.0f, 5.0f,  8.0f, 0.5f, 0.30f, 0.30f, 0.35f, false, 0.0f },
    };

    for ( u32 i = 0; i < sizeof( spec ) / sizeof( spec[ 0 ] ); i++ )
    {
        i32 idx = ed_entity_add( spec[ i ].kind );
        if ( idx < 0 )
            break;
        ed_entity_t* e = &g_ed.entities[ idx ];
        snprintf( e->name, sizeof( e->name ), "%s", spec[ i ].name );
        e->pos[ 0 ] = spec[ i ].x;  e->pos[ 1 ] = spec[ i ].y;  e->pos[ 2 ] = spec[ i ].z;
        e->scale[ 0 ] = e->scale[ 1 ] = e->scale[ 2 ] = spec[ i ].s;
        e->color[ 0 ] = spec[ i ].r;  e->color[ 1 ] = spec[ i ].g;  e->color[ 2 ] = spec[ i ].b;
        e->spin         = spec[ i ].spin;
        e->orbit_radius = spec[ i ].orbit;
    }

    g_ed.selected = 0;
    ed_logf( ED_LOG_INFO, "Editor ready -- %d entities, %d assets", 9, ed_asset_count );
    ed_logf( ED_LOG_WARN, "This is a synthetic engine stub (sandbox scaffolding)" );
}

// clang-format on
/*============================================================================================*/
