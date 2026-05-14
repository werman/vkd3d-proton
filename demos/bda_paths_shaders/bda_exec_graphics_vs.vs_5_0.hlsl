struct vs_out
{
    float4 position : SV_POSITION;
};

vs_out main(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0),
    };

    vs_out o;
    o.position = float4(positions[vertex_id], 0.0, 1.0);
    return o;
}
