/*==============================================================================================

    runtime_service/gui/draw/gui_canvas.c -- the custom-draw / canvas vocabulary.

    Placement primitives for a rect the caller already holds, rather than a self-laying-out
    control: canvas() reserves the rect (a cell like any widget); draw_rect / draw_text are the
    raw fill/text calls; text_size / draw_text_in / draw_text_clipped measure and place text
    within one; the icon section is the thin public surface over the runtime icon atlas
    (gui_icon.c, backend unit).  None of these consume the row template beyond canvas() and
    image() -- they act on a rect, not on cell_next's cursor -- so they compose with any
    custom layout.  The interaction half of the tier (gui_item / invisible_button) is
    core/gui_item.c (gui_item); together they are the substrate a user widget is written on.

    Included by gui_draw.c last -- pure vocabulary, no state of its own.  canvas() and image()
    are the only two verbs here that reserve a cell, and cell_next belongs to the FLOW unit
    above: they reach it through the one seam declaration in draw/gui_draw.h (the draw unit's
    single documented upward call), never by include order.

==============================================================================================*/
// clang-format off

/* Reserve a rectangular drawing area in the layout and hand back its screen rect, for custom
   geometry (draw_line / draw_polyline / draw_rect / draw_text).  Consumes one cell like any
   widget -- full content width, `height` pixels tall (height <= 0 fills the remaining region
   height) -- so it flows in the vertical list and the pen resumes below it.  The returned rect is
   in the same screen space the draw_* calls take, and the enclosing window clips it. */
gui_rect_t
gui_canvas( f32 height )
{
    if ( height <= 0.0f )
        height = gui_content_avail().y;
    return cell_next( height );
}

/*==============================================================================================
    Low-level draw_rect / draw_text
==============================================================================================*/

void
gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

/* Batched form: N solid rects as ONE semantic command (one quad each at flush).  For dense
   custom drawing -- timeline bars, graph columns, heatmap cells -- where per-rect draw_rect
   calls would exhaust the frame's command budget (GUI_MAX_CMDS).  Entries share the current
   clip; square corners; per-entry color. */
void
gui_draw_rects( const gui_rect_col_t* rects, u32 count )
{
    draw_push_rect_list( rects, count );
}

void
gui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

/* The transformed run: laid out from (x, y) exactly as draw_text lays one out, then scaled and
   turned about that same point.  (x, y) is therefore both the anchor and the pivot; any other
   pivot is the caller moving the anchor, which is a two-line rotation the caller already has the
   measurements for (text_size) and which no single set of parameters here could generalize. */
void
gui_draw_text_xf( f32 x, f32 y, u32 abgr, const char* str, f32 scale, f32 rot )
{
    draw_push_text_xf( x, y, abgr, str, scale, rot );
}

/* Volatile widgets (gui()->volatile_cb / volatile_begin / volatile_end) live in their own file --
   chrome/widgets/gui_volatile.c -- rather than here, since they're a distinct cross-cutting feature
   spanning both units, not a custom-draw escape hatch. */

/*==============================================================================================
    Text measurement + aligned draw -- the placement primitives for custom drawing.  draw_text gives
    a top-left anchor only; these let a caller size to text and place it within a rect by intent
    (right-align a caption, center a label) instead of hand-computing the anchor -- the arithmetic
    that silently overflows when a constant is wrong.
==============================================================================================*/

/* text_size -- the laid-out pixel size of s as draw_text_in / draw_text render it: width is the
   widest line, height spans the lines ('\n' breaks; one line is char_h, each extra adds a full line
   advance).  The CalcTextSize analogue, for sizing a rect or centering by hand. */
gui_vec2_t
gui_text_size( const char* s )
{
    if ( !s ) return ( gui_vec2_t ){ 0.0f, 0.0f };

    f32         max_w = 0.0f;
    u32         lines = 1;
    const char* line  = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            f32 w = font_text_w_n( line, (u32)( p - line ) );
            if ( w > max_w ) max_w = w;
            if ( *p == '\0' ) break;
            ++lines;
            line = p + 1;
        }
    }
    return ( gui_vec2_t ){ max_w, font_char_h() + (f32)( lines - 1 ) * font_line_h() };
}

/* draw_text_in -- draw s aligned within rect r (gui_align_t).  Multi-line: the block is placed by
   the vertical flag, each line by the horizontal flag, so RIGHT flushes every line to r's right edge. */
void
gui_draw_text_in( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32         y    = align_y( r.y, r.h, gui_text_size( s ).y, align );
    const char* line = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            u32 n = (u32)( p - line );
            draw_push_text_n( align_x( r.x, r.w, font_text_w_n( line, n ), align ), y, col, line, n );
            if ( *p == '\0' ) break;
            y   += font_line_h();
            line = p + 1;
        }
    }
}

/* draw_text_clipped -- single-line draw_text_in that ellipsizes to r's width when s does not fit
   (the fitted run is left-anchored; alignment applies only while it fits). */
void
gui_draw_text_clipped( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32 w = font_text_w( s );
    f32 y = align_y( r.y, r.h, font_char_h(), align );
    if ( w <= r.w )
        draw_push_text( align_x( r.x, r.w, w, align ), y, col, s );
    else
        draw_text_fit_n( r.x, y, col, s, 0xFFFFFFFFu, r.w );
}

/*==============================================================================================
    Icons -- thin public surface over the runtime icon atlas (gui_icon.c, backend unit).

    register_icon / load_icon / find_icon / icon_size pass straight through; image is a layout widget that
    reserves a box and fills it; draw_icon_in is the custom-draw placement primitive (the icon
    analogue of draw_text_in) for a rect the caller already holds -- a table cell, a button label,
    a canvas cut.  Both draw helpers aspect-fit the icon centered in the rect so a non-square box
    never stretches the art, and default a 0 color to opaque white (icons are usually drawn plain).
==============================================================================================*/

gui_icon_id_t
gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage )
{
    return icon_register( name, w, h, coverage );
}

gui_icon_id_t
gui_load_icon( const char* name, const char* path )
{
    char resolved[ 576 ];
    fmt_snprintf( resolved, sizeof( resolved ), "%s/%s", sys_root_dir(), path );
    return icon_load_file( name, resolved );
}

/* The distance-field twins.  Same inputs plus the stored-field size; everything downstream of the
   id -- find, size, image, draw_icon_in -- is unchanged, because the fork is in what a texel means
   and that travels in the vertex, not in the API. */

gui_icon_id_t
gui_register_icon_sdf( const char* name, u32 w, u32 h, const u8* coverage, u32 out_max )
{
    return icon_register_sdf( name, w, h, coverage, out_max );
}

gui_icon_id_t
gui_load_icon_sdf( const char* name, const char* path, u32 out_max )
{
    char resolved[ 576 ];
    fmt_snprintf( resolved, sizeof( resolved ), "%s/%s", sys_root_dir(), path );
    return icon_load_file_sdf( name, resolved, out_max );
}

gui_icon_id_t
gui_find_icon( const char* name )
{
    return icon_find( name );
}

gui_vec2_t
gui_icon_size( gui_icon_id_t id )
{
    u32 w = 0, h = 0;
    icon_get( id, NULL, NULL, NULL, NULL, &w, &h );
    return ( gui_vec2_t ){ (f32)w, (f32)h };
}

void
gui_draw_icon_in( gui_rect_t r, gui_icon_id_t id, u32 col )
{
    u32 iw = 0, ih = 0;
    if ( !icon_get( id, NULL, NULL, NULL, NULL, &iw, &ih ) || iw == 0 || ih == 0 )
        return;

    /* Aspect-fit: scale to the tighter of the two axes, then center the fitted box in r. */
    f32 sx  = r.w / (f32)iw;
    f32 sy  = r.h / (f32)ih;
    f32 s   = sx < sy ? sx : sy;
    f32 w   = (f32)iw * s;
    f32 h   = (f32)ih * s;
    gui_rect_t box = rect_align( r, w, h, GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER );

    draw_push_icon( box.x, box.y, box.w, box.h, id, col ? col : 0xFFFFFFFFu );
}

/* The same aspect-fitted icon, turned about the fitted box's centre (radians, screen space).
   Compass needles, minimap markers, spinner glyphs.  What keeps it clean at any angle is the
   icon's own sampling model: an SDF icon resolves its edge in the fragment and turns like SDF
   text does; a coverage icon shows its texels, exactly as the two font bakes differ. */
void
gui_draw_icon_xf( gui_rect_t r, gui_icon_id_t id, u32 col, f32 rot )
{
    u32 iw = 0, ih = 0;
    if ( !icon_get( id, NULL, NULL, NULL, NULL, &iw, &ih ) || iw == 0 || ih == 0 )
        return;

    f32 sx  = r.w / (f32)iw;
    f32 sy  = r.h / (f32)ih;
    f32 s   = sx < sy ? sx : sy;
    f32 w   = (f32)iw * s;
    f32 h   = (f32)ih * s;
    gui_rect_t box = rect_align( r, w, h, GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER );

    draw_push_icon_xf( box.x, box.y, box.w, box.h, id, rot, col ? col : 0xFFFFFFFFu );
}

void
gui_image( gui_icon_id_t id, f32 w, f32 h, u32 col )
{
    gui_rect_t r = cell_next_w( w, h );   /* reserve a w x h layout slot (like empty) */
    gui_draw_icon_in( r, id, col );
}

/*==============================================================================================
    RGBA textures -- display an arbitrary bindless texture (a scene render target, a loaded
    image) as a full-color quad.  Unlike image() (R8 icon atlas, texel = coverage), the texel IS
    the color: GUI_TEX_RGBA in the command's tex_idx mode field flips the fragment shader to RGBA
    sampling, with the tint color multiplied in (0 defaults to opaque white = untinted).  The
    caller owns the texture and its bindless registration (rhi register_texture) and must keep
    the slot alive until the frame that last referenced it has retired.

    HONOURS the ambient rounding radius (draw_set_rounding), which image() and icons do not:

        f32 save = gui()->draw_rounding();
        gui()->draw_set_rounding( 8.0f );
        gui()->draw_texture_in( r, scene_tex, 0 );     // rounded viewport / thumbnail
        gui()->draw_set_rounding( save );

    The corner is EXACT, not a stencil or a mask -- the fragment resolves the boundary from the
    same signed-distance field a rounded fill uses, and the texture samples underneath it, so the
    cost is the one quad the square version cost and the batch is unchanged.
==============================================================================================*/

void
gui_draw_texture_in( gui_rect_t r, u32 bindless_idx, u32 tint_abgr )
{
    if ( bindless_idx == 0 )
        return;   /* 0 is the RHI empty slot -- nothing to sample */
    /* draw_push_IMAGE, not rect_filled: this is the one textured quad the ambient rounding radius
       reaches, because it is the one that is a picture rather than a glyph.  The corner is resolved
       by the fragment's distance field with the texture sampling underneath, so a rounded thumbnail
       or a rounded viewport costs the same one quad a square one does. */
    draw_push_image( r.x, r.y, r.w, r.h, 0, 0, 1, 1,
                     bindless_idx | GUI_TEX_MODE( GUI_TEX_RGBA ),
                     tint_abgr ? tint_abgr : 0xFFFFFFFFu );
}

/* The rotated form -- one RGBA quad about its centre.  Does NOT honour the ambient rounding
   (draw_texture_in's one extra): a rotated field and a rotated picture are both fine alone, but
   the rounded textured quad routes through the axis-aligned box tessellator; a caller wanting a
   rounded rotated picture stacks gui_draw_box_xf behind it instead. */
void
gui_draw_texture_xf( gui_rect_t r, u32 bindless_idx, u32 tint_abgr, f32 rot )
{
    if ( bindless_idx == 0 )
        return;   /* 0 is the RHI empty slot -- nothing to sample */
    draw_push_image_xf( r.x, r.y, r.w, r.h, 0, 0, 1, 1,
                        bindless_idx | GUI_TEX_MODE( GUI_TEX_RGBA ), rot,
                        tint_abgr ? tint_abgr : 0xFFFFFFFFu );
}

void
gui_image_texture( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr )
{
    gui_rect_t r = cell_next_w( w, h );   /* reserve a w x h layout slot (like image) */
    gui_draw_texture_in( r, bindless_idx, tint_abgr );
}

/*==============================================================================================
    Sprites -- authored RGBA art, and the nine-slice that makes it resize.

    The caller's door onto draw/gui_sprite.c.  Registration mirrors the icon verbs one for one
    (register / load / find / size), which is deliberate: the two differ in what a texel MEANS,
    not in how you get one, so a reader who knows the icon API already knows this one.

    The one verb with no icon twin is set_slice, and it is the reason sprites exist as their own
    kind: it declares the four insets that let ONE piece of art fill ANY rect without distorting
    its corners.  Set it once after registering; every fill of that sprite inherits it.
==============================================================================================*/

gui_sprite_id_t
gui_register_sprite( const char* name, u32 w, u32 h, const u8* rgba )
{
    return sprite_register( name, w, h, rgba );
}

gui_sprite_id_t
gui_load_sprite( const char* name, const char* path )
{
    char resolved[ 576 ];
    fmt_snprintf( resolved, sizeof( resolved ), "%s/%s", sys_root_dir(), path );
    return sprite_load_file( name, resolved );
}

gui_sprite_id_t gui_find_sprite( const char* name )                     { return sprite_find( name ); }
gui_vec2_t      gui_sprite_size( gui_sprite_id_t id )                   { return sprite_size( id ); }
bool            gui_sprite_set_slice( gui_sprite_id_t id, gui_pad_t s ) { return sprite_set_slice( id, s ); }
gui_pad_t       gui_sprite_slice( gui_sprite_id_t id )                  { return sprite_slice( id ); }

/* Fill r with the sprite -- nine-sliced when it carries insets, stretched when it does not.

   FILLS rather than aspect-fits, which is the opposite of draw_icon_in and correct for both: an
   icon is a symbol that must keep its proportions inside whatever cell it lands in, and a sprite
   is most often a SURFACE -- a frame, a panel skin, a button face -- whose whole job is to cover
   the rect it was given.  A caller who wants a sprite fitted has gui_rect_align to do it with. */
void
gui_draw_sprite_in( gui_rect_t r, gui_sprite_id_t id, u32 tint_abgr )
{
    draw_push_sprite( r.x, r.y, r.w, r.h, id, tint_abgr, 1.0f, 0, true );
}

void
gui_image_sprite( gui_sprite_id_t id, f32 w, f32 h, u32 tint_abgr )
{
    gui_rect_t r = cell_next_w( w, h );   /* reserve a w x h layout slot (like image) */
    gui_draw_sprite_in( r, id, tint_abgr );
}

/*==============================================================================================
    draw_brush -- the widened paint floor, published.

    gui()->draw_rect fills a rect with a colour; this fills one with a gui_brush_t, which is the
    same thing plus three more answers to "with what".  It is the door a custom widget paints its
    face through if it wants to be skinnable by whoever uses it:

        gui()->draw_brush( face, &( gui_brush_t ){ .kind   = GUI_BRUSH_NINE,
                                                   .sprite = my_button_art,
                                                   .scale  = ui_scale } );
==============================================================================================*/

void
gui_draw_brush( gui_rect_t r, const gui_brush_t* brush )
{
    draw_fill_brush( r, brush );
}

/*==============================================================================================
    Ambient corner rounding, published.

    The radius is AMBIENT rather than a parameter because it applies to shapes pushed by verbs
    that have no business growing one -- draw_texture_in, draw_brush, draw_frame.  The backend
    resolves a rounded rect as an SDF surface (gui.h, the effect band), so this is also what
    rounds a TEXTURED quad: the image samples underneath the coverage the shader computes, which
    is a thing the old tessellated corner fan could not do at all.

    Save and restore around the shapes it should affect -- it is a plain ambient value, not a
    stack, and leaving it set leaks into whatever the caller draws next:

        f32 save = gui()->draw_rounding();
        gui()->draw_set_rounding( 8.0f );
        gui()->draw_texture_in( r, tex, 0xFFFFFFFFu );
        gui()->draw_set_rounding( save );
==============================================================================================*/

void
gui_draw_set_rounding( f32 r )
{
    draw_set_rounding( r );
}

f32
gui_draw_rounding( void )
{
    return draw_rounding();
}

/* The corner PROFILE that rides with the radius (gui_api.h): 0 = the circular arc, 1 = curvature
   ramped across the whole corner.  Same ambient discipline as the radius above -- the theme
   installs it once a frame from GUI_VAR_CORNER_SMOOTH, and a caller that overrides it for one
   shape saves and restores. */

void
gui_draw_set_corner_smooth( f32 t )
{
    draw_set_corner_smooth( t );
}

f32
gui_draw_corner_smooth( void )
{
    return draw_corner_smooth();
}

/* The animation CURVE (gui_api.h): what a normalized phase does between its endpoints, shared by
   every shape that carries a clock -- the pulse, the spinner, the marching ants.  Ambient on the
   same save/restore discipline as the radius above. */

void
gui_draw_set_anim_curve( u32 curve, f32 param )
{
    draw_set_anim_curve( curve, param );
}

void
gui_draw_get_anim_curve( u32* curve, f32* param )
{
    draw_get_anim_curve( curve, param );
}

void
gui_draw_set_anim_phase( f32 cycles )
{
    draw_set_anim_phase( cycles );
}

f32
gui_draw_anim_phase( void )
{
    return draw_anim_phase();
}

/* Border ALIGNMENT for the stroked box family (gui_api.h): 0 inside, 0.5 centred, 1 outside.
   Same ambient discipline as the radius -- save, set, draw, restore. */

void
gui_draw_set_border_align( f32 a )
{
    draw_set_border_align( a );
}

f32
gui_draw_border_align( void )
{
    return draw_border_align();
}

/*==============================================================================================
    Text edge -- the same ambient discipline, for the second colour outside a glyph.

    An outline and a drop shadow stop being separate features once the glyph is a distance field:
    both are "fill the band from the boundary out to `width`", which the fragment already has
    everything it needs to do.  So this is not a second draw of the run in a darker colour offset
    by a pixel -- it is the SAME quad, the same batch, and the same glyph sample, with the fill
    composited over the widened band.  Cost is a width and a colour on the text command.

    SDF fonts only: a coverage glyph has no signed distance to widen and ignores the setting.
    Width is limited in practice by the spread baked into the atlas, past which the field is flat
    and the outline stops growing rather than tearing.  Save and restore like the radius above:

        f32 sw; u32 sc; gui()->draw_get_text_edge( &sw, &sc );
        gui()->draw_set_text_edge( 2.0f, 0xFF000000u );   // 2 px black outline
        gui()->draw_text( x, y, 0xFFFFFFFFu, "Title" );
        gui()->draw_set_text_edge( sw, sc );
==============================================================================================*/

void
gui_draw_set_text_edge( f32 width, u32 abgr )
{
    draw_set_text_edge( width, abgr );
}

void
gui_draw_get_text_edge( f32* width, u32* col )
{
    draw_text_edge( width, col );
}

/* Font atlas access -- bridges the font registry (gui_font.h / gui_render.h) to the RGBA texture
   primitives above, so a caller can preview a font's live GPU atlas (a texture like any other) via
   image_texture / draw_texture_in without reaching into the backend's internal font_slot_t. */

u32
gui_font_atlas_idx( u32 font_id )
{
    return font_slot_atlas_idx( font_id );
}

gui_vec2_t
gui_font_atlas_size( u32 font_id )
{
    return font_slot_atlas_size( font_id );
}

/* The public gui_render_* symbol surface (draw_check_mark / draw_arrow / draw_frame /
   draw_round_rect / ... and the set_*_style setters) lives in gui_symbol.c, beside the
   draw_* helpers it wraps. */

// clang-format on
/*============================================================================================*/
