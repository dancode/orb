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

==============================================================================================*/

#pragma once
#include "orb.h"

/*==============================================================================================

    String Pool - Compact String Storage

==============================================================================================*/

typedef struct string_pool_s
{
    char*   data;        // Linear heap for all strings
    u32     used;        // Bytes currently used
    u32     capacity;    // Bytes allocated
    u32     maximum;     // Maximum bytes allowed (0xFFFE)

} string_pool_t;

/*==============================================================================================
 
    Cvar Type Flags - Define its type and behavior

==============================================================================================*/

typedef enum cvar_type_e
{
    /* Data Types - mutually exclusive base types */

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

    /* Lifetime Flags */

    CVAR_RUNTIME        = BIT( 11 ),    // Not persisted, cleared on restart
    CVAR_NORESTART      = BIT( 12 ),    // Value is protected from being reset on a restart.

    /* Config Flags */

    CVAR_ARCHIVE        = BIT( 14 ),    // Saved to configuration files (e.g., config.cfg).

    /* Visibility Flags */

    CVAR_DEVONLY        = BIT( 15 ),    // Hidden from retail/release builds.
    CVAR_HIDDEN         = BIT( 16 ),    // Hidden from console auto-completion and `cvarlist`.

    /* Network Flags - for multiplayer synchronization */
                        
    CVAR_NETSYNC        = BIT( 17 ),    // This cvar needs to be synchronized between server and clients.
    CVAR_USERINFO       = BIT( 18 ),    // Client info sent to the server on connect (e.g., name, skin).
    CVAR_SERVERINFO     = BIT( 19 ),    // Server info sent to clients on connect (e.g., mapname, hostname).
    CVAR_SYSTEMINFO     = BIT( 20 ),    // Server forces this value on all clients.

    /* System/Module Flags - for organization */

    CVAR_ENGINE         = BIT( 21 ),    // Engine subsystem
    CVAR_INPUT          = BIT( 22 ),    // Input subsystem
    CVAR_RENDER         = BIT( 23 ),    // Renderer subsystem
    CVAR_SOUND          = BIT( 24 ),    // Sound subsystem
    CVAR_GUI            = BIT( 25 ),    // GUI subsystem
    CVAR_TOOL           = BIT( 26 ),    // Tool subsystem
    CVAR_GAME           = BIT( 27 ),    // Game logic subsystem

} cvar_type_t;

#define CVAR_TYPE_MASK ( CVAR_BOOL | CVAR_INT | CVAR_FLOAT | CVAR_STR | CVAR_BUF | CVAR_REF | CVAR_USR )
#define CVAR_PROT_MASK ( CVAR_INIT | CVAR_ROM | CVAR_LATCH | CVAR_CHEAT )

/*============================================================================================== 

    Cvar State Flags (modified at runtime)

==============================================================================================*/

typedef enum cvar_flags_e
{
    CVAR_MODIFIED       = BIT( 0 ),     // Set when the cvar's value has been changed.
    CVAR_LATCHED        = BIT( 1 ),     // Has a `CVAR_LATCH`flag, indicating a restart is needed when modified.
    CVAR_CALLBACK       = BIT( 2 ),     // Has a callback function that triggers on change.
    CVAR_USER_CREATED   = BIT( 3 ),     // Created by the user via console, not predefined.

} cvar_flags_t;

/*============================================================================================== 

    CVar Structure - 32-byte aligned structure
    
    All strings stored as u16 offsets into string pool for hot-reload safety.
    Uses tagged union for different data types.

==============================================================================================*/

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
        struct { i32 value, min, max, latch, reset; } i;

        /* CVAR_FLOAT */
        struct { f32 value, min, max, latch, reset; } f;

        /* CVAR_STR - fixed string list with indexed selection */
        struct { 
            u16 base;           // String pool offset to start of list
            u16 count;          // Number of strings in list
            u16 width;          // Bytes per string slot (aligned)
            u16 value;          // Current index
            u16 latch;          // Latched index
            u16 reset;          // Default index
        } s;

        /* CVAR_BUF - writable string buffer */
        struct { 
            u16 buf;            // String pool offset to buffer
            u16 reset;          // String pool offset to default value
            u16 size;           // Buffer capacity in bytes
        } w;

        /* CVAR_REF - read-only string reference */
        struct { u16 value; } r;

        /* CVAR_USR - user-defined string */
        struct { 
            u16 value_offset;   // User cvar stores an offset to the user string pool
            u16 bucket_index;   // Bucket index for its allocation, enabling it to be freed
        } u;

        /* CVAR_USR - user-defined string */
        // struct { u16 value; } u;
    };

} cvar_t;

// Ensure the struct size is maintained.
static_assert( sizeof( cvar_t ) == 32, "cvar_t must be 32 bytes" );

/*============================================================================================== 

    Cvar Functions 

==============================================================================================*/

typedef void (*cvar_callback_fn)( cvar_t* cv );

/*==============================================================================================
    Callback Functions
==============================================================================================*/

uint16_t    cvar_callback_register              ( cvar_t* cv, cvar_callback_fn fn, i32 module_id );    
void        cvar_callback_unregister_by_module  ( i32 module_id );    
void        cvar_callback_invoke                ( cvar_t* cv );

/*==============================================================================================
    Initialiation Functions
==============================================================================================*/

void        cvar_system_init        ( void );
void        cvar_system_exit        ( void );

/*==============================================================================================
    Registration Funcitons
==============================================================================================*/

cvar_t*     cvar_register_base      ( const char* name, const char* desc, u32 type );
cvar_t*     cvar_register_b         ( const char* name, const char* desc, bool value, u32 type );
cvar_t*     cvar_register_i         ( const char* name, const char* desc, i32 val, i32 min, i32 max, u32 type );
cvar_t*     cvar_register_f         ( const char* name, const char* desc, f32 val, f32 min, f32 max, u32 type );
cvar_t*     cvar_register_s         ( const char* name, const char* desc, const char** values, u32 count, u32 def_index, u32 type );
cvar_t*     cvar_register_w         ( const char* name, const char* desc, const char* reset, u32 size, u32 type );
cvar_t*     cvar_register_r         ( const char* name, const char* desc, const char* value, u32 type );

/*==============================================================================================
    Type Query Functions
==============================================================================================*/

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
i32         cvar_get_int            ( const cvar_t* cv );
f32         cvar_get_float          ( const cvar_t* cv );
const char* cvar_get_string         ( const cvar_t* cv );

/*==============================================================================================
    Value Modification
==============================================================================================*/

void        cvar_reset              ( cvar_t* cv );
void        cvar_reset_all          ( void );

void        cvar_apply_latched      ( void );
void        cvar_clear_modified     ( void );

/*==============================================================================================
    Value Modification
==============================================================================================*/

bool        cvar_set_value          ( const char* name, const char* value );
const char* cvar_get_value          ( const char* name );

/*==============================================================================================
    Commands
==============================================================================================*/

void        cmd_print_cvar_value    ( const cvar_t* cv );
void        cmd_print_cvar_flags    ( const cvar_t* cv );

void        cmd_set                 ( int argc, char** argv );
void        cmd_seta                ( int argc, char** argv );
void        cmd_toggle              ( int argc, char** argv );
void        cmd_reset               ( int argc, char** argv );
void        cmd_reset_all           ( int argc, char** argv );
void        cmd_apply_latched       ( int argc, char** argv ); // debug testing
void        cmd_cvar_modified       ( int argc, char** argv );
void        cmd_cvarinfo            ( int argc, char** argv );
void        cmd_cvarlist            ( int argc, char** argv );

void        cvar_register_commands  ( void );

/*============================================================================================== 

    Debug API - For module and natvis visualization

==============================================================================================*/

typedef struct core_debug_api_t
{
    string_pool_t* string_pool;
    string_pool_t* user_string_pool;

} core_debug_api_t;

extern core_debug_api_t* core_debug_get_api( void );

/*============================================================================================*/
#endif // CVAR_HEADER_H