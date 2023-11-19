/**
 * @author Max Bebök
 * @license MIT
 */

#include <bit>
#include <chrono>
#include <thread>

#include <vuh/array.hpp>
#include <vuh/vuh.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

namespace
{
  constexpr u32 MAGIC_NUMBER        = 0x6c078965;
  constexpr u32 BOOTCODE_OFFSET     = 0x40;
  constexpr u32 BOOTCODE_SIZE       = 0x1000;
  constexpr u32 BOOTCODE_SIZE_WORDS = BOOTCODE_SIZE / 4;
  // Note: the checksum is located in the GLSL shader

  struct CheckSumState {
    u32 buffer[16]{0};
    u32 *input{nullptr};
  };
}

using namespace std::chrono;

void printChecksumBuffer(CheckSumState &state)
{
   printf("BUFF: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
      state.buffer[0],  state.buffer[1],  state.buffer[2],  state.buffer[3],
      state.buffer[4],  state.buffer[5],  state.buffer[6],  state.buffer[7],
      state.buffer[8],  state.buffer[9],  state.buffer[10], state.buffer[11],
      state.buffer[12], state.buffer[13], state.buffer[14], state.buffer[15]
    );
}

void checksumInit(CheckSumState &state, u32 seed)
{
    u32 init = MAGIC_NUMBER * (seed & 0xff) + 1;
    u32 data = state.input[0];

    init ^= data;

    for(u32 &loop : state.buffer) {
      loop = init;
    }
}

constexpr u32 hashMulDiff(u32 factorBase, u32 factorA, u32 factorB)
{
    // for factorBase == 0, always returns zero!
    if(factorA == 0)factorA = factorB;

    u64 prod = (u64)factorBase * (u64)factorA;
    u32 diff = (u32)(prod >> 32) - (u32)prod;

    return (diff == 0) ? factorBase : diff;
}

// The last 2 steps are done on the GPU, the first half of step 1007 can be done
// on the CPU since it is independent of the input data.
void checksumCalculateStep_DataAndLastIsZero_1007_Indep(CheckSumState &state)
{
  state.buffer[3] += hashMulDiff(5, MAGIC_NUMBER, 1007);

  if (0 < state.buffer[6]) {
    state.buffer[6] = (state.buffer[3] + state.buffer[6]) ^ (1007);
  } else {
    state.buffer[6] = (state.buffer[4]) ^ state.buffer[6];
  }
}

// Generic form of a single checksum loop, the above functions are special cases.
// This assumes a zero init state for state.buffer, and removed unused code
void checksumCalculateStep(CheckSumState &state, u32 loop)
{
  u32 dataLast = state.input[loop == 1 ? 0 : loop - 2];
  u32 data     = state.input[loop - 1];

  state.buffer[0] += hashMulDiff(1007 - loop, data, loop);
  state.buffer[2] ^= data;
  state.buffer[3] += hashMulDiff(data + 5, MAGIC_NUMBER, loop);
  state.buffer[4] += std::rotr(data, dataLast & 0x1F);
  state.buffer[5] += std::rotl(data, dataLast >> 27);

  if (data < state.buffer[6]) {
    state.buffer[6] = (state.buffer[3] + state.buffer[6]) ^ (data + loop);
  } else {
    state.buffer[6] = (state.buffer[4] + data) ^ state.buffer[6];
  }

  if(dataLast < data)
  {
    state.buffer[9] = hashMulDiff(state.buffer[9], data, loop);
  } else {
    state.buffer[9] += data;
  }

  if (loop == 1008)return;

  u32 dataNext = state.input[loop];
  state.buffer[10] = hashMulDiff(state.buffer[10] + data, dataNext, loop);
  state.buffer[11] = hashMulDiff(state.buffer[11] ^ data, dataNext, loop);
  state.buffer[12] += data;

  state.buffer[13] += std::rotr(data, data & 0x1F)
                   + std::rotr(dataNext, dataNext & 0x1F);
}

void checksumCalculate(CheckSumState &state, u32 loopEnd = 1008)
{
    // Final state.input[] values (given an all-zero init state):
    //   [00]   : ???
    //   [01]   : <ZERO>
    //   [02]   : XOR of all input u32's from 0 <= i <= 4028
    //   [03-06]: ???
    //   [07]   : <ZERO>
    //   [08]   : <ZERO>
    //   [09-11]: ???
    //   [12]   : sum of all input u32's from 0 <= i <= 4028
    //   [13]   : ???
    //   [14]   : <ZERO>
    //   [15]   : <ZERO>
    for(u32 loop = 1; loop <= loopEnd; ++loop) {
      checksumCalculateStep(state, loop);
    }
}

// Write out the bootcode to a file, byte-swapped to BE
void writeOutBuffer(u32 (&bootcode)[BOOTCODE_SIZE_WORDS])
{
  u32 bootcodeBE[BOOTCODE_SIZE_WORDS];
  for(u32 i=0; i<BOOTCODE_SIZE_WORDS; ++i) {
    bootcodeBE[i] = std::byteswap(bootcode[i]);
  }
  auto f = fopen("match.z64", "wb");
  fwrite(bootcodeBE, 1, BOOTCODE_SIZE, f);
  fclose(f);
}

// Main bruteforce loop, this will try to find a hash and then writes out a file.
void bruteforceFile(const char *filename, u32 seed = 0x9191, u32 startLoop = 0)
{
  CheckSumState state{};

  auto *fp = fopen(filename, "rb");
  if(!fp) {
    printf("Error opening file '%s'!\n", filename);
    return;
  }

  u32 bootcode[BOOTCODE_SIZE_WORDS];
  if(fread(bootcode, 1, BOOTCODE_SIZE, fp) != BOOTCODE_SIZE) {
    printf("Error reading file, too small!\n");
    fclose(fp);
    return;
  }
  fclose(fp);

  // byte-swap entire bootcode once
  for(u32 &x : bootcode) {
    x = std::byteswap(x);
  }

  // GPU init
  auto instance = vuh::Instance();
  auto device = instance.devices().at(0);

  constexpr u32 GPU_BATCH_COUNT = 128; // 99% GPU
  constexpr u32 GPU_GROUP_SPLIT = 512;

  //constexpr u32 GPU_BATCH_COUNT = 2; // DEBUG
  //constexpr u32 GPU_GROUP_SPLIT = 2;

  constexpr u32 GPU_GROUP_SIZE = GPU_GROUP_SPLIT * GPU_GROUP_SPLIT;

  // effectively the increment of the value in input[1007], used for the last checksum-step
  constexpr u64 GPU_STEP_SIZE = GPU_GROUP_SIZE * GPU_BATCH_COUNT;

  auto buffOut = std::vector<u64>(1, 0); // contains checksum (only for logging / verification)
  auto buffIn = std::vector<u32>(16, 0); // input data
  auto buffRes = std::vector<u32>(1, 0); // single flag to signal a matching checksum

  auto buffOutGPU = vuh::Array<u64>(device, buffOut);
  auto buffInGPU = vuh::Array<u32>(device, buffIn);
  auto buffResGPU = vuh::Array<u32>(device, buffRes);

  printf("==== VULKAN ====\n");
  printf("Device: %s\n", device.properties().deviceName.data());
  printf("BATCH_COUNT: %d\n", GPU_BATCH_COUNT);
  printf("GROUP_SIZE: %d\n", GPU_GROUP_SIZE);
  printf("GPU_STEP_SIZE: 0x%08lX\n", GPU_STEP_SIZE);
  printf("================\n");

  using Specs = vuh::typelist<uint32_t>;
  struct GPUPushConstants {u32 groupSize; u32 offset;};
  auto program = vuh::Program<Specs, GPUPushConstants>(device, "shader/shader.spv");
  program.grid(GPU_GROUP_SIZE);
  program.spec(GPU_BATCH_COUNT);

  // Start actual brute-forcing:

  // Counteract the initial seed
  // This forces state.input[] 1, 7, 8, 14 and 15 to be, and stay zero.
  state.input = &bootcode[BOOTCODE_OFFSET / 4];
  state.input[0] = MAGIC_NUMBER * (seed & 0xff) + 1;

  auto timeStart = high_resolution_clock::now();
  u64 totalValuesChecked = 0;

  checksumInit(state, seed);
  auto startingState = state;

  for(u32 inp1000=0; inp1000<0xFFFF'FFFF; ++inp1000)
  {
    state = startingState;
     // can be used to re-shuffle values when compensating doesn't work
     // (currently unused and always zero)
    state.input[1000] = inp1000;
    state.input[1001] = 0;
    state.input[1002] = 0;

    checksumCalculate(state, 1002);
    auto oldState = state;

    for(u32 y=startLoop; y < 0xFFFF'FFFF; ++y)
    {
      state = oldState;

      u32 buffer12Compensate = -state.buffer[12] - y;

      state.input[1003] = buffer12Compensate; // forces buffer[12] to be zero
      state.input[1004] = y; // must NOT be zero
      state.input[1005] = 0; // must be zero!
      state.input[1006] = 0; // must be zero!

      //                           input[x]: next, data, last
      checksumCalculateStep(state, 1003); // 1003, 1002, 1001
      checksumCalculateStep(state, 1004); // 1004, 1003, 1002
      checksumCalculateStep(state, 1005); // 1005, 1004, 1003
      checksumCalculateStep(state, 1006); // 1006, 1005, 1004

      assert(state.buffer[12] == 0);

      checksumCalculateStep_DataAndLastIsZero_1007_Indep(state); // ----, 1006, 1005

      // state -> GPU
      buffInGPU.fromHost(state.buffer, state.buffer + 16);

      GPUPushConstants pushConst{GPU_GROUP_SIZE, 1};
      program.bind(pushConst, buffOutGPU, buffInGPU, buffResGPU);

      // checks the entire 1 - 0xFFFF'FFFF range, zero must be ignored
      for(pushConst.offset = 1;;)
      {
        program.bind(pushConst);
        program.run();

        /*buffOutGPU.toHost(buffOut.begin());
        printf("GPU[%08X]: ", pushConst.offset);
        for(auto x : buffOut)printf(" %016lX", x);
        printf("\n");*/

        u64 newOffset = pushConst.offset + GPU_STEP_SIZE;
        if(newOffset > 0xFFFF'FFFF) {
          //printf("Overshoot: %08X\n", newOffset - 0xFFFF'FFFF);
          break; // stop on overflow
        }

        pushConst.offset = newOffset;
      }

      totalValuesChecked += 0xFFFF'FFFF;

      if(y % 4 == 0)
      {
        auto timeEnd = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(timeEnd - timeStart);
        timeStart = timeEnd;
        printf("Y: 0x%08X (+%d) | Time: %ldms (total: %ld GHashes)\n", y, y-startLoop, duration.count() / 1000,
            totalValuesChecked / 1000'000'000
        );
      }

      // To minimize transfers, only check results after a full loop.
      // The success-flag is sticky and will persist.
      {
        buffResGPU.toHost(buffRes.begin());
        u32 matchData1007 = buffRes[0]; // returns input[1007] that matched, zero if no match
        if(matchData1007 > 0) {
          printf("Found Result: %08X\n", matchData1007);
          buffOutGPU.toHost(buffOut.begin()); // only needed for debugging

          printf("Found checksum: (%08X, %08X) %016lX !!!!\n", y, matchData1007, buffOut[0]);
          state.input[1007] = matchData1007;
          writeOutBuffer(bootcode);
          return;
        }
      }
    }
  }
}

int main(int argc, char* argv[])
{
  if(argc != 3) {
    printf("Usage: ipl_gpu [rom] [seed]\n");
    printf("  Example: ipl_gpu ipl3_prod.z64 9191\n");
    return -1;
  }

  srand(time(nullptr));
  u32 seed = strtol(argv[2], nullptr, 16);
  u32 loopStart = rand() % 0xFFF'FFFF;
  //u32 loopStart = 0;

  auto timeStart = high_resolution_clock::now();

  bruteforceFile(argv[1], seed, loopStart);

  auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - timeStart);
  printf("Total-Time: %lds\n", duration.count() / 1000);
}
