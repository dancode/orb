/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench_pipeline.c -- the pipeline and text suites.

    Pipeline cases isolate one stage each: the emit walls price the widget layer (emit_ms is
    the reading; like every case they also re-tessellate fully each frame); the static pair
    prices full re-tessellation of one identical frame against the retained replay of the same
    frame -- diff_static_scene is the suite's ONE deliberate retained_on case, and the
    difference is the cache's ROI, derived in the report; the animated scene changes every
    window every frame so upload + submit carry naturally-dirty content.

    Text cases hold the glyph count fixed and vary only the draw path.

==============================================================================================*/
// clang-format off

static const bench_case_t k_pipeline_cases[] =
{
    { "pipeline", "emit_button_wall", "2000 buttons, emit_ms is the metric",
      false, NULL, scene_wall_buttons, 2000 },
    { "pipeline", "emit_slider_wall", "1000 sliders, the heavier widget emit path",
      false, NULL, scene_wall_sliders, 1000 },
    { "pipeline", "emit_label_wall",  "2000 labels, the text-emit floor",
      false, NULL, scene_wall_labels,  2000 },
    { "pipeline", "emit_table_rows",  "500 table rows, the realistic emit shape",
      false, NULL, scene_table_rows,   500 },

    { "pipeline", "diff_static_scene", "6 static windows, retained cache ON: diff dominates",
      true,  NULL, scene_static_six, 0 },
    { "pipeline", "tess_static_scene", "same 6 windows, cache OFF: full retess every frame",
      false, NULL, scene_static_six, 0 },
    { "pipeline", "submit_animated",   "every window changes every frame: upload + submit",
      false, NULL, scene_animated_six, 0 },
};

static const bench_case_t k_text_cases[] =
{
    { "text", "text_plain",   "120 x 80-char lines, draw_text",
      false, NULL, scene_text_wall, BENCH_TEXT_PLAIN },
    { "text", "text_clipped", "same lines, glyph-level cut (fewer quads IS its saving)",
      false, NULL, scene_text_wall, BENCH_TEXT_CLIPPED },
    { "text", "text_xf",      "same lines rotated through draw_text_xf",
      false, NULL, scene_text_wall, BENCH_TEXT_XF },
    { "text", "text_outline", "same lines + 2px TEXT_EDGE outline (SDF face)",
      false, NULL, scene_text_wall, BENCH_TEXT_OUTLINE },
};

// clang-format on
/*============================================================================================*/
