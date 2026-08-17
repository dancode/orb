// qp.ps.hlsl -- the one fragment stage of the sb_quad_pull proof, shared by both pipelines so
// the A/B differs only in how vertices reach the rasterizer.  Deliberately near-trivial: the
// proof measures the VERTEX stage, so the fragment must cost the same tiny amount on both arms.
// It does consume all three interpolants -- a dead input would let the compiler hollow out the
// very vertex work under measurement.  Cooked to bin/shaders/qp.ps.oshd.

struct ps_in_t
{
    float4                sv_pos : SV_Position;
    float4                color  : COLOR0;
    float2                uv     : TEXCOORD0;
    nointerpolation uint  style  : TEXCOORD1;
};

float4 main( ps_in_t i ) : SV_Target0
{
    float shade = 0.80 + 0.20 * frac( float( i.style ) * 0.618034 );
    float3 rgb  = i.color.rgb * ( 0.75 + 0.25 * i.uv.x ) * shade;
    return float4( rgb, i.color.a );
}
