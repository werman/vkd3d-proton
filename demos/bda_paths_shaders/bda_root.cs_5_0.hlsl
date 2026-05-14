cbuffer Args : register(b0)
{
    uint expected;
    uint source_value;
    uint output_index;
    uint frame_id;
};

ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    uint value = src.Load(0);
    dst.Store(4 * output_index, value == source_value ? expected : 0xffffffffu);
}
