// SPDX-License-Identifier: Apache-2.0
// 配布先だけをinclude/linkしてheaderと実装libraryの整合を確認する。
#include <acs.h>
#include <cstdio>

static_assert(sizeof(acs::FTimerHandle) == 8u, "event タイマーハンドルは 8byte の独立型です");
static_assert(sizeof(acs::game::FSceneTimerHandle) == 4u, "シーンタイマーハンドルは 4byte の packed 型です");
static_assert(sizeof(acs::FLogSinkHandle) == 8u, "ログ購読ハンドルは枠番号と世代番号の 8byte 型です");

/**
 * 配布ライブラリを経由したシーンタイマー発火を記録する。
 *
 * @param user acs::u32 のアドレス。
 */
void CountSceneTimerFire(void* user) noexcept
{
    /** 呼び出し側が所有する発火回数。 */
    auto& fire_count = *static_cast<acs::u32*>(user);
    ++fire_count;
}

/**
 * 配布ライブラリを経由したログ購読通知を記録する。
 *
 * @param severity 通知されたログ重大度。
 * @param message 通知されたnull終端本文。
 * @param user acs::u32 のアドレス。
 */
void CountLogSink(acs::ELogSeverity severity, const char* message, void* user) noexcept
{
    /** 呼び出し側が所有する通知回数。 */
    auto& notification_count = *static_cast<acs::u32*>(user);
    if (severity == acs::ELogSeverity::Info && message != nullptr) ++notification_count;
}

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

    // 呼び出し側が所有するシーンタイマー。
    game::FSceneTimer scene_timer;
    // シーンタイマーの発火回数。
    u32 scene_timer_fire_count = 0u;
    // 配布ライブラリの新しい修飾シンボルを参照する正規ハンドル。
    const game::FSceneTimerHandle scene_timer_handle = scene_timer.SetTimeout(1.0f, &CountSceneTimerFire, &scene_timer_fire_count);
    scene_timer.Tick(1.0f);
    // 登録、発火、完了をまとめて確認する結果。
    const bool scene_timer_ok = scene_timer_handle.IsValid() && scene_timer_fire_count == 1u && !scene_timer.IsActive(scene_timer_handle);

    // 配布ライブラリの複数ログ通知先を画面出力なしで検証する設定。
    FLogConfig log_config{};
    log_config.console = false;
    log_config.debug_output = false;
    FLogger::Init(log_config);
    // ログ購読から受け取った通知回数。
    u32 log_notification_count = 0u;
    FLogSinkSubscription log_subscription = FLogger::SubscribeSinkOwned(&CountLogSink, &log_notification_count);
    ACS_LOG_INFO("distribution log sink");
    FLogger::Flush();
    // 購読の有効性と通知到達をまとめて確認する結果。
    const bool log_sink_ok = log_subscription.IsValid() && log_notification_count == 1u;
    FLogger::Shutdown();

    std::printf("acs.h OK | sum=%d dist=%.1f clamp=%.1f len=%.1f hash=%016llx scene_timer=%u log_sink=%u\n", sum, dist, clamp, len, static_cast<unsigned long long>(linked_hash), scene_timer_fire_count, log_notification_count);
    return (sum == 42 && dist == 5.0f && clamp == 100.0f && len == 5.0f && linked_hash == kExpectedHash && scene_timer_ok && log_sink_ok) ? 0 : 1;
}
