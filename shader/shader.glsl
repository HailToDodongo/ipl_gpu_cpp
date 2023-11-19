#version 460

#define u32 uint

layout(local_size_x_id = 0) in;

layout(push_constant) uniform Params {
   u32 count;
   u32 offset;
} params;

// array of 64-bit values on the CPU side, final checksums
// only used for debugging purposes
layout(std430, binding = 0) buffer layout0 { u32 checksums[]; };
// input[] on the CPU side, fixed
layout(std430, binding = 1) readonly buffer layout1 { u32 inputReadOnly[]; };
// result flag, only gets written to with a non-zero value if a match is found
layout(std430, binding = 2) buffer layout2 { u32 found; };

#define MAGIC_NUMBER  0x6c078965

// Assumes that the difference check will never be zero.
// This may give wrong results (false positives & negatives).
#define FAST_HASH_MUL 1

u32 rotl(u32 n, u32 d) {
    return (n << d)|(n >> (32 - d));
}

u32 rotr(u32 n, u32 d) {
    return (n >> d)|(n << (32 - d));
}

u32 hashMulDiff(u32 factorBase, u32 factorA, u32 factorB) {
    u32 hi, lo;
    if (factorA == 0)factorA = factorB;

    umulExtended(factorBase, factorA, hi, lo);
    u32 diff = hi - lo;

    #ifdef FAST_HASH_MUL
      return diff;
    #else
      return (diff == 0) ? factorBase : diff;
    #endif
}

u32 hashMulDiff_ANonZero(u32 factorBase, u32 factorA) {
    u32 hi, lo;
    umulExtended(factorBase, factorA, hi, lo);
    u32 diff = hi - lo;

    #ifdef FAST_HASH_MUL
      return diff;
    #else
      return (diff == 0) ? factorBase : diff;
    #endif
}

// Taken from: https://github.com/phire/ipl3hasher/tree/optimizations
// @TODO: optimize out zero-value iterations
u32 finalizeLow(in u32[16] state)
{
    uvec2 buf = uvec2(state[0]);

    for(u32 i=0; i<16; i++)
    {
        u32 data = state[i];
        u32 tmp = (data & 0x02) >> 1;
        u32 tmp2 = data & 0x01;

        if (tmp == tmp2) {
            buf[0] += data;
        }  else {
            buf[0] = hashMulDiff(buf[0], data, i);
        }

        if(tmp2 == 1) {
            buf[1] ^= data;
        } else {
            buf[1] = hashMulDiff(buf[1], data, i);
        }
    }

    return buf[0] ^ buf[1];
}

void finalizeHigh_Step(inout uvec2 buf, u32 data, u32 i)
{
    buf.x += rotr(data, data & 0x1F);

    // branchless version is slightly faster
    u32 branchA = data < buf.x ? 1 : 0;
    u32 branchB = 1 - branchA;

    buf.y = (branchA * (buf.y + data))
          + (branchB * hashMulDiff(buf.y, data, i));
}

u32 finalizeHigh(in u32[16] state)
{
    uvec2 buf = uvec2(state[0]);

    finalizeHigh_Step(buf, state[0], 0);
    //if (buf[0] == 0)buf[1] = -buf[1]; // UNLIKELY, may cause false positives
    finalizeHigh_Step(buf, state[2], 2);
    finalizeHigh_Step(buf, state[3], 3);
    finalizeHigh_Step(buf, state[4], 4);
    finalizeHigh_Step(buf, state[5], 5);
    finalizeHigh_Step(buf, state[6], 6);
     /*if (buf[0] == 0) { // UNLIKELY, may cause false positives
      buf[1] = hashMulDiff_ANonZero(buf[1], 7);
      buf[1] = hashMulDiff_ANonZero(buf[1], 8);
    }*/
    finalizeHigh_Step(buf, state[9], 9);
    finalizeHigh_Step(buf, state[10], 10);
    finalizeHigh_Step(buf, state[11], 11);

    // state[12] is forced to be zero:
    // finalizeHigh_Step(buf, state[12], 12);
    // here would be the correct way to handle it with zero:
    /*if (buf[0] == 0) { // UNLIKELY, may cause false positives
      buf[1] = hashMulDiff_ANonZero(buf[1], 12);
    }*/

    finalizeHigh_Step(buf, state[13], 13);

    // skip last two steps, since they will reuslt in either zero
    // or create a situation where buf[1] wouldn't change
    /*if (buf[0] == 0) {
      buf[1] = hashMulDiff_ANonZero(buf[1], 14);
      buf[1] = hashMulDiff_ANonZero(buf[1], 15);
    }*/

    return hashMulDiff(buf.x, buf.y, 16) & 0xFFFF;
}

void checksumStep_1007_1008(inout u32[16] state, u32 data1007)
{
  // Step 1008: dataLast is always zero, data1007 is never zero
  state[0] += hashMulDiff_ANonZero(-1, data1007);
  state[2] ^= data1007;
  state[3] += hashMulDiff_ANonZero(data1007 + 5, MAGIC_NUMBER);

  state[4] += data1007;
  state[5] += data1007;

  // Branch-Version:
  if (data1007 < state[6]) {
    state[6] = (state[3] + state[6]) ^ (data1007 + 1008);
  } else {
    state[6] ^= (state[4] + data1007);
  }
/*
  // Branchless-Version: (slower?)
    u32 ifTrue = (data1007 < state[6]) ? 1 : 0;
    u32 ifFalse = 1 - ifTrue;

    state[6] = (ifTrue * ((state[3] + state[6]) ^ (data1007 + 1008)))
             + (ifFalse * (state[6] ^ (state[4] + data1007)));
*/
  state[9] = hashMulDiff_ANonZero(state[9], data1007);

  // Step 1007: data & dataLast is always zero, data1007 is never zero
  state[10] = hashMulDiff_ANonZero(state[10], data1007);
  state[11] = hashMulDiff_ANonZero(state[11], data1007);

  state[13] += rotr(data1007, data1007 & 0x1F);
}

void main()
{
  const u32 id = gl_GlobalInvocationID.x;

  // state is shared acrosss workers and even acrosss invocations, make copy
  u32 state[16];
  for(int i=0; i<16; ++i)state[i] = inputReadOnly[i];

  // last 2 steps in the checksum
  u32 data1007 = id + params.offset; // would be input[1007] on the CPU
  checksumStep_1007_1008(state, data1007);

  // finalize and write out checksums if it matches
  u32 high = finalizeHigh(state);
  if(high == 0x00008618)
  {
    u32 low = finalizeLow(state);
    //return true;
    //if((low & 0xFFFF) == 0xC2D3) // 16 bits
    //if((low & 0xFFFFFF) == 0x5BC2D3) // 24 bits
    //if((low & 0xFFFFFFF) == 0x45BC2D3) // 28 bits
    if(low == 0xA45BC2D3) // 32 bits (full)
    {
      found = data1007;
      checksums[0] = finalizeLow(state);
      checksums[1] = finalizeHigh(state);
    }
  }
}