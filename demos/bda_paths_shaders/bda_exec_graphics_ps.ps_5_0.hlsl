cbuffer Args : register(b0)
{
    uint expected;
    uint output_index;
};

RWByteAddressBuffer dst : register(u1);

float4 main() : SV_Target
{
    dst.Store(4 * output_index, expected);
    return float4(0.0, 0.0, 0.0, 1.0);
}
