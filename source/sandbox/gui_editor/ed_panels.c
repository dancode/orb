/*==============================================================================================

    sandbox/gui_editor/ed_panels.c -- The four data panels: Hierarchy, Inspector, Console,
    Assets.  Each is one dockable window over the stub engine state (ed.h / ed_engine.c).
    Included by sb_gui_editor.c (unity build).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Hierarchy -- entity list with selection and a right-click context menu.
==============================================================================================*/

static const char* s_kind_tag[ ED_KIND_COUNT ] = { "[M]", "[L]", "[C]" };

void
ed_hierarchy_panel( void )
{
    if ( !gui()->window_begin( "Hierarchy", GUI_WIN_NONE ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* Creation row. */
    gui()->row3( 0.33f, 0.33f, 0.34f );
    if ( gui()->small_button( "+ Mesh"   ) ) g_ed.selected = ed_entity_add( ED_KIND_MESH );
    if ( gui()->small_button( "+ Light"  ) ) g_ed.selected = ed_entity_add( ED_KIND_LIGHT );
    if ( gui()->small_button( "+ Camera" ) ) g_ed.selected = ed_entity_add( ED_KIND_CAMERA );

    gui()->stack();
    gui()->separator();

    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        ed_entity_t* e = &g_ed.entities[ i ];
        if ( !e->used )
            continue;

        gui()->push_id_int( i );

        char label[ 96 ];
        snprintf( label, sizeof( label ), "%s %s%s",
                  s_kind_tag[ e->kind ], e->name, e->active ? "" : "  (inactive)" );

        bool sel = ( g_ed.selected == i );
        if ( gui()->selectable( label, &sel ) )
            g_ed.selected = i;

        /* Right-click context menu on the row just emitted. */
        if ( gui()->popup_context_item_begin( "entity_ctx" ) )
        {
            bool dummy = false;
            if ( gui()->menu_item( "Duplicate", NULL, &dummy ) )
                g_ed.selected = ed_entity_duplicate( i );
            if ( gui()->menu_item( e->active ? "Deactivate" : "Activate", NULL, &dummy ) )
                e->active = !e->active;
            gui()->separator();
            if ( gui()->menu_item( "Delete", NULL, &dummy ) )
                ed_entity_delete( i );
            gui()->popup_end();
        }

        gui()->pop_id();
    }

    gui()->window_end();
}

/*==============================================================================================
    Inspector -- edit the selected entity: identity, transform, per-kind components.
==============================================================================================*/

void
ed_inspector_panel( void )
{
    if ( !gui()->window_begin( "Inspector", GUI_WIN_NONE ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    i32 sel = g_ed.selected;
    if ( sel < 0 || sel >= ED_MAX_ENTITIES || !g_ed.entities[ sel ].used )
    {
        gui()->text_disabled( "No entity selected." );
        gui()->text_disabled( "Pick one in the Hierarchy." );
        gui()->window_end();
        return;
    }

    ed_entity_t* e = &g_ed.entities[ sel ];

    /* Identity. */
    gui()->form( GUI_LABEL_LEFT, 70.0f );
    gui()->input_text( "Name", e->name, sizeof( e->name ) );
    static const char* kind_items[ ED_KIND_COUNT ] = { "Mesh", "Light", "Camera" };
    gui()->combo( "Kind", &e->kind, kind_items, ED_KIND_COUNT );

    gui()->stack();
    gui()->checkbox( "Active", &e->active );

    /* Transform. */
    if ( gui()->collapsing_header( "Transform" ) )
    {
        gui()->form( GUI_LABEL_LEFT, 70.0f );
        gui()->drag_float3( "Position", e->pos,   0.05f, -50.0f,  50.0f, "%.2f" );
        gui()->drag_float3( "Rotation", e->rot,   0.50f, -360.0f, 360.0f, "%.1f" );
        gui()->drag_float3( "Scale",    e->scale, 0.02f,   0.05f, 10.0f, "%.2f" );
        gui()->stack();
    }

    /* Per-kind component block. */
    switch ( e->kind )
    {
        case ED_KIND_MESH:
            if ( gui()->collapsing_header( "Mesh Renderer" ) )
            {
                gui()->color_edit4( "Color", e->color, GUI_COLOR_EDIT_NONE );
                gui()->checkbox( "Spin in play mode", &e->spin );
                gui()->form( GUI_LABEL_LEFT, 70.0f );
                gui()->slider_float( "Spin", &e->spin_speed, 0.0f, 360.0f );
                gui()->slider_float( "Orbit", &e->orbit_radius, 0.0f, 10.0f );
                gui()->stack();
            }
            break;

        case ED_KIND_LIGHT:
            if ( gui()->collapsing_header( "Light" ) )
            {
                gui()->color_edit4( "Color", e->color, GUI_COLOR_EDIT_NONE );
                gui()->form( GUI_LABEL_LEFT, 70.0f );
                gui()->slider_float( "Intensity", &e->intensity, 0.0f, 4.0f );
                gui()->stack();
            }
            break;

        case ED_KIND_CAMERA:
            if ( gui()->collapsing_header( "Camera" ) )
            {
                gui()->form( GUI_LABEL_LEFT, 70.0f );
                gui()->slider_float( "FOV", &e->fov, 20.0f, 120.0f );
                gui()->stack();
            }
            break;

        default:
            break;
    }

    gui()->separator();
    gui()->row2( 0.5f, 0.5f );
    if ( gui()->button( "Duplicate" ) )
        g_ed.selected = ed_entity_duplicate( sel );
    if ( gui()->button( "Delete" ) )
        ed_entity_delete( sel );

    gui()->window_end();
}

/*==============================================================================================
    Console -- level-filtered scrolling log of the ring in ed_engine.c.
==============================================================================================*/

void
ed_console_panel( void )
{
    if ( !gui()->window_begin( "Console", GUI_WIN_NOSCROLL ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* Filter strip. */
    gui()->bar();
    if ( gui()->small_button( "Clear" ) )
        g_ed.log_count = 0;
    gui()->checkbox( "Info",   &g_ed.log_show[ ED_LOG_INFO  ] );
    gui()->checkbox( "Warn",   &g_ed.log_show[ ED_LOG_WARN  ] );
    gui()->checkbox( "Errors", &g_ed.log_show[ ED_LOG_ERROR ] );
    if ( gui()->small_button( "Test" ) )
    {
        ed_logf( ED_LOG_INFO,  "Test info message" );
        ed_logf( ED_LOG_WARN,  "Test warning message" );
        ed_logf( ED_LOG_ERROR, "Test error message" );
    }

    /* Scrolling body: oldest first; the ring drops the oldest entries once full. */
    gui()->stack();
    if ( gui()->child_begin( "##log", 0.0f, 0.0f, GUI_WIN_NONE ) )
    {
        gui()->stack();

        static const u32 level_col[ 3 ] =
        {
            GUI_COLOR( 0xC8, 0xC8, 0xD0, 0xFF ),   /* info  -- neutral    */
            GUI_COLOR( 0xFF, 0xC0, 0x40, 0xFF ),   /* warn  -- amber      */
            GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ),   /* error -- red        */
        };

        u32 count = g_ed.log_count < ED_LOG_MAX ? g_ed.log_count : ED_LOG_MAX;
        u32 first = g_ed.log_count - count;
        for ( u32 k = 0; k < count; k++ )
        {
            const ed_log_entry_t* le = &g_ed.log[ ( first + k ) % ED_LOG_MAX ];
            if ( !g_ed.log_show[ le->level ] )
                continue;
            char line[ ED_LOG_MSG_MAX + 24 ];
            snprintf( line, sizeof( line ), "[%7.2f] %s", le->time, le->msg );
            gui()->text_colored( level_col[ le->level ], line );
        }
    }
    gui()->child_end();

    gui()->window_end();
}

/*==============================================================================================
    Assets -- sortable table over the static fake database.
==============================================================================================*/

static void
ed_asset_sort_value( i32 row, i32 col, gui_table_sort_value_t* out, void* user )
{
    UNUSED( user );
    const ed_asset_t* a = &ed_assets[ row ];
    switch ( col )
    {
        case 0: out->str = a->name;                   break;
        case 1: out->str = a->type;                   break;
        case 2: out->num = a->size_kb; out->is_num = true; break;
        default: break;
    }
}

void
ed_assets_panel( void )
{
    if ( !gui()->window_begin( "Assets", GUI_WIN_NOSCROLL ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* Display-order index array, reordered by the active header sort. */
    static i32  s_order[ 64 ];
    static bool s_order_init = false;
    if ( !s_order_init )
    {
        for ( i32 i = 0; i < ed_asset_count; i++ )
            s_order[ i ] = i;
        s_order_init = true;
    }

    if ( gui()->table_begin( "##assets", 3,
                             GUI_TABLE_BORDERS_OUTER | GUI_TABLE_BORDERS_V | GUI_TABLE_ROW_STRIPES
                             | GUI_TABLE_SORTABLE | GUI_TABLE_SCROLL_Y | GUI_TABLE_RESIZABLE, 0.0f ) )
    {
        gui()->table_setup_column( "Name", GUI_TABLE_COL_STRETCH, 0.0f );
        gui()->table_setup_column( "Type", GUI_TABLE_COL_FIXED,   90.0f );
        gui()->table_setup_column( "Size", GUI_TABLE_COL_FIXED,   90.0f );
        gui()->table_headers_row();

        gui()->table_sort_order( s_order, ed_asset_count, ed_asset_sort_value, NULL, NULL );

        for ( i32 k = 0; k < ed_asset_count; k++ )
        {
            const ed_asset_t* a = &ed_assets[ s_order[ k ] ];
            gui()->table_next_row( 0.0f );
            if ( gui()->table_next_column() ) gui()->text( a->name );
            if ( gui()->table_next_column() ) gui()->text( a->type );
            if ( gui()->table_next_column() ) gui()->textf( "%.0f KB", a->size_kb );
        }
        gui()->table_end();
    }

    gui()->window_end();
}

// clang-format on
/*============================================================================================*/
