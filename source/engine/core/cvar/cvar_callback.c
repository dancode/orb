/*==============================================================================================

    cvar_callback.c

    * Supports multiple callbacks per cvar with module tracking for hot reload.
    * Cvar callbacks are called on value changes.
    * Callbacks are referenced by id if a cvar has any (cvar.callback_id field)

    * Callback table linking cvars to functions.
    * Each function slot records its owning module id (from mod_current_id() at registration)
    * Hot-reload can drop exactly the pointers that are about to dangle,
    * Multiple modules can hook the same cvar.

==============================================================================================*/
// clang-format off

#define MAX_CVAR_CALLBACKS      128       // Max global callbacks
#define MAX_CVAR_FUNCS_PER_CVAR 3         // Max callbacks per cvar

// A single sentinel for every callback-related id: cv->callback_id, the per-cvar id type.
// function_id/module_id slots, and both freelist heads are all u8, so one 0xFF covers all of them.

#define CVAR_CB_NONE            0xFF

ORB_STATIC_ASSERT( MAX_CVAR_CALLBACKS < CVAR_CB_NONE, "MAX_CVAR_CALLBACKS must leave room for the CB_NONE sentinel" );

typedef struct cvar_callback_s
{
    /* The first entry of funcion_id[0] is used as a freelist link for callback struct array */

    u8 function_id  [ MAX_CVAR_FUNCS_PER_CVAR ];    // Function indices (CB_NONE = empty)
    u8 module_id    [ MAX_CVAR_FUNCS_PER_CVAR ];    // Owning module per slot (CB_NONE = host/none)

} cvar_callback_t;

// The function pointer array stores a pointer-sized encoded "next free index",
// instead of a function pointer when slot is free.

static cvar_callback_fn g_function_array[ MAX_CVAR_CALLBACKS ]; // function pointer array.
static cvar_callback_t  g_callback_table[ MAX_CVAR_CALLBACKS ]; // callback entry.
static u8               g_callback_count      = 0;               // number of used callback slots
static u8               g_function_free_head  = CVAR_CB_NONE;     // g_function_array freelist head
static u8               g_callback_free_head  = CVAR_CB_NONE;     // freed callback-table entries (next in function_id[0])

/*============================================================================================*/
/* Initialize callback system */

void
cvar_callbacks_init()
{
    g_callback_count = 0;

    /* Clear all to invalid callback */
    memset( g_callback_table, 0xFF, sizeof( g_callback_table ) );

    /* Initialize intrusive free list in g_function_array */
    for ( u16 i = 0; i < MAX_CVAR_CALLBACKS - 1; i++ )
    {
        /* Encode next index as pointer value */
        g_function_array[ i ] = ( cvar_callback_fn )( uintptr_t )( i + 1 );
    }
    g_function_array[ MAX_CVAR_CALLBACKS - 1 ] = ( cvar_callback_fn )( uintptr_t )CVAR_CB_NONE;
    g_function_free_head                       = 0; /* Head points to first free slot */
    g_callback_free_head                       = CVAR_CB_NONE;
}

/*============================================================================================*/
/* Allocate a free function slot (O(1)) */

static inline u8
alloc_function_slot( cvar_callback_fn fn )
{
    if ( g_function_free_head == CVAR_CB_NONE )
        return CVAR_CB_NONE; /* No free slots */

    u8 slot = g_function_free_head;

    /* Pop from free list */
    g_function_free_head = ( u8 )( uintptr_t )g_function_array[ slot ];

    /* Assign actual function */
    g_function_array[ slot ] = fn;

    return slot;
}

/*============================================================================================*/
/* Free a function slot (O(1)) */

static inline void
free_function_slot( u8 slot )
{
    if ( slot >= MAX_CVAR_CALLBACKS )
        return;

    /* Push this slot back into the free list */
    g_function_array[ slot ] = ( cvar_callback_fn )( uintptr_t )g_function_free_head;
    g_function_free_head     = slot;
}

/*============================================================================================*/
/* Module id provider - injected by the host via core_wire_mod_callbacks() so core never
   links against the mod library. NULL (tools, sandboxes without mod) means every
   registration is host-owned (-1 -> CVAR_CB_NONE, never matched by unload sweeps). */

static cvar_module_id_fn g_cvar_module_id_fn = NULL;

void
cvar_set_module_id_fn( cvar_module_id_fn fn )
{
    g_cvar_module_id_fn = fn;
}

/*============================================================================================*/
/* Register callback for cvar. The owning module id is resolved automatically: inside a
   DLL's init()/reload() this is that module's id; host code outside any lifecycle call
   gets -1 -> CVAR_CB_NONE (never matched by unload sweeps). */

u8
cvar_callback_register( cvar_t* cv, cvar_callback_fn fn )
{
    if ( !cv || !fn )
        return CVAR_CB_NONE;

    const i32 module_id = g_cvar_module_id_fn ? g_cvar_module_id_fn() : -1;

    /* Allocate callback slot if needed (reuse freed entries first) */
    if ( cv->callback_id == CVAR_CB_NONE )
    {
        if ( g_callback_free_head != CVAR_CB_NONE )
        {
            cv->callback_id      = g_callback_free_head;
            g_callback_free_head = g_callback_table[ g_callback_free_head ].function_id[ 0 ];
        }
        else if ( g_callback_count < MAX_CVAR_CALLBACKS )
        {
            cv->callback_id = g_callback_count++;
        }
        else
        {
            con_printf( "cvar: callback table full\n" );
            return CVAR_CB_NONE;
        }

        cv->mods |= CVAR_CALLBACK;

        cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

        for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
        {
            cb->function_id[ i ] = CVAR_CB_NONE;
            cb->module_id[ i ]   = CVAR_CB_NONE;
        }
    }

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    /* Find empty function slot */
    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        if ( cb->function_id[ i ] == CVAR_CB_NONE )
        {
            u8 slot = alloc_function_slot( fn );
            if ( slot == CVAR_CB_NONE )
            {
                con_printf( "cvar: no free callback slots available\n" );
                return CVAR_CB_NONE;
            }

            cb->function_id[ i ] = slot;
            cb->module_id[ i ]   = ( u8 )module_id;
            return cv->callback_id;
        }
    }

    con_printf( "cvar: no room for more callbacks on '%s'\n", cvar_get_name( cv ) );
    return CVAR_CB_NONE;
}

/*============================================================================================*/
/* Invoke all callbacks for cvar */

void
cvar_callback_invoke( cvar_t* cv )
{
    if ( !cv || cv->callback_id == CVAR_CB_NONE )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        u8 fid = cb->function_id[ i ];
        if ( fid != CVAR_CB_NONE && g_function_array[ fid ] )
        {
            g_function_array[ fid ]( cv );
        }
    }
}

/*============================================================================================*/
/* Utility function called by cvar_callback_unregister() and cvar_callback_unregister_by_module()
   Re-link the callback data into the freelist. */

static void
callback_table_release( cvar_t* cv )
{
    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    /* The first member of the callback_t struct (function_id[ 0 ]) acts as freelist entry */
    cb->function_id[ 0 ] = g_callback_free_head;
    g_callback_free_head = cv->callback_id;

    cv->callback_id = CVAR_CB_NONE;
    cv->mods &= ~CVAR_CALLBACK;
}

/*============================================================================================*/
/* Clear all callbacks associated with a single cvar */

void
cvar_callback_unregister( cvar_t* cv )
{
    if ( !cv || cv->callback_id == CVAR_CB_NONE )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        const u8 fid = cb->function_id[ i ];
        if ( fid != CVAR_CB_NONE )
        {
            free_function_slot( fid );
            cb->function_id[ i ] = CVAR_CB_NONE;
        }
        cb->module_id[ i ] = CVAR_CB_NONE;
    }

    callback_table_release( cv );
}

/*============================================================================================*/
/* Remove all callbacks owned by a module (fired by the mod system's unload hook).
   Only the module's own function slots are freed; callbacks other modules registered
   on the same cvar survive. The table entry is released once every slot is empty. */

void
cvar_callback_unregister_by_module( i32 module_id )
{
    int total_cvars = cvar_get_count();

    for ( int i = 0; i < total_cvars; i++ )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || cv->callback_id == CVAR_CB_NONE )
            continue;

        cvar_callback_t* cb        = &g_callback_table[ cv->callback_id ];
        bool             any_left  = false;

        for ( int k = 0; k < MAX_CVAR_FUNCS_PER_CVAR; k++ )
        {
            if ( cb->function_id[ k ] == CVAR_CB_NONE )
                continue;

            if ( cb->module_id[ k ] == ( u8 )module_id )
            {
                free_function_slot( cb->function_id[ k ] );
                cb->function_id[ k ] = CVAR_CB_NONE;
                cb->module_id[ k ]   = CVAR_CB_NONE;
            }
            else
            {
                any_left = true;
            }
        }

        if ( !any_left )
        {
            callback_table_release( cv );
        }
    }
}

/*============================================================================================*/
// clang-format on
