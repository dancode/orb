#ifndef DEV_SHIP_H
#define DEV_SHIP_H
/*==============================================================================================

    dev_ship.h -- Developer ship-pipeline library.

    Shared implementation of the build -> cook -> stage -> package -> deploy pipeline that
    turns a project into a standalone, non-development release layout.  Clients stay thin
    front-ends: ship_tool (the batch CLI) today, the editor's Deploy window later (which
    spawns ship_tool rather than linking this lib -- anything the button does, a batch file
    can do).

    Stages (each independently callable; dev_ship_run drives them in order):

        build     compile the ship exe set: shells out to build_tool.  Default is the
                  monolithic <project>_ship single exe (the "final correct" shape);
                  DEV_SHIP_MODULAR ships host_game.exe + the module DLLs instead
        cook      convert source assets to runtime formats (font/shader/asset tools);
                  currently a no-op -- all cooking is offline and committed
        stage     gather the runtime file set into a clean directory in the shipped layout
        package   write <out>/manifest.txt (build stamp + crc32/size/path per staged file);
                  zip-bundling assets lands here later
        deploy    reconciled directory publish into deploy_dir: manifests diffed first,
                  files the previous deploy shipped but this one drops are deleted (exact
                  paths only -- never a blanket clear), then the staged tree copies over.
                  A non-empty destination without our manifest is refused, not overwritten.
                  No destination = stated no-op (the staged dir already is the deliverable)

    The staged directory is itself the shippable output: zip it and send it.  Its layout
    mirrors the dev tree because sys_root_dir() resolves the asset root as ONE LEVEL ABOVE
    the executable -- shipped exes find assets/ and config/ with zero path changes:

        <out>/<project>.bat      launcher (root convenience; exe lives in bin/)
        <out>/bin/               exe (+ module DLLs when modular, + shaders/ when cooked)
        <out>/assets/font/       cooked runtime assets only -- sources and caches stay home
        <out>/assets/icon/
        <out>/config/

    All stages take the same immutable dev_ship_desc_t.  Paths are resolved against
    desc->root_dir, which must be the engine root (the directory holding orb.targets);
    ship_tool validates this before dispatching.

    Link deps: sys (process run, file copy, dir walk), pack (crc32 for the manifest)

==============================================================================================*/

#include "orb.h"

/* Pipeline configuration -- filled by the front-end, read-only to every stage. */
typedef struct
{
    const char* project;     /* project name, e.g. "sample_game"; ship target = <project>_ship */
    const char* config;      /* build configuration: "Debug" or "Release" */
    const char* root_dir;    /* engine root (directory holding orb.targets) */
    const char* out_dir;     /* staging root; "" or NULL = <root>/build/ship/<project> */
    const char* deploy_dir;  /* deploy destination; "" or NULL = deploy stage is a no-op */
    u32         flags;       /* DEV_SHIP_* */

} dev_ship_desc_t;

/* dev_ship flags */
#define DEV_SHIP_PDB         ( 1u << 0 )    /* stage .pdb files alongside the exe */
#define DEV_SHIP_CLEAN       ( 1u << 1 )    /* delete staged files before staging */
#define DEV_SHIP_SKIP_BUILD  ( 1u << 2 )    /* stage prebuilt bin/ output; do not compile */
#define DEV_SHIP_MODULAR     ( 1u << 3 )    /* ship host_game.exe + module DLLs instead of
                                               the monolithic single exe (the default) */

/* Pipeline stages, in execution order.  DEV_SHIP_STAGE_COUNT sizes iteration. */
typedef enum
{
    DEV_SHIP_BUILD = 0,
    DEV_SHIP_COOK,
    DEV_SHIP_STAGE,
    DEV_SHIP_PACKAGE,
    DEV_SHIP_DEPLOY,
    DEV_SHIP_STAGE_COUNT

} dev_ship_stage_t;

/* Stage name for logs and CLI parsing ("build", "cook", ...); NULL if out of range. */
const char* dev_ship_stage_name( dev_ship_stage_t stage );

/* Run one stage.  Returns false on failure (see dev_ship_last_error()). */
bool        dev_ship_run_stage( const dev_ship_desc_t* desc, dev_ship_stage_t stage );

/* Run the full pipeline in order, stopping at the first failing stage. */
bool        dev_ship_run( const dev_ship_desc_t* desc );

const char* dev_ship_last_error( void );

/*============================================================================================*/
#endif /* DEV_SHIP_H */
