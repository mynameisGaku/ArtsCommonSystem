// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"
#include "event/Timer.h"
#include "foundation/TypeTraits.h"
#include "gameframework/SceneTimer.h"
#include "gameframework/SceneTimerHandle.h"

#include <cstddef>
#include <cstring>
using namespace acs;

static_assert(sizeof(game::FSceneTimerHandle) == 4u, "シーンタイマーハンドルは 4byte である必要があります");
static_assert(alignof(game::FSceneTimerHandle) == alignof(u32), "シーンタイマーハンドルの配置が変わりました");
static_assert(offsetof(game::FSceneTimerHandle, m_Packed) == 0u, "packed 値は先頭にある必要があります");
static_assert(__is_standard_layout(game::FSceneTimerHandle), "シーンタイマーハンドルは標準レイアウトである必要があります");
static_assert(IsTriviallyCopyableV<game::FSceneTimerHandle>, "シーンタイマーハンドルはバイト復元可能である必要があります");
static_assert(sizeof(game::CSceneTimer) == 40u, "シーンタイマー本体の公開配置が変わりました");
static_assert(sizeof(FTimerHandle) == 8u, "event タイマーハンドルは独立した 8byte 型です");
static_assert(!IsSameV<FTimerHandle, game::FSceneTimerHandle>, "event とシーンのハンドルを統合してはいけません");
static_assert(IsSameV<game::TimerCallback, void(*)(void*) noexcept>, "シーンタイマーのコールバック契約が変わりました");
static_assert(IsSameV<decltype(static_cast<game::CSceneTimer*>(nullptr)->SetTimeout(1.0f, nullptr, nullptr)), game::FSceneTimerHandle>, "SetTimeout は正規シーンタイマーハンドルを返す必要があります");
static_assert(IsSameV<decltype(static_cast<game::CSceneTimer*>(nullptr)->SetInterval(1.0f, nullptr, nullptr)), game::FSceneTimerHandle>, "SetInterval は正規シーンタイマーハンドルを返す必要があります");
static_assert(noexcept(game::FSceneTimerHandle::Pack(0u, 1u)), "Pack は noexcept である必要があります");
static_assert(noexcept(game::FSceneTimerHandle{}.IsValid()), "IsValid は noexcept である必要があります");
static_assert(noexcept(game::FSceneTimerHandle{}.Index()), "Index は noexcept である必要があります");
static_assert(noexcept(game::FSceneTimerHandle{}.Gen()), "Gen は noexcept である必要があります");
static_assert(noexcept(static_cast<game::CSceneTimer*>(nullptr)->SetTimeout(1.0f, nullptr, nullptr)), "SetTimeout は noexcept である必要があります");
static_assert(noexcept(static_cast<game::CSceneTimer*>(nullptr)->SetInterval(1.0f, nullptr, nullptr)), "SetInterval は noexcept である必要があります");
static_assert(noexcept(static_cast<game::CSceneTimer*>(nullptr)->Cancel(game::FSceneTimerHandle{})), "Cancel は noexcept である必要があります");
static_assert(noexcept(static_cast<game::CSceneTimer*>(nullptr)->CancelAll()), "CancelAll は noexcept である必要があります");
static_assert(noexcept(static_cast<const game::CSceneTimer*>(nullptr)->IsActive(game::FSceneTimerHandle{})), "IsActive は noexcept である必要があります");
static_assert(noexcept(static_cast<const game::CSceneTimer*>(nullptr)->ActiveCount()), "ActiveCount は noexcept である必要があります");
static_assert(noexcept(static_cast<game::CSceneTimer*>(nullptr)->Tick(0.0f)), "Tick は noexcept である必要があります");

namespace {

/** シーンタイマーの発火順を固定長で記録する。 */
struct FSceneTimerTrace {
    /** 発火種別を登録順に保持する領域。 */
    u32 values[8]{};

    /** 記録済みの発火数。 */
    u32 count = 0u;
};

/**
 * 発火種別を追跡領域へ追加する。
 *
 * @param trace 更新する追跡領域。
 * @param value 記録する発火種別。
 */
void AppendTrace(FSceneTimerTrace& trace, u32 value) noexcept
{
    if (trace.count < 8u) {
        trace.values[trace.count] = value;
        ++trace.count;
    }
}

/**
 * 周期タイマーの発火を記録する。
 *
 * @param user FSceneTimerTrace のアドレス。
 */
void RecordInterval(void* user) noexcept
{
    /** 呼び出し側が所有する追跡領域。 */
    auto& trace = *static_cast<FSceneTimerTrace*>(user);
    AppendTrace(trace, 1u);
}

/**
 * 一度きりタイマーの発火を記録する。
 *
 * @param user FSceneTimerTrace のアドレス。
 */
void RecordTimeout(void* user) noexcept
{
    /** 呼び出し側が所有する追跡領域。 */
    auto& trace = *static_cast<FSceneTimerTrace*>(user);
    AppendTrace(trace, 2u);
}

} // namespace

ACS_TEST(SceneTimerHandle, PackedBoundaryValuesStayStable)
{
    /** 生の 0 値から復元した無効ハンドル。 */
    const game::FSceneTimerHandle raw_zero{0u};
    EXPECT_EQ(raw_zero.m_Packed, 0u);
    EXPECT_FALSE(raw_zero.IsValid());

    /** 先頭スロットと最初の世代番号を詰めたハンドル。 */
    const game::FSceneTimerHandle first = game::FSceneTimerHandle::Pack(0u, 1u);
    EXPECT_EQ(first.m_Packed, 0x01000000u);
    EXPECT_EQ(first.Index(), 0u);
    EXPECT_EQ(first.Gen(), static_cast<u8>(1u));

    /** 全ビットを 1 にする境界値ハンドル。 */
    const game::FSceneTimerHandle packed_max = game::FSceneTimerHandle::Pack(game::FSceneTimerHandle::kMaxIndex, static_cast<u8>(255u));
    EXPECT_EQ(packed_max.m_Packed, 0xFFFFFFFFu);
    EXPECT_EQ(packed_max.Index(), game::FSceneTimerHandle::kMaxIndex);
    EXPECT_EQ(packed_max.Gen(), static_cast<u8>(255u));

    /** 管理側が実際に発行できる最後のスロット番号。 */
    constexpr u32 kLastIssuedIndex = 0x00FFFFFEu;
    /** 最後に発行可能なスロットと世代番号を詰めたハンドル。 */
    const game::FSceneTimerHandle last_issued = game::FSceneTimerHandle::Pack(kLastIssuedIndex, static_cast<u8>(255u));
    EXPECT_EQ(last_issued.m_Packed, 0xFFFFFFFEu);
    EXPECT_EQ(last_issued.Index(), kLastIssuedIndex);
    EXPECT_EQ(last_issued.Gen(), static_cast<u8>(255u));
}

ACS_TEST(SceneTimerHandle, PackedGoldenBytesRoundTrip)
{
    /** 下位 24bit に収める既知のスロット番号。 */
    constexpr u32 kIndex = 0x00123456u;
    /** 上位 8bit に収める既知の世代番号。 */
    constexpr u8 kGeneration = 0xABu;
    /** Windows x64 の little endian 表現で期待する packed 値。 */
    constexpr u32 kExpectedPacked = 0xAB123456u;

    /** 既知値から組み立てた正規ハンドル。 */
    const game::FSceneTimerHandle handle = game::FSceneTimerHandle::Pack(kIndex, kGeneration);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(handle.m_Packed, kExpectedPacked);
    EXPECT_EQ(handle.Index(), kIndex);
    EXPECT_EQ(handle.Gen(), kGeneration);

    /** 永続化境界を模した 4byte の格納領域。 */
    u8 bytes[sizeof(game::FSceneTimerHandle)]{};
    std::memcpy(bytes, &handle, sizeof(handle));
    EXPECT_EQ(bytes[0], static_cast<u8>(0x56u));
    EXPECT_EQ(bytes[1], static_cast<u8>(0x34u));
    EXPECT_EQ(bytes[2], static_cast<u8>(0x12u));
    EXPECT_EQ(bytes[3], static_cast<u8>(0xABu));

    /** 格納済み 4byte から復元するハンドル。 */
    game::FSceneTimerHandle restored{};
    std::memcpy(&restored, bytes, sizeof(restored));
    EXPECT_EQ(restored.m_Packed, kExpectedPacked);
    EXPECT_EQ(restored.Index(), kIndex);
    EXPECT_EQ(restored.Gen(), kGeneration);
}

ACS_TEST(SceneTimerHandle, ExistingSceneTimerTraceRemainsStable)
{
    /** 呼び出し側が所有して毎フレーム進めるシーンタイマー。 */
    game::CSceneTimer timer;
    /** 発火順を観測する追跡領域。 */
    FSceneTimerTrace trace{};

    /** 1 秒周期で発火するタイマー。 */
    const game::FSceneTimerHandle interval = timer.SetInterval(1.0f, &RecordInterval, &trace);
    /** 1.5 秒後に一度だけ発火するタイマー。 */
    const game::FSceneTimerHandle timeout = timer.SetTimeout(1.5f, &RecordTimeout, &trace);
    EXPECT_TRUE(interval.IsValid());
    EXPECT_TRUE(timeout.IsValid());
    EXPECT_EQ(timer.ActiveCount(), 2u);

    timer.Tick(0.5f);
    EXPECT_EQ(trace.count, 0u);
    timer.Tick(0.5f);
    EXPECT_EQ(trace.count, 1u);
    timer.Tick(0.5f);
    EXPECT_EQ(trace.count, 2u);
    timer.Tick(1.5f);

    EXPECT_EQ(trace.count, 4u);
    EXPECT_EQ(trace.values[0], 1u);
    EXPECT_EQ(trace.values[1], 2u);
    EXPECT_EQ(trace.values[2], 1u);
    EXPECT_EQ(trace.values[3], 1u);
    EXPECT_FALSE(timer.IsActive(timeout));
    EXPECT_TRUE(timer.IsActive(interval));
    EXPECT_TRUE(timer.Cancel(interval));
    EXPECT_FALSE(timer.IsActive(interval));
    EXPECT_EQ(timer.ActiveCount(), 0u);

    timer.Tick(3.0f);
    EXPECT_EQ(trace.count, 4u);
}

ACS_TEST(SceneTimerHandle, SceneAndEventZeroDelayPoliciesStayDistinct)
{
    /** 0 秒登録を拒否するシーン寿命のタイマー。 */
    game::CSceneTimer scene_timer;
    /** 発火回数を記録する追跡領域。 */
    FSceneTimerTrace trace{};
    /** シーンタイマーが 0 秒登録を拒否した結果。 */
    const game::FSceneTimerHandle scene_zero = scene_timer.SetTimeout(0.0f, &RecordTimeout, &trace);
    EXPECT_FALSE(scene_zero.IsValid());
    EXPECT_EQ(scene_timer.ActiveCount(), 0u);

    /** 0 秒登録を受理する event 寿命のタイマー。 */
    CTimerManager event_timer;
    /** event タイマーが発行した独立ハンドル。 */
    const FTimerHandle event_zero = event_timer.SetTimeout(0.0f, &RecordTimeout, &trace);
    EXPECT_TRUE(event_zero.IsValid());
    EXPECT_EQ(event_timer.ActiveCount(), 1u);
    event_timer.Tick(0.0f);
    EXPECT_EQ(trace.count, 1u);
    EXPECT_EQ(event_timer.ActiveCount(), 0u);
}
