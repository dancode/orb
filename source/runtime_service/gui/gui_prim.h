#ifndef GUI_PRIM_H
#define GUI_PRIM_H
/*==============================================================================================

    runtime_service/gui/gui_prim.h -- the record layer: what the tessellator writes and
    the shaders read.

    Every shape the GUI draws crosses to the GPU as bindless-indexed RECORDS, not vertex
    data.  Three record types carry everything:

      gui_quad_t -- one per shape INSTANCE: placement, colour, and a tagged index word
                    naming everything below
      gui_prim_t -- the shared STYLE record: which field the fragment evaluates
                    (gui_fx_mode_t), the GUI_OP_* modifier bits, and the parameter rows
      gui_fx_t   -- the instance EXTRAS a minority of quads carry: turn, animation phase,
                    border colour, texture rect

    This header is the contract between the tessellator (render/pipeline/) and the
    shaders -- gui_common.hlsli spells these layouts again, so a change here changes it
    too.  Nothing here is drawing API: callers draw through gui_api.h's draw_* verbs and
    never build a record.  gui.h includes this header, so every consumer of gui types
    still sees these definitions.

==============================================================================================*/

#include "orb.h"

// clang-format off

/*==============================================================================================
    GUI_DRAW -- the effect band: a shape the FRAGMENT resolves

    Previous UI iterations would use the CPU to tessellates geometry outright. Without a way 
    for the fragment to resolve a shape itself, rounding a corner means walking an arc table 
    and fanning triangles -- a rounded rect at ~37 vertices, with hard stair-stepped edges
    and no room for a texture -- and a soft shadow means six stacked rects pretending to be
    a gaussian.  Both are the same shortfall: an effect the rasterizer could evaluate exactly,
    approximated in vertices because the vertex has nowhere to say what shape it belongs to.

    So the vertex names a shape, and the fragment evaluates it.

    The effect band is that missing sentence.  A primitive names a RECORD (gui_prim_t) that
    states its shape outright -- rect, radii, softness, rotation -- and the fragment evaluates
    the field from its own pixel position: analytic antialiasing, arbitrary softness, and 
    the texture still sampling underneath it.  A rounded box is FOUR VERTICES.

    Field 0 (GUI_FX_NONE) is what every other primitive writes, and the fragment tests it first:
    text, lines, sprites and square fills pay one compare and are byte-for-byte unchanged.

    This value acts as a header as the first value in the primitive's record, so the fragment
    can switch on it and branch to the right shape evaluation.

    THE FOLD IS GONE, and it is worth saying what it was, because it shaped everything above.  
    The coordinate used to be `|p| - c` computed at each VERTEX -- and an absolute value is 
    not affine across the line where it folds, so the hardware could only interpolate it 
    correctly inside one quadrant. 
    
    That is why a rounded box cost four quadrant quads and a capsule two, why the vertex
    carried a HALF2 coordinate at all, and why a shadow could never have a DIRECTION: |p| 
    threw the sign away, and the sign is which side you are on.  
    
    Handing the fragment the rect instead makes the fold its own business -- `abs(p) - c`
    on a value it computes exactly -- so the quadrants collapse to one quad, the 
    coordinate leaves the vertex, and the sign survives.

    There is no vertex buffer.  The pipeline is BUFFERLESS: the vertex stage pulls one 16-byte
    QUAD RECORD per shape (gui_quad_t, below) from a bindless storage buffer by SV_VertexID and
    expands the covering corners itself.  The quad carries only what varies per INSTANCE --
    placement, colour, clip -- plus the index naming everything that does not.

    Colour is one value for the whole shape.  A ramp across it rides the STYLE record
    (GUI_OP_GRAD), where it spans the shape exactly and can be radial or conic rather than only
    linear; a gradient rect is that op, not two colours interpolated across corners.

    The record's lanes are PACKED and the shader unpacks them by hand (gui_common.hlsli).  The one
    decision worth recording is why the uv packing is safe, because its failure mode is invisible
    until it is not:

        uv as a unorm16 pair -- 1/65535, against a largest atlas of 1024 px, is 64 steps per texel,
        so a glyph's sample lands where it did.  What it cannot represent is U OUTSIDE [0,1], and one
        primitive needs that: a dashed line spans U 0..len/period and lets the sampler's REPEAT
        tile the atlas stipple row.  That is what GUI_OP_TILE_U exists for -- the repeat count
        lives in the record and the FRAGMENT multiplies, so the stored UV stays inside [0,1] and
        the sampled one is unchanged.  Any future primitive that wants to tile does the same;
        storing U > 1 directly would silently CLAMP.

    THE TEXTURE TRAVELS PER PRIMITIVE, for the same reason the shape does: so it cannot split a
    batch.  Coverage, SDF and sprite art are a pixel format and a sampler apart, so they cannot
    share a texture -- if the texture rode per-DRAW, they could not share a draw call either, and a
    window's background fill, its SDF label and an icon would be three draws alternating by
    z-order.  Named by the record, NOTHING opens a new draw call but a viewport change.

    This is the one place the design leans on being bindless.  Slate must batch by texture because
    it binds a descriptor per batch; here the fragment indexes a 2048-entry array, so the slot is
    just a number and a number can live in a record.  The fragment indexes with nonuniformEXT since
    neighbouring primitives in one draw legitimately name different textures.
==============================================================================================*/

/* What the fragment does at this primitive.  A full 32-bit member of the record, so the list grows
   by naming a value -- there is no nibble to run out of. */

typedef enum
{
    GUI_FX_NONE      = 0,  /* no effect -- (ex, ey) and the parameters are ignored (the default) */
    GUI_FX_BOX       = 1,  /* filled rounded box: coverage 1 inside the boundary, feathered across it */

    GUI_FX_NGON      = 2,  /* filled regular polygon: `sides` flat edges inscribed in radius hw,
                              corners rounded by r_tl (row [2]: r_tl = rounding, r_tr = sides,
                              r_br = star inner-radius ratio -- 0 keeps the regular polygon,
                              (0..1] pulls each edge midpoint in to ratio * radius, making an
                              n-pointed star from the same fold).  Lands in the shared decode, so
                              BAND / GRAD / CUT / INSET compose -- a stroked hexagon badge is this
                              field plus the band op, one quad. */

    GUI_FX_TRI       = 3,  /* solid triangle: three points about the shape centre, in the record's
                              radius + param lanes (a = r_tl,r_tr  b = r_br,r_bl  c = param_a,_b).
                              Exact signed distance, so BAND strokes it and the feather antialiases
                              it like any field -- one quad over the bbox, no real triangle
                              rasterized. */

    GUI_FX_BEZIER    = 4,  /* stroked quadratic bezier: p0, control c, p1 about the shape centre,
                              in the same radius + param lanes GUI_FX_TRI uses (a = r_tl,r_tr
                              b = r_br,r_bl  c = param_a,_b), plus a half-thickness riding
                              `border`.  Distance is a cheap fixed-sample approximation, not an
                              exact curve solve -- nearest of a handful of points along the curve,
                              refined against its two neighbour segments. */

    GUI_FX_SEG       = 5,  /* CAPSULE: a line segment `radius` px thick, with round caps        */

    /* The CIRCULAR-SECTOR modes.  All read the effect coordinate as a SIGNED offset from the
       shape centre, already rotated so the sector's bisector points +y in that local frame -- see
       the note below on why these need no fold and therefore cost ONE quad. */
    GUI_FX_ARC       = 6,  /* annular sector: a band of `tube` px centred on radius ra, round caps */
    GUI_FX_PIE       = 7,  /* filled wedge: the disc of radius ra cut to the aperture, sharp edges */

    GUI_FX_TEX       = 8,  /* BAKED art: the field is SAMPLED from the SDF atlas rather than
                              evaluated, so a silhouette no closed-form expression describes -- a
                              keyhole, a gear, a badge -- carries the whole op cascade.  The
                              fragment recovers a PIXEL distance from the screen-space derivative
                              of the texel (gui_fx.hlsli), which is what makes it scale and rotate
                              like every analytic field beside it.

                              Row 1 is unused; row 2's `feather` means exactly what it means for
                              GUI_FX_BOX -- the AA band, and GUI_OP_INSET's depth -- and that reuse
                              is what lets the ops run unchanged.  The quad's uv spans the whole
                              padded tenant and its rect is the padded box, so the SKIRT rule's
                              existing uv grow-and-clamp samples out into the margin and goes flat
                              at the tenant edge.

                              Two limits are structural rather than unfinished.  The field holds
                              only the `spread` texels the bake padded for, so a border or glow
                              reaching further saturates and stops.  And an R8 field carries no
                              arc-length coordinate, so it states no boundary coordinate: GUI_OP_
                              DASH and GUI_OP_GRAD_ALONG have nothing to walk and are dropped at
                              the emit site rather than drawing something arbitrary. */

    /* RETIRED fields, all folded into ops -- a field is the SHAPE itself, and each of these was
       an effect an op can now apply to ANY shape.  Do not re-add as fields:
         TILE_U -> GUI_OP_TILE_U        TEXT_EDGE -> GUI_OP_TEXT_EDGE
         SKIRT  -> GUI_OP_CUT           CHECKER / GRID -> GUI_OP_CHECKER / GUI_OP_GRID
         ARC_DASH -> ARC + GUI_OP_DASH  ARC_GRAD -> ARC + GUI_OP_GRAD + GUI_OP_GRAD_ALONG */

} gui_fx_mode_t;

/* WHAT A FIELD STATES, AND WHY THE OPS COMPOSE.

   The fragment does not branch per field and then paint.  Each field resolves to the same small
   vocabulary, and every op afterwards reads only that -- so an op never has to know which shape
   it landed on, and a shape never has to reimplement an op:

       d        signed distance to the boundary in px, negative inside.  BAND bends it into a
                border, FRAME measures its band from it, the feather resolves it to coverage.
       s        the coordinate ALONG the boundary, in px.  This is what DASH cuts on and what
                GRAD_ALONG ramps on, and it is why one op serves every shape: a box states
                perimeter arc-length, a sector states radius * angle, a segment states distance
                along its axis.
       len      the boundary's total length in px -- what normalizes s.  A shape that states s
                states len with it, so s / len is progress along the boundary on any of them.
       aa       the width d resolves through.  Usually the style's feather; a field with no
                feather of its own (a sector) states the 1 px band it wants instead.
       mul      coverage a field contributes without having a boundary at all -- the lattice
                fields multiply here rather than cutting d.

   Fields that state no boundary (NONE, TILE_U, TEXT_EDGE, the lattice pair) sit out the d-based
   ops and still take the ones that only touch coverage, PULSE among them. */

/* HOW THE FRAGMENT GETS ITS SHAPE-LOCAL COORDINATE.  Every field below works in a frame of its
   own, and every one of them derives it the same way: take the pixel position, subtract the
   quad's centre, and un-rotate by the quad's (cos, sin) -- gui_quad_t.xform, per instance.

       d     = SV_Position.xy - (cx, cy)
       local = ( d.x * cos + d.y * sin, -d.x * sin + d.y * cos )

   BOX folds that itself -- `q = |local| - c`, where c is the half-extent minus the corner radius
   -- and picks WHICH corner radius from the sign of local, since the sign says which quadrant the
   fragment is in.  Then `d = min(max(q.x,q.y),0) + length(max(q,0)) - radius`; the interior term is
   what keeps a border wider than the radius, or a shadow softer than it, correct in the core.

   SEG uses the segment's own axis as the rotation, so local is (along, across) about the midpoint,
   and folds only the along axis: `q = (|local.x| - halflen, local.y)`, then
   `d = length(vec2(max(q.x,0), q.y)) - radius`.  That form is the true distance to the segment
   inside and out, so unlike the box it needs no interior term at all.

   THE SECTOR FIELDS use the same two numbers as a REFLECTION rather than a rotation -- the frame
   whose +y is the sector's bisector -- which works out to the box's expression with its components
   swapped.  They fold nothing but |x|, and that is what makes an arc expressible at all: the SIGN
   of the coordinate is the angle, and a shape that threw it away could never tell where on the
   circle a fragment lies.

       q   = ( |local.x|, local.y )
       ARC = the distance to the circle of radius ra, cut to the aperture, minus the tube
       PIE = that disc intersected with the angular half-plane

   The frame is a reflection (det -1), which is harmless: the shape is symmetric about the bisector
   by construction, and the pipeline does not cull.

   ALL OF THIS USED TO HAPPEN AT THE VERTEX, in a HALF2 attribute, and the cost was structural
   rather than arithmetic: |p| is not affine across the line it folds on, so the hardware could
   only interpolate it correctly WITHIN one quadrant -- hence four quadrant quads for a box, two
   for a capsule, and no way to express a direction, because |p| had already discarded the sign.
   The fragment computes the same quantity exactly, from numbers the record states, so the geometry
   is now whatever covers the shape and nothing more. */

/* THE FRAME CLOCK the fragment sees, in seconds, wrapped to GUI_FX_TIME_WRAP.  It rides the PUSH
   CONSTANT, not the vertex, and that is the whole point: time is the same number for every shape
   in the frame, so spending 4 bytes per vertex to repeat it would tax every glyph on screen to say
   nothing new.  In the push constant it is free in the other direction too -- the flush writes it
   once before the first draw of a surface and never touches it again, so unlike a per-draw effect
   parameter it splits no batch.  A time-driven effect therefore re-emits NO geometry and adds NO
   draw call: the retained cache keeps last frame's vertices and only the constant moves.
   Caveat, and it is the whole cost: the idle skip means a frame is only presented when something
   asks for one.  A purely shader-driven animation has no emit to raise wants_redraw, so whatever
   owns the effect must call gui()->request_redraw() while it runs -- exactly as a volatile widget
   does.  Time advancing is not the same as the frame advancing.
   The wrap is a power of two so f32 still resolves ~0.1 ms at the far end, and so any effect whose
   period divides 1024 s -- every power-of-two fraction of a second -- runs continuously across it.
   Any other period sees one discontinuity every ~17 minutes. */

#define GUI_FX_TIME_WRAP     1024.0

/*==============================================================================================
    GUI_DRAW -- the STYLE record: per-shape-kind constants, stored once and shared.

    Per-shape constants live in a bindless storage buffer, the quad names one by index, and the
    fragment resolves it with a dependent load -- exactly the shape the clip band already runs
    (gui_clip_entry_t, gui_render.h), generalized to the whole effect band.  Fields are PLAIN,
    not packed: a corner radius is an f32 in pixels, a shape is an enum in a 32-bit word, and
    there is no bit budget to run out of.

    Placement and clip are deliberately ABSENT -- both ride the quad record (gui_quad_t), which
    is what lets identically-styled shapes share one record across placements and scroll
    regions (tess_prim_local's dedup).

    Indices are SLOT-LOCAL -- relative to the window cache slot's own run of records -- and the
    flush adds the slot's base through a push constant.  That is what lets a record index
    survive in cached geometry across a repack: the arena moves, the baked index does not.

    ROW [1] IS PER-FIELD.  Corner radii are what the box family stores there and what it is named
    for; the fields that have no corners reuse the row rather than pad it out:

        BOX / SEG      r_tl, r_tr, r_br, r_bl   -- per-corner radii (SEG: r_tl = half-thickness)
        NGON           r_tl = corner rounding (px), r_tr = side count,
                       r_br = star inner ratio (0 = regular polygon)
        TRI            r_tl..r_bl = two of the three points, about the quad's centre
        ARC_DASH       r_tl = dash period (turns), r_tr = on-duty fraction
        GRID           r_tl, r_tr = lattice phase per axis
        everything else                          -- unused, left zero

    Leaving unused fields ZERO is not tidiness, it is what makes the record memo work: two plain
    fills differ only in the placement their quads carry, so their records compare equal and
    collapse to one entry (tess_prim_local).  A writer that scribbled a parameter into a NONE
    record would give every fill its own record and blow the arena.
==============================================================================================*/

typedef struct
{
    /* Row 0 -- read by EVERY fragment, which is why it leads: a glyph or a flat fill resolves
       its texture from here and never touches the rows below.  Placement and clip are NOT here
       -- both ride the quad record (gui_quad_t), which is what lets one style serve every
       placement and every scroll region. */

    u32 field;          // gui_fx_mode_t -- which field the fragment evaluates (0 = none)
    u32 ops;            // GUI_OP_* -- modifiers on whatever field arrived, orthogonal to it
    u32 tex;            // sampling model | bindless slot (GUI_TEX_MODE | index)

    /* GUI_OP_GLOW's dropoff, 1/px, and zero under every other record.  It rides this row rather
       than one of its own because the row is the one EVERY fragment already holds -- so a glow
       costs no second fetch, and a shape that does not glow pays nothing for the lane. */
    f32 glow_k;

    /* Row 1 -- per-field payload; see the alias table above. */

    f32 r_tl, r_tr, r_br, r_bl;

    /* Row 2 -- the EDGE: how wide the transition is, how thick a band is, and how the corner
       curves.  The turn is NOT here -- it rides the quad (gui_quad_t.xform), because an angle is
       per-instance and a style that carried one would mint a record per angle.

       corner_pow is the corner PROFILE: the exponent of the norm the corner arc is measured in.
       2 (and 0, the default) is a circular arc; higher fills the arc out toward the square it is
       inset from -- the continuously-curved corner.  Authored as a 0..1 smoothing amount and
       mapped once at the emit site (draw_set_corner_smooth).  It sits in this row rather than with
       the scalar parameters because every box fragment already loads this row for the feather. */

    f32 feather, border, corner_pow;

    /* The PATTERN ops' rotation, radians, wrapped to [0, pi) -- a lattice at `a` and at `a + pi`
       are the same lattice.  It sits in this row because row 7 has no lane left and every shape
       that carries a pattern already loads this one. */
    f32 pat_angle;

    /* Row 3 -- the scalar parameters and the second colour.
         ARC / PIE      param = radius, tube half-thickness, aperture (radians, HALF the sweep)
         CHECKER        param = cell, phase x, phase y            col_b = the alternate colour
         GRID           param = cell, line thickness, angle
         TILE_U         param_a = repeat count
         TEXT_EDGE      param_a = outline width                   col_b = the outline colour
         GUI_OP_PULSE   param_a = depth 0..1   (the rate and its wave are row 5)
         GUI_OP_GRAD    col_b   = the ramp's far colour
       (BOX's corner profile moved to row 2, beside the feather that shares its fetch.) */

    f32 param_a, param_b, param_c;
    u32 col_b;

    /* Row 4 -- the two things that are a SECOND of something the rows above state once, both in
       the shape's local frame (prim_local's frame, so both turn with the shape).
         GUI_OP_GRAD  grad = the ramp's axis, a UNIT direction -- under GRAD_CONIC the direction
                             the ramp PEAKS toward, and under a linear ramp the axis it runs
                             along, which the fragment divides by the extent the shape spans
                             there.  Deliberately NOT pre-divided: the divisor is a property of
                             the SIZE, and baking it in here gave the same ramp a record per size.
                             GRAD_RADIAL has no axis and leaves it zero.
         GUI_OP_CUT   cut  = the centre of the boundary the cut is taken against, as an offset
                             from this shape's own centre.  (0,0) -- every caller before the
                             directional shadow -- cuts against the shape itself. */

    f32 grad_x, grad_y, cut_dx, cut_dy;

    /* Row 5 -- the ANIMATION CLOCK, plus the ramp's midpoint.  The clock is decomposed into a
       TIMEBASE and a SHAPE, and every animating op reads the result rather than pc.time:

           phi = frac( anim_rate * time + phase )     where in the cycle we are, 0..1
           k   = curve( phi )                          how far along the effect that is, 0..1

       anim_rate is CYCLES PER SECOND for every op -- turns/sec for a spin, dash-periods/sec for
       the ants, Hz for a pulse are all the same number -- so one timebase serves all three and a
       record carrying several animates them coherently instead of letting them drift.  The PHASE
       is deliberately absent from this row: it rides the instance record (gui_fx_t.phase),
       since a set of staggered elements shares one rate and differs only in phase.

       At rate 0 the clock STANDS STILL at the instance's phase -- phi IS the phase, so
       k = curve( phase ) -- and the phase lane becomes a per-instance 0..1 VALUE PORT: a static
       level, angle or depth reaching the fragment through the fx record, with no style record
       per value and no re-tessellation when it changes.  A meter's fill (GUI_OP_CELL_FILL), a
       fixed spin angle, a per-element pulse depth are all this one lane; any op that reads k
       serves either way, so an op needs no separate "static" form.

       anim_curve / anim_param are the shaping stage (gui_curve_t): what a normalized phase does
       between its endpoints.  The param's meaning belongs to the curve -- an exponent, a step
       count, a duty -- the same way anim_rate's unit belongs to its op.  This is what makes a
       stepped spinner, an eased dash and a square-wave blink one mechanism rather than three.

       What each op does with k:
         GUI_OP_SPIN   k turns the local frame through one full revolution.  The frame carries
                       every field and op with it, so a spinner, a radar sweep and a rotating
                       dashed ring are all this one op over shapes that already exist.
         GUI_OP_DASH   k slides the pattern one dash period along the shape's boundary
                       coordinate -- the marching ants.
         GUI_OP_PULSE  k is the depth of the breath: coverage *= 1 - param_a * k.
         GUI_OP_SWELL  k moves the boundary: d -= swell * k, the instance's own amplitude
                       (gui_fx_t.swell) -- the shape grows or shrinks on the clock.
         GUI_OP_GRAD   grad_mid = the exponent bending the ramp's t about its midpoint, mapped
                       once at the emit site (ln 0.5 / ln mid); 0 means the linear default. */

    f32 anim_rate;
    u32 anim_curve;
    f32 anim_param, grad_mid;

    /* Row 6 -- PER-OP, and at most one of its claimants is live per record.  Each needs four
       lanes and none composes with another (a repeated shape has no single perimeter for a dash
       to walk; a notched one has no pattern), so they share the row rather than each taking one:

         GUI_OP_DASH           dash_period, dash_duty, dash_scroll  -- unused fourth lane
         GUI_OP_REPEAT         pitch x, pitch y, CELL half-extent x, y
         GUI_OP_REPEAT_POLAR   copy count, orbit radius, CELL half-extent x, y
         GUI_OP_CUT_SHAPE      cut half-extent x, y, corner radius, edge aa

       Both repetition ops put the cell in the SAME two lanes, so the field reads its extent from
       one place whichever fold ran.

       GUI_OP_DASH's pattern is in ARC-LENGTH px along the shape's perimeter (the draw_arc_dashed
       vocabulary, walked around a box instead of a circle).  The emit site snaps the period so
       whole cycles fit the perimeter -- a closed dashed border meets itself.  This is its own row
       for the reason row 4 exists: PULSE owns param_a, and a dash that fought it for the lane would
       rebuild the "ops that cannot compose" trap the record deleted.

       dash_scroll is how far the clock slides the pattern: periods per animation cycle, so 1 is
       the marching ants and 0 pins the dashes to the shape.  0 is what a SPINNING dashed ring
       wants -- its boundary coordinate is measured in the rotating frame, so the dashes already
       travel with it and any scroll on top of that is a second, unwanted crawl.

       GUI_OP_REPEAT states the lattice the fragment folds into: how far apart the copies sit, and
       how big ONE of them is.  The count is recovered from the quad's extent (see the op). */

    f32 dash_period, dash_duty, dash_scroll, reserved_c;

    /* Row 7 -- the PATTERN ops, which paint or cut across a shape rather than being one.  At most
       one of them is live per record: they share this row, and a quad that wanted a checkerboard
       AND a lattice is two quads by nature.  They were fields once, which is exactly why they
       could not sit inside a rounded panel or a sector -- a field IS the shape.

       CHECKER and GRID work in SV_Position pixels, not the shape's HALF2 effect coordinate,
       whose ulp reaches a full pixel at the corners of a fullscreen panel.  The pattern ANCHORS
       AT THE QUAD'S OWN RECT: the fragment takes the pixel coordinate relative to the box origin
       it already receives (the placement interpolant, exact quarter-pixel fixed point), so the
       record holds no absolute position and one record serves every placement -- a row of
       checkered swatches dedups to a single style, and a moving patterned surface changes only
       its quad.  pat_phase carries only the residue of an EXPLICIT anchor relative to that
       origin (draw_grid's panning content origin); box-anchored callers send zero.  The emit
       site derives that residue against the same quantized pitch this row carries, since a phase
       and a pitch that disagree walk the pattern off its anchor across a wide panel.

         GUI_OP_TILE_U     pat_size = repeat count multiplied into u before sampling
         GUI_OP_TEXT_EDGE  pat_size = outline width px      pat_col = the outline colour
         GUI_OP_CHECKER    pat_cell = cell px               pat_col = the alternate colour
                           pat_phase = per-axis anchor residue, a fraction of the TWO-cell
                           colour period (one cell of phase would simply swap the two colours)
         GUI_OP_GRID       pat_cell = cell px, pat_size = line width px, pat_angle = row 2
                           pat_phase = per-axis anchor residue, a fraction of ONE cell
       pat_phase is a unorm16 pair through gui_uv_pack, x in the low half. */

    f32 pat_cell, pat_size;
    u32 pat_phase, pat_col;

} gui_prim_t;

/* 128 bytes = eight std430 rows of four 32-bit components, so the fragment indexes the buffer as
   `prim * GUI_PRIM_ROWS + row` with no padding to account for.  Pinned because the shaders spell
   that stride as a literal. */

#define GUI_PRIM_ROWS   8u
#define GUI_PRIM_BYTES  ( GUI_PRIM_ROWS * 16u )

ORB_STATIC_ASSERT( sizeof( gui_prim_t ) == GUI_PRIM_BYTES,
                   "gui_prim_t must stay whole 16-byte rows -- the shaders index it as vec4[]" );

/* The modifier bits -- a WORD OF THEIR OWN (gui_prim_t.ops): an op composes with any field and
   with any other op, so it can never share space with something a particular field
   re-partitions.  Grouped by what they touch and numbered in that order; the shader-side copy
   (gui_common.hlsli) spells these values again, so the two lists move together.

   FIELD OPS -- reshape the boundary's distance before it resolves to coverage. */

#define GUI_OP_BAND       ( 1u << 0 )   /* bend the field into a border of `border` px           */
#define GUI_OP_CUT        ( 1u << 1 )   /* cut the interior away -- the drop shadow's skirt      */
#define GUI_OP_INSET      ( 1u << 2 )   /* turn the falloff inward -- the inner shadow           */

/* GLOW resolves the OUTSIDE of the field through an exponential instead of the feather's linear
   ramp -- light rather than blur.  The interior stays solid, which is the filled core a halo
   behind a translucent subject wants, and under GUI_OP_BAND the field is already the band's, so
   the falloff runs both ways off a ring for free.  `glow_k` (row 0) is the dropoff; the emit site
   derives it from the reach the caller asked for.  Reads only the field's distance, so every
   shape in the catalogue glows without a line of per-field work. */
#define GUI_OP_GLOW       ( 1u << 3 )

/* The clock-driven DISTANCE BIAS: the boundary itself moves.  The field's distance is shifted by
   swell * k, so the shape grows from its authored rect to `swell` px past it (negative shrinks)
   as the clock's k runs 0..1 -- geometry animating with byte-identical commands, where the CPU
   tween (anim_ease) re-tessellates every frame.  Runs BEFORE the field-bending ops, so BAND's
   ring rides the moving boundary outward (the sonar ripple), GLOW's halo swells, INSET breathes.

   The amplitude is per-INSTANCE (gui_fx_t.swell), not a style lane: differently-sized swells off
   one shared record, and the vertex stage grows the covering by the same lane it already fetches
   -- the reach costs no extra loads in either stage.  At rate 0 the clock stands still at the
   instance's phase, so the lane is a static per-element size offset -- the value port, again.

   Box family only (the fx_box faces): sectors spend their param lanes, and their covering does
   not grow.  Excluded from the band covering (tess_band_worth_it) -- the hole is measured at
   rest, and a swelling boundary moves into it. */
#define GUI_OP_SWELL      ( 1u << 4 )

/* SUBTRACTION proper: a SECOND rounded box, stated whole in row 6 (half-extents, corner radius,
   edge aa) and centred by row 4's cut vector, carved out of whatever coverage the field
   produced.  This is the one composition the renderer cannot get from more quads: painter's
   order unions, the clip table intersects, but nothing downstream of the blend can un-paint --
   so the notched avatar, the ticket silhouette and the punched card live on the record.

   Distinct from GUI_OP_CUT on purpose: CUT is the drop shadow's flush hard cut against the
   shape's OWN outline (zero extra lanes, dedups with the shadow), where this one carries its own
   geometry and antialiases its edge through its own aa.  It claims row 6, so it is mutually
   exclusive with DASH and the repetition folds -- a notched shape has no perimeter pattern to
   walk, and the one place row 6 is written keeps the exclusivity visible. */
#define GUI_OP_CUT_SHAPE  ( 1u << 5 )

/* PAINT OPS -- what colour the resolved coverage takes. */

#define GUI_OP_SELF       ( 1u << 6 )   /* solid colour: do not consult the texel at all         */
#define GUI_OP_GRAD       ( 1u << 7 )   /* ramp the fill from its own colour toward col_b        */

/* The ramp's SHAPE, under GUI_OP_GRAD.  At most one; none is the linear ramp, and the fragment
   tests along-the-boundary first, then radial, so an over-set record reads as one of them rather
   than as undefined.  Bits rather than a small enum because they belong to the op word every
   other modifier lives in -- and unlike the modifiers they are alternatives, which is a property
   of what a ramp IS, not of the storage. */

#define GUI_OP_GRAD_RADIAL  ( 1u << 8 )   /* centre -> rim, against the shape's own half-extent */
#define GUI_OP_GRAD_CONIC   ( 1u << 9 )   /* angular, mirrored about the grad axis -- a sheen   */

/* The ramp taken along the BOUNDARY instead of across the shape: t = s / len, the field's own
   arc-length coordinate over its total -- the same axis GUI_OP_DASH cuts on, so it reaches every
   shape that states one.  The value-sweep arc (draw_arc_gradient) is this op on GUI_FX_ARC; a
   dashed border or a capsule stroke ramps the same way for free.  Reads col_b like every other
   ramp, and grad_mid bends it like every other ramp. */

#define GUI_OP_GRAD_ALONG   ( 1u << 10 )

/* The CELL ops -- per-copy variation under the repetition folds.  The fold computes which copy a
   fragment is in to place it; these read that index back as a slot coordinate, (i + 0.5) / n
   over the whole set, so the copies can DIFFER while still sharing one record and one quad.
   Both anchor to the animation clock's k, which at rate 0 is simply the instance's phase -- a
   static per-element value reaching the fragment through the fx record's phase lane.

   GRAD_CELL is a GRAD family shape (set GUI_OP_GRAD with it): the ramp runs toward col_b by COPY
   rather than by position.  On REPEAT_POLAR it TRAILS the clock's anchor and wraps -- drive
   anim_rate and the bright head marches around a ring of dots that never move, fading behind
   itself: the tail spinner, re-tessellating nothing.  On REPEAT the strip has ends, so the ramp
   clamps instead of wrapping.

   CELL_FILL cuts coverage to the copies whose slot sits at or under k -- the segmented meter,
   still the set's one quad.  0 lights none, 1 lights all, uniform steps between; no shape ever
   shows a partial copy. */

#define GUI_OP_GRAD_CELL    ( 1u << 11 )
#define GUI_OP_CELL_FILL    ( 1u << 12 )

#define GUI_OP_FRAME    ( 1u << 13 )  /* composite a border band of `border` px OVER the fill --
                                         body + border in ONE quad.  The band's colour rides the
                                         INSTANCE record (gui_fx_t.col_border), not the style -- an
                                         animated border never adds a style record              */

/* PATTERN OPS -- what a shape is FILLED or CUT with, as opposed to what shape it is.  Each was
   a field until it became clear that occupying the field slot is what stopped a checkerboard
   from being round.  All read row 7; at most one per record. */

#define GUI_OP_TILE_U     ( 1u << 14 )  /* multiply u by pat_size before sampling -- the tiled
                                           atlas strip a dashed line's stipple row wants        */
#define GUI_OP_TEXT_EDGE  ( 1u << 15 )  /* SDF text with pat_col OUTSIDE the glyph boundary      */
#define GUI_OP_CHECKER    ( 1u << 16 )  /* alternate the fill with pat_col in cell-sized squares */
#define GUI_OP_GRID       ( 1u << 17 )  /* cut coverage to a line lattice; the fill colour draws
                                           the LINES, so it layers over anything                */
#define GUI_OP_STRIPES    ( 1u << 18 )  /* GRID: cut on one axis only -- a stripe field          */

/* REPETITION OPS -- fold the shape-local frame so ONE quad draws many copies (row 6). */

/* REPEAT folds the shape-local frame into one cell of a bounded lattice, so ONE quad draws nx by
   ny copies of whatever field the record names: a dot grid, a tick strip, a segmented bar.  The
   quad spans the whole set and the fragment states the cell, which is why the cell's half-extent
   rides row 6 -- the quad's own extents are already spoken for by the covering.

   The copy COUNT is not stored: it falls out of the quad's extent against the pitch, which is what
   makes four lanes enough.  That works only while the emit site sizes the quad as
   (n-1)/2 * pitch + cell on each axis, and keeps the cell under half the pitch so copies do not
   touch -- both true of every lattice a UI draws.  See tess_repeat_box. */
#define GUI_OP_REPEAT       ( 1u << 19 )

/* The same fold taken ANGULARLY: n copies on a circle of radius `orbit` about the shape centre.
   The cell's frame turns with its position, so a rectangular cell points outward and a ring of
   them is a dial face; a square or round one is a dot ring.

   It composes with GUI_OP_SPIN for free, and that is the reason it is worth having: SPIN turns the
   frame upstream of this fold, so a whole rotating dot ring is one quad whose record is
   byte-identical every frame.  What it cannot do is vary the copies -- they share one record and
   one quad colour, so the ring turns as a rigid body rather than chasing a bright head. */
#define GUI_OP_REPEAT_POLAR ( 1u << 20 )

/* CLOCK OPS -- animate on the frame clock (row 5's timebase, shaped by its curve).  All read
   the record's anim_rate and the instance's phase against pc.time, which is why an animation
   re-emits NOTHING: the record is byte-identical every frame and only the push constant moves.
   The owner still calls gui()->request_redraw() while it runs (GUI_FX_TIME_WRAP). */

#define GUI_OP_PULSE      ( 1u << 21 )  /* breathe coverage on the frame clock (param_a depth)   */
#define GUI_OP_SPIN       ( 1u << 22 )  /* rotate the local frame at anim_rate turns/sec         */
#define GUI_OP_DASH       ( 1u << 23 )  /* cut coverage by the perimeter dash pattern (row 6),
                                           scrolled at anim_rate px/sec -- the marching ants     */

/* OUTPUT OPS -- touch the final colour only. */

#define GUI_OP_DITHER     ( 1u << 24 )  /* add +-0.5/255 screen-space noise to the output, so a
                                           wide soft ramp lands on 8-bit without banding         */

/*==============================================================================================
    The QUAD RECORD -- the renderer's per-shape geometry unit.

    Note: each 4 element group translates to a <float4> in the shader (see GUI_QUAD_ROWS) 

    There is no vertex buffer and no index buffer.  A shape is stored ONCE: a draw is a plain
    `cmd_draw` of 6 * N bare vertices, and the vertex stage computes quad = SV_VertexID / 6,
    corner = SV_VertexID % 6, fetches this record from a bindless storage buffer and expands
    `centre +- (half-extent + pad)` itself.  The pad is the style's feather plus the AA guard,
    applied at expansion -- cx/cy/hw/hh here are the TRUE shape extents, never pre-inflated.

    The record names a gui_prim_t used as a pure STYLE record.  Everything that varies per INSTANCE
    while the shape stays the same lives off the style: placement, colour and clip here, and the
    rarer lanes -- turn, animation phase, border colour, texture rect -- one indirection away in a
    gui_fx_t.  That is the whole rule the split follows: a value on the style side mints a record
    per instance, a value on the quad side costs four bytes on every glyph that will never read it,
    and a value in the fx record costs neither as long as most quads want none of it.
==============================================================================================*/

/* One resident glyph's atlas rect, ID-indexed (draw/gui_glyph_table.c).  A glyph quad names an
   entry instead of carrying the rect: the table rewrites in place when the atlas repacks, so
   cached text geometry survives a move that would leave a baked UV pointing at another tenant's
   pixels.  Both corners use the gui_uv_pack encoding, x in the low half. */

typedef struct
{
    u32 uv0;            // texcoord min corner, packed unorm16 pair
    u32 uv1;            // texcoord max corner, packed unorm16 pair

} gui_glyph_uv_t;

/* Entries per font registry slot: the dense ASCII block plus room for extended codepoints, as a
   FIXED stride rather than a packed per-slot base.  Packing would be smaller and would shift every
   later slot's IDs when a font loads or is released, invalidating IDs already baked into retained
   window geometry -- so an ID depends on nothing that moves.  The table's 8192 entries are exactly
   the 13 bits the compacted quad record reserves for one.  Sized against GUI_FONT_REGISTRY_MAX by
   a static assert in draw/gui_glyph_table.c, which owns the build. */

#define GUI_GLYPH_SLOT_STRIDE  512u
#define GUI_GLYPH_TABLE_MAX    ( 16u * GUI_GLYPH_SLOT_STRIDE )

typedef struct
{
    /* Placement, in QUARTER-PIXEL fixed point (gui_quad_pos_pack / gui_quad_ext_pack).  The
       coordinate space is the SURFACE's, the same pixel-space ortho the mvp is built in, so no
       origin travels with the record.

       The centre is signed because clipping happens in the fragment: a scrolled-out row still
       reaches the vertex stage with its real coordinates and is discarded per pixel, so negative
       placements are ordinary traffic rather than an error.  The half-extents are unsigned and
       reach four times further, which is what a fullscreen backdrop on a large display needs. */

    i16 cx, cy;         // centre,      1/4 px, +-8192 px
    u16 hw, hh;         // half-extent, 1/4 px, 0..16383.75 px

    u32 abgr;           // packed colour
    u32 idx;            // tag | clip | rule | style | glyph | fx -- see gui_quad_idx below

} gui_quad_t;

/* 16 bytes = ONE std430 row, indexed by the vertex stage as `quad * GUI_QUAD_ROWS`, so a shape
   costs a single load.  Pinned because the shaders spell that stride as a literal. */

#define GUI_QUAD_ROWS   1u
#define GUI_QUAD_BYTES  ( GUI_QUAD_ROWS * 16u )

ORB_STATIC_ASSERT( sizeof( gui_quad_t ) == GUI_QUAD_BYTES,
                   "gui_quad_t must stay whole 16-byte rows -- the vertex stage indexes it as vec4[]" );

/* The placement quantum: quarter pixels.  A snapped fill and a whole glyph land on it exactly --
   both have integer origins and integer sizes, so their centres and half-extents are multiples of
   1/2 -- and the shapes that deliberately do NOT snap (a disc, a rotated box, a polyline segment)
   keep a quarter-pixel of positional detail, which is finer than the antialiasing band they are
   drawn with.

   Out-of-range placements CLAMP rather than wrap.  A coordinate past +-8192 px is scrolled-out
   content far outside every clip rect, so the clamped quad lands outside them too and is discarded
   exactly as the true one would have been; wrapping would drag it back into view. */

#define GUI_QUAD_POS_SCALE   4.0f
#define GUI_QUAD_POS_MAX     32767.0f
#define GUI_QUAD_EXT_MAX     65535.0f

static inline i16
gui_quad_pos_pack( f32 px )
{
    f32 q = px * GUI_QUAD_POS_SCALE;
    q = ( q < 0.0f ) ? q - 0.5f : q + 0.5f;
    if ( q >  GUI_QUAD_POS_MAX ) q =  GUI_QUAD_POS_MAX;
    if ( q < -GUI_QUAD_POS_MAX - 1.0f ) q = -GUI_QUAD_POS_MAX - 1.0f;
    return (i16)q;
}

static inline u16
gui_quad_ext_pack( f32 px )
{
    f32 q = px * GUI_QUAD_POS_SCALE + 0.5f;
    if ( q < 0.0f ) q = 0.0f;
    if ( q > GUI_QUAD_EXT_MAX ) q = GUI_QUAD_EXT_MAX;
    return (u16)q;
}

/* The INSTANCE EXTRAS record -- the lanes only a minority of quads carry, named by the quad's fx
   index instead of costing bytes on every glyph that will never read them.

   All of them are per-INSTANCE, not per-shape: a rotation, a stagger, a border colour and a
   texture rect each vary while the shape stays the same, so putting them on the style record would
   mint one style per angle, per stagger, per animated frame, per icon.  They are also rare
   TOGETHER -- text carries none of them -- which is what makes a side record cheaper than a lane.
   Consecutive quads that want the same values share one record (tess_fx_local), so a run of
   identically framed rows or a polyline of one direction costs a single entry.

   Records live in the STYLE ARENA, four to a gui_prim_t slot ("fx page"), and the quad names one
   by its slot-local ROW index.  That is what keeps the whole feature free of new bookkeeping: the
   record arena's per-slot base, relocation, volatile reservation and upload ranges already carry
   anything stored there, and a row index is as repack-stable as the style index beside it. */

typedef struct
{
    /* Row A -- the instance lanes every fetch of this record starts with. */

    // xform: the shape's TURN, as a unit (cos, sin) through the uv pair's encoding, remapped from
    //   [-1,1] (gui_xform_pack).  Exactly 0 means IDENTITY, which is what an unrotated shape
    //   leaves behind -- so a quad with no fx record reads as unrotated.  A CAPSULE's direction is
    //   this same pair.
    //   BOTH stages read it.  The vertex stage rotates the covering corners (every rule but BBOX,
    //   whose covering is already axis-aligned), and the fragment builds the shape-local frame
    //   every field works in from it -- composed with OP_SPIN's clock angle, when that op is set.
    u32 xform;

    // phase: the animation phase, a unorm16 over one cycle (gui_phase_pack).  The RATE stays on
    //   the style -- every spinner in a set turns at the same speed, and staggering the set is the
    //   only thing phase is for.  0 = in step with the clock.  When the style's rate is 0 the
    //   clock stands still HERE: this lane is then a per-instance 0..1 value port (a meter level,
    //   a fixed angle -- see the row 5 doc), updated without touching any style record.
    u32 phase;

    // col_border: GUI_OP_FRAME's border band colour, and nothing else.  Named for the one thing it
    //   carries rather than "the second colour", because the STYLE has a second colour of its own
    //   (gui_prim_t.col_b) and the two are not interchangeable: every second colour belonging to
    //   the SHAPE -- GRAD's far end, CHECKER's alternate, TEXT_EDGE's outline -- lives on the
    //   style and deduplicates with it.  A border colour does not belong to
    //   the shape, so it rides the instance: an animated border -- or an animated fill, which
    //   rides the quad in `abgr` -- would otherwise mint a style record per frame.
    u32 col_border;

    // swell: GUI_OP_SWELL's amplitude, px -- how far the boundary travels at the clock's k = 1
    //   (negative shrinks).  Per-instance for the same reason the turn is: a set of elements
    //   swelling by their own ranges shares one style record.  BOTH stages read it -- the vertex
    //   stage grows the SKIRT covering by its positive part off the row it already fetches, the
    //   fragment biases the field's distance -- so the reach costs no extra load anywhere.
    //   0.0f when unused, which is also all-zero bits, so the "no record" default still holds.
    f32 swell;

    /* Row B -- the TEXTURE RECT, the min and max corners of the atlas span this instance samples,
       each two unorm16 over [0,1] (gui_uv_pack).  It sits here for the same reason the three lanes
       above do: a sprite's rect is per-instance, so on the style record it would mint one style per
       icon, and on the quad it would cost eight bytes on every glyph and every flat fill that will
       never sample a texel.

       Most textured quads need nothing from row A, and most quads that need row A sample no
       texture, so a record usually carries one half or the other.  The pair is kept as ONE record
       kind anyway -- two would need a second tag bit the index word does not have.

       Whole GLYPHS never come here: their rect is resolved from the glyph table by ID (gui_quad_
       idx_glyph), which is what lets cached text survive an atlas repack. */

    u32 uv0;            // texcoord min corner, packed unorm16 pair
    u32 uv1;            // texcoord max corner, packed unorm16 pair
    u32 reserved_b;     // both zero-written, for the dedup memo above
    u32 reserved_c;

} gui_fx_t;

#define GUI_FX_ROWS     2u
#define GUI_FX_BYTES    ( GUI_FX_ROWS * 16u )

ORB_STATIC_ASSERT( sizeof( gui_fx_t ) == GUI_FX_BYTES,
                   "gui_fx_t must stay whole 16-byte rows -- it is addressed by row index" );

ORB_STATIC_ASSERT( GUI_PRIM_ROWS % GUI_FX_ROWS == 0,
                   "fx records tile a style record exactly -- a page holds GUI_PRIM_ROWS/GUI_FX_ROWS" );

/*==============================================================================================

    VERTEX EXPANSION RULES:

    The vertex stage's EXPANSION RULE (flags bits 0-1): how the covering corners derive from
    the stored extents. 
   
    Only SKIRT and CAPSULE take the pad; the other two cover exactly what they  state.
==============================================================================================*/

#define GUI_QUAD_RULE_EXACT    0u   // corners at +-hw/hh, turned by the quad's own xform 
#define GUI_QUAD_RULE_SKIRT    1u   // EXACT, grown by the SDF pad (style feather/2 + 1) on both
                                    // axes, with the uv span scaled out to match.

#define GUI_QUAD_RULE_CAPSULE  2u   // hw = half-length, hh = radius: along grows by hh + pad
#define GUI_QUAD_RULE_BBOX     3u   // stored extents ARE the covering, expanded axis-aligned and
                                    // NOT turned (the arc family -- its local frame is a
                                    // reflection the vertex rotation cannot reproduce, so the
                                    // fragment takes the turn instead).

/* The `idx` word is a TAGGED UNION, not one fixed layout.  That is what makes the budget close:
   a whole glyph needs an atlas ID and no style record, and every other shape needs a style record
   and no atlas ID, so the two never have to fit side by side.

   The tag is the top two bits.  Clip sits at the bottom of ALL layouts, in the same place, so it
   decodes without consulting the tag at all.

     tag SHAPED (0)   bits 0-3    clip entry, slot-local (GUI_WIN_CLIP_MAX = 16 per window slab)
                      bits 4-5    GUI_QUAD_RULE_* -- the expansion rule
                      bits 6-16   style record, slot-local
                      bits 17-29  fx record, slot-local ROW index into the style arena

     tag GLYPH  (1)   bits 0-3    clip entry, as above
                      bit  4      the SDF atlas rather than the coverage one
                      bits 5-17   glyph-table ID (GUI_GLYPH_TABLE_MAX = 8192)
                      bits 18-29  fx record, twelve bits of the same row index

     tag BAND   (2)   bits 0-3    clip entry, as above
                      bits 4-5    WHICH BAND of the four (top, bottom, left, right)
                      bits 6-16   style record, as SHAPED
                      bits 17-29  fx record, as SHAPED
                    SHAPED's layout with the rule field re-read: a band is always drawn SKIRT, so
                    those two bits are free to name the band instead.  See gui_quad_band below.

     tag GLYPH_STYLED (3)
                      bits 0-3    clip entry, as above
                      bit  4      the SDF atlas bit, as GLYPH
                      bits 5-17   glyph-table ID, as GLYPH
                      bits 18-28  style record, slot-local or palette
                      bit  29     spare
                    A glyph that names a STYLE: the ops ride the record (an SDF outline, a
                    gradient, a glow) while the atlas rect stays a table ID and the texture stays
                    the push block's -- so styled text keeps everything that makes plain text
                    cheap.  No fx bits: a ROTATED styled run takes the SHAPED fallback instead.

   The words are exactly full: there is no spare bit to widen a field with, so raising a pool
   past what its field can name is a re-plan of this union, not a constant bump.  The glyph-table ID
   and the clip entry sit AT their fields' ceilings; the style and fx fields have slack at the caps
   currently shipping, and the static asserts beside GUI_MAX_PRIMS are what keep that true.

   Clip is per QUAD, not per style: two identically styled rows in different scroll regions must
   still share one style.  The rule is per quad for the same reason -- it is a property of the
   SHAPE KIND, and one style (a plain fill) serves shapes whose coverings differ.  A GLYPH spends
   no bits on it because a glyph is always drawn EXACT.

   fx 0 = no record, which reads as identity turn / zero phase / no border.  Row 0 of a slot's
   records is never an fx record, which is what lets 0 mean that -- the tessellator leaves record 0
   alone rather than opening a page there. */

#define GUI_QUAD_TAG_SHIFT     30u
#define GUI_QUAD_TAG_MASK      0x3u
#define GUI_QUAD_TAG_SHAPED    0u
#define GUI_QUAD_TAG_GLYPH     1u
#define GUI_QUAD_TAG_BAND      2u
#define GUI_QUAD_TAG_GLYPH_STYLED  3u

#define GUI_QUAD_CLIP_SHIFT    0u
#define GUI_QUAD_CLIP_MASK     0xFu
#define GUI_QUAD_RULE_SHIFT    4u
#define GUI_QUAD_BAND_SHIFT    4u   // the rule field, re-read under the BAND tag
#define GUI_QUAD_STYLE_SHIFT   6u
#define GUI_QUAD_STYLE_MASK    0x7FFu
#define GUI_QUAD_FX_SHIFT      17u
#define GUI_QUAD_FX_MASK       0x1FFFu

/*==============================================================================================

    THE BAND COVERING -- four quads where one would do, to stop rasterizing what cannot paint.

    A quad's covering is a rectangle, so a shape that only paints near its boundary rasterizes its
    whole middle at zero coverage: a drop-shadow skirt (GUI_OP_CUT) whose hole is the caster, an
    outline (GUI_OP_BAND) a few px thick on a panel-sized rect, an inner shadow (GUI_OP_INSET)
    that reaches a fixed depth in.  On a 600x400 window the paying region is under a tenth of the
    covering.

    A BAND-tagged quad carries the SAME placement, style, clip, colour and fx row as the single
    quad it replaces -- only the expansion differs.  Four of them tile the frame between the shape's
    padded outer rect and the rect its field provably leaves at zero, so:

      - the FRAGMENT is untouched.  Placement still arrives from the quad, so the field is
        evaluated in exactly the frame it was before and the omitted region is one the shader
        would have resolved to zero anyway.  This cannot change a pixel.
      - nothing about batching changes.  These are ordinary quads in the same buffer and the same
        draw, so sort keys, clip entries and z-order against other windows are untouched.

    The vertex stage derives the hole from the style it is already reading (gui_quad.vs.hlsl,
    band_local) and CLAMPS it, so the four bands tile without gap or overlap whatever the record
    holds.  The tessellator decides only WHETHER the trade is worth four quads instead of one.
==============================================================================================*/

#define GUI_QUAD_BAND_TOP      0u
#define GUI_QUAD_BAND_BOTTOM   1u
#define GUI_QUAD_BAND_LEFT     2u
#define GUI_QUAD_BAND_RIGHT    3u
#define GUI_QUAD_BAND_COUNT    4u

#define GUI_QUAD_SDF_BIT       ( 1u << 4 )
#define GUI_QUAD_GLYPH_SHIFT   5u
#define GUI_QUAD_GLYPH_MASK    0x1FFFu
#define GUI_QUAD_GFX_SHIFT     18u
#define GUI_QUAD_GFX_MASK      0xFFFu
#define GUI_QUAD_GSTYLE_SHIFT  18u   // GLYPH_STYLED: the style record, where GLYPH keeps fx bits
#define GUI_QUAD_GSTYLE_MASK   0x7FFu

/* Pack a SHAPED index word.  Each field is masked rather than trusted: an index past its own
   width would otherwise silently corrupt the field above it, where a clamped one draws with the
   wrong style or clip and the arena's own overflow flag reports the real cause. */

static inline u32
gui_quad_idx( u32 rule, u32 clip, u32 style, u32 fx_row )
{
    ORB_ASSERT( clip   <= GUI_QUAD_CLIP_MASK &&
                style  <= GUI_QUAD_STYLE_MASK && 
                fx_row <= GUI_QUAD_FX_MASK );

    return ( ( clip   & GUI_QUAD_CLIP_MASK  ) << GUI_QUAD_CLIP_SHIFT  )
         | ( ( rule   & 0x3u                ) << GUI_QUAD_RULE_SHIFT  )
         | ( ( style  & GUI_QUAD_STYLE_MASK ) << GUI_QUAD_STYLE_SHIFT )
         | ( ( fx_row & GUI_QUAD_FX_MASK    ) << GUI_QUAD_FX_SHIFT    )
         | ( GUI_QUAD_TAG_SHAPED << GUI_QUAD_TAG_SHIFT );
}

/* Pack a BAND index word -- SHAPED's, with the rule field naming which of the four bands this quad
   expands to.  Everything else is identical by construction: the four quads of one shape differ in
   this field and nothing else, so they resolve the same style, clip and fx record. */

static inline u32
gui_quad_idx_band( u32 band, u32 clip, u32 style, u32 fx_row )
{
    ORB_ASSERT( band   <  GUI_QUAD_BAND_COUNT &&
                clip   <= GUI_QUAD_CLIP_MASK  &&
                style  <= GUI_QUAD_STYLE_MASK &&
                fx_row <= GUI_QUAD_FX_MASK );

    return ( ( clip   & GUI_QUAD_CLIP_MASK  ) << GUI_QUAD_CLIP_SHIFT  )
         | ( ( band   & 0x3u                ) << GUI_QUAD_BAND_SHIFT  )
         | ( ( style  & GUI_QUAD_STYLE_MASK ) << GUI_QUAD_STYLE_SHIFT )
         | ( ( fx_row & GUI_QUAD_FX_MASK    ) << GUI_QUAD_FX_SHIFT    )
         | ( GUI_QUAD_TAG_BAND << GUI_QUAD_TAG_SHIFT );
}

/* Pack a GLYPH index word.  `sdf` picks which of the two text atlases the fragment samples; the
   slot itself comes from the push block, so a glyph names no style record at all. */

static inline u32
gui_quad_idx_glyph( u32 clip, u32 glyph_id, bool sdf, u32 fx_row )
{
    ORB_ASSERT( clip     <= GUI_QUAD_CLIP_MASK && 
                glyph_id <= GUI_QUAD_GLYPH_MASK && 
                fx_row   <= GUI_QUAD_GFX_MASK );

    return ( ( clip     & GUI_QUAD_CLIP_MASK  ) << GUI_QUAD_CLIP_SHIFT  )
         | ( sdf ? GUI_QUAD_SDF_BIT : 0u )
         | ( ( glyph_id & GUI_QUAD_GLYPH_MASK ) << GUI_QUAD_GLYPH_SHIFT )
         | ( ( fx_row   & GUI_QUAD_GFX_MASK   ) << GUI_QUAD_GFX_SHIFT   )
         | ( GUI_QUAD_TAG_GLYPH << GUI_QUAD_TAG_SHIFT );
}

/* Pack a GLYPH_STYLED index word: the glyph arm with a STYLE where the fx bits were.  The record
   carries the run's ops (an outline, a gradient); the texture still comes from the push block, so
   styled text keeps the repack stability plain text has. */

static inline u32
gui_quad_idx_glyph_styled( u32 clip, u32 glyph_id, bool sdf, u32 style )
{
    ORB_ASSERT( clip     <= GUI_QUAD_CLIP_MASK &&
                glyph_id <= GUI_QUAD_GLYPH_MASK &&
                style    <= GUI_QUAD_GSTYLE_MASK );

    return ( ( clip     & GUI_QUAD_CLIP_MASK   ) << GUI_QUAD_CLIP_SHIFT   )
         | ( sdf ? GUI_QUAD_SDF_BIT : 0u )
         | ( ( glyph_id & GUI_QUAD_GLYPH_MASK  ) << GUI_QUAD_GLYPH_SHIFT  )
         | ( ( style    & GUI_QUAD_GSTYLE_MASK ) << GUI_QUAD_GSTYLE_SHIFT )
         | ( GUI_QUAD_TAG_GLYPH_STYLED << GUI_QUAD_TAG_SHIFT );
}

static inline u32 gui_quad_tag  ( u32 idx ) { return ( idx >> GUI_QUAD_TAG_SHIFT   ) & GUI_QUAD_TAG_MASK;   }
static inline u32 gui_quad_clip ( u32 idx ) { return ( idx >> GUI_QUAD_CLIP_SHIFT  ) & GUI_QUAD_CLIP_MASK;  }
static inline u32 gui_quad_rule ( u32 idx ) { return ( idx >> GUI_QUAD_RULE_SHIFT  ) & 0x3u;                }
static inline u32 gui_quad_band ( u32 idx ) { return ( idx >> GUI_QUAD_BAND_SHIFT  ) & 0x3u;                }
static inline u32 gui_quad_style( u32 idx ) { return ( idx >> GUI_QUAD_STYLE_SHIFT ) & GUI_QUAD_STYLE_MASK; }
static inline u32 gui_quad_fx   ( u32 idx ) { return ( idx >> GUI_QUAD_FX_SHIFT    ) & GUI_QUAD_FX_MASK;    }
static inline u32 gui_quad_glyph( u32 idx ) { return ( idx >> GUI_QUAD_GLYPH_SHIFT ) & GUI_QUAD_GLYPH_MASK; }

/* UV -> two unorm16 -- the packing the instance record's uv0/uv1 lanes carry.  Clamped, because that
   is the only thing the format can do with an out-of-range coordinate -- a caller that wants U
   past 1 asks for GUI_OP_TILE_U instead.

   The assert is the point of this function: clamping is SILENT, and a primitive that quietly loses
   its tiling renders as one stretched texel rather than as an error.  Debug catches the mistake at
   the quad that made it; release keeps the clamp, which at least stays inside the atlas. */

static inline u32
gui_uv_pack( f32 u, f32 v )
{
    ORB_ASSERT( u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f );
    u = ( u < 0.0f ) ? 0.0f : ( ( u > 1.0f ) ? 1.0f : u );
    v = ( v < 0.0f ) ? 0.0f : ( ( v > 1.0f ) ? 1.0f : v );
    return (u32)( u * 65535.0f + 0.5f ) | ( (u32)( v * 65535.0f + 0.5f ) << 16 );
}

/* The turn, packed into the fx record's `xform`: a unit (cos, sin) through the same unorm16 pair, so
   both sides share one encoding.  The all-zero word is reserved for IDENTITY and cannot collide --
   (1, 0) packs to (0xFFFF, 0x8000). */

static inline u32
gui_xform_pack( f32 cs, f32 sn )
{
    if ( cs == 1.0f && sn == 0.0f )
        return 0u;                        /* identity, the common case, states itself as zero */
    return gui_uv_pack( cs * 0.5f + 0.5f, sn * 0.5f + 0.5f );
}

/* The animation phase into the fx record's `phase` lane: cycles, wrapped to [0,1), as a unorm16. */

static inline u32
gui_phase_pack( f32 cycles )
{
    f32 f = cycles - (f32)(i32)cycles;             /* wrap; the sign is handled just below */
    if ( f < 0.0f ) f += 1.0f;
    return (u32)( f * 65535.0f + 0.5f ) & 0xFFFFu;
}

/* The phase that makes a cycle BEGIN at t0 -- the whole of what turns the periodic shader clock
   into a one-shot, and the reason a per-instance start time never had to reach the quad.

   The fragment computes phase = frac( rate*time + phase ), so choosing phase = -t0*rate puts a
   cycle boundary exactly on t0: from there the phase rises 0 -> 1 across one duration and IS the
   transition's progress.  What the shader cannot tell on its own is WHICH cycle it is in, so the
   caller stops asking for the animation when the duration is up (gui_api.h, anim_once) -- which
   is the same frame it would switch to drawing the settled state anyway.

   `duration` is seconds; the rate that goes with this phase is 1/duration.  The result is in
   cycles and may be far outside [0,1) -- gui_phase_pack wraps it, in either direction.

   The half-step bias is what keeps the transition from starting at its END.  The phase lane is
   unorm16, so packing rounds either way; a phase that rounds DOWN puts t0 a hair before the cycle
   boundary, where the fragment's frac() reads ~1 rather than 0.  Biasing by half a step makes the
   rounding error one-sided, so the first instant is always at the beginning of the wave. */

static inline f32
gui_phase_anchor( f32 t0, f32 duration )
{
    if ( duration <= 0.0f )
        return 0.0f;
    return -( t0 / duration ) + ( 0.5f / 65535.0f );
}

// clang-format on

#endif  // GUI_PRIM_H
