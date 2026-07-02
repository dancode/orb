/*==============================================================================================

    runtime_service/gui/dock/gui_dock_serialize.c -- Dock layout persistence (Phase 3).

    Serialize a viewport's dock tree to a small ASCII blob and rebuild it later (across a restart
    when the host writes the blob to disk).  The format is a pre-order line stream -- each node
    self-describes and its children follow, so it parses with a one-pass line cursor and no lookahead:

        ORBDOCK 1                 (header + version)
        S <axis> <ratio>          (internal split: axis 0=X/1=Y, then its two children)
        L <active_tab> <count>     (leaf, followed by `count` tab lines)
        T <id_hex> <name>          (one tab: the window id and its display name)

    The window id is stored explicitly (not re-hashed from the name) so a "Title##key" window -- whose
    stored name is the stripped visible span -- restores to the exact id window_begin produces.  Node
    ids are NOT stored: they are runtime handles, freshly assigned on load.  gui owns only the blob;
    the host owns the file I/O (read at startup, write on change).

    Needs gui_dock_core.c's node pool (dock_node_alloc / dock_node_free) already in scope; included
    after it (and after gui_dock.c / gui_dock_drag.c -- order among the three doesn't matter, this
    file calls nothing of theirs).

==============================================================================================*/
// clang-format off

/* Bounds-tracking text appender: writes while within cap but always counts the bytes a full write
   would need, so gui_dock_save can report the required size like snprintf. */
typedef struct { char* p; u32 cap; u32 len; } dock_writer_t;

static void
dw_emit( dock_writer_t* w, const char* s, u32 n )
{
    for ( u32 i = 0; i < n; ++i )
    {
        if ( w->len + 1u < w->cap )
            w->p[ w->len ] = s[ i ];
        w->len++;
    }
}

static void
dock_serialize_node( dock_writer_t* w, gui_dock_node_t* n )
{
    char line[ 64 ];
    if ( !n )
        return;
    if ( n->split == DOCK_SPLIT_NONE )
    {
        int k = snprintf( line, sizeof line, "L %u %u\n", n->active_tab, n->tab_count );
        dw_emit( w, line, (u32)k );
        for ( u32 t = 0; t < n->tab_count; ++t )
        {
            k = snprintf( line, sizeof line, "T %08x ", n->tabs[ t ] );
            dw_emit( w, line, (u32)k );
            dw_emit( w, n->names[ t ], (u32)strlen( n->names[ t ] ) );
            dw_emit( w, "\n", 1 );
        }
    }
    else
    {
        int k = snprintf( line, sizeof line, "S %d %.4f\n",
                          ( n->split == DOCK_SPLIT_Y ) ? 1 : 0, n->ratio );
        dw_emit( w, line, (u32)k );
        dock_serialize_node( w, n->child[ 0 ] );
        dock_serialize_node( w, n->child[ 1 ] );
    }
}

/* Serialize viewport vp's dock tree into buf (NUL-terminated, truncated to bufsz).  Returns the byte
   count a full write needs (excluding the NUL), so a caller can size the buffer like snprintf. */
u32
gui_dock_save( gui_vp_t vp, char* buf, u32 bufsz )
{
    dock_writer_t w = { buf, bufsz, 0u };
    if ( vp < 0 || vp >= (gui_vp_t)g_ctx->max_viewports )
    {
        if ( bufsz ) buf[ 0 ] = '\0';
        return 0u;
    }
    dw_emit( &w, "ORBDOCK 1\n", 10u );
    dock_serialize_node( &w, g_ctx->viewports[ vp ].dock_root );
    if ( bufsz )
        buf[ ( w.len < bufsz ) ? w.len : bufsz - 1u ] = '\0';
    return w.len;
}

/* Free every node belonging to viewport vp and clear its dock_root -- the load path's clean slate. */
static void
dock_free_viewport_tree( u32 vp )
{
    for ( u32 i = 0; i < s_dock_node_count; ++i )
        if ( s_dock_nodes[ i ].id != 0 && s_dock_nodes[ i ].viewport == vp )
            dock_node_free( &s_dock_nodes[ i ] );
    g_ctx->viewports[ vp ].dock_root = NULL;
}

/* Line cursor over the blob: copy the next line (sans newline) into out, advance past it; false at
   end of input. */
typedef struct { const char* p; } dock_reader_t;

static bool
dr_line( dock_reader_t* r, char* out, u32 cap )
{
    if ( !*r->p )
        return false;
    u32 i = 0;
    while ( *r->p && *r->p != '\n' )
    {
        if ( i + 1u < cap ) out[ i++ ] = *r->p;
        r->p++;
    }
    if ( *r->p == '\n' ) r->p++;
    out[ i ] = '\0';
    return true;
}

/* Recursively parse one node (and, for a split, its two children) for viewport vp. */
static gui_dock_node_t*
dock_parse_node( dock_reader_t* r, u32 vp )
{
    char line[ 128 ];
    if ( !dr_line( r, line, sizeof line ) )
        return NULL;

    if ( line[ 0 ] == 'S' )
    {
        int   axis  = 0;
        float ratio = 0.5f;
        if ( sscanf( line + 1, " %d %f", &axis, &ratio ) != 2 )
            return NULL;
        gui_dock_node_t* n = dock_node_alloc( vp );
        if ( !n )
            return NULL;
        n->split = axis ? DOCK_SPLIT_Y : DOCK_SPLIT_X;
        n->ratio = ratio;
        n->child[ 0 ] = dock_parse_node( r, vp );
        n->child[ 1 ] = dock_parse_node( r, vp );
        if ( n->child[ 0 ] ) n->child[ 0 ]->parent = n;
        if ( n->child[ 1 ] ) n->child[ 1 ]->parent = n;
        return n;
    }

    if ( line[ 0 ] == 'L' )
    {
        unsigned active = 0, count = 0;
        if ( sscanf( line + 1, " %u %u", &active, &count ) != 2 )
            return NULL;
        gui_dock_node_t* n = dock_node_alloc( vp );
        if ( !n )
            return NULL;
        n->split = DOCK_SPLIT_NONE;
        if ( count > GUI_DOCK_TABS_MAX ) count = GUI_DOCK_TABS_MAX;
        for ( u32 t = 0; t < count; ++t )
        {
            if ( !dr_line( r, line, sizeof line ) || line[ 0 ] != 'T' )
                break;
            unsigned id = 0;
            int      adv = 0;
            sscanf( line + 1, " %x%n", &id, &adv );
            const char* name = line + 1 + adv;
            if ( *name == ' ' ) name++;            /* the single space before the name */
            u32 ln = (u32)strlen( name );
            if ( ln >= GUI_DOCK_NAME_CAP ) ln = GUI_DOCK_NAME_CAP - 1u;
            n->tabs[ n->tab_count ] = (gui_id_t)id;
            memcpy( n->names[ n->tab_count ], name, ln );
            n->names[ n->tab_count ][ ln ] = '\0';
            n->tab_count++;
        }
        n->active_tab = ( active < n->tab_count ) ? active : 0u;
        return n;
    }

    return NULL;
}

/* Replace viewport vp's dock tree with the one encoded in `text` (from a prior gui_dock_save).
   Returns true if the header is valid.  CAUTION: this frees + rebuilds the tree, so call it at a SAFE
   point -- between frames, or at the top of the build before any docked window is emitted -- never
   from inside a docked window's body (its node would be freed mid-render). */
bool
gui_dock_load( gui_vp_t vp, const char* text )
{
    if ( vp < 0 || vp >= (gui_vp_t)g_ctx->max_viewports || !text )
        return false;

    dock_reader_t r = { text };
    char header[ 32 ];
    if ( !dr_line( &r, header, sizeof header ) || strncmp( header, "ORBDOCK", 7 ) != 0 )
        return false;

    dock_free_viewport_tree( (u32)vp );
    g_ctx->viewports[ vp ].dock_root = dock_parse_node( &r, (u32)vp );
    return true;
}

// clang-format on
/*============================================================================================*/
