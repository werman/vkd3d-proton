cbuffer Args : register(b0)
{
    uint expected;
    uint source_value;
    uint output_index;
    uint frame_id;
};

ByteAddressBuffer timestamps : register(t0);
RWByteAddressBuffer dst : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    uint2 a = timestamps.Load2(0);
    uint2 b = timestamps.Load2(8);
    bool ordered = b.y > a.y || (b.y == a.y && b.x >= a.x);
    bool nonzero = any(a != uint2(0, 0)) || any(b != uint2(0, 0));
    dst.Store(4 * output_index, ordered && nonzero ? expected : 0xffffffffu);
}
