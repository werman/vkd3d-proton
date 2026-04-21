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

    /* Seed the location with a 64-bit value that has distinct high/low halves
     * and a low half that forces carry into the high half once we add below. */
    uav.Store2(byte_offset, uint2(0xfffffffeu, data));
    DeviceMemoryBarrier();

    /* Add high=1, low=3. Combined with the seed low 0xfffffffe this rolls the
     * low half over, feeding an additional +1 carry into the high half. */
    uint64_t add_val = (uint64_t(1) << 32) | uint64_t(3);
    uint64_t orig;
    uav.InterlockedAdd64(byte_offset, add_val, orig);

    uint2 raw = uav.Load2(byte_offset);
    uint64_t new_val = (uint64_t(raw.y) << 32) | uint64_t(raw.x);

    fb.load1 = uint(orig);
    fb.load2_x = uint(orig >> 32);
    fb.load2_y = uint(new_val);
    fb.load3_x = uint(new_val >> 32);
    fb.load3_y = 0;
    fb.load3_z = 0;
    fb.load4_x = 0;
    fb.load4_y = 0;
    fb.load4_z = 0;
    fb.load4_w = 0;

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
