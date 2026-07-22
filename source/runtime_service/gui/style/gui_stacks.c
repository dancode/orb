/*==============================================================================================

    runtime_service/gui/style/gui_stacks.c -- Bracketing vocabulary: id scope, item flags, style.

    The thin public wrappers for the three push / pop / next stacks a caller brackets widgets with:
        push_id / pop_id           -- id-scope levels for repeated widgets (id stack, core/gui_id.c)
        push_item_flag / next_     -- per-item behavior tweaks (item-flag stack, core/gui_ctx.c)
        push_style_color / _var    -- per-item theme overrides (style stacks, gui_style_core.c)

    Pure caller vocabulary (the machinery / vocabulary split: the stacks and their resolution
    live in the machinery files; the verbs a user speaks live here).  Nothing in the lib below
    depends on these wrappers; internal uses (combo's push_id for its list rows) are deliberate
    dogfooding through the gui_host.h declarations.

    NOTE the cross-cut, carried whole into the style unit at R5 (the plan's mapping): the id
    and item-flag brackets forward DOWN to interact-server seams (id_push / item_flag_push,
    core/gui_core.h -- style -> core is the graph's own edge); the style color / var / scale
    brackets forward to this unit's own statics (gui_style_core.c, included above, so those
    stay static).  If a later increment wants the core brackets beside their machinery, only
    the down-forwarding wrappers move.

    Included by gui_style.c LAST -- above both machinery files.  gui_api.c (frame unit) wires
    these into the vtable through the gui_host.h declarations.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    push_id / pop_id -- add a temporary id-scope level for repeated widgets within one region.

    Widget ids are already region-seeded, so this is only needed to separate widgets that share a
    label in the same region (e.g. list rows keyed by index).  push_id combines its key onto the
    current scope; pop_id removes one level.  Always balance them -- a region pop restores the
    scope depth anyway, so a stray push cannot escape its region, but balancing keeps ids stable.
==============================================================================================*/

void gui_push_id    ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ) ) ); }
void gui_push_id_int( i32 i )           { id_push( id_combine( id_seed(), (u32)i ) ); }
void gui_pop_id     ( void )            { id_pop(); }

/*==============================================================================================
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    push/pop affect every widget until popped (and nest); next_item_flag is a one-shot override the
    very next widget consumes, no pop needed.  The merged value is resolved once per widget at emit
    time and read by item_state / the widget, so a new flag never touches a call site or the
    vtable layout consumers see.  See gui_item_flags_t in gui.h for the model and the flags.

        gui()->push_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Off A" );  gui()->button( "Off B" );   // both disabled
        gui()->pop_item_flag();

        gui()->next_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Only this one is disabled" );
==============================================================================================*/

void gui_push_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_push( flag, enable ); }
void gui_pop_item_flag ( void )                               { item_flag_pop(); }
void gui_next_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_next( flag, enable ); }

/*==============================================================================================
    disabled_begin / disabled_end -- the named-scope shorthand for GUI_ITEM_DISABLED (the ImGui
    BeginDisabled / EndDisabled).  disabled_begin( true ) brackets a run of widgets so they all draw
    dimmed and reject input; disabled_begin( false ) pushes a no-op scope (so a conditional disable
    still balances with one disabled_end).  Nests cleanly via the item-flag stack.

        gui()->disabled_begin( !has_selection );
        gui()->button( "Delete" );          // inert + dimmed while nothing is selected
        gui()->disabled_end();
==============================================================================================*/

void
gui_disabled_begin( bool disabled )
{
    /* OR the bit in -- never clear it -- so disabled_begin( false ) nested inside an outer
       disabled_begin( true ) keeps the widgets disabled (the ImGui nesting rule). */
    bool now = ( ( s_build.item_flags & GUI_ITEM_DISABLED ) != 0 ) || disabled;
    item_flag_push( GUI_ITEM_DISABLED, now );
}

void gui_disabled_end( void ) { item_flag_pop(); }

/*==============================================================================================
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
==============================================================================================*/

void gui_push_style_color( gui_col_t slot, u32 abgr )       { style_push_color( slot, abgr ); }
void gui_pop_style_color ( u32 count )                        { style_pop_color( count ); }
void gui_next_style_color( gui_col_t slot, u32 abgr )       { style_next_color( slot, abgr ); }

void gui_push_style_var( gui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void gui_pop_style_var ( u32 count )                          { style_pop_var( count ); }
void gui_next_style_var( gui_style_var_t var, f32 value )   { style_next_var( var, value ); }

/* Resolved read of one palette slot -- theme base with any push / next override applied, the
   same value a stock widget would paint with right now.  THE public door to the user-extended
   range (GUI_COL_USER_*): a kit seeds a user slot (style_get()->colors[...] or push) and paints
   its custom drawing with this read, so its colors ride the theme + stacks like stock chrome.
   Reading a core slot (border, backgrounds) for custom chrome is equally legitimate. */
u32 gui_style_color( gui_col_t slot )
{
    return ( slot < GUI_COL_COUNT ) ? style_col( slot ) : 0u;
}

/*==============================================================================================
    scale_push / scale_pop -- scope a named density step (the theme's scale ramp, gui_scale_t)
    over the widgets until the matching pop: one declaration instead of per-row pixel sizes.

    A step is a paired push of the three metric slots (LINE_SIZE, WIDGET_PAD, WIDGET_GAP) with
    that step's theme values, so every metric read and counting helper inside the scope --
    WIDGET_H, sz_rows_h( n ), sz_fit_row -- speaks the step with no widget changes.

    Region PADDING (the inset between the region box and its content) is still captured when the
    region opens, so a push meant to affect a child's pad must precede child_begin/window_begin.
    Row HEIGHT and the GAP between rows resolve live, per row (mod_gap_x/_y in gui_layout_core.c,
    mirroring row_h) -- so a push placed anywhere before the first row of a body still lands, even
    after the region itself was opened by callee code the caller doesn't control (e.g. inside a
    combo dropdown, whose body region is opened by combo_begin, not the call site):

        gui()->scale_push( GUI_SCALE_DENSE );        // this panel is a dense list
        gui()->child_begin( "entities", 0, 0, 0 );
        ... selectable rows at the dense height ...
        gui()->child_end();
        gui()->scale_pop();

        if ( gui()->combo_begin( "##pick", preview, GUI_COMBO_NONE ) )   // box stays std scale
        {
            gui()->scale_push( GUI_SCALE_DENSE );     // only the dropdown body goes dense
            ... selectable rows ...
            gui()->scale_pop();
            gui()->combo_end();
        }
==============================================================================================*/

void
gui_scale_push( gui_scale_t s )
{
    if ( (u32)s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;   /* clamp, like the other stacks */
    const gui_scale_metrics_t* m = &s_style.scales[ s ];
    style_push_var( GUI_VAR_LINE_SIZE,  (f32)m->row );
    style_push_var( GUI_VAR_WIDGET_PAD, (f32)m->pad );
    style_push_var( GUI_VAR_WIDGET_GAP, (f32)m->gap );
}

void
gui_scale_pop( void )
{
    style_pop_var( 3 );   /* the three slots scale_push pushed */
}

/* The row height of a ramp step, without pushing it -- size a child to another step's rows
   (a BAR header band above a DENSE list), or feed custom chrome. */
f32
gui_sz_scale_row( gui_scale_t s )
{
    if ( (u32)s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;
    return (f32)s_style.scales[ s ].row;
}

/* Global indicator-shape setters (gui_check_style_t / gui_bullet_style_t / gui_arrow_style_t):
   persistent writes to the active style record (from draw/gui_symbol.c at R8 -- a style write
   is style-unit material).  Scope a change with push_style_var on the matching GUI_VAR_*_STYLE
   instead; the styled emitters that read the picks live in element/gui_symbol_style.c. */
void gui_set_check_style ( u8 style ) { s_style.check_style  = style; }
void gui_set_bullet_style( u8 style ) { s_style.bullet_style = style; }
void gui_set_arrow_style ( u8 style ) { s_style.arrow_style  = style; }

// clang-format on
/*============================================================================================*/
