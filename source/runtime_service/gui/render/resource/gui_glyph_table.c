/*==============================================================================================

    runtime_service/gui/render/resource/gui_glyph_table.c -- the glyph uv table (see the header).

    The uv math here is font_slot_glyph's (draw/gui_glyph_internal.c) applied per table window
    instead of per drawn character: rebase each glyph record's page-space rect by the tenant's
    live origin, scale by the backing atlas's inverse dimensions.  Both read the same inputs, so
    an id-addressed glyph and a uv-baked one can never sample different texels.

    Included by gui_render.c after resource/gui_res_atlas.c.

==============================================================================================*/
// clang-format off

typedef struct
{
    f32 u0, v0, u1, v1;   // atlas uv rect, normalized to the font's backing atlas

} glyph_entry_t;

/* The CPU mirror -- rebuilt whole on any change, uploaded whole.  128 KB resident. */
static glyph_entry_t s_glyph_table[ GUI_GLYPH_TABLE_SLOTS ];

static rhi_buffer_t  s_glyph_buf;
static u32           s_glyph_buf_idx;
static bool          s_glyph_buf_failed;   /* creation failed once -- do not retry every latch */

/* The change signature of the last build: both atlas generations plus what each font window was
   built from.  ext_count guards the one same-tenant change a generation can miss -- a reload
   that keeps the page footprint but reshapes the ext set. */
typedef struct
{
    u32  tenant;       // 0 = the slot had no packed page
    bool sdf;          // which atlas the tenant lives in
    u32  ext_count;

} glyph_font_sig_t;

static u32              s_seen_cov_gen;
static u32              s_seen_sdf_gen;
static glyph_font_sig_t s_seen_font[ GUI_FONT_REGISTRY_MAX ];

/*==============================================================================================
    The rebuild -- mirror + upload.
==============================================================================================*/

static void
glyph_table_write_font( u32 font_id, const font_slot_t* slot )
{
    glyph_entry_t* win = &s_glyph_table[ font_id * GUI_FONT_GLYPH_TABLE_PER_FONT ];

    u32 px, py;
    f32 iw, ih;
    if ( slot->sdf_range )
    {
        res_sdf_origin( slot->atlas_tenant, &px, &py );
        iw = res_sdf_inv_w();
        ih = res_sdf_inv_h();
    }
    else
    {
        res_atlas_origin( slot->atlas_tenant, &px, &py );
        iw = res_atlas_inv_w();
        ih = res_atlas_inv_h();
    }

    /* The dense ASCII tier, then ext records up to the window's end -- the same order
       font_glyph_table_index assigns, which is what makes the ids and the entries agree. */
    u32 ext_n = slot->ext_count;
    if ( ext_n > GUI_FONT_GLYPH_TABLE_PER_FONT - ORB_FONT_CP_COUNT )
        ext_n = GUI_FONT_GLYPH_TABLE_PER_FONT - ORB_FONT_CP_COUNT;

    for ( u32 i = 0; i < ORB_FONT_CP_COUNT + ext_n; ++i )
    {
        const orb_font_glyph_t* g = ( i < ORB_FONT_CP_COUNT )
                                  ? &slot->lookup[ i ]
                                  : &slot->ext[ i - ORB_FONT_CP_COUNT ];
        win[ i ] = ( glyph_entry_t ){
            .u0 = (f32)( px + g->atlas_x ) * iw,
            .v0 = (f32)( py + g->atlas_y ) * ih,
            .u1 = (f32)( px + g->atlas_x + g->w ) * iw,
            .v1 = (f32)( py + g->atlas_y + g->h ) * ih,
        };
    }
}

static bool
glyph_table_rebuild( void )
{
    if ( s_glyph_buf_idx == 0 )
    {
        if ( s_glyph_buf_failed )
            return false;

        s_glyph_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
            .size       = sizeof( s_glyph_table ),
            .usage      = RHI_BUFFER_USAGE_STORAGE,
            .memory     = RHI_MEMORY_CPU_TO_GPU,
            .debug_name = "gui_glyph_table",
        } );
        if ( rhi_handle_valid( s_glyph_buf ) )
            s_glyph_buf_idx = rhi()->register_buffer( s_glyph_buf );
        if ( s_glyph_buf_idx == 0 )
        {
            if ( rhi_handle_valid( s_glyph_buf ) )
                rhi()->buffer_destroy( s_glyph_buf );
            s_glyph_buf        = ( rhi_buffer_t ){ 0 };
            s_glyph_buf_failed = true;
            GUI_WARN_ONCE( "glyph table buffer unavailable -- glyph-id addressing disabled "
                           "(consumers fall back to baked uvs)\n" );
            return false;
        }
    }

    memset( s_glyph_table, 0, sizeof( s_glyph_table ) );

    u32 fonts = 0;
    for ( u32 id = 0; id < GUI_FONT_REGISTRY_MAX; ++id )
    {
        const font_slot_t* slot = font_slot_ptr( id );
        if ( !slot || !slot->used || slot->atlas_tenant == 0 )
            continue;
        glyph_table_write_font( id, slot );
        ++fonts;
    }

    rhi()->buffer_write( s_glyph_buf, s_glyph_table, sizeof( s_glyph_table ), 0 );

    gui_log( GUI_LOG_INFO, "glyph table rebuilt: %u font windows, %u slots, %u KB",
             fonts, (u32)GUI_GLYPH_TABLE_SLOTS, (u32)( sizeof( s_glyph_table ) / 1024u ) );
    return true;
}

/*==============================================================================================
    The frame latch.
==============================================================================================*/

bool
glyph_table_sync( void )
{
    bool changed = ( res_atlas_generation() != s_seen_cov_gen )
                || ( res_sdf_generation()   != s_seen_sdf_gen );

    glyph_font_sig_t sig[ GUI_FONT_REGISTRY_MAX ];
    bool             any_tenant = false;
    for ( u32 id = 0; id < GUI_FONT_REGISTRY_MAX; ++id )
    {
        const font_slot_t* slot = font_slot_ptr( id );
        bool               live = slot && slot->used && slot->atlas_tenant != 0;

        sig[ id ] = live ? ( glyph_font_sig_t ){ .tenant    = slot->atlas_tenant,
                                                 .sdf       = slot->sdf_range != 0,
                                                 .ext_count = slot->ext_count }
                         : ( glyph_font_sig_t ){ 0 };
        any_tenant = any_tenant || live;

        changed = changed
               || sig[ id ].tenant    != s_seen_font[ id ].tenant
               || sig[ id ].sdf       != s_seen_font[ id ].sdf
               || sig[ id ].ext_count != s_seen_font[ id ].ext_count;
    }

    if ( !changed )
        return false;

    s_seen_cov_gen = res_atlas_generation();
    s_seen_sdf_gen = res_sdf_generation();
    memcpy( s_seen_font, sig, sizeof( s_seen_font ) );

    /* Nothing packed and no buffer standing: the signature is captured, the build is deferred --
       this is what keeps a fontless build from ever allocating the table. */
    if ( !any_tenant && s_glyph_buf_idx == 0 )
        return false;

    return glyph_table_rebuild();
}

void
glyph_table_shutdown( void )
{
    if ( s_glyph_buf_idx )
        rhi()->unregister_buffer( s_glyph_buf_idx );
    if ( rhi_handle_valid( s_glyph_buf ) )
        rhi()->buffer_destroy( s_glyph_buf );

    s_glyph_buf        = ( rhi_buffer_t ){ 0 };
    s_glyph_buf_idx    = 0;
    s_glyph_buf_failed = false;
    s_seen_cov_gen     = 0;
    s_seen_sdf_gen     = 0;
    memset( s_seen_font,   0, sizeof( s_seen_font ) );
    memset( s_glyph_table, 0, sizeof( s_glyph_table ) );
}

/*==============================================================================================
    Accessors.
==============================================================================================*/

u32
glyph_table_idx( void )
{
    return s_glyph_buf_idx;
}

u32
glyph_table_slot( u32 font_id, u32 cp )
{
    if ( font_id >= GUI_FONT_REGISTRY_MAX )
        font_id = 0;
    return font_id * GUI_FONT_GLYPH_TABLE_PER_FONT
         + font_glyph_table_index( font_slot_ptr( font_id ), cp );
}

bool
glyph_table_entry( u32 slot, f32* u0, f32* v0, f32* u1, f32* v1 )
{
    if ( slot >= GUI_GLYPH_TABLE_SLOTS )
        return false;
    const glyph_entry_t* e = &s_glyph_table[ slot ];
    if ( u0 ) *u0 = e->u0;
    if ( v0 ) *v0 = e->v0;
    if ( u1 ) *u1 = e->u1;
    if ( v1 ) *v1 = e->v1;
    return true;
}

u32
glyph_table_gpu_bytes( void )
{
    return s_glyph_buf_idx ? (u32)sizeof( s_glyph_table ) : 0u;
}

// clang-format on
/*============================================================================================*/
