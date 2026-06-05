// SPDX-License-Identifier: Apache-2.0
// Standalone consumer test: includes ONLY <acs.h> (no ACS source tree on the
// include path) and links ONLY acs.lib (auto-linked via pragma). Proves the
// single-header + single-lib distribution is self-contained.
#include <acs.h>
#include <cstdio>

int main() {
    using namespace acs;

    // --- container module ---
    TArray<i32> v;
    v.PushBack(10);
    v.PushBack(32);
    i32 sum = 0;
    for (usize i = 0; i < v.Size(); ++i) sum += v[i];

    // --- math module ---
    FVec2 a{0.0f, 0.0f};
    FVec2 b{3.0f, 4.0f};
    FMat4 m = FMat4::Identity();
    (void)m;

    // --- easy module (pure helpers; no window/GPU needed) ---
    f32 dist  = easy::Distance(a.x, a.y, b.x, b.y);
    f32 clamp = easy::Clamp(123.0f, 0.0f, 100.0f);
    f32 len   = easy::Length(b.x, b.y);

    std::printf("acs.h OK | sum=%d dist=%.1f clamp=%.1f len=%.1f\n",
                sum, dist, clamp, len);
    return (sum == 42 && dist == 5.0f && clamp == 100.0f && len == 5.0f) ? 0 : 1;
}
