// SPDX-License-Identifier: Apache-2.0
// 配布先だけをinclude/linkしてheaderと実装libraryの整合を確認する。
#include <acs.h>
#include <cstdio>

/** 配布SDKのheader、外部symbol、基本計算を検証し、失敗時は1を返す。 */
int main()
{
    using namespace acs;

    // containerの基本操作を検証する値。
    TArray<i32> v;
    v.PushBack(10);
    v.PushBack(32);
    // containerから得た合計値。
    i32 sum = 0;
    for (usize i = 0; i < v.Size(); ++i)
    {
        sum += v[i];
    }

    // 距離計算の始点。
    FVec2 a{0.0f, 0.0f};
    // 距離計算の終点。
    FVec2 b{3.0f, 4.0f};
    // 行列APIの生成結果。
    FMat4 m = FMat4::Identity();
    (void)m;

    // windowやGPUを使わない距離結果。
    const f32 dist = easy::Distance(a.x, a.y, b.x, b.y);
    // 範囲制限helperの結果。
    const f32 clamp = easy::Clamp(123.0f, 0.0f, 100.0f);
    // 二次元vector長の結果。
    const f32 len = easy::Length(b.x, b.y);

    // acs.libの非inline実装を必ずlinkさせる入力。
    constexpr char kHashProbe[] = "acs";
    // 現行HashBytes契約で固定した期待値。
    constexpr u64 kExpectedHash = 0x2773fad09b34e937ull;
    // header宣言と配布library実装を跨いだhash結果。
    const u64 linked_hash = HashBytes(kHashProbe, sizeof(kHashProbe) - 1u);

    std::printf("acs.h OK | sum=%d dist=%.1f clamp=%.1f len=%.1f hash=%016llx\n", sum, dist, clamp, len, static_cast<unsigned long long>(linked_hash));
    return (sum == 42 && dist == 5.0f && clamp == 100.0f && len == 5.0f && linked_hash == kExpectedHash) ? 0 : 1;
}
