#ifndef ED_H
#define ED_H
/*==============================================================================================

    sandbox/gui/sb_gui_editor/ed.h -- Shared state for the sb_gui_editor sandbox.

    A synthetic "engine" just deep enough to drive a believable editor shell: a flat entity
    pool (mesh / light / camera kinds with a transform and per-kind knobs), a play-mode state
    machine with snapshot/restore, a console log ring, a static fake asset database, and the
    orbit camera + offscreen render target behind the Scene panel.

    Everything lives in one global ed_state_t (g_ed) -- this is sandbox scaffolding, not
    engine code; a real editor would sit these on real engine systems.

==============================================================================================*/

#include "orb.h"
#include "runtime_service/rhi/rhi.h"
#include "ed_viewcam.h"

// clang-format off

/*==============================================================================================
    Entities
==============================================================================================*/

#define ED_MAX_ENTITIES  64
#define ED_NAME_MAX      64

typedef enum
{
    ED_KIND_MESH   = 0,
    ED_KIND_LIGHT  = 1,
    ED_KIND_CAMERA = 2,
    ED_KIND_COUNT  = 3,

} ed_kind_t;

typedef struct
{
    bool used;                    // pool slot occupied
    bool active;                  // participates in the scene render
    char name[ ED_NAME_MAX ];
    i32  kind;                    // ed_kind_t

    f32  pos  [ 3 ];
    f32  rot  [ 3 ];              // euler degrees; only Y is used by the stub renderer
    f32  scale[ 3 ];
    f32  color[ 4 ];              // linear rgba fed straight to draw()

    /* per-kind knobs -- enough for the Inspector to have something real to edit */
    bool spin;                    // mesh: animate rot.y in play mode
    f32  spin_speed;              // mesh: degrees per second
    f32  orbit_radius;            // mesh: > 0 orbits the origin in play mode
    f32  intensity;               // light
    f32  fov;                     // camera

} ed_entity_t;

/*==============================================================================================
    Console log
==============================================================================================*/

#define ED_LOG_MAX      256
#define ED_LOG_MSG_MAX  160

typedef enum
{
    ED_LOG_INFO  = 0,
    ED_LOG_WARN  = 1,
    ED_LOG_ERROR = 2,

} ed_log_level_t;

typedef struct
{
    u8  level;
    f32 time;                     // seconds since editor start
    char msg[ ED_LOG_MSG_MAX ];

} ed_log_entry_t;

/*==============================================================================================
    Assets (static fake database)
==============================================================================================*/

typedef struct
{
    const char* name;
    const char* type;
    f32         size_kb;

} ed_asset_t;

/*==============================================================================================
    Play mode
==============================================================================================*/

typedef enum
{
    ED_MODE_EDIT  = 0,
    ED_MODE_PLAY  = 1,
    ED_MODE_PAUSE = 2,

} ed_mode_t;

/*==============================================================================================
    Scene viewport render target (camera is a viewcam_t -- see ed_viewcam.h)
==============================================================================================*/

/* Double-buffered offscreen target: the CPU records up to RHI_MAX_FRAMES_IN_FLIGHT frames
   ahead, so a single texture would be rewritten by frame N+1's scene pass while frame N's gui
   pass still samples it (visible as torn/twisted geometry whenever the view changes per
   frame).  Frame N draws into and displays tex[cur]; cur flips every frame, so the texture
   the in-flight previous frame samples is never the one being written. */
typedef struct
{
    rhi_texture_t tex[ 2 ];           // offscreen color targets, alternated per frame
    rhi_texture_t depth[ 2 ];         // transient depth buffers, one per color target (not sampled)
    u32           bindless_idx[ 2 ];  // gui image_texture slots; 0 = not created yet
    u32           cur;                // which of the two this frame renders + displays
    bool          first_frame[ 2 ];   // next barrier uses UNDEFINED (fresh texture)
    i32           w, h;               // current texture size (both match)
    i32           want_w, want_h;     // size the panel asked for this frame
    i32           stable_frames;      // frames want_* has differed from w/h (resize settle)

} ed_target_t;

/*==============================================================================================
    Editor state
==============================================================================================*/

typedef struct
{
    ed_entity_t entities[ ED_MAX_ENTITIES ];
    ed_entity_t snapshot[ ED_MAX_ENTITIES ];   // edit-state copy captured on Play, restored on Stop
    i32         selected;                      // entity index; -1 = none

    ed_mode_t   mode;
    f64         sim_time;                      // advances only in PLAY
    f32         frame_dt;                      // last frame's dt; camera fly uses it in the panel
    bool        realtime;                      // toolbar toggle: render the scene pass every frame

    ed_log_entry_t log[ ED_LOG_MAX ];
    u32            log_count;                  // total ever logged (ring: newest = log_count-1)
    bool           log_show[ 3 ];              // per-level filter
    f64            start_time;

    viewcam_t   cam;                           // Scene viewport camera controller
    ed_target_t target;

    /* panel visibility (Window menu toggles) */
    bool show_hierarchy;
    bool show_inspector;
    bool show_console;
    bool show_assets;
    bool show_viewport;

    bool request_quit;

    i32 disp_w, disp_h;                        // main window client size, set by the host each frame

} ed_state_t;

extern ed_state_t g_ed;

/*==============================================================================================
    Unit entry points (all units included by sb_gui_editor.c)
==============================================================================================*/

/* ed_engine.c -- synthetic engine stub */
void ed_engine_init( void );
void ed_tick( f32 dt );
void ed_logf( ed_log_level_t level, const char* fmt, ... );
i32  ed_entity_add( ed_kind_t kind );          // returns new index or -1
void ed_entity_delete( i32 idx );
i32  ed_entity_duplicate( i32 idx );           // returns new index or -1
void ed_play( void );
void ed_pause( void );
void ed_stop( void );
extern const ed_asset_t ed_assets[];
extern const i32        ed_asset_count;

/* ed_viewport.c -- offscreen target + camera + Scene panel */
bool ed_viewport_init( void );
void ed_viewport_shutdown( void );
void ed_viewport_maintain( void );             // create/resize the targets; call between frames
bool ed_scene_changed( void );                 // anything the scene pass draws moved since last call
void ed_viewport_flip( void );                 // swap write/display target; call before an emit
void ed_viewport_render( rhi_cmd_t cmd );      // offscreen scene pass; pair 1:1 with flip+emit
void ed_viewport_panel( void );

/* ed_panels.c -- Hierarchy / Inspector / Console / Assets */
void ed_hierarchy_panel( void );
void ed_inspector_panel( void );
void ed_console_panel( void );
void ed_assets_panel( void );

/* ed_shell.c -- menu bar, toolbar, dockspace, layout persistence */
void ed_shell_build( void );

// clang-format on
/*============================================================================================*/
#endif    // ED_H
