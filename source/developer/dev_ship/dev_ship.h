#ifndef DEV_SHIP_H
#define DEV_SHIP_H
/*==============================================================================================

    dev_ship.h -- Developer ship-pipeline library.

    Shared implementation of the build -> verify -> stage -> package -> deploy pipeline that
    turns a project (or any exe target) into a standalone, non-development release layout.
    Clients stay thin front-ends: ship_tool (the batch CLI) today, the editor's Deploy window
    later (which spawns ship_tool rather than linking this lib -- anything the button does, a
    batch file can do).

    Stages (each independently callable; dev_ship_run drives them in order):

        build     compile the ship exe set: shells out to build_tool, which also cooks the
                  content the image names (build/content).  Default is the monolithic
                  <project>_ship single exe; DEV_SHIP_MODULAR ships host_game.exe + the
                  module DLLs instead
        verify    check the content set: every name in the image's resource manifests and
                  every reference inside a cooked file resolves to exactly one file, cooked
                  where it must be; then the complete content roots are walked for stems
                  claimed twice (an error) and files nothing references (reported)
        stage     gather the runtime file set into a clean directory in the shipped layout
        package   write <out>/manifest.txt (build stamp + crc32/size/path per staged file);
                  zip-bundling content lands here later
        deploy    reconciled directory publish into deploy_dir: manifests diffed first,
                  files the previous deploy shipped but this one drops are deleted (exact
                  paths only -- never a blanket clear), then the staged tree copies over.
                  A non-empty destination without our manifest is refused, not overwritten.
                  No destination = stated no-op (the staged dir already is the deliverable)

    The content set is derived, never listed by hand: build_tool writes one resource
    manifest per image (build/obj/<target>/<target>_res_manifest.txt -- every RID() name the
    image's code spells), the ship image is the exe target plus the modules its mono_dep line
    in orb.targets names, and each cooked file's reference section (engine/res/res_ref.h)
    closes the set over what it needs in turn.

    The staged directory is itself the shippable output: zip it and send it.  Its layout
    mirrors the dev tree because sys_root_dir() resolves the content root as ONE LEVEL ABOVE
    the executable, so a shipped exe mounts <root>/content exactly as a dev build does:

        <out>/<project>.bat      launcher (root convenience; exe lives in bin/)
        <out>/bin/               exe (+ module DLLs when modular)
        <out>/content/           exactly the files the image reads, each under its resource
                                 name with the extension the runtime asks for: a cooked file
                                 (.orb_font, .oshd) sits where its recipe or .hlsl would, a
                                 loose file (.png) is copied as it is

    All stages take the same immutable dev_ship_desc_t.  Paths are resolved against
    desc->root_dir, which must be the engine root (the directory holding orb.targets);
    ship_tool validates this before dispatching.

    Link deps: sys (process run, file copy, dir walk), pack (crc32 for the manifest)

==============================================================================================*/

#include "orb.h"

/* Pipeline configuration -- filled by the front-end, read-only to every stage. */
typedef struct
{
    const char* project;     /* project name, e.g. "sample_game" (ship target = <project>_ship),
                                or with DEV_SHIP_TARGET an exe target name, e.g. "sb_gui" */
    const char* config;      /* build configuration: "Debug" or "Release" */
    const char* root_dir;    /* project root (dir holding orb.targets): the engine root for an
                                engine-resident target, the child project dir for a child */
    const char* engine_dir;  /* engine root supplying build_tool (and, with-engine, the host,
                                module DLLs and content); NULL or "" = same as root_dir */
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
#define DEV_SHIP_NO_ENGINE   ( 1u << 4 )    /* light "without engine" shape: ship only the
                                               project's own game DLL -- no host exe, no engine
                                               module DLLs, no content, no launcher.  The recipient
                                               supplies the engine (host_game -project <dir>).
                                               Fast; for dev sharing, not a platform deliverable.
                                               Takes precedence over DEV_SHIP_MODULAR. */
#define DEV_SHIP_TARGET      ( 1u << 5 )    /* `project` is an exe target in orb.targets, shipped
                                               under its own name: -monolithic by default, or
                                               with DEV_SHIP_MODULAR the exe plus its mono_dep
                                               DLLs.  No <name>_ship target is involved. */

/* Pipeline stages, in execution order.  DEV_SHIP_STAGE_COUNT sizes iteration. */
typedef enum
{
    DEV_SHIP_BUILD = 0,
    DEV_SHIP_VERIFY,
    DEV_SHIP_STAGE,
    DEV_SHIP_PACKAGE,
    DEV_SHIP_DEPLOY,
    DEV_SHIP_STAGE_COUNT

} dev_ship_stage_t;

/* Stage name for logs and CLI parsing ("build", "verify", ...); NULL if out of range. */
const char* dev_ship_stage_name( dev_ship_stage_t stage );

/* Run one stage.  Returns false on failure (see dev_ship_last_error()). */
bool        dev_ship_run_stage( const dev_ship_desc_t* desc, dev_ship_stage_t stage );

/* Run the full pipeline in order, stopping at the first failing stage. */
bool        dev_ship_run( const dev_ship_desc_t* desc );

const char* dev_ship_last_error( void );

/*============================================================================================*/
#endif /* DEV_SHIP_H */
