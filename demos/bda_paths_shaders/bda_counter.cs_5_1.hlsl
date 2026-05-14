cbuffer Args : register(b0)
{
    uint expected;
    uint source_value;
    uint output_index;
    uint frame_id;
};

RWStructuredBuffer<uint> counted : register(u0);
RWByteAddressBuffer dst : register(u1);

[numthreads(1, 1, 1)]
void main()
{
    uint old_value = counted.IncrementCounter();
    counted[0] = frame_id;
    dst.Store(4 * output_index, old_value == 0 ? expected : 0xffffffffu);
}
