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

RWByteAddressBuffer uav : register(u0);

RWBuffer<uint> feedback : register(u1);

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
    uav.GetDimensions(fb.size);

    fb.load1 = uav.Load<uint16_t>(byte_offset);

    uint16_t2 v2 = uav.Load<uint16_t2>(byte_offset);
    fb.load2_x = v2.x;
    fb.load2_y = v2.y;

    uint16_t3 v3 = uav.Load<uint16_t3>(byte_offset);
    fb.load3_x = v3.x;
    fb.load3_y = v3.y;
    fb.load3_z = v3.z;

    uint16_t4 v4 = uav.Load<uint16_t4>(byte_offset);
    fb.load4_x = v4.x;
    fb.load4_y = v4.y;
    fb.load4_z = v4.z;
    fb.load4_w = v4.w;

    uav.Store<uint16_t4>(byte_offset, v4 + uint16_t4(1, 1, 1, 1));

    feedback[feedback_offset +  0] = fb.size;
    feedback[feedback_offset +  1] = fb.load1;
    feedback[feedback_offset +  2] = fb.load2_x;
    feedback[feedback_offset +  3] = fb.load2_y;
    feedback[feedback_offset +  4] = fb.load3_x;
    feedback[feedback_offset +  5] = fb.load3_y;
    feedback[feedback_offset +  6] = fb.load3_z;
    feedback[feedback_offset +  7] = fb.load4_x;
    feedback[feedback_offset +  8] = fb.load4_y;
    feedback[feedback_offset +  9] = fb.load4_z;
    feedback[feedback_offset + 10] = fb.load4_w;
}
