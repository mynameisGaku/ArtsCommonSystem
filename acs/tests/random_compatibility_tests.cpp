// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "gameframework/Random.h"
#include "gameframework/RandomSnapshot.h"

#include <cstddef>
#include <limits>
#include <type_traits>

using namespace acs;

namespace {

/**
 * 二つのsnapshotが同じ乱数位置を表すことを検査する。
 *
 * @param actual 操作後の値。
 * @param expected 操作前の値。
 */
void ExpectSameSnapshot(const game::FRandomSnapshot& actual, const game::FRandomSnapshot& expected) {
    EXPECT_EQ(actual.version, expected.version);
    EXPECT_EQ(actual.state0, expected.state0);
    EXPECT_EQ(actual.state1, expected.state1);
    EXPECT_EQ(actual.state2, expected.state2);
    EXPECT_EQ(actual.state3, expected.state3);
    EXPECT_EQ(actual.reserved, expected.reserved);
    EXPECT_EQ(actual.signature, expected.signature);
}

} // namespace

static_assert(sizeof(game::FRandom) == 16u, "FRandomの公開layoutを維持してください");
static_assert(alignof(game::FRandom) == 4u, "FRandomの公開alignmentを維持してください");
static_assert(sizeof(game::FRandomSnapshot) == 32u, "FRandomSnapshotは32byte契約です");
static_assert(alignof(game::FRandomSnapshot) == 8u, "FRandomSnapshotは8byte alignment契約です");
static_assert(std::is_standard_layout_v<game::FRandomSnapshot>, "FRandomSnapshotは標準layout値型です");
static_assert(std::is_trivially_copyable_v<game::FRandomSnapshot>, "FRandomSnapshotは単純copy可能な値型です");
static_assert(offsetof(game::FRandomSnapshot, version) == 0u, "version offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, state0) == 4u, "state0 offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, state1) == 8u, "state1 offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, state2) == 12u, "state2 offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, state3) == 16u, "state3 offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, reserved) == 20u, "reserved offsetを固定します");
static_assert(offsetof(game::FRandomSnapshot, signature) == 24u, "signature offsetを固定します");

ACS_TEST(RandomCompatibility, RawSequenceRemainsStable)
{
    /** seed 0の既存列を検査する乱数器。 */
    game::FRandom zero{ 0u };
    EXPECT_EQ(zero.NextU32(), 0xDEC9045Du);
    EXPECT_EQ(zero.NextU32(), 0x9A089D75u);
    EXPECT_EQ(zero.NextU32(), 0xAB77D362u);

    /** seed 42の既存列を検査する乱数器。 */
    game::FRandom forty_two{ 42u };
    EXPECT_EQ(forty_two.NextU32(), 0x69E85A2Au);
    EXPECT_EQ(forty_two.NextU32(), 0xF843FAD0u);
    EXPECT_EQ(forty_two.NextU32(), 0x0105185Fu);

    /** 64bit seedの既存列を検査する乱数器。 */
    game::FRandom wide{ 0x123456789ABCDEF0ULL };
    EXPECT_EQ(wide.NextU32(), 0x358E68EFu);
    EXPECT_EQ(wide.NextU32(), 0xB3D15E4Eu);
    EXPECT_EQ(wide.NextU32(), 0x4ECE3EBBu);
}

ACS_TEST(RandomCompatibility, SnapshotLayoutAndSignatureRemainStable)
{
    /** 固定seedの初期状態を取得する乱数器。 */
    game::FRandom random{ 42u };
    /** raw stateと標準FNV-1a検査値を含むsnapshot。 */
    const game::FRandomSnapshot snapshot = random.CaptureSnapshot();

    EXPECT_EQ(snapshot.version, 1u);
    EXPECT_EQ(snapshot.state0, 0x2FEB6E95u);
    EXPECT_EQ(snapshot.state1, 0xBDD73226u);
    EXPECT_EQ(snapshot.state2, 0xB266F103u);
    EXPECT_EQ(snapshot.state3, 0x28EFE333u);
    EXPECT_EQ(snapshot.reserved, 0u);
    EXPECT_EQ(snapshot.signature, 0x5E8E647A6E9A4318ULL);
}

ACS_TEST(RandomCompatibility, SnapshotRestoresInConstantWork)
{
    /** 復元前後の列を比較する乱数器。 */
    game::FRandom random{ 0x123456789ABCDEF0ULL };
    (void)random.NextU32();
    /** 復元先となる再生位置。 */
    const game::FRandomSnapshot snapshot = random.CaptureSnapshot();
    /** snapshot直後の第1値。 */
    const u32 expected_first = random.NextU32();
    /** snapshot直後の第2値。 */
    const u32 expected_second = random.NextU32();

    EXPECT_TRUE(random.TryRestoreSnapshot(snapshot));
    EXPECT_EQ(random.NextU32(), expected_first);
    EXPECT_EQ(random.NextU32(), expected_second);
}

ACS_TEST(RandomCompatibility, SnapshotRejectsInvalidFieldsWithoutMutation)
{
    /** 不正snapshotを拒否する乱数器。 */
    game::FRandom random{ 42u };
    /** 失敗時不変性の基準状態。 */
    const game::FRandomSnapshot before = random.CaptureSnapshot();

    /** 版だけを破損したsnapshot。 */
    game::FRandomSnapshot damaged = before;
    damaged.version = 2u;
    damaged.signature = 0x870AB48A5C4460A7ULL;
    EXPECT_FALSE(random.TryRestoreSnapshot(damaged));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    damaged = before;
    damaged.reserved = 1u;
    damaged.signature = 0xFE891082C4CFFFA9ULL;
    EXPECT_FALSE(random.TryRestoreSnapshot(damaged));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    damaged = before;
    damaged.state0 = 0u;
    damaged.state1 = 0u;
    damaged.state2 = 0u;
    damaged.state3 = 0u;
    damaged.signature = 0x5B2A969B42D238A4ULL;
    EXPECT_FALSE(random.TryRestoreSnapshot(damaged));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    damaged = before;
    damaged.signature ^= 1u;
    EXPECT_FALSE(random.TryRestoreSnapshot(damaged));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);
}

ACS_TEST(RandomCompatibility, DiscardHonorsBoundWithoutPartialConsumption)
{
    /** 一括消費する乱数器。 */
    game::FRandom discarded{ 99u };
    /** 逐次消費する比較用乱数器。 */
    game::FRandom repeated{ 99u };
    EXPECT_TRUE(discarded.TryDiscard(1024u));
    for (u32 index = 0u; index < 1024u; ++index) (void)repeated.NextU32();
    EXPECT_EQ(discarded.NextU32(), repeated.NextU32());

    /** 0回消費前の状態。 */
    const game::FRandomSnapshot before_zero = discarded.CaptureSnapshot();
    EXPECT_TRUE(discarded.TryDiscard(0u));
    ExpectSameSnapshot(discarded.CaptureSnapshot(), before_zero);

    /** 上限超過拒否前の状態。 */
    const game::FRandomSnapshot before_failure = discarded.CaptureSnapshot();
    EXPECT_FALSE(discarded.TryDiscard(1048577u));
    ExpectSameSnapshot(discarded.CaptureSnapshot(), before_failure);

    /** 公開上限ちょうどを受け付ける乱数器。 */
    game::FRandom maximum{ 7u };
    EXPECT_TRUE(maximum.TryDiscard(1048576u));
}

ACS_TEST(RandomCompatibility, ExistingRangeIntKeepsOneDrawAndDefinedConversion)
{
    /** 非対称な広い範囲を既存方式で生成する乱数器。 */
    game::FRandom broad{ 42u };
    EXPECT_EQ(broad.RangeInt(-1, 2147483647), 1776835113);
    EXPECT_EQ(broad.NextU32(), 0xF843FAD0u);

    /** i32全域を既存方式で生成する乱数器。 */
    game::FRandom full{ 42u };
    EXPECT_EQ(full.RangeInt(-2147483647 - 1, 2147483647), 1776835114);
    EXPECT_EQ(full.RangeInt(-2147483647 - 1, 2147483647), -129762608);
    EXPECT_EQ(full.NextU32(), 0x0105185Fu);

    /** 交換した範囲でも1回消費を確認する乱数器。 */
    game::FRandom reversed{ 42u };
    EXPECT_EQ(reversed.RangeInt(6, 1), 5);
    EXPECT_EQ(reversed.NextU32(), 0xF843FAD0u);
}

ACS_TEST(RandomCompatibility, WeightedIndexUsesValidatedHighThenLowDraws)
{
    /** 正常な抽選に使う重み。 */
    const f32 weights[] = { 1.0f, 2.0f, 3.0f };
    /** 53bit抽選を行う乱数器。 */
    game::FRandom random{ 42u };
    /** 成功時だけ更新される選択結果。 */
    u32 selected = 77u;
    EXPECT_TRUE(random.TryWeightedIndex(weights, 3u, selected));
    EXPECT_EQ(selected, 1u);
    EXPECT_EQ(random.NextU32(), 0x0105185Fu);

    /** 0重みを選択対象外として確認する配列。 */
    const f32 sparse_weights[] = { 0.0f, 0.0f, 4.0f };
    /** 0重みを許可する抽選器。 */
    game::FRandom sparse{ 42u };
    selected = 99u;
    EXPECT_TRUE(sparse.TryWeightedIndex(sparse_weights, 3u, selected));
    EXPECT_EQ(selected, 2u);

    /** 加算overflowを避ける最大有限重み。 */
    const f32 maximum_weights[] = { std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max() };
    /** 最大値正規化を検査する抽選器。 */
    game::FRandom maximum{ 42u };
    selected = 99u;
    EXPECT_TRUE(maximum.TryWeightedIndex(maximum_weights, 2u, selected));
    EXPECT_EQ(selected, 0u);
    EXPECT_EQ(maximum.NextU32(), 0x0105185Fu);
}

ACS_TEST(RandomCompatibility, WeightedIndexRejectsAllInvalidInputsWithoutMutation)
{
    /** 失敗経路を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 失敗前の再生位置。 */
    const game::FRandomSnapshot before = random.CaptureSnapshot();
    /** 失敗時に維持される出力。 */
    u32 selected = 0xA5A5A5A5u;
    /** 全重み0の配列。 */
    const f32 zero_weights[] = { 0.0f, 0.0f };
    /** 負値を含む配列。 */
    const f32 negative_weights[] = { 1.0f, -1.0f };
    /** NaNを含む配列。 */
    const f32 nan_weights[] = { 1.0f, std::numeric_limits<f32>::quiet_NaN() };
    /** 無限大を含む配列。 */
    const f32 infinite_weights[] = { 1.0f, std::numeric_limits<f32>::infinity() };

    EXPECT_FALSE(random.TryWeightedIndex(nullptr, 1u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(zero_weights, 0u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(zero_weights, 4097u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(zero_weights, 2u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(negative_weights, 2u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(nan_weights, 2u, selected));
    EXPECT_FALSE(random.TryWeightedIndex(infinite_weights, 2u, selected));
    EXPECT_EQ(selected, 0xA5A5A5A5u);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** alignment不正pointerを作るbyte領域。 */
    alignas(f32) u8 storage[sizeof(f32) * 2u + 1u]{};
    /** 1byteずらした不正な重みpointer。 */
    const f32* misaligned = reinterpret_cast<const f32*>(storage + 1u);
    EXPECT_FALSE(random.TryWeightedIndex(misaligned, 2u, selected));

    /** address加算がoverflowする整列済みpointer。 */
    const uptr near_end = (~static_cast<uptr>(0u)) - (alignof(f32) - 1u);
    /** 実memoryへ触れず範囲検査だけに渡すpointer。 */
    const f32* overflowing = reinterpret_cast<const f32*>(near_end);
    EXPECT_FALSE(random.TryWeightedIndex(overflowing, 2u, selected));
    EXPECT_EQ(selected, 0xA5A5A5A5u);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** FRandom自身を重み配列として示すpointer。 */
    const f32* overlapping_weights = reinterpret_cast<const f32*>(&random);
    EXPECT_FALSE(random.TryWeightedIndex(overlapping_weights, 4u, selected));
    EXPECT_EQ(selected, 0xA5A5A5A5u);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** FRandom自身の先頭へ重ねた出力参照。 */
    u32& overlapping_output = *reinterpret_cast<u32*>(&random);
    EXPECT_FALSE(random.TryWeightedIndex(zero_weights, 2u, overlapping_output));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** 重みと出力の重なりを検査する配列。 */
    f32 overlapping_values[] = { 1.0f, 1.0f };
    /** 重み配列の先頭へ重ねた出力参照。 */
    u32& weights_output = *reinterpret_cast<u32*>(&overlapping_values[0]);
    EXPECT_FALSE(random.TryWeightedIndex(overlapping_values, 2u, weights_output));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);
}

ACS_TEST(RandomCompatibility, FillRangeF32IsAtomicAndEqualRangeConsumesNothing)
{
    /** 0件と同値範囲を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 0件操作前の再生位置。 */
    const game::FRandomSnapshot before_empty = random.CaptureSnapshot();
    EXPECT_TRUE(random.TryFillRangeF32(nullptr, 0u, std::numeric_limits<f32>::quiet_NaN(), 0.0f));
    ExpectSameSnapshot(random.CaptureSnapshot(), before_empty);

    /** 同値で埋める出力配列。 */
    f32 equal_values[] = { -1.0f, -1.0f, -1.0f };
    EXPECT_TRUE(random.TryFillRangeF32(equal_values, 3u, 2.5f, 2.5f));
    EXPECT_EQ(equal_values[0], 2.5f);
    EXPECT_EQ(equal_values[1], 2.5f);
    EXPECT_EQ(equal_values[2], 2.5f);
    EXPECT_EQ(random.NextU32(), 0x69E85A2Au);

    /** 正常範囲を一括生成する乱数器。 */
    game::FRandom generated{ 42u };
    /** 生成結果を受け取る配列。 */
    f32 values[2] = { 9.0f, 9.0f };
    EXPECT_TRUE(generated.TryFillRangeF32(values, 2u, -2.0f, 2.0f));
    EXPECT_TRUE(values[0] >= -2.0f && values[0] < 2.0f);
    EXPECT_TRUE(values[1] >= -2.0f && values[1] < 2.0f);
    EXPECT_EQ(generated.NextU32(), 0x0105185Fu);

    /** f32全有限域の補間を検査する乱数器。 */
    game::FRandom extreme{ 42u };
    /** f64補間で桁あふれを避けた結果。 */
    f32 extreme_value = 0.0f;
    EXPECT_TRUE(extreme.TryFillRangeF32(&extreme_value, 1u, -std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()));
    EXPECT_TRUE(extreme_value >= -std::numeric_limits<f32>::max());
    EXPECT_TRUE(extreme_value < std::numeric_limits<f32>::max());
    EXPECT_EQ(extreme.NextU32(), 0xF843FAD0u);
}

ACS_TEST(RandomCompatibility, FillRangeF32RejectsUnsafeBuffersWithoutMutation)
{
    /** 失敗経路を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 失敗前の再生位置。 */
    const game::FRandomSnapshot before = random.CaptureSnapshot();
    /** 失敗時に全要素を維持する配列。 */
    f32 values[] = { 11.0f, 13.0f };

    EXPECT_FALSE(random.TryFillRangeF32(nullptr, 1u, 0.0f, 1.0f));
    EXPECT_FALSE(random.TryFillRangeF32(values, 4097u, 0.0f, 1.0f));
    EXPECT_FALSE(random.TryFillRangeF32(values, 2u, 2.0f, 1.0f));
    EXPECT_FALSE(random.TryFillRangeF32(values, 2u, std::numeric_limits<f32>::quiet_NaN(), 1.0f));
    EXPECT_FALSE(random.TryFillRangeF32(values, 2u, 0.0f, std::numeric_limits<f32>::infinity()));
    EXPECT_EQ(values[0], 11.0f);
    EXPECT_EQ(values[1], 13.0f);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** alignment不正pointerを作るbyte領域。 */
    alignas(f32) u8 storage[sizeof(f32) + 1u]{};
    /** 1byteずらした不正な出力pointer。 */
    f32* misaligned = reinterpret_cast<f32*>(storage + 1u);
    EXPECT_FALSE(random.TryFillRangeF32(misaligned, 1u, 0.0f, 1.0f));

    /** address加算がoverflowする整列済みpointer。 */
    const uptr near_end = (~static_cast<uptr>(0u)) - (alignof(f32) - 1u);
    /** 実memoryへ触れず範囲検査だけに渡すpointer。 */
    f32* overflowing = reinterpret_cast<f32*>(near_end);
    EXPECT_FALSE(random.TryFillRangeF32(overflowing, 2u, 0.0f, 1.0f));

    /** FRandom自身の領域へ重ねた出力pointer。 */
    f32* overlapping_state = reinterpret_cast<f32*>(&random);
    EXPECT_FALSE(random.TryFillRangeF32(overlapping_state, 4u, 0.0f, 1.0f));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);
}

ACS_TEST(RandomCompatibility, FillRangeIntUsesUnbiasedKitContract)
{
    /** 棄却が1回発生する広い範囲を検査する乱数器。 */
    game::FRandom broad{ 42u };
    /** 広い範囲の生成結果。 */
    i32 broad_value = 0;
    EXPECT_TRUE(broad.TryFillRangeIntUnbiased(&broad_value, 1u, -1, 2147483647));
    EXPECT_EQ(broad_value, 2017721038);
    EXPECT_EQ(broad.NextU32(), 0x0105185Fu);

    /** i32全域を1回消費で生成する乱数器。 */
    game::FRandom full{ 42u };
    /** i32全域の生成結果。 */
    i32 full_value = 0;
    EXPECT_TRUE(full.TryFillRangeIntUnbiased(&full_value, 1u, -2147483647 - 1, 2147483647));
    EXPECT_EQ(full_value, -370648534);
    EXPECT_EQ(full.NextU32(), 0xF843FAD0u);

    /** 同値範囲で消費しない乱数器。 */
    game::FRandom equal{ 42u };
    /** 同値で埋める配列。 */
    i32 equal_values[] = { 0, 0 };
    EXPECT_TRUE(equal.TryFillRangeIntUnbiased(equal_values, 2u, 7, 7));
    EXPECT_EQ(equal_values[0], 7);
    EXPECT_EQ(equal_values[1], 7);
    EXPECT_EQ(equal.NextU32(), 0x69E85A2Au);
}

ACS_TEST(RandomCompatibility, FillRangeIntRejectsUnsafeBuffersWithoutMutation)
{
    /** 失敗経路を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 失敗前の再生位置。 */
    const game::FRandomSnapshot before = random.CaptureSnapshot();
    /** 失敗時に全要素を維持する配列。 */
    i32 values[] = { 11, 13 };

    EXPECT_TRUE(random.TryFillRangeIntUnbiased(nullptr, 0u, 8, 1));
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(nullptr, 1u, 1, 8));
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(values, 4097u, 1, 8));
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(values, 2u, 8, 1));
    EXPECT_EQ(values[0], 11);
    EXPECT_EQ(values[1], 13);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** alignment不正pointerを作るbyte領域。 */
    alignas(i32) u8 storage[sizeof(i32) + 1u]{};
    /** 1byteずらした不正な出力pointer。 */
    i32* misaligned = reinterpret_cast<i32*>(storage + 1u);
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(misaligned, 1u, 0, 1));

    /** FRandom自身の領域へ重ねた出力pointer。 */
    i32* overlapping_state = reinterpret_cast<i32*>(&random);
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(overlapping_state, 4u, 0, 1));

    /** address加算がoverflowする整列済みpointer。 */
    const uptr near_end = (~static_cast<uptr>(0u)) - (alignof(i32) - 1u);
    /** 実memoryへ触れず範囲検査だけに渡すpointer。 */
    i32* overflowing = reinterpret_cast<i32*>(near_end);
    EXPECT_FALSE(random.TryFillRangeIntUnbiased(overflowing, 2u, 0, 1));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);
}

ACS_TEST(RandomCompatibility, ShuffleUsesUnbiasedHighThenLowDraws)
{
    /** 64bit棄却法で並べ替える乱数器。 */
    game::FRandom random{ 42u };
    /** 並べ替えるindex配列。 */
    u32 indices[] = { 0u, 1u, 2u, 3u };
    EXPECT_TRUE(random.TryShuffleIndicesUnbiased(indices, 4u));
    EXPECT_EQ(indices[0], 2u);
    EXPECT_EQ(indices[1], 3u);
    EXPECT_EQ(indices[2], 1u);
    EXPECT_EQ(indices[3], 0u);
    EXPECT_EQ(random.NextU32(), 0xAF4213E7u);
}

ACS_TEST(RandomCompatibility, ShuffleNoopAndFailurePreserveStateAndOutput)
{
    /** 0件、1件、失敗経路を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 操作前の再生位置。 */
    const game::FRandomSnapshot before = random.CaptureSnapshot();
    EXPECT_TRUE(random.TryShuffleIndicesUnbiased(nullptr, 0u));

    /** 1件操作で維持する値。 */
    u32 single = 19u;
    EXPECT_TRUE(random.TryShuffleIndicesUnbiased(&single, 1u));
    EXPECT_EQ(single, 19u);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** 失敗時に全要素を維持する配列。 */
    u32 indices[] = { 3u, 2u, 1u, 0u };
    EXPECT_FALSE(random.TryShuffleIndicesUnbiased(nullptr, 1u));
    EXPECT_FALSE(random.TryShuffleIndicesUnbiased(indices, 4097u));
    EXPECT_EQ(indices[0], 3u);
    EXPECT_EQ(indices[1], 2u);
    EXPECT_EQ(indices[2], 1u);
    EXPECT_EQ(indices[3], 0u);
    ExpectSameSnapshot(random.CaptureSnapshot(), before);

    /** FRandom自身の領域へ重ねた出力pointer。 */
    u32* overlapping_state = reinterpret_cast<u32*>(&random);
    EXPECT_FALSE(random.TryShuffleIndicesUnbiased(overlapping_state, 4u));

    /** alignment不正pointerを作るbyte領域。 */
    alignas(u32) u8 storage[sizeof(u32) + 1u]{};
    /** 1byteずらした不正な出力pointer。 */
    u32* misaligned = reinterpret_cast<u32*>(storage + 1u);
    EXPECT_FALSE(random.TryShuffleIndicesUnbiased(misaligned, 1u));

    /** address加算がoverflowする整列済みpointer。 */
    const uptr near_end = (~static_cast<uptr>(0u)) - (alignof(u32) - 1u);
    /** 実memoryへ触れず範囲検査だけに渡すpointer。 */
    u32* overflowing = reinterpret_cast<u32*>(near_end);
    EXPECT_FALSE(random.TryShuffleIndicesUnbiased(overflowing, 2u));
    ExpectSameSnapshot(random.CaptureSnapshot(), before);
}

ACS_TEST(RandomCompatibility, CheckedBatchMaximumIsAccepted)
{
    /** 最大件数の操作を検査する乱数器。 */
    game::FRandom random{ 42u };
    /** 最大件数の重み配列。 */
    f32 weights[4096]{};
    /** 最大件数のf32出力。 */
    f32 floats[4096]{};
    /** 最大件数のi32出力。 */
    i32 integers[4096]{};
    /** 最大件数のindex出力。 */
    u32 indices[4096]{};
    for (u32 index = 0u; index < 4096u; ++index) {
        weights[index] = 1.0f;
        indices[index] = index;
    }

    /** 最大件数の重み抽選結果。 */
    u32 selected = 0u;
    EXPECT_TRUE(random.TryWeightedIndex(weights, 4096u, selected));
    EXPECT_TRUE(selected < 4096u);
    EXPECT_TRUE(random.TryFillRangeF32(floats, 4096u, 3.0f, 3.0f));
    EXPECT_TRUE(random.TryFillRangeIntUnbiased(integers, 4096u, 7, 7));
    EXPECT_TRUE(random.TryShuffleIndicesUnbiased(indices, 4096u));

    /** shuffle後の全indexを照合する合計。 */
    u64 sum = 0u;
    /** shuffle後に各indexが1回だけ現れたかを記録する。 */
    bool seen[4096]{};
    for (u32 index = 0u; index < 4096u; ++index) {
        EXPECT_EQ(floats[index], 3.0f);
        EXPECT_EQ(integers[index], 7);
        sum += indices[index];
        EXPECT_TRUE(indices[index] < 4096u);
        if (indices[index] < 4096u) {
            EXPECT_FALSE(seen[indices[index]]);
            seen[indices[index]] = true;
        }
    }
    EXPECT_EQ(sum, 8386560ULL);
}
