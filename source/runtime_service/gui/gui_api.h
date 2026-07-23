#ifndef GUI_API_H
#define GUI_API_H
/*==============================================================================================

    runtime_service/gui/gui_api.h -- gui module API struct and gateway macro.
    Always statically linked into the host.

    Function groups (all called through gui() vtable or as gui_* direct calls):
        Lifecycle : init / shutdown
        Frame     : frame_begin / ctx_begin / ctx_end / frame_end / render
        Panels    : window_begin / window_end
        Widgets   : text / button / checkbox / slider_float / input_text
        Draw      : draw_rect / draw_text / push_clip / pop_clip

==============================================================================================*/

#include "runtime_service/gui/gui.h"

#include "engine/app/app.h" /* app_event_t for event()   */
#include "engine/mod/mod_import.h"

/* forward declare so the API can take a cmd argument without including rhi_api.h */
struct rhi_cmd_s; typedef struct rhi_cmd_s* rhi_cmd_t;

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct gui_api_s
{

    /*============================================================================================================
        GUI_DRAW -- render server  (render/ + draw/)
        Fonts, icons, textures, the draw_* primitive set, paths, clips, volatile blocks.
        S0 style stratum: NO ambient style -- every call takes explicit colors / widths.
        Draws what it is told under the ambient clip / z; never asks how a rect was made.
    =============================================================================================================*/

    /*====================================  fonts -- id-addressed registry  =====================================*/

    /* Font -- select / load fonts; call between frames (outside frame_begin / render), except
       push_font / pop_font which may bracket a section or widget mid-frame.

       Fonts live in an id-addressed registry.  Slot 0 is the default; it is empty until the first
       font_load / font_load_into( 0, path ) call -- call one right after gui()->init(), before any
       frame renders.  font_load() loads a .orb_font into a fresh id; font_load_into() loads one
       into an existing id (id 0 swaps the default).  font_use() makes a loaded id active; another
       context can select its own font this way.  push_font() / pop_font() bracket a temporary
       font and restore the previous one.  Each font_load/font_load_into uses its own bindless
       texture.  Widget layout dimensions follow the active font's metrics. */

    bool ( *font_load_into     )( u32 id, const char* path );
    void ( *font_use           )( u32 id );
    void ( *push_font          )( u32 id );
    void ( *pop_font           )( void );
    u32  ( *font_active_id     )( void );   // id of the currently active font (save/restore, or just to inspect)

    /*===========================  custom draw -- canvas primitives, symbols, paths  ============================*/

    /* Low-level draw list access -- may be called anywhere between frame_begin and render.
       draw_rect and draw_text push geometry directly into the draw list.
       draw_rects pushes N solid rects as ONE command -- the batched form for dense custom
       drawing (timeline bars, graph columns) that would otherwise exhaust the frame's command
       budget one draw_rect at a time.
       push_clip / pop_clip set the current scissor rectangle. */

    void ( *draw_rect  )( f32 x, f32 y, f32 w, f32 h, u32 abgr );
    void ( *draw_rects )( const gui_rect_col_t* rects, u32 count );
    void ( *draw_text  )( f32 x, f32 y, u32 abgr, const char* str );

    /* volatile_cb -- runs `fn` inline, as ordinary code, wrapped so its command range can be
       replayed standalone on frames where the rest of the UI build is skipped (frame_begin
       returned false; frame_end runs the replay internally -- see frame_dirty below).
       `fn` calls ordinary emit functions (text, rect_filled,
       etc) and should bracket them with volatile_begin()/volatile_end() from inside its own body.
       `label` is hashed the same way item_id() hashes a label -- combined with the current id
       scope, so it need only be stable and unique within its own call site, same as any other
       widget label.  Interactive widgets are safe to call from `fn` but are inert during replay --
       see gui.h (gui_volatile_fn) for the contract. */
    void ( *volatile_cb    )( const char* label, gui_volatile_fn fn );
    void ( *volatile_begin )( void );   // called from inside fn: stamp the callback's start position
    void ( *volatile_end   )( void );   // called from inside fn: reserved, no-op today

    /* text_size -- laid-out pixel size of s (widest line x line span; '\n' breaks).  CalcTextSize. */
    gui_vec2_t ( *text_size )( const char* s );

    /* draw_text_in -- draw s aligned within rect r (gui_align_t; multi-line, each line aligned).
       The placement primitive: "right-align this caption in the canvas" with no hand-computed edge.
       draw_text_clipped is the single-line variant that ellipsizes to r's width. */
    void ( *draw_text_in      )( gui_rect_t r, gui_align_t align, u32 col, const char* s );
    void ( *draw_text_clipped )( gui_rect_t r, gui_align_t align, u32 col, const char* s );

    /* Icons -- a runtime-built R8 atlas of arbitrary symbols (folder, gear, check, editor glyphs).
       register_icon packs a raw monochrome bitmap (row-major coverage, w*h bytes) and returns a
       handle (0 = atlas full); the pixels live in the same flush as text and tint by `col`.
       load_icon is the from-disk source: it decodes an image file (PNG and the other stb_image
       formats) to R8 coverage -- alpha channel when present, else luminance -- and registers it the
       same way, so a loaded icon is identical to a procedural one downstream.  `path` is resolved
       through asset_path -- a plain path relative to the assets root ("assets/icon/foo.png"), no
       need to call asset_path yourself first.  find_icon looks one
       up by the name it was registered with (built-in icons register at gui init); icon_size is its
       native pixel size (for layout).  image is a layout widget (reserve w x h, draw centered/fit);
       draw_icon_in places an icon in a rect the caller already holds (cell / button label / canvas
       cut).  col 0 means white. */

    gui_icon_id_t ( *register_icon )( const char* name, u32 w, u32 h, const u8* coverage );
    gui_icon_id_t ( *load_icon     )( const char* name, const char* path );
    gui_icon_id_t ( *find_icon     )( const char* name );
    gui_vec2_t    ( *icon_size     )( gui_icon_id_t id );
    void          ( *image         )( gui_icon_id_t id, f32 w, f32 h, u32 col );
    void          ( *draw_icon_in  )( gui_rect_t r, gui_icon_id_t id, u32 col );


    /* RGBA textures -- display an arbitrary bindless texture (a scene render target, a loaded
       image) as a full-color quad; the texel is the color, tint_abgr multiplies (0 = untinted).
       image_texture flows in the layout like image(); draw_texture_in fills a rect the caller
       already holds.  The caller owns the texture + its bindless slot (rhi register_texture). */

    void ( *image_texture   )( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr );
    void ( *draw_texture_in )( gui_rect_t r, u32 bindless_idx, u32 tint_abgr );

    /* Font atlas access -- the bindless index + pixel size backing a loaded font id, for previewing
       its live GPU atlas through image_texture / draw_texture_in above (0 / {0,0} if empty). */
    u32        ( *font_atlas_idx  )( u32 font_id );
    gui_vec2_t ( *font_atlas_size )( u32 font_id );

    /* Symbol + shape draw primitives (the draw_* family, Dear ImGui's AddXxx / Render* analogue),
       drawn through the normal vertex pipeline (lines / triangles / circles), NOT the icon atlas.
       They share the draw_* verb with draw_rect / draw_text / draw_line above -- everything that
       pushes geometry into the draw list is draw_*; render() is reserved for the frame flush.  The
       built-in widgets draw their check marks, arrows, bullets and close crosses through these, and
       the broader shape
       palette (frames, per-corner rounded rects, polygons, arcs / pie, beziers, dashes, checker /
       hatch / gradient fills, soft shadows, outlined / shadowed text, grips, spinners) is exposed so
       editor / custom widgets can paint them.  Implemented in gui_symbol.c.  (The global
       indicator-shape selectors set_check_style / _bullet_style / _arrow_style live with the style
       API above, since they are style state rather than draw calls.)

       Pipeline note: draw_gradient is an exact one-quad blend via per-vertex color
       (GUI_CMD_RECT_GRADIENT); draw_shadow (layered rings) is still an approximation that a
       future multi-corner-color command would make exact, without changing this surface.  Angles
       for arc / pie / progress are radians, screen-space (y
       down).  `thickness` is the stroke width for the stroked forms. */

    void ( *draw_check_mark        )( gui_rect_t box, u32 col );
    void ( *draw_arrow             )( gui_rect_t box, gui_dir_t dir, u32 col );
    void ( *draw_bullet            )( f32 cx, f32 cy, f32 r, u32 col );
    void ( *draw_close             )( gui_rect_t box, u32 col );
    void ( *draw_arrow_pointing_at )( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 col );
    void ( *draw_chevron           )( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 col );
    void ( *draw_plus_minus        )( gui_rect_t box, bool plus, f32 thickness, u32 col );
    void ( *draw_frame             )( gui_rect_t box, u32 col_bg, u32 col_border, f32 border );
    void ( *draw_round_rect        )( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                                        bool filled, f32 thickness, u32 col );
    void ( *draw_ngon              )( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col );
    void ( *draw_circle            )( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
    void ( *draw_arc               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col );
    void ( *draw_pie               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col );
    void ( *draw_bezier_quad       )( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_bezier_cubic      )( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_dashed_line       )( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col );
    void ( *draw_checker           )( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
    void ( *draw_hatch             )( gui_rect_t box, f32 spacing, f32 thickness, u32 col );
    void ( *draw_gradient          )( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
    void ( *draw_shadow            )( gui_rect_t box, f32 spread, u32 col );
    void ( *draw_text_outline      )( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
    void ( *draw_text_shadow       )( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );
    void ( *draw_grip              )( gui_rect_t box, u32 col );
    void ( *draw_spinner           )( gui_rect_t box, f32 t, f32 thickness, u32 col );
    void ( *draw_progress_arc      )( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );

    /* Line / path stroking (gui_stroke_align_t; see gui.h for the pixel model).
       draw_line     -- one segment, CENTER_BIASED: H/V lines render pixel-crisp, others antialiased.
       draw_polyline -- a connected point array with miter-limited corners (always antialiased);
                        `closed` joins the last point back to the first (rect / polygon outlines).
       path_*        -- the retained form: clear, append points with path_line_to, then path_stroke
                        (which strokes and clears the buffer).  Up to GUI_PATH_MAX points.

           gui()->draw_line( 10, 10, 200, 80, 2.0f, col );      // a 2px antialiased diagonal
           gui()->path_line_to( x0, y0 ); gui()->path_line_to( x1, y1 ); ...
           gui()->path_stroke( 1.5f, GUI_STROKE_CENTER, false, col ); */

    void ( *draw_line     )( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr );
    void ( *draw_polyline )( const gui_vec2_t* pts, u32 count, f32 thickness,
                             gui_stroke_align_t align, bool closed, u32 abgr );
    void ( *path_clear    )( void );
    void ( *path_line_to  )( f32 x, f32 y );
    void ( *path_stroke   )( f32 thickness, gui_stroke_align_t align, bool closed, u32 abgr );

    void ( *push_clip )( f32 x, f32 y, f32 w, f32 h );
    void ( *pop_clip  )( void );

    /*============================================================================================================
        GUI_CORE -- interaction server  (core/ + interact/ + nav/ + surface/ + user/)
        The ambient, id-keyed services every layer composes over: identity (id scopes), the
        item() state machine over caller rects, keyed state, animation, drag and drop, root
        surfaces (clip / scroll / z / persist), io snapshot queries, redraw levers.
        item( id, rect ) -> state is the coordination axis: layout of any kind PRODUCES a
        rect, a widget of any kind CONSUMES one, and this server cannot tell them apart.
    =============================================================================================================*/

    /*===============================  animation service -- keyed value stepping  ===============================*/

    /* Animation service -- the general value-stepping surface any interface drives transitions with.
       Two models, both keyed on a caller-owned gui_id_t (compose with id_combine to avoid slot
       collisions); storage is proportional to in-flight animations and self-evicts once settled.
       Every call that steps a value raises wants_redraw, so animations keep frames coming under
       idle-skip / frame_pace with no caller bookkeeping.

         anim_f32  -- exponential-decay damper: chase a moving `target` at `speed` (Hz-like; 10 ~=
                      250 ms to 95%, 20 ~= 150 ms).  No definite end -- ideal for hover/state blends
                      and "glide to wherever the target is now".  Retargets smoothly mid-flight.
         anim_start / anim_ease -- fixed-duration eased tween.  anim_start seeds a clock on `id` for
                      `secs` (<= 0 == instant: the next anim_ease reads settled and the caller snaps);
                      anim_ease advances it and returns eased progress in [0,1] to lerp your own
                      from/to with.  Several channels on one id share the clock (depart + ARRIVE
                      together) and the tween has a definite end (*out_active goes false at t==1).
         anim_color / anim_vec2 / anim_rect -- typed dampers (one anim_f32 per component) so a color,
                      point, or rect glides to a new data state without hand-rolling the blend. */
    f32      ( *anim_f32    )( gui_id_t id, f32 target, f32 speed );
    void     ( *anim_start  )( gui_id_t id, f32 secs );
    f32      ( *anim_ease   )( gui_id_t id, gui_ease_t ease, bool* out_active );
    u32      ( *anim_color  )( gui_id_t id, u32 target_abgr, f32 speed );
    gui_vec2_t ( *anim_vec2 )( gui_id_t id, gui_vec2_t target, f32 speed );
    gui_rect_t ( *anim_rect )( gui_id_t id, gui_rect_t target, f32 speed );

    /*===================================  surfaces -- panes, root regions + scroll  ===================================*/

    /* pane_begin / pane_end -- the MINIMAL top-level surface occupant (gui_pane_t, gui.h): the
       raw block every window is built from, for callers assembling their own chrome.  Opens
       identity (items inside attribute to this pane), enters the hover/z contest at the tier's
       band (same contest windows and popups compete in), and pushes the base clip (draw + hit)
       to the rect -- NOTHING else: no pool record, no persistence, no layout, no background
       paint, no scroll.  The caller owns every pixel (el_* / draw_* over carved rects) and any
       cross-frame state; open flow inside with flow_begin( pane.rect ) if wanted.  Flags
       honored: GUI_WIN_NO_INPUT (pure display), GUI_WIN_NO_CLIP, GUI_WIN_DEBUG_BAND.  vp
       GUI_VP_INVALID = primary surface.  Root-level, never nests; always pair with pane_end.
       region_begin below = this + persisted scroll + a layout; window_begin = this + the
       persisted record + stock chrome. */
    gui_pane_t ( *pane_begin )( const char* id, gui_rect_t r, gui_region_tier_t tier,
                                gui_vp_t vp, gui_win_flags_t flags );
    void       ( *pane_end   )( void );

    /* region_begin / region_end -- a root-level layout region: an explicit screen rect with no
       window chrome (no title, no drag/resize, no dock, no z-order competition, no pool record).
       It is the third caller of the same scroll-region engine window_begin and child_begin sit
       on, stripped to just a rect + persisted scroll/content state, for a HUD-style element that
       needs a fixed, caller-positioned box rather than a movable window -- the perf overlay is
       the reference case.  w/h <= 0 autosizes that axis to last frame's measured content, like
       child_begin's AutoResizeY.  Unlike window_begin / child_begin, it takes no parent region --
       call it directly at the top of a frame.  Paints on the main viewport at the z tier picked
       by `tier` (gui_region_tier_t: MID over windows / under popups, BG, FG); interactive by
       default -- competes for hover in the same z contest as windows (opt out with
       GUI_WIN_NO_INPUT; see gui_region.c).  Always returns true; always pair with region_end. */
    bool ( *region_begin )( const char* id, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                            gui_win_flags_t flags );
    void ( *region_end   )( void );

    /* scroll_by -- nudge the currently open region's scroll offset by (dx, dy) px (0=top origin);
       a large delta drives to an edge, so +BIG reaches the bottom / tail and -BIG the top.  Applied
       THIS frame (re-bases the live pen), so call it right after opening the region, before content
       -- no one-frame lag, unlike the wheel.  Pairs with GUI_WIN_ANCHOR_BOTTOM to drive a console's
       wheel + PageUp/Down + jump-to-tail keys without any offset bookkeeping in the caller. */
    void       ( *scroll_by     )( f32 dx, f32 dy );

    /*=================================  window features as mechanisms  =================================*/

    /* The feat_* kit: every window feature as a freestanding
       id-keyed mechanism, so chrome is assembled feature by feature over a pane -- anything
       can be a move handle, a collapse, or a maximize.  State rule: in-flight gesture state
       is arbitrated by active_id (one drag at a time); PERSISTENT state is the caller's
       pointers -- you see every byte.  Call these inside the owning pane/window bracket
       (hover gating reads the ambient scope).  The open latch needs no mechanism: it is a
       caller bool your close button clears; scroll is region_begin ("region owns scroll").

           gui_pane_t p     = gui()->pane_begin( "tool", st->rect, GUI_REGION_MID, 0, 0 );
           gui_rect_t r     = p.rect;
           gui_rect_t title = gui_rect_cut_top( &r, 26.0f );          // titlebar = a band...
           gui()->feat_move( p.id, title, &st->rect.x, &st->rect.y ); // ...that drags
           if ( gui()->el_button( gui_rect_cut_right( &title, title.h ), "x##c" ) )
               st->open = false;                                      // close = your bool
           r.h = gui()->feat_collapse( p.id, !st->folded, 26.0f, r.h ) - 26.0f;
           gui()->feat_resize( p.id, &st->rect, GUI_RESIZE_R | GUI_RESIZE_B, 120, 80 );
           ... body: carve r, or flow_begin( r ) ...
           gui()->pane_end();

       feat_move     -- drag handle over any rect: press in `handle` (deferred: click vs
                        drag) grabs; the caller-owned origin follows the cursor.  True on
                        frames it moved.
       feat_resize   -- edge-resize the caller-owned rect: `edges` masks the exposed sides
                        (GUI_RESIZE_L/R/T/B), min_w/h floors against the grabbed edge's
                        pinned far side.  Returns the live (hot/dragging) edges.
       feat_collapse -- tweened height channel over a caller bool: full_h open, head_h
                        closed, eased between after YOUR toggle.  Returns this frame's height.
       feat_maximize -- rect <-> work-area swap (work passed IN -- the B rule): saves *r
                        into *restore on the way up, tweens both directions, tracks a
                        resizing work area while maximized.  Writes *r every call.
       feat_clamp    -- boundary policy over passed-in bounds: the handle row stays
                        reachable (never above work's top; `margin` sliver at other edges). */
    bool ( *feat_move     )( gui_id_t id, gui_rect_t handle, f32* x, f32* y );
    u8   ( *feat_resize   )( gui_id_t id, gui_rect_t* r, u8 edges, f32 min_w, f32 min_h );
    f32  ( *feat_collapse )( gui_id_t id, bool open, f32 head_h, f32 full_h );
    void ( *feat_maximize )( gui_id_t id, bool maximized, gui_rect_t* r, gui_rect_t* restore,
                             gui_rect_t work );
    void ( *feat_clamp    )( gui_rect_t* r, gui_rect_t work, f32 margin );

    /*=================================  identity + item flags + drag and drop  =================================*/

    /* Id scope -- disambiguate widgets that would otherwise share an id.  Widget ids are already
       seeded by the enclosing window / child region automatically, so identical labels in
       different regions never collide; push_id adds a temporary scope level for repeated widgets
       within one region (e.g. rows in a list keyed by index).  Always pair with pop_id.

           for ( i = 0; i < n; ++i ) {
               gui()->push_id_int( i );
               gui()->selectable( name[i], &sel[i] );   // distinct id even if name[] repeats
               gui()->pop_id();
           }

       The "##" / "###" label suffixes are the per-call alternative: "Text##key" displays "Text"
       but ids from the whole string; "pre###key" ids only from "###key", so a changing visible
       prefix (a counter) keeps a stable id. */

    void ( *push_id     )( const char* str );
    void ( *push_id_int )( i32 i );
    void ( *pop_id      )( void );

    /* Item flags -- the push-model per-item behavior set (gui_item_flags_t).  push/pop tune every
       widget until popped (and nest); next_item_flag is a one-shot override the very next widget
       consumes, no pop needed.  The mechanism is callsite-free: widgets read the resolved flags at
       emit time, so a new flag never changes a widget signature.  GUI_ITEM_DISABLED is honored
       for every widget today (inert + dimmed).

           gui()->push_item_flag( GUI_ITEM_DISABLED, true );
           gui()->button( "A" );  gui()->button( "B" );    // both disabled
           gui()->pop_item_flag();

           gui()->next_item_flag( GUI_ITEM_DISABLED, true );
           gui()->button( "Only this one" );                 // disabled, no pop needed */

    void ( *push_item_flag )( gui_item_flags_t flag, bool enable );
    void ( *pop_item_flag  )( void );
    void ( *next_item_flag )( gui_item_flags_t flag, bool enable );

    /* disabled_begin / disabled_end -- named-scope shorthand for GUI_ITEM_DISABLED (BeginDisabled
       / EndDisabled).  disabled_begin( true ) dims + inerts the bracketed widgets; ( false ) pushes
       a no-op scope so a conditional disable still balances.  Nests: an inner ( false ) never
       re-enables widgets an outer ( true ) disabled. */
    void ( *disabled_begin )( bool disabled );
    void ( *disabled_end   )( void );


    /* Drag and drop -- typed payload transfer between items (see gui_drag_flags_t /
       gui_drag_payload_t in gui.h).  One drag exists at a time; the payload bytes are copied.

       USAGE CONTRACT:
         Source -- right after the widget that should be draggable:
           if ( gui()->drag_source_begin( GUI_DRAG_NONE ) )      // true while dragging from it
           {
               gui()->drag_payload_set( "ASSET", &index, sizeof index );   // every frame is fine
               gui()->textf( "Move %s", name );                  // preview widgets follow the cursor
               gui()->drag_source_end();
           }
         Target -- right after any widget that should receive drops:
           if ( gui()->drag_target_begin() )                     // true while a drag hovers it
           {
               const gui_drag_payload_t* p = gui()->drag_payload_accept( "ASSET", GUI_DRAG_NONE );
               if ( p )                                          // non-NULL on the drop frame
                   place_asset( *(const i32*)p->data );
               gui()->drag_target_end();
           }

       drag_payload_accept highlights the target while the types match and returns the payload on
       the release frame (or every hover frame with GUI_DRAG_ACCEPT_PEEK).  drag_active reports a
       drag in flight anywhere; drag_payload_peek inspects it without being a target.  The dock
       tab-strip publishes its tab drags as type "gui.dock_tab" (payload: the window's gui_id_t). */

    bool ( *drag_source_begin   )( gui_drag_flags_t flags );
    void ( *drag_source_end     )( void );
    bool ( *drag_payload_set    )( const char* type, const void* data, u32 size );
    bool ( *drag_target_begin   )( void );
    const gui_drag_payload_t* ( *drag_payload_accept )( const char* type, gui_drag_flags_t flags );
    void ( *drag_target_end     )( void );
    bool ( *drag_active         )( void );
    const gui_drag_payload_t* ( *drag_payload_peek   )( void );

    /*=================================  item() -- behavior over a caller rect  =================================*/

    /* item -- the user-UI behavior seam: run the shared widget interaction state machine over a
       rect the CALLER derived (a canvas() cut, an empty() slot, split/carve panels, custom math)
       and report the resolved state (hover / active / pressed / clicked).  A custom widget is
       rect + item() + draw_*: it hovers, press-captures, clicks, and registers for keyboard nav
       exactly like a stock widget, with the presentation entirely yours.  Owns no layout
       reservation, so it composes with the rect helpers.  invisible_button is item() reduced to
       its click bit; for only a hover tint, use is_mouse_hovering_rect. */
    gui_item_state_t ( *item )( const char* id_str, gui_rect_t r );
    bool ( *invisible_button )( const char* id_str, gui_rect_t r );

    /*===========================  queries -- io snapshot, item state, redraw state  ============================*/

    /* IO accessors -- the frame-coherent input snapshot the widgets see, for UI / tool code that
       would otherwise re-query app() and so bypass gui's frame timing and its input capture.

       want_capture_mouse / want_capture_keyboard are the fence: a true return means gui owns the
       device this frame (the cursor is over a window, a widget is dragging, or a field is focused),
       so non-UI code should NOT also act on it.  Gate direct app() input reads in gameplay / tools
       on the inverse:

           if ( !gui()->want_capture_keyboard() && app()->key_pressed( APP_KEY_SPACE ) )
               jump();

       The is_key_* / is_mouse_* / get_* readers return the same per-frame state the widgets use
       (keyed by app_key_t / app_mouse_button_t).  is_key_pressed / is_mouse_clicked are the down-
       edge this frame.  get_time is seconds since the first frame (accumulated dt); get_delta_time
       is this frame's.

       Key repeat is per-query (no mode to set): is_key_pressed is the initial press only, while
       is_key_pressed_repeat also fires on each OS auto-repeat tick at the user's system rate -- the
       Dear ImGui IsKeyPressed(key, repeat=true) case.  Use the repeat reader for held-key actions
       (text nav, a spinner); use the plain one for discrete actions that must fire once per press. */

    bool ( *want_capture_mouse       )( void );
    bool ( *want_capture_keyboard    )( void );

    /* is_mouse_hovering_rect -- cursor is over r and r is interactable (front-most window, inside the
       region clip, no drag in flight): the IsMouseHoveringRect analogue for custom-drawn hit tests. */
    bool ( *is_mouse_hovering_rect   )( gui_rect_t r );

    /* Last-item introspection (the ImGui IsItem* family) -- each reports on the widget just emitted,
       so call immediately after it.  hovered / active / clicked / focused mirror the widget's own
       interaction; activated / deactivated are the press / release edges (deactivated is the natural
       "commit on release" seam); visible is true when any of the item's rect survives the region
       clip; get_item_rect returns its screen rect (GetItemRectMin/Max/Size in one). */
    bool         ( *is_item_hovered     )( void );
    bool         ( *is_item_active      )( void );
    bool         ( *is_item_clicked     )( void );
    bool         ( *is_item_focused     )( void );
    bool         ( *is_item_activated   )( void );
    bool         ( *is_item_deactivated            )( void );
    bool         ( *is_item_deactivated_after_edit )( void );
    bool         ( *is_item_visible                )( void );
    gui_rect_t ( *get_item_rect       )( void );

    bool ( *is_key_down              )( app_key_t key );
    bool ( *is_key_pressed           )( app_key_t key );
    bool ( *is_key_pressed_repeat    )( app_key_t key );
    bool ( *is_key_released          )( app_key_t key );
    bool ( *is_mouse_down            )( app_mouse_button_t b );
    bool ( *is_mouse_clicked         )( app_mouse_button_t b );
    bool ( *is_mouse_released        )( app_mouse_button_t b );
    bool ( *is_mouse_double_clicked  )( app_mouse_button_t b );
    void ( *get_mouse_pos            )( f32* x, f32* y );
    f32  ( *get_mouse_wheel          )( void );
    f32  ( *get_delta_time           )( void );
    f64  ( *get_time                 )( void );

    /* Hardware cursor.  The widgets already drive the shape from their own hover (resize edges show
       the directional sizers, a text field shows the I-beam).  cursor_set lets UI code request
       a shape gui cannot infer -- e.g. APP_CURSOR_HAND over a custom clickable -- for this frame;
       the last request wins and is flushed to the OS window under the pointer while gui owns the
       mouse, then reset to APP_CURSOR_ARROW next frame.  get_mouse_cursor reads the current request. */
    void         ( *cursor_set )( app_cursor_t c );
    app_cursor_t ( *get_mouse_cursor )( void );

    /* set_keyboard_focus -- queue a programmatic focus request: the next focusable widget emitted
       (a text input box) takes keyboard focus as if clicked.  Call just before emitting the
       widget; the request persists across frames until a focusable widget consumes it, so a
       request made after this frame's field lands on the same field next frame (the "refocus
       after Enter" console pattern). */
    void         ( *set_keyboard_focus )( void );

    /* set_edit_cursor_end -- queue a caret request: the focused text field seats its caret at the
       end of its buffer (selection collapsed) the next time it runs.  Call after replacing the
       field's buffer programmatically (history recall, tab completion); the request persists
       across frames until a focused field consumes it. */
    void         ( *set_edit_cursor_end )( void );

    /* set_edit_key_hook -- register a key passthrough for the next FOCUSED text field: the hook
       (gui_edit_key_fn, gui.h) runs before the field's own key handling for every key down this
       frame, and a key it consumes is cleared from the frame io.  One-shot: re-register just
       before emitting the field each frame.  This is the Quake-console seam -- history on
       Up/Down, completion on Tab, scrollback on PgUp/PgDn/Ctrl+Home/Ctrl+End -- while every
       unconsumed key edits the line as normal. */
    void         ( *set_edit_key_hook )( gui_edit_key_fn fn, void* user );

    /* wants_redraw -- true when at least one animated widget has not yet reached its target this
       frame (the currently bound context's flag).  frame_pace already folds this across every
       context internally; the query remains for hosts that run their own pacing. */
    bool ( *wants_redraw )( void );

    /* request_redraw -- one-shot: mark the bound context so the NEXT frame_begin returns dirty
       and runs a full emit.  Self-clearing (the pin is set_force_redraw below).  Call it when a
       state change made DURING this build only the next build can show -- the click that switches
       which screen is emitted, a custom widget (gui()->item) mutating the model it draws.  Input
       edges dirty only the frame they land on; without this, that next frame reads clean and
       render() replays the stale cached geometry until the mouse moves again.  Stock widgets do
       not need it (they re-read state in the same frame); the internal pop-time mutations
       (scroll, dock collapse, style edits) already raise the same flag. */
    void ( *request_redraw )( void );

    /* frame_dirty -- true when the current frame must perform a full widget emit: input changed,
       an animation is in flight, or last frame's render found a structural change.  This is the
       value frame_begin returns; the query remains for reads later in the frame (e.g. to pair
       external per-emit work such as a scene-texture flip with an actual emit). */
    bool ( *frame_dirty )( void );

    /* volatile_live -- true while at least one volatile block (volatile_cb) would actually patch
       on an idle frame: registered, on screen, its window's cached slot current.  A host that
       runs its OWN pacing and block-waits on input once wants_redraw/frame_dirty settle must add
       this to the gate: volatile blocks advance only when a frame runs, so a blocking wait
       freezes them until a timeout / spurious wakeup and the animation stutters at the wait
       interval.  frame_pace folds this in internally, same as wants_redraw. */
    bool ( *volatile_live )( void );

    /* set_force_redraw -- pins frame_dirty (and so frame_begin's return) true every frame,
       defeating the retained-cache clean-frame skip so the UI rebuilds and re-renders
       unconditionally.  Off by default.  Two uses: a debug lever to isolate a "did not update
       until input moved" symptom from a real emit bug, and the legitimate live-data pin -- a host
       whose sim mutates displayed state every frame (play mode) sets it so panels track the sim. */
    void ( *set_force_redraw )( bool on );

    /* force_redraw -- current state of the set_force_redraw override. */
    bool ( *force_redraw )( void );

    /*============================================================================================================
        GUI_RECT -- rect kit  (stateless carve math)
        Pure rect producers: no pen, no region state, no draw -- feed the results to elements,
        item(), push_layout_overlay, or draw_* directly.  The inline half of this library
        (cut / inset / align / anchor_box + the geometry types) lives in gui_rect.h.
    =============================================================================================================*/

    /* content_rect -- the current region's available area as a screen rect (cursor_screen_pos joined
       with content_avail).  split -- carve a rect into panels along an axis using the overloaded
       column unit ( >1 px, ==1 fill, (0,1) fraction ), writing each panel rect into out[] and
       returning the count ( <= GUI_LAYOUT_COLS ).  Pure rect math: fill each panel with
       push_layout_overlay, and nest by splitting a returned rect again.  Single-pass and known-size --
       it never measures content, so size panels with px / fraction / fill, not content-driven sizes. */
    gui_rect_t ( *content_rect )( void );
    u32        ( *split )( gui_rect_t area, gui_axis_t axis, const f32* sizes, f32 gap, gui_rect_t* out );

    /* carve -- a whole nested partition from one flat f32 `form`: the recursive form of split.  The
       form is a GUI_END-terminated list in the same overloaded unit as cols, with GUI_CUT_X /
       GUI_CUT_Y sentinels marking which tracks subdivide (a size followed by a CUT is a container of
       that size on the named axis; otherwise a leaf).  Opens with a leading CUT filling `area`.  Leaf
       rects land in out[] in reading order; returns the leaf count ( <= max ).  One resolve per
       container, no per-leaf storage -- store a form as data and carve it each frame. */
    u32        ( *carve )( const f32* form, gui_rect_t area, f32 gap, gui_rect_t* out, u32 max );

    /* anchor -- place a child rect inside `parent` from a normalized anchor frame (UE4 Slate model),
       the general free-placement primitive for overlays / HUDs.  Per axis: min == max point-pins a
       fixed `size` child to that parent fraction (hung off the line by `pivot`, shifted by `off`);
       min < max stretches the child between the two fractions with `off` as per-edge insets.  Pure
       rect math -- fill the result with push_layout_overlay or draw into it.  The corner / edge cases
       are the inline gui_rect_align / gui_anchor_box (gui_rect.h); reach for anchor when you need a
       fraction-relative position or a stretch-with-margins band.  See gui_anchor_t for the fields. */
    gui_rect_t ( *anchor )( gui_rect_t parent, gui_anchor_t a );

    /*============================================================================================================
        GUI_FLOW -- layout engine  (flow/)
        The stateful pen: templates (stack / cols / grid / form / pack), sizing, avail,
        row virtualization, and the rect<->flow seams (empty, canvas, push_layout_overlay).
        Produces rects and opens regions; draws nothing, and no widget core depends on it.
    =============================================================================================================*/

    /*================================  containers -- child boxes + sub-layouts  ================================*/

    /* Child regions -- a nested scrollable layout box inside the current window (or another
       child).  child_begin carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the GUI_WIN_*SCROLL policy bits.  h <= 0
       auto-sizes the height to the content (AutoResizeY).  GUI_WIN_CHILD_RESIZE_X / _Y add a
       draggable grip on the right / bottom border (flow children only): that axis becomes
       user-owned and persisted, seeded from w/h then driven by the drag, the way a window owns
       its size.  window_set_next_size_constraints (GUI_CHROME: window/) bounds the resolved size, so an
       auto-sized box can grow with its content up to a max height and then scroll.  Always pair
       with child_end -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  Always returns true. */

    bool ( *child_begin )( const char* id, f32 w, f32 h, gui_win_flags_t flags );

    /* Sub-layout -- carve the next cell into its own little layout, the way a window or child hosts
       one, but transient: no scroll, no clip, no persistent state, no frame.  push_layout consumes
       one cell (advancing the parent like any widget), opens a layout filling it (default single
       column; shape it with row / grid / widgets inside), and pop_layout closes it -- the parent
       resumes at the following cell.  The cell is one standard line tall unless the row height was
       declared larger first; the sub-layout does not grow the parent to fit, and does not clip.
       Always pair, like push_id / pop_id.

           gui()->row_cols_n( 0, 3 );                     // 3 columns
           gui()->push_layout();                          // column 0 becomes a sub-layout...
               gui()->button("A"); gui()->button("B");  // ...stacked inside that one cell
           gui()->pop_layout();
           gui()->text("col 1");  gui()->text("col 2"); */

    void ( *push_layout )( void );

    /* push_layout_overlay -- open a sub-layout over an explicit screen rect rather than the next
       template cell; the parent flow is left untouched (no cell consumed).  The seam an external
       layout pass (a two-pass "layout island") uses to hand a resolved box back to the immediate
       widgets, which fill it like any region.  Pair with pop_layout. */
    void ( *push_layout_overlay )( gui_rect_t rect );

    void ( *pop_layout  )( void );
    void ( *child_end   )( void );

    /*==============================  layout verbs, sizing, virtualization, seams  ==============================*/

    /* Layout -- declare the active region's next-item methodology (its "mode"), then shape it.
       A region opens UNDECLARED: the first header below names the mode (stack / columns / grid /
       form / ...), and a widget emitted before any header is a usage error (debug assert; release
       falls back to a stack).  The template then persists + repeats for every widget until set
       again.  Sizes use one overloaded f32: >1 px, (0,1) fraction of the available space, 1 fill
       (equal share of the rest), 0 natural (zero-width; reserved), <0 ends the list (GUI_END).
       Widgets fill whatever cell they are handed, agnostic to the shape.

           gui()->row_cols_n( 0, 2 );  gui()->button("A");  gui()->button("B");  // two columns
           gui()->row_cols( 24, (f32[]){ 200, 1, GUI_END } );                     // 200px + fill

       stack()      -- single full-width flex column, scrolling: the canonical vertical-list header
                         (what a region used to be by default; now declared explicitly).
       cols()       -- N explicit column tracks (GUI_END-terminated), auto height, scrolling.
       cols_n()     -- n equal flex columns, auto height.
       form()       -- a stack with a fixed-width label track on `side`: the "Label  [control]"
                         form header (label_w <= 0 = plain stack).
       layout_default() -- clear back to a plain stack (one flex column, no field split); the
                         single "reset everything" verb.
       row()        -- a stack with an explicit row height (0 = auto).
       row_cols()   -- explicit per-column tracks (GUI_END-terminated) of height row_h: cols + height.
       row_cols_n() -- n equal columns of height row_h: cols_n + height.
       row2/3/4()   -- fixed-arity weighted columns (auto height): row2( 0.3f, 0.7f ).
       field_split()  -- labeled widgets split their cell into a label + control track (overloaded
                         units, label left or right); input_text / slider_float / checkbox then lay
                         out as an aligned "Label  [control]" form from a single call.
       field_label_left() / field_label_right() -- field_split sugar: a fixed-width label column on
                         the left / right with a flex control filling the rest (0 = off).

       Grid mode -- cols x rows partition a bounded box (the region content from the pen to its
       bottom) into a fixed matrix, both axes resolved up front; widgets fill cells row-major and
       nothing scrolls.  For titlebars, split panes (cell -> child_begin), dashboards, image grids.
       grid() takes the full descriptor (cols + rows); grid_cells() is the uniform nc x nr case.

       gui()->grid_cells( 3, 2 );  for (i<6) gui()->button(name[i]);  // 3x2 of buttons
       grid()       -- cols x rows from the descriptor (row_h ignored; grid uses rows).

       Pack mode -- the print run: place items one after another along an axis at natural size, the
       widget sizing itself (vs columns/grid, where the cell sizes the widget).  pack_size() overrides
       the next item's main-axis measure (resolved against the space left on the line); pack_nextline()
       breaks to a fresh line.  The toolbar / tag-row / inline-controls case.

       gui()->bar();  gui()->button("Save");  gui()->button("Open");   // a toolbar
       bar() / strip() -- open a run: horizontal (the toolbar) / vertical.
       pack_size()  -- next packed item's main-axis size (0 natural, 1 fill, (0,1) frac, >1 px).
       pack_nextline() -- break the run to a new line.
       pack_wrap()  -- opt the run into auto-wrap: a natural / fixed item that overruns the line
                       breaks to a fresh one first (flex-wrap; a fill always fits, never wraps). */

    void ( *layout_default    )( void );
    void ( *stack             )( void );
    void ( *row               )( f32 row_h );
    void ( *cols              )( const f32* tracks );
    void ( *cols_n            )( u32 n );
    void ( *row_cols          )( f32 row_h, const f32* tracks );
    void ( *row_cols_n        )( f32 row_h, u32 n );
    void ( *row2              )( f32 a, f32 b );
    void ( *row3              )( f32 a, f32 b, f32 c );
    void ( *row4              )( f32 a, f32 b, f32 c, f32 d );
    void ( *form              )( gui_label_side_t side, f32 label_w );
    void ( *field_split       )( gui_label_side_t side, f32 label, f32 control );
    void ( *field_label_left  )( f32 width );
    void ( *field_label_right )( f32 width );
    void ( *grid              )( gui_layout_t desc );
    void ( *grid_cells        )( u32 ncols, u32 nrows );
    void ( *bar               )( void );
    void ( *strip             )( void );
    void ( *pack_size         )( f32 unit );
    void ( *pack_nextline     )( void );
    void ( *pack_wrap         )( void );

    /* push_layout_state / pop_layout_state -- save the region's declared shape (mode + template +
       modifiers) and restore it later, so a helper that switches into bar() / grid() / whatever
       for its own widgets can hand the caller's shape back verbatim, instead of the caller having
       to remember and re-declare it (stack() is not always right -- the caller may have been mid
       cols() or grid()).  Always pair, like push_id / pop_id; small fixed depth, coarse scope
       brackets only.

           gui()->push_layout_state();
               gui()->bar();
               gui()->button( "Save" );  gui()->button( "Open" );
           gui()->pop_layout_state();       // caller's stack() / grid() / cols() ... is back */

    void ( *push_layout_state )( void );
    void ( *pop_layout_state  )( void );

    /* align() -- set the content alignment within each cell (gui_align_t, LEFT | TOP by default).
       Persists like the row template and is independent of the columns: row() / row_cols() leave it
       untouched, layout_default() clears it.  Governs where natural-sized content sits (a text run, a
       checkbox box, a button's label); a frame-filling widget still fills its cell.  The `align`
       field of layout() / grid() sets the same thing as part of a full descriptor.

           gui()->row2( 0.5f, 0.5f );  gui()->align( GUI_ALIGN_RIGHT );   // right-aligned columns

       next_item_fit() -- one-shot override of the next cell item's size (STACK / COLUMNS / GRID),
                      instead of its implicit natural_w signal.  Same overloaded unit as a column
                      track (>1 px, (0,1) fraction, 1 fill, 0 explicit natural); the fit-then-align
                      pair -- align seats whatever box this (or the widget's own natural_w) picks.

           gui()->next_item_fit( 1.0f ); gui()->button( "Save" );  // stretch across its column

       next_item_h() -- one-shot override of the next item's HEIGHT (the vertical twin), resolved
                      against the room left below the pen: >1 px, 1 fill the rest of the region,
                      (0,1) a fraction of it, 0 the widget's own h.  Flow: lands when the item
                      opens its row; ignored mid-row and in grid cells (the matrix height wins).

           gui()->next_item_h( 1.0f ); gui()->button( "Fill" );    // rest of the region

       next_item_align() -- one-shot align for the next item only (flexbox's align-self), over the
                      region's persistent align(); restored at the following emit, so call it
                      immediately before the item.

       same_line() -- keep the next widget on the line just emitted instead of breaking to a new
                      row; it takes its natural width.  `spacing` is a gap, not a track size: its
                      own natural is a literal 0 (flush), and < 0 defers to the theme default gap.
                      Mirrors ImGui::SameLine.  new_line() is its vertical mirror: a fresh line of
                      height h, 0 = literal zero-height break, < 0 = the theme's line height.

           gui()->button("OK");  gui()->same_line( 0.0f );  gui()->button("Cancel");
           gui()->text("A");  gui()->new_line( -1.0f );  gui()->text("B");  // one blank line between

       Spacers -- cell-consuming composition that emits nothing interactive:
       skip()      -- leave one blank cell (a hole; the natural way to step over a grid slot).
       separator() -- a thin horizontal rule centered in its cell. */

    void ( *align      )( gui_align_t a );
    void ( *next_item_fit )( f32 unit );
    void ( *next_item_h )( f32 unit );
    void ( *next_item_align )( gui_align_t a );
    void ( *same_line  )( f32 spacing );
    void ( *stack_same_line )( f32 spacing );
    void ( *skip       )( void );
    void ( *separator  )( void );

    /* canvas() -- reserve a full-width drawing area of `height` px in the layout (height <= 0 fills
       the rest of the region) and return its screen rect, for custom geometry drawn with the
       draw_* / path_* calls.  It flows like any widget and the window clips it. */
    gui_rect_t ( *canvas )( f32 height );

    /* Sizing (sz_) -- the one family that turns intent into a pixel dimension; layout verbs
       (row, cols, child_begin, window_set_next_size) consume what these produce.  Grid-first,
       in order of preference:

       sz_u( n ) -- n grid quanta in pixels (the theme's grid_quantum lattice, 4 by default):
       the unit-first spelling for any authored px size (tracks, row heights, child sizes), so
       geometry stays on the theme lattice and retunes with it.  q <= 1 degenerates to raw px.

       sz_row_gap() -- the vertical gap the flow places between consecutive rows, and the
       top/bottom pad a window body / child opens with.  Owed once above the first row, once
       below the last, and once between every pair.

       sz_rows_h( n ) -- fixed box height for n uniform WIDGET_H rows stacked with the default
       pad/gap (a fixed-size list of buttons/fields, a popup sized to its item count).  Reads
       through the style stack, so inside a scale_push scope it speaks that step's metrics.

       sz_child_rows_h( n ) -- the OUTER height to give child_begin / a bare window so its interior
       holds exactly n such rows.  sz_rows_h is the interior; a container also carves its border off
       the box, so passing sz_rows_h( n ) to a child clips the last row -- use this instead when the
       row count must be exact inside a child.

       sz_scale_row( s ) -- one row height at a named ramp step (gui_scale_t) without pushing
       the scope: size a header band or custom chrome to a step.

       Content-fit escape hatches (prefer letting the layout measure via natural sizing):

       sz_fit_row / sz_fit_col -- content px plus the standard margin a row / cell puts around
       its content; fit( 0 ) is the bare margin (the "size without content").
       sz_line_h() -- the raw font line advance, for text-shaped custom-draw rects.  Text
       measurement itself lives with the draw family (text_size), not here.
       sz_chars( n ) -- width of n characters (n * a representative glyph advance), for sizing a
       field to a fixed character count without measuring a placeholder string.

           gui()->row( gui()->sz_fit_row( 128 ) );               // a row sized for a 128px image
           f32 w = gui()->sz_fit_col( gui()->text_size("Name").x ); // a column sized to a label */
    f32 ( *sz_u         )( f32 n );
    f32 ( *sz_row_gap   )( void );
    f32 ( *sz_rows_h    )( u32 n );
    f32 ( *sz_child_rows_h )( u32 n );    /* outer child/window height to hold exactly n rows */
    f32 ( *sz_scale_row )( gui_scale_t s );
    f32 ( *sz_line_h    )( void );
    f32 ( *sz_chars     )( f32 n );
    f32 ( *sz_fit_row   )( f32 content_h );
    f32 ( *sz_fit_col   )( f32 content_w );

    /* content_avail() -- remaining free space in the current region from the layout pen: the width
       a flex widget would fill and the height left before the region bottom.  The ImGui
       GetContentRegionAvail analogue -- size a child_begin to the leftover, or lay out by hand. */
    gui_vec2_t ( *content_avail )( void );

    /* view_avail() -- content_avail clamped to the visible view.  The content column can run wider
       than the view when a sibling overflowed horizontally; content_avail reports that full column
       (right for passive rows), this never exceeds the visible track (right for sizing an opaque
       interactive surface -- a child box, a text editor -- which must not seat itself under the
       scrollbar gutter).  Scroll-free: a box sized by it keeps its width while the region scrolls. */
    gui_vec2_t ( *view_avail )( void );

    /* rows_clip -- fixed-pitch row virtualization (the ImGuiListClipper analogue).  Reserves
       `count` rows of layout extent, skips the offscreen head, and returns the visible
       [first, last) range; the caller emits only those rows, so a 10000-row list costs what its
       visible slice costs.  Rows must be fixed pitch: row_h 0 defaults to the template's fixed
       row_h (row_cols) else WIDGET_H -- pass the true height when rows are anything else.  Scroll
       range and extent measure as if every row emitted; nav only sees the emitted rows.

           gui_span_t s = gui()->rows_clip( count, row_h );
           for ( i32 i = s.first; i < s.last; ++i ) { ...emit row i... }

       rows_clip_end() -- jump past the reserved tail; needed only when more content follows the
       run in the same region (a footer), else omit. */
    gui_span_t ( *rows_clip     )( i32 count, f32 row_h );
    void       ( *rows_clip_end )( void );

    /* cursor_screen_pos -- screen position where the next item would land (GetCursorScreenPos): anchor
       custom draw_* geometry to the pen.  empty -- reserve a w x h block and return its screen rect
       (the ImGui Dummy analogue): blank space, or a slot to fill with custom draw / make clickable
       with invisible_button.  `w` is the main-axis size (honored in pack / same_line; column flow
       sizes to the track). */
    gui_vec2_t ( *cursor_screen_pos )( void );
    gui_rect_t ( *empty )( f32 w, f32 h );

    /* flow_begin / flow_cell / flow_end -- the named rect <-> flow seam pair.  flow_begin opens
       the layout engine inside ANY rect, however it was produced (cut_* algebra, split, carve,
       anchor, a flow cell, custom math) -- push_layout_overlay under its first-class name.
       flow_cell takes the next flow element back out AS a rect (w / h <= 0 = natural: the
       resolved track width / one standard row), so the two verbs cross the seam in both
       directions and nest to the layout stack depth -- the recursive contract:

           carve -> flow_begin -> flow_cell -> carve -> flow_begin -> ...

       A fresh flow opens UNDECLARED: name a mode inside (stack / cols / ...).  Flow never
       scrolls -- when the carved area needs scroll / clip / persistence, open a core surface
       first (region_begin) and flow inside it.  Always pair flow_begin with flow_end. */
    void       ( *flow_begin )( gui_rect_t rect );
    gui_rect_t ( *flow_cell  )( f32 w, f32 h );
    void       ( *flow_end   )( void );

    /* split_begin / split_next / split_end -- split the current row into a fixed-width left panel
       and a fill remainder (recurse via split_next), id-scoped with per-id height caching.  Each
       panel is an independent flow region -- declare a mode (stack / cols / ...) inside.  A layout
       composition (flow/gui_split.c); pair with button_fill or any fill widget for side-by-side
       panels that share the row height. */
    void ( *split_begin   )( const char* id, f32 right_w );
    void ( *split_next    )( void );
    void ( *split_end     )( void );

    /*============================================================================================================
        GUI_ELEMENT -- building blocks  (rect-consuming widget cores)
        The el_* set: every element fills EXACTLY the rect it is handed -- no hidden padding,
        no flow, no layout reservation -- composing item() behavior + draw_* presentation +
        the slim installed element style (gui_el_style_t, gui_element.h).  Rects come from
        anywhere: gui_rect.h math, split / carve / anchor, flow_cell, your own numbers.
        Lifted from the proven sb_gui_diablo ui layer.
    =============================================================================================================*/

    /* el_style -- mutable access to the INSTALLED element style, the kit (S3) tuning door.
       The theme system re-derives the installed values at every theme / font landing
       (style_apply / theme_set / theme_reset / font activation), so a kit that owns the
       element look re-installs its palette after those calls.  Elements read only this. */
    gui_el_style_t* ( *el_style )( void );

    /* The cores.  el_panel -- inert framed backdrop (the DIM surface).  el_label -- a text run
       seated per align.  el_button -- framed press element, id from the label, true on click.
       el_check -- square toggle inscribed centered in r, explicit id, true on change.
       el_slider -- bare horizontal drag track (caller draws any value text), nav steps 5%.
       el_meter -- framed fill bar; the fill color is a CALL PARAMETER (per-widget color is
       kit business, not a style slot).  el_cycle -- "< value >" selector with square chevron
       caps, wraps; caps id under a push of id_str.  Value-mutating elements request the next
       frame's redraw themselves. */
    void ( *el_panel  )( gui_rect_t r );
    void ( *el_label  )( gui_rect_t r, gui_align_t align, const char* text );
    bool ( *el_button )( gui_rect_t r, const char* label );
    bool ( *el_check  )( gui_rect_t r, const char* id_str, bool* v );
    bool ( *el_slider )( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi );
    void ( *el_meter  )( gui_rect_t r, f32 frac, u32 fill_abgr );
    bool ( *el_cycle  )( gui_rect_t r, const char* id_str, i32* idx,
                         const char* const* items, i32 count );

    /*============================================================================================================
        GUI_CHROME -- convenience / editor UI  (window/ + dock/ + popup/ + widgets/ + table/)
        The imgui-style design layer over the blocks below: persistent windows, docking,
        popups / menus / toolbars, the stock flow-adapted widget set, tables, and the theme
        system (S2: a compiler that resolves down to the strata beneath it).
    =============================================================================================================*/

    /*=====================================  window/ -- persistent windows  =====================================*/

    /* Panels -- open a window panel; must be matched with window_end().
       flags is a bitmask of gui_win_flags_t (0 / GUI_WIN_NONE for the defaults) that
       switches off built-in behavior per window -- title bar, collapse, or edge resize.

       window_begin() returns false when the window is collapsed (title bar only).  Guard
       the body widgets with it -- skipped widgets cost nothing -- but always call
       window_end() regardless of the return value:

           if ( gui()->window_begin( "Tools", GUI_WIN_NONE ) )
           {
               gui()->text( "..." );          // skipped while collapsed
           }
           gui()->window_end();               // always called */

    /* window_set_next_pos / _size -- queue geometry for the NEXT window_begin, applied per the
       condition (gui_cond_t) and then cleared.  Decouples the value from when it is applied:
       ONCE seeds an initial position/size (apply once on first appearance, then user-owned),
       ALWAYS forces it every frame (layout managers, snapping, animation -- pair with NOMOVE /
       NORESIZE), APPEARING re-applies it each time the window is shown after being absent.
       Call immediately before window_begin. */
    void ( *window_set_next_pos  )( f32 x, f32 y, gui_cond_t cond );
    void ( *window_set_next_size )( f32 w, f32 h, gui_cond_t cond );

    /* window_set_next_viewport -- assign the NEXT window_begin to a specific viewport.  Sticky: it
       lands on the window record and persists across frames until reassigned.  Omit to inherit the
       ambient viewport -- the one most recently emitted into this frame -- so windows created from
       within a viewport's panels naturally land on the same surface without explicit assignment.
       If the assigned viewport is later closed, the window automatically reverts to the primary. */
    void ( *window_set_next_viewport )( gui_vp_t vp );

    /* window_set_next_size_constraints -- queue a one-shot [min,max] size box for the NEXT
       child_begin, then cleared.  The Dear ImGui SetNextWindowSizeConstraints analogue, in its
       most useful form: it bounds the child's resolved width / height, so an auto-sized (h <= 0)
       box grows with its content up to max_h and then scrolls, never collapses below min_h, and a
       CHILD_RESIZE_* drag cannot leave the range.  A bound <= 0 is "unconstrained" on that side
       (e.g. 0, 0, 0, max_h to cap height only).  Call immediately before child_begin. */
    void ( *window_set_next_size_constraints )( f32 min_w, f32 min_h, f32 max_w, f32 max_h );

    bool ( *window_begin )( const char* title, gui_win_flags_t flags );
    void ( *window_end   )( void );

    /* window_set_open / window_is_open -- drive a CLOSEABLE window's visibility by title (the same
       key window_begin hashes).  The window's close (X) button hides it; the host re-opens it by
       calling window_set_open( title, true ) from a button.  window_is_open reports the current
       state (a window with no record yet -- never begun -- reads as open). */
    void ( *window_set_open )( const char* title, bool open );
    bool ( *window_is_open  )( const char* title );

    /* Window state-transition animation (maximize / minimize / restore).  On by default: the window
       tweens between rects through the gui() animation service.  Off snaps instantly.  A global
       preference, not per-context. */
    void ( *window_anim_enable     )( bool on );
    bool ( *window_anim_is_enabled )( void );

    /*==========================  dock/ -- dock tree, tab groups, layout persistence  ===========================*/

    /* Docking -- tile + tab windows into a dock tree that fills a viewport (the DockSpaceOverViewport
       analogue).  The programmatic path: build a layout in code, then windows whose titles were
       dock_window'd render into their node (no per-window title bar -- the node draws a shared tab
       strip) instead of free-floating.  Mouse drag-to-dock and layout persistence (dock_save/load
       below) build on the same tree.  Free-floating windows still overlap on top of the dockspace.

       dockspace_over_viewport() -- ensure viewport vp hosts a dock tree, lay it out over the surface,
                                    draw + interact its splitters, and return the tree ROOT node id.
                                    Call once per frame at the TOP of the build, before the docked
                                    windows' window_begin (which read their resolved node rects).
       dock_split()              -- split a LEAF node in two; returns the NEW empty leaf on the `dir`
                                    side and writes the REMAINING node id to *out_remain (may be NULL).
                                    `ratio` is the new side's fraction of the axis.  The DockBuilder
                                    idiom -- keep splitting the returned remainder to carve a layout.
       dock_window()             -- add a window (matched to window_begin by title) as a tab in a leaf,
                                    moving it out of any node it was already in; it becomes active.
       dock_undock()             -- remove a window from its node, returning it to free-floating.
       window_is_docked()        -- true while the window is tabbed into some node (dormant included).
       dock_window_maximize()    -- maximize the window's node over its WHOLE dockspace (fullscreen
                                    the docked pane -- the other nodes are obscured and stop emitting)
                                    or restore the tiled layout; animated like the floater maximize
                                    (window_anim_enable gates it).  GUI_WIN_DOCK_MAXIMIZE gates only
                                    the tab strip's button; this verb works regardless, so a host can
                                    bind fullscreen-toggle to a hotkey without offering the chrome.
       window_is_dock_maximized() -- true while the window's node holds the dockspace maximize.

       A dockspace is EMIT-GATED like every immediate-mode element: on frames the host does not call
       dockspace_over_viewport, the viewport's tree is DORMANT -- retained but inert.  Windows tabbed
       in a dormant tree keep their membership but render nothing (window_begin returns false,
       inactive-tab semantics), and title drags offer no dock chips; floating tab groups are
       independent of the tree and unaffected.  Re-emitting the dockspace revives the layout exactly
       as it was, so a host can swap whole UI modes in and out just by (not) running the dock code
       path.  dock_clear (below) is the only thing that destroys the tree.

           gui_dock_id_t root  = gui()->dockspace_over_viewport( 0, GUI_DOCKSPACE_NONE );
           gui_dock_id_t left  = gui()->dock_split( root, GUI_DIR_LEFT, 0.25f, &root );
           gui()->dock_window( "Scene Tree", left );
           gui()->dock_window( "Viewport",   root );   // center; tab more windows here with root */

    gui_dock_id_t ( *dockspace_over_viewport )( gui_vp_t vp, gui_dockspace_flags_t flags );
    gui_dock_id_t ( *dock_split )( gui_dock_id_t node, gui_dir_t dir, f32 ratio,
                                     gui_dock_id_t* out_remain );
    /* dock_split_root() -- split the WHOLE viewport tree, carving a new leaf along a full edge (`dir`).
       Unlike dock_split (a single leaf), this wraps the root in a new split so the pane spans the entire
       side -- the way to place a full-height column beside an existing top/bottom stack.  Returns the
       new leaf id (dock windows into it), or GUI_DOCK_NONE.  Also the commit path of an edge drop. */
    gui_dock_id_t ( *dock_split_root )( gui_vp_t vp, gui_dir_t dir, f32 ratio );
    void ( *dock_window )( const char* title, gui_dock_id_t node );
    void ( *dock_undock )( const char* title );
    bool ( *window_is_docked )( const char* title );
    void ( *dock_window_maximize )( const char* title, bool on );
    bool ( *window_is_dock_maximized )( const char* title );

    /* Floating tab groups -- tabbing WITHOUT split panes.  window_tab() merges window `title` onto
       window `onto_title`'s frame: a free target grows a floating tab group around itself (shared
       frame, tab strip in place of a title bar; drag the strip's empty band to move it, its edges
       to resize); a target already tabbed somewhere -- a group or a dockspace leaf -- just gains
       the tab.  The same merge exists as a gesture: title-drag one free window onto another's
       title bar and drop on the center chip.  dock_undock() pulls a window back out; a group
       dissolves by itself once a single tab remains.  A window flagged GUI_WIN_NO_TAB_TARGET
       never hosts tabs (no drop chip; refused as onto_title) -- flag control panels whose body
       the host gates on window_begin's return.  To keep a whole DOCKSPACE tabs-only instead,
       pass GUI_DOCKSPACE_NO_SPLIT to dockspace_over_viewport: only the center (tab) drop chip
       is offered and the split verbs above refuse. */
    void ( *window_tab )( const char* title, const char* onto_title );

    /* Layout persistence.  dock_save() serializes viewport vp's dock tree into buf as a small ASCII
       blob and returns the byte count a full write needs (like snprintf -- pass a 0 bufsz to size
       first).  dock_load() rebuilds the tree from such a blob; returns false on a bad header.  The
       host owns the file: write the blob on change, read + load it at startup.  CALL dock_load at a
       safe point -- between frames or at the top of the build before any docked window's window_begin
       -- never from inside a docked window (it frees + rebuilds the tree). */
    u32  ( *dock_save )( gui_vp_t vp, char* buf, u32 bufsz );
    bool ( *dock_load )( gui_vp_t vp, const char* text );

    /* dock_clear() -- DESTROY viewport vp's dock tree: free every node and clear the root.  Windows
       lose their tab membership permanently and free-float from their next begin (at the rect their
       node last gave them).  Not needed to merely stop docking for a while -- a dockspace that is
       not emitted goes DORMANT (see above) and revives intact.  Clear is for discarding a layout
       wholesale, e.g. before hand-building a fresh one.  Same safe-point rule as dock_load: top of
       the build, never from inside a docked window.  Floating tab groups stay standing. */
    void ( *dock_clear )( gui_vp_t vp );


    /* Host-reserved top band (pixels) above viewport vp's dock area -- the height of a main menu
       bar / toolbar strip the host draws itself; the dock tree lays out below it.  Sticky until
       re-published; pass 0 to reclaim.  Publish before dockspace_over_viewport in the build. */

    void ( *dockspace_inset )( gui_vp_t vp, f32 top );

    /*==========================  popup/ -- popups, tooltips, menus, combo + listbox  ===========================*/

    /* Popups -- transient overlay windows on top of everything.  A regular popup auto-closes when
       the user clicks outside it; a modal blocks input behind it and dims the background, closing
       only via popup_close_current.  The string id namespaces both the open request and the body,
       so popup_open("x") and popup_begin("x") must use the same id.  Popups stack (a popup opened
       while inside another nests under it); a click keeps the deepest popup under the cursor and
       closes the rest.  Popup / tooltip bodies lay out like a window body: declare a layout header
       (stack / columns / ...) before emitting widgets.

           if ( gui()->button( "Open" ) )    gui()->popup_open( "menu" );
           if ( gui()->popup_begin( "menu", GUI_WIN_NONE ) ) {
               gui()->stack();
               if ( gui()->selectable( "Cut",  NULL ) ) { ... }
               if ( gui()->selectable( "Copy", NULL ) ) { ... }
               gui()->popup_end();
           }

       popup_begin / popup_modal_begin return true only when the popup is open AND visible -- guard
       the body and call popup_end only on a true return (like window_begin's collapsed contract).
       Auto-sized popups (the default) measure their content on the appearing frame off-screen and
       snap into place the next frame, so there is no first-frame size pop. */

    void ( *popup_open          )( const char* id );
    bool ( *popup_begin         )( const char* id, gui_win_flags_t flags );
    bool ( *popup_modal_begin   )( const char* id, const char* title, gui_win_flags_t flags );
    void ( *popup_end           )( void );
    void ( *popup_close_current )( void );
    bool ( *popup_is_open        )( const char* id );

    /* 
        Context menus -- open a popup on a right-click.  _item binds to the previous widget (the one
        emitted just before the call); _window binds to empty space in the current window.  Use them
        in place of the popup_open + popup_begin pair:

            gui()->selectable( "Row", NULL );
            if ( gui()->popup_context_item_begin( "row_ctx" ) ) { ...; gui()->popup_end(); }
    */
    bool ( *popup_context_item_begin   )( const char* id );
    bool ( *popup_context_window_begin )( const char* id );

    /* 
        Tooltips -- a non-interactive overlay shown at the cursor while the previous widget is
        hovered.  set_item_tooltip is the one-liner; tooltip_begin / tooltip_end wrap a multi-widget
        body (guard the body on the true return, always call tooltip_end).

           gui()->button( "Hover me" );
           gui()->set_item_tooltip( "Does the thing" );

        help_marker draws a dim "(?)" hint that pops `text` on hover -- the Dear ImGui footnote,
        typically emitted on the same line after a control:

           gui()->checkbox( "No mouse", &flag );
           gui()->same_line( 0.0f );
           gui()->help_marker( "Disable mouse inputs and interactions." ); 
    */
    void ( *set_item_tooltip )( const char* text );
    bool ( *tooltip_begin    )( void );
    void ( *tooltip_end      )( void );
    void ( *help_marker      )( const char* text );

    /* Menus -- a coordination layer over the popup stack.  A menu bar holds menu_begin entries;
       each opens a submenu popup that holds menu_items and further menu_begin entries (nesting on
       the popup stack).  Disabled state reuses the item-flag stack: push_item_flag(GUI_ITEM_DISABLED).

       main_menu_bar_begin pins a bar across the top of the display; menu_bar_begin fills the strip a
       window reserved with GUI_WIN_MENUBAR (and returns false on a window without the flag).  Both
       return true only when visible -- guard the entries on the return and call the matching end only
       then, exactly like window_begin / popup_begin.

           if ( gui()->main_menu_bar_begin() ) {
               if ( gui()->menu_begin( "File" ) ) {
                   if ( gui()->menu_item( "Open", "Ctrl+O", NULL ) ) { ... }
                   gui()->menu_item( "Show grid", NULL, &show_grid );   // checkable
                   if ( gui()->menu_begin( "Recent" ) ) {              // submenu
                       gui()->menu_item( "a.txt", NULL, NULL );
                       gui()->menu_end();
                   }
                   gui()->menu_end();
               }
               gui()->main_menu_bar_end();
           }

       menu_begin renders horizontally in a bar (its popup drops below) and as a full-width row with
       a submenu arrow inside a menu (its popup opens to the side); the orientation follows the active
       layout mode, so no flag is needed.  menu_item returns true on the clicked frame and dismisses
       the whole menu chain; shortcut is display-only (may be NULL); selected may be NULL (a plain
       command) or a bool* (a checkable item, toggled on click). */

    bool ( *main_menu_bar_begin )( void );
    void ( *main_menu_bar_end   )( void );

    /* main_menu_bar_h() -- the band height main_menu_bar_begin occupies (theme-derived).  Use it
       to stack host strips (toolbars, dockspace_inset) below the bar instead of re-deriving the
       height from font metrics -- it stays truthful when the theme or scale ramp retunes. */
    f32  ( *main_menu_bar_h     )( void );
    bool ( *menu_bar_begin      )( void );
    void ( *menu_bar_end        )( void );
    bool ( *menu_begin )( const char* label );
    void ( *menu_end   )( void );
    bool ( *menu_item  )( const char* label, const char* shortcut, bool* selected );

    /* Toolbar -- an icon strip built on bar() (flow/).  toolbar_begin id-scopes the strip so
       two toolbars' buttons never collide, then opens a bar() run; toolbar_end pops it.  Emit
       inside any window / child -- it owns no window of its own, matching bar() itself.  It does
       NOT push a scale -- wrap it in the caller's own scale_push/scale_pop (GUI_SCALE_BAR is the
       density step authored for icon toolbars, but any GUI_SCALE_* works) so a single app can mix
       toolbar sizes, e.g. a large main-panel strip next to a regular-scale one.

       toolbar_button / toolbar_toggle are square icon cells (press / latched-on); their id_str is
       the id only ("##save") -- pass a display label there and it is still just the id, nothing
       is drawn from it.  toolbar_dropdown_begin/end is the split-button form: the icon plus an
       adjacent down-arrow column, opening an arbitrary-widget popup below the button -- the same
       anchor / dismiss mechanics as combo_begin/combo_end, so put ANY widgets in the body,
       including menu_item rows for the icon + label + shortcut three-column layout menus already
       give you. tooltip may be NULL.

           gui()->scale_push( GUI_SCALE_BAR );
           gui()->toolbar_begin( "main" );
               if ( gui()->toolbar_button( "##save", icon_save, "Save (Ctrl+S)" ) ) save();
               gui()->toolbar_toggle( "##wire", icon_wire, &wireframe, "Wireframe" );
               gui()->toolbar_separator();
               if ( gui()->toolbar_dropdown_begin( "##view", icon_eye, "View Mode" ) ) {
                   gui()->menu_item( "Lit", NULL, NULL );
                   gui()->menu_item( "Wireframe", NULL, NULL );
                   gui()->toolbar_dropdown_end();
               }
           gui()->toolbar_end();
           gui()->scale_pop(); */

    bool ( *toolbar_begin           )( const char* str_id );
    void ( *toolbar_end             )( void );
    bool ( *toolbar_button          )( const char* id_str, gui_icon_id_t icon, const char* tooltip );
    bool ( *toolbar_toggle          )( const char* id_str, gui_icon_id_t icon, bool* v, const char* tooltip );
    bool ( *toolbar_dropdown_begin  )( const char* id_str, gui_icon_id_t icon, const char* tooltip );
    void ( *toolbar_dropdown_end    )( void );
    void ( *toolbar_separator       )( void );

    /*===================================  widgets/ -- the stock widget set  ====================================*/

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched window_begin / window_end pair, and only
       when window_begin returned true -- a collapsed window draws no clip, so widgets emitted
       into it render straight onto the screen.  The bool guard is the caller's job. */

    void ( *text        )( const char* str );
    void ( *textf       )( const char* fmt, ... );
    void ( *bullet_text )( const char* str );

    /* text_colored / text_disabled -- a text run in an explicit colour / the dim secondary colour.
       text_wrapped -- a run word-wrapped to the region content width (paragraphs, help blurbs).
       bullet -- a standalone bullet glyph; new_line -- break + a blank line of height h (undo
       same_line).  h == 0 is a literal zero-height break; h < 0 defers to the theme's line height
       (the vertical mirror of same_line's own 0-literal / negative-defers rule). */
    void ( *text_colored  )( u32 abgr, const char* str );
    void ( *text_disabled )( const char* str );
    void ( *text_wrapped  )( const char* str );
    void ( *bullet        )( void );
    void ( *new_line      )( f32 h );

    /* label_text -- a read-only "value + label" row that lays out like the labeled value widgets
       (label track / control track under a form or field_split, trailing label otherwise) but is
       pure display.  For information rows that align with the editable widgets around them. */
    void ( *label_text  )( const char* label, const char* value );
    bool ( *button      )( const char* label );

    /* small_button -- a compact button with no vertical frame padding (a text-height row), for
       inline controls packed onto a text line.  progress_bar -- a filled completion track showing
       `fraction` (0..1) with a centered caption (NULL = "NN%" percentage, "" = no text). */
    bool ( *small_button )( const char* label );
    void ( *progress_bar )( f32 fraction, const char* overlay );

    /* arrow_button -- a square, framed, non-text button drawing a triangle pointing `dir`.  The id
       comes from the label (use a "##id" string, nothing is displayed).  Combine with
       push_item_flag( GUI_ITEM_BUTTON_REPEAT, true ) for press-and-hold stepping (spin buttons). */
    bool ( *arrow_button )( const char* id_str, gui_dir_t dir );

    bool ( *checkbox    )( const char* label, bool* v );

    /* radio_button -- one option of a mutually-exclusive set: shows on while *v == value, a click
       sets *v = value.  Emit several against the same v (same_line between them for a row) to form
       a group; returns true only on the frame a click changes the selection. */
    bool ( *radio_button )( const char* label, i32* v, i32 value );
    /* slider_float -- draggable [lo,hi] slider; returns true while dragging.  The current value is
       drawn centered on the track by default ("%.3f"); set GUI_ITEM_NO_VALUE_TEXT (push or
       next_item_flag) to hide it for a bare slider. */
    bool ( *slider_float)( const char* label, f32* v, f32 lo, f32 hi );

    /* slider_float_step -- slider_float that quantizes the value to `step` (e.g. 0.25 snaps to the
       quarter marks); step <= 0 is continuous, identical to slider_float. */
    bool ( *slider_float_step)( const char* label, f32* v, f32 lo, f32 hi, f32 step );

    /* slider_int -- integer slider over [lo,hi]; every track position lands on a whole value, drawn
       centered ("%d").  Same GUI_ITEM_NO_VALUE_TEXT suppression as slider_float. */
    bool ( *slider_int  )( const char* label, i32* v, i32 lo, i32 hi );

    /* drag_int -- a framed integer field driven by a left/right drag (the DragInt analogue): no
       track, so no max travel -- v_speed units of value per pixel.  v_min < v_max bounds it; both
       equal leaves it unbounded.  format is the printf form of the shown value ("%d" when NULL,
       e.g. "HP: %d").  Returns true only on frames the drag changes the value. */
    bool ( *drag_int    )( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format );

    /* drag_float -- the floating-point DragFloat: a framed value changed by a left/right drag,
       v_speed units per pixel, no track travel.  v_min < v_max bounds it; both equal is unbounded.
       fmt is the printf form ("%.3f" when NULL).  drag_float2/3/4 lay N equal sub-boxes (vector edit). */
    bool ( *drag_float  )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float2 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float3 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float4 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );

    bool ( *color_edit3 )( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags );
    bool ( *color_edit4 )( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags );

    bool ( *input_text    )( const char* label, char* buf, u32 bufsz );

    /* input_text_ex -- like input_text but with an on_change callback fired after any frame
       that modifies the buffer.  Pass NULL for on_change to suppress.  cb_user is forwarded
       verbatim to the callback. */
    bool ( *input_text_ex )( const char* label, char* buf, u32 bufsz,
                              gui_text_cb_fn on_change, void* cb_user );

    /* input_text_with_hint -- like input_text but shows `hint` in dim text inside the box
       when the buffer is empty and the field is not focused.  The hint is never written to buf. */
    bool ( *input_text_with_hint )( const char* label, const char* hint, char* buf, u32 bufsz );

    /* input_text_multiline -- multi-line text editor box of pixel height h (0 = eight lines).
       Enter inserts a newline (it never submits or drops focus; Escape reverts to the
       focus-gain content and leaves).  Arrow keys move the caret in 2D with a sticky
       preferred column, Home/End are line-local (Ctrl jumps to the buffer ends), PageUp/Dn
       page by the visible height.  Selection, clipboard, and undo/redo match input_text;
       paste keeps newlines.  No word wrap: long lines pan, chasing the caret.  The box is a
       child region (the listbox recipe), so vertical overflow gets the standard region
       scrollbar / wheel / clipping, and the label trails the box's right edge.  Returns true
       on any frame that modifies the buffer. */
    bool ( *input_text_multiline )( const char* label, char* buf, u32 bufsz, f32 h );

    /* input_int / _float / _double -- numeric text field that parses on Enter or focus loss.
       step != 0 shows [-][+] buttons at the right of the box; Ctrl uses step_fast.
       fmt is the snprintf format for display and focus-seed ("%.3f" / "%d" when NULL).
       Scientific notation is accepted ("1e+8").  Returns true when the value changes. */
    bool ( *input_int    )( const char* label, i32* v, i32 step, i32 step_fast );
    bool ( *input_float  )( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt );
    bool ( *input_double )( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt );

    /* input_floatN -- N-component float row: N equal text boxes across the control track.
       fmt applies to every component (NULL -> "%.3f").  Returns true if any component changes. */
    bool ( *input_float2 )( const char* label, f32* v, const char* fmt );
    bool ( *input_float3 )( const char* label, f32* v, const char* fmt );
    bool ( *input_float4 )( const char* label, f32* v, const char* fmt );

    /* selectable -- a full-width row that highlights on hover and fills when selected; the
       list-box building block.  A click toggles *selected (pass NULL for click-only); returns
       true on the clicked frame so a caller managing single-selection can set its own index. */
    bool ( *selectable  )( const char* label, bool* selected );

    /* Combo box -- a framed preview box (selected text + a down arrow) with a trailing label that
       drops a popup of rows below it on click.  combo_begin opens the dropdown: it returns true
       only while the dropdown is open, so -- like window_begin's collapse -- guard the rows on the
       return and call combo_end only then.  preview_value is the text shown in the closed box (the
       caller's current selection, usually items[current]).  A row clicked in the body dismisses the
       combo automatically, so emit selectables and set your selection from their return:

           if ( gui()->combo_begin( "mode", items[cur], GUI_COMBO_NONE ) ) {
               for ( i32 i = 0; i < n; ++i )
                   if ( gui()->selectable( items[i], NULL ) ) cur = i;
               gui()->combo_end();
           }

       flags is gui_combo_flags_t: the HEIGHT_* group caps the dropdown to a fixed row count
       (then it scrolls), 0 (GUI_COMBO_NONE) is the ~8-row default.  combo() is the one-liner over
       an array of strings (*current_item is the selected index; out of range shows an empty
       preview).  Both return true on the frame the selection changes. */
    bool ( *combo_begin )( const char* label, const char* preview_value, gui_combo_flags_t flags );
    void ( *combo_end   )( void );
    bool ( *combo       )( const char* label, i32* current_item, const char* const items[], i32 count );

    /* List box -- a framed, independently scrolling box of selectable rows with a trailing label.
       listbox_begin opens the box (w / h in pixels; w <= 0 fills the line after the label, h <= 0
       is ~7 rows tall) and always returns true -- always pair with listbox_end, and fill it with
       selectables exactly like a child_begin:

           if ( gui()->listbox_begin( "items", 0, 0 ) ) {
               for ( i32 i = 0; i < n; ++i ) {
                   bool sel = ( cur == i );
                   if ( gui()->selectable( names[i], &sel ) ) cur = i;
               }
               gui()->listbox_end();
           }

       listbox() is the one-liner over an array of strings; height_in_items <= 0 picks
       min(count, 7).  Returns true on the frame the selection changes. */
    bool ( *listbox_begin )( const char* label, f32 w, f32 h );
    void ( *listbox_end   )( void );
    bool ( *listbox       )( const char* label, i32* current_item, const char* const items[],
                             i32 count, i32 height_in_items );

    /* collapsing_header -- a clickable fold bar (arrow + label) that returns its open state; the
       caller guards the section body with the return ( if ( header(...) ) {...} ), so a closed
       header skips its contents.  Open state persists by id; closed by default.
       separator_text   -- a labeled horizontal rule, "-- Text --------". */
    bool ( *collapsing_header )( const char* label );
    void ( *separator_text    )( const char* label );

    /* tree_node / tree_pop -- a collapsing_header without the frame: an arrow + label row that
       folds and indents a nested block while open (file explorers, outline views).  Guard the body
       with the return and, when true, close it with tree_pop, which removes the indent the open
       node added:

           if ( gui()->tree_node( "Parent" ) )
           {
               gui()->text( "Child" );
               gui()->tree_pop();
           }

       indent / unindent -- shift the content column right (or back) by w pixels (w <= 0 = one row
       height) so a block of widgets lays out inset; the mechanism behind tree_node, usable alone.
       Balance every indent with an unindent of the same width.  Flow layouts only. */
    bool ( *tree_node )( const char* label );
    void ( *tree_pop  )( void );
    void ( *indent    )( f32 w );
    void ( *unindent  )( f32 w );

    /* Tab bar -- an in-window tabbed content switcher (the ImGuiTabBar analogue): a strip of
       clickable chips with only the selected tab's body emitted below it.  Distinct from docking,
       which tabs whole windows into a dock node -- this tabs SECTIONS of one window's body.

       tab_bar_begin opens the bar and reserves the strip row; it always returns true (guard-and-pair
       like child_begin) -- always call tab_bar_end.  Each tab_item_begin draws one chip and returns
       true only for the selected tab, so -- like window_begin's collapse -- guard the body on the
       return and call tab_item_end only then.  The active selection persists per bar id; the first
       tab is the default.  p_open (optional, may be NULL): when non-NULL a close (x) appears on the
       chip and clicking it sets *p_open = false (the caller drops the item next frame).

           if ( gui()->tab_bar_begin( "settings", GUI_TAB_BAR_NONE ) )
           {
               if ( gui()->tab_item_begin( "General", NULL, GUI_TAB_ITEM_NONE ) )
               {
                   gui()->checkbox( "Vsync", &vsync );
                   gui()->tab_item_end();
               }
               if ( gui()->tab_item_begin( "Audio", NULL, GUI_TAB_ITEM_NONE ) )
               {
                   gui()->slider_float( "Volume", &vol, 0.0f, 1.0f );
                   gui()->tab_item_end();
               }
               gui()->tab_bar_end();
           } */
    bool ( *tab_bar_begin  )( const char* str_id, gui_tab_bar_flags_t flags );
    void ( *tab_bar_end    )( void );
    bool ( *tab_item_begin )( const char* label, bool* p_open, gui_tab_item_flags_t flags );
    void ( *tab_item_end   )( void );

    /*  split_begin / split_next / split_end -- two panels side by side sharing a Y-level.

        split_begin( id, right_w ) opens a split: the left panel fills, the right panel is
        right_w pixels wide.  split_next() closes the left panel and opens the right.
        split_end() closes the right panel.  Each panel is an independent flow region -- declare
        a layout mode (stack/cols/...) inside each as usual.  Heights are cached per-id across
        frames (one-frame lag on first appearance, then stable).

        Use button_width() to size the right panel to fit a specific button label exactly:

            const char* title = "Bake & Preview";
            gui()->split_begin( "##src", gui()->button_width( title ) );
                gui()->stack();
                gui()->combo_begin( ... ); ... gui()->combo_end();
                gui()->slider_int( ... );
            gui()->split_next();
                gui()->stack();
                gui()->button_fill( title );
            gui()->split_end();

        button_width( label ) -- natural pixel width of a button with that label.
        button_fill  -- a button that fills the remaining height of its containing region.
        Identical to button() but height = content_avail().y.  Designed for the right panel
        of a split so it matches the adjacent left panel's content height naturally. */

    f32  ( *button_width  )( const char* label );
    bool ( *button_fill   )( const char* label );

    /*==========================  table/ -- multi-column rows over the layout engine  ===========================*/

    /* Tables -- a multi-column layout with self-fitting cells (one table clip, no per-cell clip) and
       optional scrolling, sortable headers, and resizable columns.  Conceptually a grid whose rows accumulate and scroll
       (like flow) with column tracks resolved once per table (like grid), plus frozen header support.

       USAGE CONTRACT:
         1. table_begin()            -- open the table; returns true (always, like child_begin).
                                        Consume it paired with table_end() regardless.
         2. table_setup_column()     -- call ncols times between table_begin and the first row.
                                        The calls may be omitted; all columns default to stretch.
         3. table_headers_row()      -- optional; draws and clips a non-scrolling header strip and
                                        runs header sort-click interaction.
                                        Call after all table_setup_column, before the first data row.
         4. for each row:
              table_next_row()       -- begin a new data row.  First call sets row 0.
              for each column:
                table_next_column()  -- advance to the next column and return true; the cell sizes the
                                        widget and long text ellipsizes to the column (self-fit, no
                                        per-cell clip).  Returns false past the last column.
                <emit widgets>       -- normal widget calls; they land inside the cell.
         5. table_end()              -- close the table; restores the parent layout.

       Column widths use the overloaded-unit rule (same as columns / grid):
           > 1.0  fixed pixels   1.0  fill / stretch   (0,1)  fraction   0.0  natural (= stretch)
       Height: 0 = auto (8 rows tall), > 0 = fixed pixels.

           if ( gui()->table_begin( "my_table", 3, GUI_TABLE_NONE, 0 ) )
           {
               gui()->table_setup_column( "Name",  GUI_TABLE_COL_STRETCH,  0     );
               gui()->table_setup_column( "Value", GUI_TABLE_COL_FIXED,    80.0f );
               gui()->table_setup_column( "Unit",  GUI_TABLE_COL_FIXED,    40.0f );
               for ( i32 i = 0; i < count; ++i )
               {
                   gui()->table_next_row( 0 );
                   if ( gui()->table_next_column() ) gui()->text( name[i]  );
                   if ( gui()->table_next_column() ) gui()->text( value[i] );
                   if ( gui()->table_next_column() ) gui()->text( unit[i]  );
               }
               gui()->table_end();
           }

       table_set_column_index( col ) -- jump to a specific column (0-based) rather than advancing.
       table_get_column_count()      -- number of columns the table was opened with.
       table_get_column_index()      -- current column index (-1 before the first next_column).
       table_get_row_index()         -- current row index (-1 before the first next_row).
       table_get_sort_specs( out )   -- read raw sort state (column + direction); returns true on
                                        the frame a header was clicked.  Use when you want to sort
                                        your own data structure by hand.
       table_sort_order( order, n, val_fn, cmp_fn, user )
                                     -- built-in sort: reorder a display-order index array to match
                                        the active sort.  Pass val_fn for automatic alphabetical /
                                        numeric ordering, or cmp_fn for a custom comparator.  Cheap
                                        to call every frame (only reorders on a header click).
       table_set_bg_color( target, abgr ) -- override the current row's or cell's background. */

    bool ( *table_begin            )( const char* id, i32 ncols, gui_table_flags_t flags, f32 height );
    void ( *table_end              )( void );
    void ( *table_setup_column     )( const char* label, gui_table_col_flags_t flags, f32 width );
    void ( *table_headers_row      )( void );
    void ( *table_next_row         )( f32 min_h );
    /* table_rows_clip -- rows_clip's table face: call after the header with the same min_h the
       rows pass to table_next_row (0 = WIDGET_H), then loop only the returned [first, last).
       Stripes/dividers keep phase and the scrollbar sees all `count` rows; use with SCROLL_Y. */
    gui_span_t ( *table_rows_clip  )( i32 count, f32 min_h );
    bool ( *table_next_column      )( void );
    bool ( *table_set_column_index )( i32 col );
    i32  ( *table_get_column_count )( void );
    i32  ( *table_get_column_index )( void );
    i32  ( *table_get_row_index    )( void );
    bool ( *table_get_sort_specs   )( gui_table_sort_specs_t* out );
    bool ( *table_sort_order       )( i32* order, i32 count, gui_table_sort_value_fn val_fn,
                                      gui_table_sort_cmp_fn cmp_fn, void* user );
    void ( *table_set_bg_color     )( gui_table_bg_target_t target, u32 abgr );

    /* window_set_drag() -- select how windows may be dragged (global default TITLEBAR).
       Call between frames; affects every window. */
    void ( *window_set_drag )( gui_win_drag_t mode );

    /* window_set_nav() -- aim keyboard navigation at a window by title (the explicit-focus entry).
       Clears the nav cursor so the window's first item takes focus and engages the nav highlight.
       Nav otherwise follows the front-most window automatically; Ctrl+Tab cycles among windows and
       Alt enters the main menu bar.  An open popup / menu always captures nav while it is open. */
    void ( *window_set_nav )( const char* title );

    /*============================================================================================================
        GUI_STYLE -- style resolution  (style/)
        Theme, style stacks, density scale, indicator-shape selectors.  A SERVICE tier: the style
        unit implements all of these, so any UI on the service API -- not just chrome -- styles
        through them; a game kit gets colors + metrics without pulling in windowing.
    =============================================================================================================*/

    gui_style_t*       (*style_get)( void );    /* mutable base -- marks the theme anonymous       */
    const gui_style_t* (*style_peek)( void );   /* read-only base -- does NOT mark theme anonymous  */
    void               (*style_apply)( void );

    /* Theme -- named style presets that form the root of the push/pop stack.

       theme_list()  -- returns the built-in theme array and writes the count to *count_out.
       theme_set()   -- copies the named theme into the base style and immediately resets the
                        push stacks; returns false if the name is not found (no-op).
       theme_get()   -- returns the active theme name, or NULL after a raw style_get() edit.
       theme_reset() -- if a named theme is active, restores the base from it; then clears the
                        color + var push stacks (the "large style change" escape hatch -- call
                        this instead of issuing many paired push/pop calls just to revert).

           u32 n;
           const gui_theme_t* list = gui()->theme_list( &n );
           gui()->theme_set( list[0].name );       // switch to first built-in
           // ... many style pushes ...
           gui()->theme_reset();                   // clear everything, back to base */

    const gui_theme_t* (*theme_list )( u32* count_out );
    bool               (*theme_set  )( const char* name );
    const char*        (*theme_get  )( void );
    void               (*theme_reset)( void );

    /* Style stacks -- the push-model theme override (gui_col_t colors, gui_style_var_t metrics).
       push overrides a slot until the matching pop (pop takes a count, like ImGui); next_style_*
       overrides for just the next widget, no pop.  Colors are abgr (GUI_COLOR); vars are f32 px.
       Like the item flags, this is callsite-free: every widget already reads the palette + metrics
       through the resolver, so an override reaches them without any widget change.

           gui()->push_style_color( GUI_COL_WIDGET_BG, GUI_COLOR( 0xFF, 0, 0, 0xFF ) );
           gui()->push_style_var( GUI_VAR_WIDGET_PAD, 20.0f );
           gui()->button( "Big Red" );
           gui()->pop_style_var( 1 );
           gui()->pop_style_color( 1 ); */

    void ( *push_style_color )( gui_col_t slot, u32 abgr );
    void ( *pop_style_color  )( u32 count );
    void ( *next_style_color )( gui_col_t slot, u32 abgr );
    void ( *push_style_var   )( gui_style_var_t var, f32 value );
    void ( *pop_style_var    )( u32 count );
    void ( *next_style_var   )( gui_style_var_t var, f32 value );

    /* style_color() -- resolved read of one palette slot (theme base + push/next overrides):
       the value a stock widget would paint with right now.  The public door to the
       user-extended range (GUI_COL_USER_*): seed a user slot, paint custom drawing with this
       read, and it rides the theme + stacks like stock chrome. */
    u32  ( *style_color      )( gui_col_t slot );

    /* scale_push / scale_pop -- scope a named density step (gui_scale_t: DENSE / STD / ROOMY /
       BAR) over the widgets until the pop: the theme's row + pad + gap for that step land on
       the style-var stack, so every metric read and counting helper (sz_rows_h, sz_fit_row)
       inside speaks the step.  Push before opening the region/child it styles.  To size against
       a step without pushing it, query sz_scale_row( s ) from the sizing family. */
    void ( *scale_push )( gui_scale_t s );
    void ( *scale_pop  )( void );

    /* Global indicator-shape selectors -- set the default check / bullet / arrow glyph the chrome
       draws (gui_check_style_t / gui_bullet_style_t / gui_arrow_style_t).  These are style
       state, not draw calls: scope a change locally instead with push_style_var on
       GUI_VAR_CHECK_STYLE / _BULLET_STYLE / _ARROW_STYLE. */
    void ( *set_check_style  )( u8 style );
    void ( *set_bullet_style )( u8 style );
    void ( *set_arrow_style  )( u8 style );

    /*============================================================================================================
        GUI_DEBUG -- overlays, dashboard, stepper  (debug/)
        Diagnostic surfaces and retained-cache levers, hotkey-armed via debug_enable.
    =============================================================================================================*/

    /* Debug overlay -- a separate draw list painted last, on top of the UI.  Pass a bitmask
       of gui_dbg_layer_t to debug_set_layers() to choose which visualizations show; pass
       GUI_DBG_NONE (0) to turn it off.  Compiled in for Debug builds only: in Release,
       set_layers is a no-op and get_layers returns 0.  The two slots stay in the vtable in
       every build so func_api_size is identical across a hot-reload. */

    void ( *debug_set_layers )( u32 layers );
    u32  ( *debug_get_layers )( void );

    /* Master debug switch.  When on, gui owns the debug hotkeys and overlay emission -- the host
       adds nothing to its loop.  Every hotkey below is gated behind a master ARM so the broad
       single-letter keys never fire during normal use:

         NP_DOT  master arm ('.'): toggle every debug hotkey below on / off as a group; off by
                 default, so nothing below responds until it is armed.  Disarming resets every
                 debug mode back to normal (overlays off, selector menu closed, render mode
                 normal, layers cleared) and re-arming restores the selector menu's remembered
                 lever values (debug_restore, gui_frame_overlay.c)
         NP1-NP5 debug overlay layers (window frames / interaction rects / resize bands / layout /
                 clips; Debug builds)
         NP6     content-rect outlines over scrollable regions (GUI_DBG_CONTENT -- drawn in
                 the main list so the box scrolls with the content it measures)
         NP7     region screen geometry (GUI_DBG_REGION -- view rect, reserved scrollbar
                 gutters, body interaction clip)
         F9      render mode: normal -> wireframe -> batch tint
         F10     pipeline dashboard window (backend memory maps / uploads / batches)
         NP+     perf overlay tier   (off / FPS / +timings / +counts & lever status / +retained)
         NP-     state overlay tier  (off / ids / +focus,nav / +popups)

       While armed, a dense checkbox/slider selector menu (right edge of the viewport) is also up:
       retained skip (tessellation cache), force redraw, and idle skip are toggled there now
       instead of the old C / F / I letters, alongside the NP+/NP- tiers as sliders.  A host that
       writes set_force_redraw (or set_retained_skip / set_idle_skip) itself every frame should
       check debug_hotkeys_armed() first and stand down while armed, or its own write will fight
       the menu's checkbox every frame the two disagree -- see debug_hotkeys_armed below.

       The perf / state overlays and the dashboard are emitted internally, last in the default
       context's build (at its ctx_end), so they draw on top and are counted like any widget.
       Letter hotkeys are fenced by want_capture_keyboard, so typing never toggles them. */
    void ( *debug_enable     )( bool enable );
    bool ( *debug_is_enabled )( void );

    /* debug_hotkeys_armed -- true while the NP_DOT master arm is on, i.e. the selector menu is up
       and owns force redraw / retained skip / idle skip.  A host with its own per-frame lever
       write (sb_gui_editor's scene-pass set_force_redraw is the reference case) should gate that
       write on !debug_hotkeys_armed() so the menu's checkbox wins instead of being silently
       reverted the next time the host's own trigger condition re-fires. */
    bool ( *debug_hotkeys_armed )( void );

    /* Debug render mode -- how the main UI draw list is rasterized (gui_render_mode_t): NORMAL,
       WIREFRAME (triangle edges), or BATCH (per-draw-call color tint).  A pipeline + push-constant
       switch, so it is live in every build (not gated to Debug like the overlay layers above). */
    void                ( *debug_set_render_mode )( gui_render_mode_t mode );
    gui_render_mode_t ( *debug_get_render_mode )( void );

    /* Dump the retained cache's slot table (each window's vertex/index/command bounds) to stdout.
       Debug builds also assert every frame that no two slots share buffer space, so a geometry-
       corruption bug traps at its source instead of showing as flicker/warping downstream. */
    void                ( *debug_dump_geometry )( void );

    /* Retained-skip: when on (default), an unchanged frame skips tessellation.  Toggle to benchmark
       or confirm that the hash-upfront path produces identical output to the reference. */
    void ( *set_retained_skip )( bool on );
    bool ( *retained_skip     )( void );

    /*============================================================================================================
        GUI_FRAME -- glue  (frame/)
        The conductor: lifecycle, boot, frame phases, pacing, viewports, contexts, event
        routing, memory / render stats.  Owns per-frame ordering and this vtable; no widgets.
    =============================================================================================================*/

    /* GPU resource lifecycle.
        init()      -- call after rhi()->init(); creates pipeline, font atlas, GPU buffers.
                       `font` optionally loads one of the built-in presets (gui_builtin_font_t,
                       gui.h) into slot 0; pass GUI_FONT_NONE to load nothing and call font_load()
                       yourself. A failed built-in load is non-fatal (a warning; init still
                       succeeds without text).
        shutdown()  -- call before rhi()->shutdown(); destroys all GPU resources.
        font_load() -- load a pre-baked .orb_font atlas into a new font id and make it active;
                       call after init(). Returns the new id (>= 1), or 0 on failure.
        font_load_builtin() -- font_load for a built-in preset (gui_builtin_font_t): the enum
                       already knows its asset path, so no path plumbing at the call site.
                       Same contract as font_load -- a NEW id, activated (the init()/boot()
                       preset in slot 0 is untouched; font_use( 0 ) switches back).  Returns
                       the new id, or 0 for GUI_FONT_NONE / an unknown preset / a failed load.
        asset_path() -- resolve `relative` (e.g. "assets/icon/foo.png") against sys_root_dir() --
                       the build root, one level above the executable -- the same convention
                       load_icon and the built-in font/icon presets resolve through. Writes the
                       resolved path into `out` (out_size bytes); for a caller that wants the
                       absolute path itself (e.g. a plain fopen) rather than a load_icon call. */

    bool                ( *init      )( gui_builtin_font_t font );
    void                ( *shutdown  )( void );
    u32                 ( *font_load )( const char* path );
    u32                 ( *font_load_builtin )( gui_builtin_font_t font );
    void                ( *asset_path )( const char* relative, char* out, int out_size );

    /* boot() -- TEST-BED tier: the one-call alternative to the block above for sandboxes, demos,
       and quick tools whose main window IS a gui surface -- gui owns the window + render context
       end to end, exactly like its tear-off floaters.  Non-idiomatic for engine hosts: those run
       through the runtime host (run_host_main), which keeps ownership of the window/loop and
       wires gui as an optional service.  Stands up the whole stack from a single descriptor
       (gui_boot_desc_t, gui.h): rhi()->init() (idempotent -- safe if the host already ran it),
       app window (borderless by default, with the chrome shell then auto-emitted each frame;
       os_chrome opts back into the stock OS frame), rhi context, init(font),
       set_frame_hooks, debug_enable, and the primary viewport -- returned, or GUI_VP_INVALID
       with everything unwound on failure.  Call once after mod_init_all, before any other window
       opens.  shutdown() tears down what boot created (context + window); rhi()->shutdown()
       stays with the host, last.  Pairs with frame_poll / present below for the full easy-mode
       loop; a host needing manual control of any stage simply keeps calling the explicit block
       instead -- boot is composition, not replacement, and viewport_open still attaches gui to a
       host-owned window. */

    gui_vp_t            ( *boot )( const gui_boot_desc_t* desc );

    /* Full memory footprint currently held by gui, in bytes: GPU buffers + atlases, the fixed CPU
       backend buffers, and the per-context heap blocks -- see gui_mem_stats_t (gui.h) for the
       bucket breakdown.  print_mem_stats() dumps the same breakdown to stdout as a table. */

    gui_mem_stats_t     ( *mem_stats       )( void );
    void                ( *print_mem_stats )( void );

    /* Per-frame render statistics (geometry + batch counts) for the LAST completed frame.
       Published at frame_begin, so a read during the build reflects the previous frame -- the
       standard one-frame lag.  Feeds an FPS / performance overlay without re-deriving counts. */
    gui_render_stats_t  ( *render_stats )( void );

    /* NOTE: the built-in perf overlay, state overlay, and pipeline dashboard are no longer emitted
       by host code.  debug_enable( true ) arms an internal hotkey driver (numpad '.' arms the group,
       then P / O / F10 ...) and gui emits them itself, last in the default context's build -- see
       debug_enable (GUI_DEBUG section).  The perf overlay's clock arrives once through set_frame_hooks. */

    /* Frame hooks -- one-time wiring (after init) of the host OS services gui cannot reach itself
       (gui links only app + rhi, no sys):

         clock       -- monotonic seconds source (sys_tick_seconds); brackets the frame for the
                        perf overlay's emit / render cost readouts.  NULL leaves timing at zero.
         sleep_ms    -- thread sleep (sys_sleep_milliseconds); frame_pace's spin/animation sleep.
         wait_events -- block until OS input or timeout (sys_wait_for_os_events_ms); enables the
                        idle-skip path of frame_pace.  NULL disables idle skip entirely. */

    void ( *set_frame_hooks )( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events );

    /* Frame lifecycle.  A frame is four explicit phases -- this is a multi-context system and the
       API does not hide it; even a single-context host names its one context:

         if ( frame_begin(dt) )        -- global: snapshot app input, compute frame_dirty, reset the
         {                                draw list on dirty frames.  Binds NO context; call once at
                                          the top of the frame.  Returns frame_dirty: emit the UI
                                          build only when true -- on a false (clean) frame skip the
                                          context scopes entirely; render() replays the preserved
                                          geometry verbatim and frame_end patches the volatile
                                          widgets (gui()->volatile_cb) internally.
           ctx_begin(GUI_CTX_DEFAULT) -- bind a context and run its per-frame init; emit its
              ... emit windows ...        windows immediately after.
           ctx_end()                    -- close it, rebinding the previously-bound context.  Closing
         }                                the DEFAULT context also auto-emits the debug overlays
                                          when debug_enable is on.
         frame_end()                   -- seal the build (latches emit cost; asserts ctx balance).
                                          Call on clean frames too -- it runs the volatile replay.

       frame_begin/frame_end and ctx_begin/ctx_end are balanced scopes, exactly like
       window_begin/window_end: every begin has an end, and each end restores the scope its begin
       opened.  render() runs AFTER frame_end and consumes the sealed draw list.

       render()    -- flush one viewport's geometry partition to GPU; opens a LOAD render pass on
                      that viewport's swapchain, emits all draw calls, and closes the pass.  Also
                      paints the debug overlay when vp is the primary (index 0).
                      Call once per live viewport, each with the matching context cmd.

       frame_pace( spin_sleep_ms, anim_sleep_ms )
                   -- end-of-loop frame pacing; call once at the very bottom of the main loop.
                      Default path: sleep spin_sleep_ms between frames (4 ~= 250 Hz).  With idle
                      skip on (set_idle_skip, or the I hotkey under debug_enable): block on OS
                      input so a static UI burns no frames, sleeping anim_sleep_ms (16 ~= 60 Hz)
                      only while a widget animation settles.  0 opts that sleep out (no call),
                      even while the feature is on -- free-run for that path.  A no-op until
                      set_frame_hooks provides the sleep / wait callbacks. */

    bool ( *frame_begin )( f32 dt );
    void ( *frame_end   )( void );
    void ( *render      )( gui_vp_t vp, rhi_cmd_t cmd );
    void ( *frame_pace  )( i32 spin_sleep_ms, i32 anim_sleep_ms );

    /* Easy-mode loop wrappers (TEST-BED tier, same audience as boot() above -- engine hosts get
       this loop from run_host instead).  With boot() these shrink a sandbox's main loop to its
       essence.  frame_poll() works for any host (it needs only app + rhi routing); present_begin
       / present_end are boot-tier (they render through the boot-owned context) and form a
       balanced pair like every other begin/end: begin's bool gates the host's own passes, end
       is called unconditionally.

         while ( gui()->frame_poll( &dt ) )       -- pump the OS, route events (rhi swapchain
         {                                           resize, gui input + floater lifecycle),
                                                     return dt from the boot clock hook; false on
                                                     quit or main-window close.
             ...frame_begin/build/frame_end...    -- unchanged (see above).
             rhi_cmd_t cmd;
             if ( gui()->present_begin( &cmd ) )  -- viewport_update + minimized guard + rhi
                 ...host render passes...            frame open + swapchain clear (boot clear
                                                     color); true hands out the live cmd for the
                                                     host's own passes (offscreen scenes, custom
                                                     draws under the UI).
             gui()->present_end();                -- gui draw + present + all owned floaters.
                                                     Call unconditionally (minimized-safe);
                                                     no-op without a matching present_begin.
             gui()->frame_pace( 4, 16 );
         }

       The host keeps reading input through app()'s snapshot API (key_pressed etc.) as before --
       frame_poll only owns the event ring.  A host that needs the loop's internals (extra
       swapchains, custom event handling) writes the explicit loop instead; these are sugar over
       the same public calls. */

    bool ( *frame_poll    )( f32* out_dt );
    bool ( *present_begin )( rhi_cmd_t* out_cmd );
    void ( *present_end   )( void );

    /* Idle-skip control -- the programmatic twin of the I hotkey.  When on, frame_pace blocks on
       OS input while the UI is idle instead of spinning.  Off by default. */
    void ( *set_idle_skip )( bool on );
    bool ( *idle_skip     )( void );

    /* Viewport management.  A viewport is a render surface backed by an OS window.  One frame's build
       gathers every window's geometry into a single draw list; render() dispatches each window's
       partition to the viewport it is assigned to (window_set_next_viewport, or inherited from
       whichever viewport was most recently emitted into this frame).

       viewport_open()   -- open a surface for OS window win_id.  The initial drawable size is
                            queried from app() internally -- no redundant w/h parameters.
                            Returns a valid handle or GUI_VP_INVALID if the pool is full.
                            The first call creates the primary (index 0); call before any frames.
                            win_id routes mouse events from that OS window to this surface.
       viewport_close()  -- close a viewport and release its GPU geometry buffers.  Works for both
                            the primary and secondary viewports.  Windows on the closed viewport
                            automatically fall back to the primary.  The host owns the OS window and
                            rhi context; gui owns only the geometry.
       viewport_resize() -- update a viewport's drawable size.  Prefer rhi()->event() +
                            gui()->event() for automatic routing; call this directly only when
                            explicit control is needed.
       viewport_shell()  -- emit the window chrome for a borderless viewport.  Every host window is
                            one of two things: a UI window (window_begin -- a body full of widgets)
                            or a chrome shell for the OS window itself -- this call.  It emits a
                            frame-only GUI_WIN_NATIVE window: its titlebar IS the OS caption (title
                            text + min/max/close buttons, drag to move, double-click to maximize),
                            its border the OS sizing frame, and its body is empty and click-through
                            -- every other window lives on top of it.  Call it FIRST inside
                            ctx_begin, every frame, before any other window on that viewport.
                            Returns the caption band height so the host can stack its own strips
                            (menu bar, toolbar) below it -- the built-in main_menu_bar, free-window
                            clamping, and the dock tree already inset themselves automatically.
                            On a viewport whose OS window has its own chrome (opened without
                            APP_WIN_BORDERLESS) it is a no-op returning 0: call it unconditionally
                            and flip only the window_open flag to switch chrome modes.  flags may
                            add GUI_WIN_NOTITLEBAR / NO_MINIMIZE / NO_MAXIMIZE / NORESIZE.
       viewport_caption_h() -- the caption band height (px) a chrome shell published on this
                            viewport; 0 for an OS-chrome window.  The query twin of
                            viewport_shell's return, for hosts on the boot path (where the shell
                            is emitted internally) that stack pinned strips below the caption.
       viewport_size()      -- the viewport's current drawable size (disp_w/disp_h) -- the query
                            twin of viewport_resize.  Either out pointer may be NULL; an invalid
                            viewport reports 0 x 0.
       viewport_content_y() -- the y where host content starts on this viewport: 0 for an
                            OS-chrome window, the caption band on a gui-shelled native window,
                            plus the main menu bar when one was emitted (this frame or last --
                            emit the bar before querying).  The same bound the maximize pin and
                            free-window clamp use, published so hosts place windows below the
                            viewport chrome without summing the parts themselves. */

    gui_vp_t    ( *viewport_open      )( i32 win_id );
    void        ( *viewport_close     )( gui_vp_t vp );
    void        ( *viewport_resize    )( gui_vp_t vp, i32 w, i32 h );
    f32         ( *viewport_shell     )( gui_vp_t vp, const char* title, gui_win_flags_t flags );
    f32         ( *viewport_caption_h )( gui_vp_t vp );
    void        ( *viewport_size      )( gui_vp_t vp, i32* out_w, i32* out_h );
    f32         ( *viewport_content_y )( gui_vp_t vp );

    /* gui-OWNED floater surfaces.  Where viewport_open hands gui a host-created window+context
       to flush into, these own the OS window + rhi context end to end -- gui creates them on
       spawn and tears them down on close.  This is the lifecycle the tear-off gesture drives;
       a host may also call viewport_spawn directly to place a panel in its own OS window.

       viewport_spawn()          -- open a floater hosting its own OS window at (x,y) sized w x h;
                                    returns its viewport handle (assign windows via
                                    window_set_next_viewport) or GUI_VP_INVALID.  Between frames.
       viewport_update()         -- reconcile owned floaters with their OS windows: apply tear-off /
                                    merge-back and tear down closed or abandoned surfaces.  Call once
                                    per frame AFTER the UI build and BEFORE rendering (the safe point
                                    to free a surface).
       viewport_render_floaters() -- present every owned floater from the shared draw list, each on
                                    its own rhi context (frame_begin/clear/flush/frame_end).  The
                                    host still presents the main surface (index 0) via render(). */

    gui_vp_t   ( *viewport_spawn           )( const char* title, i32 x, i32 y, i32 w, i32 h );
    void       ( *viewport_update          )( void );
    void       ( *viewport_render_floaters )( void );

    /* Multi-context -- isolated per-context retained state (windows, nav, popups, keyed widget state,
       id namespace).  The primary context (GUI_CTX_DEFAULT / 0) is always live after init().

       ctx_create()       -- allocate a fresh secondary context, sized to `cfg` (NULL / zero fields =
                             the internal maxima the library was compiled with).
                             Each gets a unique id_salt so same-named widgets in different contexts
                             never alias.  Returns GUI_CTX_INVALID on pool exhaustion.  Between frames.
       ctx_destroy()      -- free a secondary context; rebinds the default if it was current.  Never
                             destroys GUI_CTX_DEFAULT.  Call between frames.
       ctx_bind()         -- make ctx the current context with no per-frame init: a mid-build "switch
                             retained state" escape hatch.  ctx_begin/ctx_end are the normal scope;
                             reach for ctx_bind only to peek at another context's state mid-frame.
                             GUI_CTX_DEFAULT (0) or any invalid handle rebinds the default.
       ctx_set_listening() -- set whether a context receives hover/click/nav input.  The default context
                             starts listening; secondary contexts start deaf.  Multiple contexts may
                             listen simultaneously; a deaf context renders but returns inert widget state.
                             Call between frames.
       ctx_begin()/ctx_end() -- bind a context for the frame and run its per-frame init, then close it.
                             A balanced scope: ctx_end rebinds whatever ctx_begin found bound.  ctx_begin
                             always runs the full frame init (hover promotion, nav, popup stale-close)
                             regardless of the listening flag, and leaves g_ctx pointing at the context,
                             so emit its windows IMMEDIATELY after the call.

       FRAME CONTRACT:
         if ( frame_begin(dt) )         -- once: input poll; true = emit this frame (frame_dirty).
         {
           ctx_begin(GUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
           ctx_end()                    -- close it (auto-emits debug overlays when debug is on).
           ctx_begin(ctx2)              -- a second context, if any; emit its windows.
           ctx_end()
         }
         frame_end()                    -- seal the build; volatile replay on clean frames.
       A single-context host runs exactly one ctx_begin(GUI_CTX_DEFAULT)/ctx_end pair. */

    gui_ctx_id_t( *ctx_create        )( const gui_ctx_config_t* cfg );
    void        ( *ctx_destroy       )( gui_ctx_id_t ctx );
    void        ( *ctx_bind          )( gui_ctx_id_t ctx );
    void        ( *ctx_set_listening )( gui_ctx_id_t ctx, bool listen );
    void        ( *ctx_begin         )( gui_ctx_id_t ctx );
    void        ( *ctx_end           )( void );

    /* Host input -- the host owns the app event ring drain and forwards each event here.
       event() handles:
         - APP_EV_CHAR / MOUSE_WHEEL / CLIPBOARD: input state; returns true (consumed).
         - APP_EV_MOUSE_MOVE / _DOWN / _UP: routes the cursor to the correct viewport; returns false.
         - APP_EV_WIN_RESIZE: updates the matching viewport's drawable size (primary or owned
           floater).  Also drives rhi context resize for owned floaters (gui owns those contexts).
           Returns true only for owned floater events; primary resize returns false so
           rhi()->event() can also handle the swapchain rebuild.
         - APP_EV_WIN_CLOSE: marks an owned floater for teardown (returns true); primary window
           close returns false so the host can exit. */

    bool ( *event )( const app_event_t* ev );

} gui_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( GUI_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    MOD_GATEWAY_STATIC( gui_api_t, gui )
    #define MOD_USE_GUI    /* static build */
    #define MOD_FETCH_GUI  true
#else
    MOD_GATEWAY_DYNAMIC( gui_api_t, gui )
    #define MOD_USE_GUI    MOD_DEFINE_API_PTR( gui_api_t, gui )
    #define MOD_FETCH_GUI  MOD_FETCH_API( gui_api_t, gui )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GUI_API_H
