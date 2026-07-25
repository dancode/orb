/*==============================================================================================

    runtime_service/gui/chrome/dock/gui_dock_serialize.c -- Dock layout persistence.

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
    if ( n->split == GUI_DOCK_SPLIT_NONE )
    {
        int k = fmt_snprintf( line, sizeof line, "L %u %u\n", n->active_tab, n->tab_count );
        dw_emit( w, line, (u32)k );
        for ( u32 t = 0; t < n->tab_count; ++t )
        {
            k = fmt_snprintf( line, sizeof line, "T %08x ", n->tabs[ t ] );
            dw_emit( w, line, (u32)k );
            dw_emit( w, n->names[ t ], (u32)strlen( n->names[ t ] ) );
            dw_emit( w, "\n", 1 );
        }
    }
    else
    {
        int k = fmt_snprintf( line, sizeof line, "S %d %.4f\n",
                          ( n->split == GUI_DOCK_SPLIT_Y ) ? 1 : 0, n->ratio );
        dw_emit( w, line, (u32)k );
        dock_serialize_node( w, dock_at( n->child[ 0 ] ) );
        dock_serialize_node( w, dock_at( n->child[ 1 ] ) );
    }
}

/* Serialize viewport vp's dock tree into buf (NUL-terminated, truncated to bufsz).  Returns the byte
   count a full write needs (excluding the NUL), so a caller can size the buffer like snprintf. */
u32
gui_dock_save( gui_vp_t vp, char* buf, u32 bufsz )
{
    dock_writer_t w = { buf, bufsz, 0u };
    if ( vp >= GUI_MAX_VIEWPORTS )
    {
        if ( bufsz ) buf[ 0 ] = '\0';
        return 0u;
    }
    dw_emit( &w, "ORBDOCK 1\n", 10u );
    dock_serialize_node( &w, dock_at( s_vp_pool[ vp ].dock_root ) );
    if ( bufsz )
        buf[ ( w.len < bufsz ) ? w.len : bufsz - 1u ] = '\0';
    return w.len;
}

/* Free every TREE node belonging to viewport vp and clear its dock_root -- the load path's clean
   slate.  Floating tab groups (gui_dock_float.c) share the pool but are not part of the tree the
   blob describes, so a load must leave them standing. */
static void
dock_free_viewport_tree( u32 vp )
{
    for ( u32 i = 0; i < g_ctx->dock.count; ++i )
        if ( g_ctx->dock.pool[ i ].id != 0 && g_ctx->dock.pool[ i ].viewport == vp
             && !g_ctx->dock.pool[ i ].floating )
            dock_node_free( &g_ctx->dock.pool[ i ] );
    s_vp_pool[ vp ].dock_root = GUI_DOCK_REF_NONE;

    /* Any dockspace-maximize referenced a node just freed -- session state, never serialized. */
    s_vp_pool[ vp ].dock_max_id      = 0;
    s_vp_pool[ vp ].dock_max_on      = false;
    s_vp_pool[ vp ].dock_max_settled = false;
}

/* Public DESTROY verb: free viewport vp's whole dock tree and clear its root.  Every tree-docked
   window loses its membership permanently and free-floats from its next begin, at the rect its node
   last gave it (window_begin_docked mirrors node geometry onto the record each frame).  Distinct
   from dormancy: a tree whose dockspace merely stops being emitted is retained and revives intact
   (dock_seen_frame, core/gui_ctx.h); clear is for discarding a layout wholesale.  Same safe-point
   rule as dock_load above: call at the top of the build, never from inside a docked window's body.
   Floating tab groups are not part of the tree and stay standing. */
void
gui_dock_clear( gui_vp_t vp )
{
    if ( !g_ctx->dock.pool || vp >= GUI_MAX_VIEWPORTS )
        return;
    dock_free_viewport_tree( vp );
    redraw_request();   /* wholesale layout discard, typically driven from between frames */
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
        n->split = axis ? GUI_DOCK_SPLIT_Y : GUI_DOCK_SPLIT_X;
        n->ratio = ratio;
        gui_dock_node_t* c0 = dock_parse_node( r, vp );
        gui_dock_node_t* c1 = dock_parse_node( r, vp );
        if ( !c0 || !c1 )
        {
            /* Truncated / corrupt blob: a split missing a side cannot lay out, collapse, or hang a
               splitter sanely.  Heal in place -- the surviving child takes this node's slot in the
               tree (the caller links its parent), so every split in a LOADED tree has two children,
               the same invariant every runtime edit maintains. */
            dock_node_free( n );
            return c0 ? c0 : c1;
        }
        n->child[ 0 ] = dock_ref( c0 );
        n->child[ 1 ] = dock_ref( c1 );
        c0->parent    = dock_ref( n );
        c1->parent    = dock_ref( n );
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
        n->split = GUI_DOCK_SPLIT_NONE;
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
    if ( vp >= GUI_MAX_VIEWPORTS || !text )
        return false;

    dock_reader_t r = { text };
    char header[ 32 ];
    if ( !dr_line( &r, header, sizeof header ) || strncmp( header, "ORBDOCK", 7 ) != 0 )
        return false;

    dock_free_viewport_tree( vp );
    s_vp_pool[ vp ].dock_root = dock_ref( dock_parse_node( &r, vp ) );
    redraw_request();   /* a restored layout is the classic between-frames mutation: without this
                           the UI keeps replaying the pre-load geometry until something else moves */
    return true;
}

// clang-format on
/*============================================================================================*/
