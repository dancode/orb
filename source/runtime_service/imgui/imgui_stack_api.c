/*==============================================================================================

    runtime_service/imgui/imgui_stack_api.c -- Push-model public API: id scope, item flags, style.

    The thin public wrappers for the three push / pop / next stacks a caller brackets widgets with:
        push_id / pop_id           -- id-scope levels for repeated widgets (id stack, imgui_ctx.c)
        push_item_flag / next_     -- per-item behavior tweaks (item-flag stack, imgui_ctx.c)
        push_style_color / _var    -- per-item theme overrides (style stacks, imgui_style.c)

    Each just forwards to the static stack operations in imgui_ctx.c / imgui_style.c; they are
    grouped here, out of imgui_layout.c (where they had accreted only because it was the public
    API file), so the layout file is layout and the push-model API is one named unit.

    Included by imgui.c after the modules that implement the stacks, and before imgui_api.c, which
    wires these into the vtable.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    push_id / pop_id -- add a temporary id-scope level for repeated widgets within one region.

    Widget ids are already region-seeded, so this is only needed to separate widgets that share a
    label in the same region (e.g. list rows keyed by index).  push_id combines its key onto the
    current scope; pop_id removes one level.  Always balance them -- a region pop restores the
    scope depth anyway, so a stray push cannot escape its region, but balancing keeps ids stable.
----------------------------------------------------------------------------------------------*/

void imgui_push_id    ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ) ) ); }
void imgui_push_id_int( i32 i )           { id_push( id_combine( id_seed(), (u32)i ) ); }
void imgui_pop_id     ( void )            { id_pop(); }

/*----------------------------------------------------------------------------------------------
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    push/pop affect every widget until popped (and nest); next_item_flag is a one-shot override the
    very next widget consumes, no pop needed.  The merged value is resolved once per widget at emit
    time and read by widget_behavior / the widget, so a new flag never touches a call site or the
    vtable layout consumers see.  See imgui_item_flags_t in imgui.h for the model and the flags.

        imgui()->push_item_flag( IMGUI_ITEM_DISABLED, true );
        imgui()->button( "Off A" );  imgui()->button( "Off B" );   // both disabled
        imgui()->pop_item_flag();

        imgui()->next_item_flag( IMGUI_ITEM_DISABLED, true );
        imgui()->button( "Only this one is disabled" );
----------------------------------------------------------------------------------------------*/

void imgui_push_item_flag( imgui_item_flags_t flag, bool enable ) { item_flag_push( flag, enable ); }
void imgui_pop_item_flag ( void )                                 { item_flag_pop(); }
void imgui_next_item_flag( imgui_item_flags_t flag, bool enable ) { item_flag_next( flag, enable ); }

/*----------------------------------------------------------------------------------------------
    push_style_color / push_style_var (+ pop / next) -- the push-model theme override.

    push overrides a slot for every widget until the matching pop; pop takes a count, so two pushes
    are undone with one pop_style_*( 2 ), mirroring ImGui.  next_style_* overrides a slot for just
    the next widget, no pop needed.  Colors are abgr (IMGUI_COLOR); vars are f32 pixels.  The slots
    are imgui_col_t / imgui_style_var_t.  See imgui_style.c for the resolution model.

        imgui()->push_style_color( IMGUI_COL_WIDGET_BG,  IMGUI_COLOR( 0xFF,0,0,0xFF ) );  // red
        imgui()->push_style_color( IMGUI_COL_WIDGET_HOT, IMGUI_COLOR( 0xFF,0x40,0x40,0xFF ) );
        imgui()->button( "Red Button" );
        imgui()->pop_style_color( 2 );                                                    // both

        imgui()->push_style_var( IMGUI_VAR_WIDGET_PAD, 20.0f );
        imgui()->button( "Roomy" );
        imgui()->pop_style_var( 1 );
----------------------------------------------------------------------------------------------*/

void imgui_push_style_color( imgui_col_t slot, u32 abgr )       { style_push_color( slot, abgr ); }
void imgui_pop_style_color ( u32 count )                        { style_pop_color( count ); }
void imgui_next_style_color( imgui_col_t slot, u32 abgr )       { style_next_color( slot, abgr ); }

void imgui_push_style_var( imgui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void imgui_pop_style_var ( u32 count )                          { style_pop_var( count ); }
void imgui_next_style_var( imgui_style_var_t var, f32 value )   { style_next_var( var, value ); }

// clang-format on
/*============================================================================================*/
