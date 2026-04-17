struct Feedback
{
    uint size;
    uint load1;
    uint load2_x;
    uint load2_y;
    uint load3_x;
    uint load3_y;
    uint load3_z;
    uint load4_x;
    uint load4_y;
    uint load4_z;
    uint load4_w;
};

ByteAddressBuffer srv : register(t0);

RWStructuredBuffer<Feedback> feedback : register(u1);

cbuffer Args : register(b0)
{
    uint byte_offset;
    uint data;
    uint feedback_offset;
};

[numthreads(1,1,1)]
void main()
{
    Feedback fb;
    srv.GetDimensions(fb.size);
    fb.load1 = srv.Load(byte_offset);

    uint2 v2 = srv.Load2(byte_offset);
    fb.load2_x = v2.x;
    fb.load2_y = v2.y;

    uint3 v3 = srv.Load3(byte_offset);
    fb.load3_x = v3.x;
    fb.load3_y = v3.y;
    fb.load3_z = v3.z;

    uint4 v4 = srv.Load4(byte_offset);
    fb.load4_x = v4.x;
    fb.load4_y = v4.y;
    fb.load4_z = v4.z;
    fb.load4_w = v4.w;

    feedback[feedback_offset] = fb;
}
