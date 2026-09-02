// gui_quad.ps.hlsl -- the gui pipeline's fragment entry: the whole effect band (gui_fx.hlsli).
// Placement and clip arrive as per-quad interpolants from gui_quad.vs.hlsl; the record is a
// pure STYLE, which is what lets one style serve every placement and every scroll region.
// The resource name is "shader/gui_quad.ps"; the build cooks it to
// build/content/shader/gui_quad.ps.oshd.

#include "gui_fx.hlsli"
