Texture2D<uint> reference_values : register(t0);
StructuredBuffer<uint> result_values : register(t1);

static const float4 colors[6] =
{
    float4(0.15, 0.85, 0.25, 1.0),
    float4(0.10, 0.55, 1.00, 1.0),
    float4(1.00, 0.75, 0.10, 1.0),
    float4(0.80, 0.35, 1.00, 1.0),
    float4(0.00, 0.85, 0.85, 1.0),
    float4(1.00, 0.45, 0.15, 1.0),
};

float4 color_from_value(uint value)
{
    uint path = value & 0xffu;
    uint frame = (value >> 8) & 1u;
    float4 color = path < 6 ? colors[path] : float4(1.0, 0.0, 1.0, 1.0);
    if (frame != 0)
        color.rgb = lerp(color.rgb, float3(1.0, 1.0, 1.0), 0.22);
    return color;
}

float4 main(float4 position : SV_POSITION) : SV_Target
{
    uint col = min((uint)position.x * 6u / 960u, 5u);
    uint row = min((uint)position.y * 2u / 360u, 1u);
    uint ref_value = reference_values.Load(int3(col, 0, 0));
    uint value = row == 0 ? ref_value : result_values[col];

    float4 color = row == 0 || value == ref_value ? color_from_value(ref_value) : float4(1.0, 0.0, 1.0, 1.0);

    uint local_x = (uint)position.x - col * 160u;
    uint local_y = (uint)position.y - row * 180u;
    bool border = local_x < 3u || local_y < 3u || local_x >= 157u || local_y >= 177u;
    bool stripe = ((local_x + local_y) & 31u) < 8u;

    if (border)
        color.rgb = float3(0.0, 0.0, 0.0);
    else if (stripe)
        color.rgb *= 0.65;

    return color;
}
