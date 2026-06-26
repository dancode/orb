/*==============================================================================================

    runtime_service/gui/gui_stacks.c -- Push-model public API: id scope, item flags, style.

    The thin public wrappers for the three push / pop / next stacks a caller brackets widgets with:
        push_id / pop_id           -- id-scope levels for repeated widgets (id stack, gui_ctx.c)
        push_item_flag / next_     -- per-item behavior tweaks (item-flag stack, gui_ctx.c)
        push_style_color / _var    -- per-item theme overrides (style stacks, gui_style.c)

    Each just forwards to the static stack operations in gui_ctx.c / gui_style.c; they are
    grouped here, out of gui_layout.c (where they had accreted only because it was the public
    API file), so the layout file is layout and the push-model API is one named unit.

    Included by gui.c after the modules that implement the stacks, and before gui_api.c, which
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

void gui_push_id    ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ) ) ); }
void gui_push_id_int( i32 i )           { id_push( id_combine( id_seed(), (u32)i ) ); }
void gui_pop_id     ( void )            { id_pop(); }

/*----------------------------------------------------------------------------------------------
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    push/pop affect every widget until popped (and nest); next_item_flag is a one-shot override the
    very next widget consumes, no pop needed.  The merged value is resolved once per widget at emit
    time and read by widget_behavior / the widget, so a new flag never touches a call site or the
    vtable layout consumers see.  See gui_item_flags_t in gui.h for the model and the flags.

        gui()->push_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Off A" );  gui()->button( "Off B" );   // both disabled
        gui()->pop_item_flag();

        gui()->next_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Only this one is disabled" );
----------------------------------------------------------------------------------------------*/

void gui_push_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_push( flag, enable ); }
void gui_pop_item_flag ( void )                                 { item_flag_pop(); }
void gui_next_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_next( flag, enable ); }

/*----------------------------------------------------------------------------------------------
    disabled_begin / disabled_end -- the named-scope shorthand for GUI_ITEM_DISABLED (the ImGui
    BeginDisabled / EndDisabled).  disabled_begin( true ) brackets a run of widgets so they all draw
    dimmed and reject input; disabled_begin( false ) pushes a no-op scope (so a conditional disable
    still balances with one disabled_end).  Nests cleanly via the item-flag stack.

        gui()->disabled_begin( !has_selection );
        gui()->button( "Delete" );          // inert + dimmed while nothing is selected
        gui()->disabled_end();
----------------------------------------------------------------------------------------------*/

void
gui_disabled_begin( bool disabled )
{
    /* OR the bit in -- never clear it -- so disabled_begin( false ) nested inside an outer
       disabled_begin( true ) keeps the widgets disabled (the ImGui nesting rule). */
    bool now = ( ( s_build.item_flags & GUI_ITEM_DISABLED ) != 0 ) || disabled;
    item_flag_push( GUI_ITEM_DISABLED, now );
}

void gui_disabled_end( void ) { item_flag_pop(); }

/*----------------------------------------------------------------------------------------------
    push_style_color / push_style_var (+ pop / next) -- the push-model theme override.

    push overrides a slot for every widget until the matching pop; pop takes a count, so two pushes
    are undone with one pop_style_*( 2 ), mirroring ImGui.  next_style_* overrides a slot for just
    the next widget, no pop needed.  Colors are abgr (GUI_COLOR); vars are f32 pixels.  The slots
    are gui_col_t / gui_style_var_t.  See gui_style.c for the resolution model.

        gui()->push_style_color( GUI_COL_WIDGET_BG,  GUI_COLOR( 0xFF,0,0,0xFF ) );  // red
        gui()->push_style_color( GUI_COL_WIDGET_HOT, GUI_COLOR( 0xFF,0x40,0x40,0xFF ) );
        gui()->button( "Red Button" );
        gui()->pop_style_color( 2 );                                                    // both

        gui()->push_style_var( GUI_VAR_WIDGET_PAD, 20.0f );
        gui()->button( "Roomy" );
        gui()->pop_style_var( 1 );
----------------------------------------------------------------------------------------------*/

void gui_push_style_color( gui_col_t slot, u32 abgr )       { style_push_color( slot, abgr ); }
void gui_pop_style_color ( u32 count )                        { style_pop_color( count ); }
void gui_next_style_color( gui_col_t slot, u32 abgr )       { style_next_color( slot, abgr ); }

void gui_push_style_var( gui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void gui_pop_style_var ( u32 count )                          { style_pop_var( count ); }
void gui_next_style_var( gui_style_var_t var, f32 value )   { style_next_var( var, value ); }

// clang-format on
/*============================================================================================*/
