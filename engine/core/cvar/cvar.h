#ifndef CVAR_HEADER_H
#define CVAR_HEADER_H
// clang-format off
/*==============================================================================================

    cvar.h

    System for managing console variables.

    This system provides a robust mechanism for creating, querying, and
    modifying engine and game variables at runtime

    - A centralized string pool to minimize memory fragmentation and overhead.
    - A fast hash table for cvar lookups.
    - Support for various data types: bool, int, float, and strings.
    - Flags for controlling cvar behavior (e.g., read-only, archive, cheat).
    - Latched variables that only apply their new value after a specific event (e.g., map restart).
    - A callback system for reacting to cvar changes.
    - Hot-reload safety: The system is designed to persist across dynamic library reloads.
    - A compact 32-byte `cvar_t` structure.
    - Debug infomration is done via (Natvis) and can resolve offsets into strings for display.

    ROADMAP

    - Namespaces: force . separated names, treat gfx.* as category. Support Cvar_List("gfx").
    - Profiles / Layers: layered config stacks (builtin defaults < server defaults < user < session overrides).
    - Metadata: store min/max/enum-list in reflection metadata so UI sliders, dropdowns can be auto-built.
    - Permissions: flags like CVAR_PROTECTED that require a developer token to change.
    - Network replication: store a replication policy per cvar (server->clients, client->server only on connect, dynamic replication).
    - Editor binding: expose a JSON endpoint so external editors can enumerate and change CVARs live.
    - Type-safety & validation: add validators for ints/floats (min/max) and enum lists.

==============================================================================================*/

#pragma once
#include "../../orb.h"
#include "string_pool.h"

/*==============================================================================================

    Cvar Type Flags - Define its type and behavior

==============================================================================================*/

typedef enum cvar_type_e
{
    /* Data Types - mutually exclusive base types */

    CVAR_NONE           = 0,            // No type assigned

    CVAR_BOOL           = BIT( 0 ),     // Boolean value (true or false)
    CVAR_INT            = BIT( 1 ),     // Integer value with optional min/max
    CVAR_FLOAT          = BIT( 2 ),     // Float value with optional min/max
    CVAR_STR            = BIT( 3 ),     // Index into a fixed string list
    CVAR_BUF            = BIT( 4 ),     // A Writable string buffer (fixed size)
    CVAR_REF            = BIT( 5 ),     // Read-only string reference
    CVAR_USR            = BIT( 6 ),     // User-created variable (via console)

    /* Protection / Safety Flags */

    CVAR_ROM            = BIT( 7 ),     // Read-Only Memory. Cannot be changed by the user.
    CVAR_INIT           = BIT( 8 ),     // Can only be set from the command-line or startup .cfg.
    CVAR_LATCH          = BIT( 9 ),     // Value is only applied after a specific event (e.g., restart).
    CVAR_CHEAT          = BIT( 10 ),    // Can only be changed if cheats are enabled.

 // CVAR_SANDBOXED      = BIT( 11 ),    // Isolated from write access by untrusted modules/scripts
 // CVAR_PROTECTED      = BIT( 12 ),    // Requires elevated permission / developer mode.

    /* Lifetime Flags */

    CVAR_RUNTIME        = BIT( 13 ),    // Not persisted, always cleared on restart (timescale, com_frametime)
    CVAR_NORESTART      = BIT( 14 ),    // Value is protected from being reset on a restart.
    CVAR_ARCHIVE        = BIT( 15 ),    // Saved to configuration files (e.g., config.cfg).

    /* Visibility Flags */

    CVAR_DEVONLY        = BIT( 16 ),    // Hidden from retail/release builds.
    CVAR_HIDDEN         = BIT( 17 ),    // Hidden from console auto-completion and `cvarlist`.

    /* Network Flags - for multiplayer synchronization */

    CVAR_NETSYNC        = BIT( 18 ),    // This cvar needs to be synchronized between server and clients.
    CVAR_USERINFO       = BIT( 19 ),    // Client info sent to the server on connect (e.g., name, skin).
    CVAR_SERVERINFO     = BIT( 20 ),    // Server info sent to clients on connect (e.g., mapname, hostname).
    CVAR_SYSTEMINFO     = BIT( 21 ),    // Server forces this value on all clients.

    /* System/Module Flags - for organization */

 // CVAR_ENGINE         = BIT( 22 ),    // Engine subsystem
 // CVAR_INPUT          = BIT( 23 ),    // Input subsystem
 // CVAR_RENDER         = BIT( 24 ),    // Renderer subsystem
 // CVAR_SOUND          = BIT( 25 ),    // Sound subsystem
 // CVAR_GUI            = BIT( 26 ),    // GUI subsystem
 // CVAR_TOOL           = BIT( 27 ),    // Tool subsystem
 // CVAR_GAME           = BIT( 28 ),    // Game logic subsystem

} cvar_type_t;

#define CVAR_TYPE_MASK ( CVAR_BOOL | CVAR_INT | CVAR_FLOAT | CVAR_STR | CVAR_BUF | CVAR_REF | CVAR_USR )
#define CVAR_PROT_MASK ( CVAR_INIT | CVAR_ROM | CVAR_LATCH | CVAR_CHEAT )
#define CVAR_LIFE_MASK ( CVAR_RUNTIME | CVAR_NORESTART | CVAR_ARCHIVE )
#define CVAR_VIS_MASK  ( CVAR_DEVONLY | CVAR_HIDDEN )
#define CVAR_NET_MASK  ( CVAR_NETSYNC | CVAR_USERINFO | CVAR_SERVERINFO | CVAR_SYSTEMINFO )
#define CVAR_ALL       ( CVAR_TYPE_MASK | CVAR_PROT_MASK | CVAR_LIFE_MASK | CVAR_VIS_MASK | CVAR_NET_MASK )

/*==============================================================================================

    Cvar State Flags (modified at runtime)

==============================================================================================*/

typedef enum cvar_flag_e
{
    CVAR_MODIFIED       = BIT( 0 ),     // Set when the cvar's value has been changed.
    CVAR_LATCHED        = BIT( 1 ),     // Has a value currently latched.
    CVAR_CALLBACK       = BIT( 2 ),     // Has a callback function that triggers on change.
    CVAR_USER_CREATED   = BIT( 3 ),     // Was at one point a user created variable.

} cvar_flag_t;

typedef enum cvar_apply_e
{
    CVAR_APPLY_IMMEDIATE,               // Apply now.
    CVAR_APPLY_RESTART,                 // Requires full engine restart
    CVAR_APPLY_SUBSYS,                  // Requires subsystem restart (renderer/audio/etc)
    CVAR_APPLY_CALLBACK,                // Custom logic (per-variable)
    CVAR_APPLY_IGNORE,                  // No effect (runtime only)

} cvar_apply_t;

/*==============================================================================================

    CVar Structure - 32-byte aligned structure

    All strings stored as u16 offsets into string pool for hot-reload safety.
    Uses tagged union for different data types.

==============================================================================================*/

__declspec(align(8))            // Ensure 8-byte alignment for performance
typedef struct cvar_s
{
    u16         name;           // String pool offset to variable name
    u16         desc;           // String pool offset to description
    u16         flag;           // Runtime modification flags (cvar_flags_t)
    u16         callback_id;    // Callback index; 0xFFFF = none.

    cvar_type_t type;           // Bitmask of flags from `cvar_type_t`.

    union
    {
        /* CVAR_BOOL */
        struct { bool value, latch, reset; } b;

        /* CVAR_INT */
        struct { i32 value, latch, reset, min, max; } i;

        /* CVAR_FLOAT */
        struct { f32 value, latch, reset, min, max; } f;

        /* CVAR_STR - fixed string table with indexed selection */
        struct {
            u16 base;           // String pool offset to start of list
            u16 count;          // Number of strings in list
            u16 width;          // Bytes per string slot (aligned)
            u16 value;          // Current index
            u16 latch;          // Latched index
            u16 reset;          // Default index
        } s;

        /* CVAR_BUF - writable string buffer of fixed size */
        struct {
            u16 buf;            // String pool offset to buffer
            u16 reset;          // String pool offset to default value
            u16 size;           // Buffer capacity in bytes
        } w;

        /* CVAR_REF - read-only string reference */
        struct { u16 value; } r;

        /* CVAR_USR - user-defined string with string value */
        struct {
            u16 value_offset;   // User cvar stores an offset to the user string pool
            u16 bucket_index;   // Bucket index for its allocation, enabling it to be freed
        } u;
    };

} cvar_t;

/* ensure the struct size is maintained.*/
// static_assert( sizeof( cvar_t ) == 32, "cvar_t must be 32 bytes" );

/*==============================================================================================

    Cvar Functions

==============================================================================================*/

typedef void (*cvar_callback_fn)( cvar_t* cv );

/*==============================================================================================
    Callback Functions
==============================================================================================*/

// TODO:    make module id use a module specific function call.
//          e.g. a get current module id from module system.

uint16_t    cvar_callback_register              ( cvar_t* cv, cvar_callback_fn fn, i32 module_id );
void        cvar_callback_unregister            ( cvar_t* cv );
void        cvar_callback_unregister_by_module  ( i32 module_id );
void        cvar_callback_invoke                ( cvar_t* cv );

/*==============================================================================================
    Initialiation Functions
==============================================================================================*/

                                    // Initialize the cvar system
void        cvar_system_init        ( void );

                                    // Shutdown the cvar system
void        cvar_system_exit        ( void );

                                    // Compact user cvar pool to reduce fragmentation
void        cvar_compact_user_pool  ( void );

/*==============================================================================================
    Registration Functions
==============================================================================================*/

                                    // Generic registration function
cvar_t*     cvar_register_base      ( const char* name, const char* desc, u32 type );

                                    // Type-specific registration functions
cvar_t*     cvar_register_b         ( const char* name, const char* desc, bool value, u32 type );
cvar_t*     cvar_register_i         ( const char* name, const char* desc, i32 val, i32 min, i32 max, u32 type );
cvar_t*     cvar_register_f         ( const char* name, const char* desc, f32 val, f32 min, f32 max, u32 type );
cvar_t*     cvar_register_s         ( const char* name, const char* desc, const char** values, u32 count, u32 def_index, u32 type );
cvar_t*     cvar_register_w         ( const char* name, const char* desc, const char* reset, u32 size, u32 type );
cvar_t*     cvar_register_r         ( const char* name, const char* desc, const char* value, u32 type );
cvar_t*     cvar_register_u         ( const char* name, const char* value );

/*==============================================================================================
    Type Query Functions
==============================================================================================*/

                                    // Type check functions
bool        cvar_is_bool            ( const cvar_t* cv );
bool        cvar_is_int             ( const cvar_t* cv );
bool        cvar_is_float           ( const cvar_t* cv );
bool        cvar_is_str             ( const cvar_t* cv );
bool        cvar_is_buf             ( const cvar_t* cv );
bool        cvar_is_ref             ( const cvar_t* cv );
bool        cvar_is_user            ( const cvar_t* cv );

/*==============================================================================================
    Lookup Functions
==============================================================================================*/

cvar_t*     cvar_find               ( const char* name );
cvar_t*     cvar_get_by_index       ( u32 index );
u32         cvar_get_count          ( void );

/*==============================================================================================
    Value Access
==============================================================================================*/

const char* cvar_get_name           ( const cvar_t* cv );
const char* cvar_get_desc           ( const cvar_t* cv );
bool        cvar_get_bool           ( const cvar_t* cv );
i32         cvar_get_int            ( const cvar_t* cv );
f32         cvar_get_float          ( const cvar_t* cv );
const char* cvar_get_string         ( const cvar_t* cv );
const char* cvar_get_string_from_id ( const cvar_t* cv, i32 value_id );

/*==============================================================================================
    Value Modification
==============================================================================================*/

                                    // Set cvar to default value
void        cvar_reset              ( cvar_t* cv );

                                    // Set all cvars to default values
void        cvar_reset_all          ( void );

                                    // Apply all latched cvar changes
void        cvar_apply_latched      ( void );

                                    // Clear modified flag on all cvars
void        cvar_clear_modified     ( void );

/*==============================================================================================
    Value Modification
==============================================================================================*/

                                    // Set cvar value by name, returns success/failure
bool        cvar_set_value          ( const char* name, const char* value );

                                    // Get cvar string value by name.
const char* cvar_get_value          ( const char* name );

/*==============================================================================================
    Cvar Output
==============================================================================================*/

                                    /* Print cvar value to output */
void        cvar_print_value        ( const cvar_t* cv );

                                    /* Print cvar flags to output */
void        cvar_print_flags        ( const cvar_t* cv );

/*==============================================================================================
    Cvar Commands
==============================================================================================*/

                                    /* Register all cvar console commands */
void        cvar_register_commands  ( void );

                                    /* Set a cvar value (create user var if not found) */
void        cmd_set                 ( int argc, char** argv );

                                    /* Same as "set" nit mark for archiving to config file */
void        cmd_seta                ( int argc, char** argv );

                                    /* Toggle a boolean cvar */
void        cmd_toggle              ( int argc, char** argv );

                                    /* Reset a cvar to default value */
void        cmd_reset               ( int argc, char** argv );

                                    /* Reset all cvars to defaults */
void        cmd_reset_all           ( int argc, char** argv );

                                    /* Apply all latched cvar changes */
void        cmd_apply_latched       ( int argc, char** argv );

                                    /* List all modified cvars */
void        cmd_cvar_modified       ( int argc, char** argv );

                                    /* Display detailed cvar information */
void        cmd_cvarinfo            ( int argc, char** argv );

                                    /* List all cvars with optional filtering */
void        cmd_cvarlist            ( int argc, char** argv );

                                    /* Console command: writeconfig [filename] */
void        cmd_writeconfig         ( int argc, char** argv );

/*==============================================================================================
    Cvar Config
==============================================================================================*/

                                    /* Write all archived cvars to a config file */
bool        cvar_write_config       ( const char* filename, u32 type_filter );

                                    /* Load and execute cvar commands from a config file */
bool        cvar_exec_config        ( const char* filename );

                                    /* Load default config sequence (default.cfg, config.cfg, autoexec.cfg) */
void        cvar_load_defaults      ( void );

                                    /* Save current config to config.cfg */
void        cvar_save_config        ( void );

//                                  /* Process command-line arguments for cvar settings (+set, +seta) */
// int         cvar_process_args       ( int argc, char** argv, int start_index );

                                    /* Console command: exec <filename> */
// void        cmd_exec                ( int argc, char** argv );
// 
                                    /* Register config system console commands */
void        cvar_register_commands  ( void );

/*==============================================================================================
    Command Buffer
==============================================================================================*/

/*============================================================================================*/
#endif // CVAR_HEADER_H
