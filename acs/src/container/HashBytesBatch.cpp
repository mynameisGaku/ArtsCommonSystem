// SPDX-License-Identifier: Apache-2.0
#include "container/HashBytesBatch.h"

namespace acs {

void HashBytesBatch(const FHashBytesInput* inputs, usize count, u64* output) noexcept {
    if (inputs == nullptr || output == nullptr) return;
    for (usize index = 0u; index < count; ++index) {
        output[index] = HashBytes(inputs[index].data, inputs[index].length, inputs[index].seed);
    }
}

void HashMix64Batch4(const u64 (&input)[4], u64 (&output)[4]) noexcept {
    // 独立して混合する四レーン。
    u64 lanes[4] = {input[0], input[1], input[2], input[3]};
    for (u32 lane = 0u; lane < 4u; ++lane) lanes[lane] ^= lanes[lane] >> 33;
    for (u32 lane = 0u; lane < 4u; ++lane) lanes[lane] *= 0xFF51AFD7ED558CCDull;
    for (u32 lane = 0u; lane < 4u; ++lane) lanes[lane] ^= lanes[lane] >> 33;
    for (u32 lane = 0u; lane < 4u; ++lane) lanes[lane] *= 0xC4CEB9FE1A85EC53ull;
    for (u32 lane = 0u; lane < 4u; ++lane) output[lane] = lanes[lane] ^ (lanes[lane] >> 33);
}

} // namespace acs
