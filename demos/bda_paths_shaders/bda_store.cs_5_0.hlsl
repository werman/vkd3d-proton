cbuffer Args : register(b0)
{
    uint expected;
    uint source_value;
    uint output_index;
    uint frame_id;
};

RWByteAddressBuffer dst : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    dst.Store(4 * output_index, expected);
}
