// SPDX-License-Identifier: Apache-2.0
// 時間は参考値とし、安定した退行判定には決定的カウンタを併記する。
#include "container/Array.h"
#include "container/HashMap.h"
#include "container/Json.h"
#include "container/StableStringKey.h"
#include "container/String.h"
#include "container/StringHasher.h"
#include "math/MathDispatch.h"
#include "memory/PoolAllocator.h"
#include "memory/SystemAllocator.h"
#include "memory/TypedPoolAllocator.h"

#include <chrono>
#include <cstdio>

using namespace acs;

namespace {

/** 最適化で計測対象が消えないよう結果を集約する。 */
volatile u64 g_Sink = 0u;

/**
 * monotonic clock の現在値を nanosecond で返す。
 *
 * @return steady clock の epoch からの nanosecond 数。
 */
u64 NowNs() noexcept
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** Alloc、Realloc、Free の呼び出し回数を観測する allocator。 */
struct FCountingAllocator final : IAllocator {
    /** Alloc を計数して backing へ転送する。 */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++AllocCalls;
        return Backing.Alloc(Size, Alignment, Location);
    }

    /** Realloc を計数して backing へ転送する。 */
    void* Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++ReallocCalls;
        return Backing.Realloc(Pointer, OldSize, NewSize, Alignment, Location);
    }

    /** 非 nullptr の Free を計数して backing へ転送する。 */
    void Free(void* Pointer) noexcept override
    {
        if (Pointer != nullptr) ++FreeCalls;
        Backing.Free(Pointer);
    }

    /** 実際の確保を担当する allocator。 */
    CSystemAllocator Backing;

    /** Alloc 呼び出し回数。 */
    u64 AllocCalls = 0u;

    /** Realloc 呼び出し回数。 */
    u64 ReallocCalls = 0u;

    /** 非 nullptr の Free 呼び出し回数。 */
    u64 FreeCalls = 0u;
};

/** u64 hash 呼び出し回数を観測する hasher。 */
struct FCountingU64Hasher {
    /** 全インスタンスで共有する呼び出し回数。 */
    static u64 CallCount;

    /**
     * u64 を hash 化して呼び出し回数を増やす。
     *
     * @param Value hash 化する値。
     * @return avalanche 済みの hash。
     */
    u64 operator()(u64 Value) const noexcept
    {
        ++CallCount;
        return HashMix64(Value);
    }
};

/** u64 hasher の共有呼び出し回数。 */
u64 FCountingU64Hasher::CallCount = 0u;

/** T01/T21 の配列成長時間と確保経路を計測する。 */
void BenchArray() noexcept
{
    // 1 回の要素数。
    constexpr usize kCount = 65536u;
    // 時間計測の反復数。
    constexpr usize kRepeats = 64u;

    // 決定的な確保回数を観測する allocator。
    FCountingAllocator Allocator;
    {
        // 確保経路計測用の trivial 配列。
        TArray<u32> Values(Allocator);
        for (usize Index = 0u; Index < kCount; ++Index) Values.PushBack(static_cast<u32>(Index));
        g_Sink += Values.Back();
    }

    // 時間計測の開始時刻。
    const u64 Started = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        // 現在反復で成長させる配列。
        TArray<u32> Values;
        for (usize Index = 0u; Index < kCount; ++Index) Values.PushBack(static_cast<u32>(Index));
        g_Sink += Values.Back();
    }
    // 反復全体の経過時間。
    const u64 Elapsed = NowNs() - Started;

    std::printf("T01 array_push ns=%llu alloc=%llu realloc=%llu free=%llu\nT21 relocatable_u32=%u\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(Allocator.AllocCalls), static_cast<unsigned long long>(Allocator.ReallocCalls), static_cast<unsigned long long>(Allocator.FreeCalls), IsTriviallyRelocatableV<u32> ? 1u : 0u);
}

/** T02/T22 の map 検索・更新と安定キー検索を計測する。 */
void BenchHashMap() noexcept
{
    // map の確保回数を観測する allocator。
    FCountingAllocator Allocator;
    // 更新と検索の対象 map。
    THashMap<u64, u64, FCountingU64Hasher> Values(Allocator);
    for (u64 Index = 0u; Index < 12u; ++Index) Values.Insert(Index, Index);

    // 更新前の確保回数。
    const u64 AllocBefore = Allocator.AllocCalls;
    // 検索前の hash 呼び出し回数。
    const u64 HashBefore = FCountingU64Hasher::CallCount;
    // u64 検索の開始時刻。
    const u64 Started = NowNs();
    for (u64 Index = 0u; Index < 1000000u; ++Index) {
        // 現在キーで見つけた値。
        u64* Value = Values.Find(Index % 12u);
        if (Value != nullptr) g_Sink += *Value;
    }
    // u64 検索の経過時間。
    const u64 Elapsed = NowNs() - Started;

    Values.Insert(0u, 99u);
    std::printf("T02 hashmap_find ns=%llu update_alloc=%llu update_hash=%llu\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(Allocator.AllocCalls - AllocBefore), static_cast<unsigned long long>(FCountingU64Hasher::CallCount - HashBefore - 1000000u));

    // heterogeneous 検索対象の文字列 map。
    THashMap<FString, u64, FStringHasher> StringValues;
    StringValues.Insert(FString("foundation.optimization.stable-key"), 37u);
    // 毎回 hash 化する動的キー。
    const FStringView DynamicKey("foundation.optimization.stable-key");
    // コンパイル時に hash を固定した安定キー。
    constexpr FStableStringKey StableKey = MakeStableStringKey("foundation.optimization.stable-key");
    // 動的キー検索の開始時刻。
    const u64 DynamicStarted = NowNs();
    for (u64 Index = 0u; Index < 1000000u; ++Index) {
        // 動的キーで見つけた値。
        const u64* Value = StringValues.FindAs(DynamicKey);
        if (Value != nullptr) g_Sink += *Value;
    }
    // 動的キー検索の経過時間。
    const u64 DynamicElapsed = NowNs() - DynamicStarted;
    // 安定キー検索の開始時刻。
    const u64 StableStarted = NowNs();
    for (u64 Index = 0u; Index < 1000000u; ++Index) {
        // 安定キーで見つけた値。
        const u64* Value = StringValues.FindByHash(StableKey.View, StableKey.Hash);
        if (Value != nullptr) g_Sink += *Value;
    }
    // 安定キー検索の経過時間。
    const u64 StableElapsed = NowNs() - StableStarted;
    std::printf("T22 lookup_dynamic ns=%llu lookup_stable ns=%llu literal_hash=%llu\n", static_cast<unsigned long long>(DynamicElapsed), static_cast<unsigned long long>(StableElapsed), static_cast<unsigned long long>(StableKey.Hash));
}

/** T03 の文字列成長経路と byte 検索を計測する。 */
void BenchString() noexcept
{
    // 検索対象の byte 長。
    constexpr usize kLength = 16384u;
    // 検索の反復数。
    constexpr usize kRepeats = 25000u;

    // 文字列の確保回数を観測する allocator。
    FCountingAllocator Allocator;
    // 長い検索対象文字列。
    FString Text(Allocator);
    for (usize Index = 0u; Index < kLength; ++Index) {
        Text.Append(static_cast<char>('a' + (Index % 23u)));
    }
    Text.Append(FStringView("needle"));

    // 検索対象 view。
    const FStringView Haystack = Text.View();
    // 末尾に存在する検索語。
    const FStringView Needle("needle");
    // byte 検索の開始時刻。
    const u64 Started = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        g_Sink += Haystack.Find(Needle);
    }
    // byte 検索の経過時間。
    const u64 Elapsed = NowNs() - Started;

    std::printf("T03 string_find ns=%llu append_alloc=%llu append_realloc=%llu\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(Allocator.AllocCalls), static_cast<unsigned long long>(Allocator.ReallocCalls));
}

/** T04/T23 の JSON 分類・パース・書き出しを計測する。 */
void BenchJson() noexcept
{
    // 長い非 escape 区間を多く含む JSON。
    FString Json;
    Json.Append(FStringView("{\"items\":["));
    for (u32 Index = 0u; Index < 128u; ++Index) {
        if (Index != 0u) Json.Append(',');
        Json.Append(FStringView("{\"name\":\"a_long_unescaped_asset_name_for_parser_batching\",\"value\":12345.625,\"enabled\":true}"));
    }
    Json.Append(FStringView("]}"));

    // parse と write の反復数。
    constexpr usize kRepeats = 100u;
    // parse 計測の開始時刻。
    const u64 Started = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        // 現在反復のパース結果。
        TResult<FJsonValue> Parsed = ParseJson(Json.View());
        if (Parsed.IsOk()) g_Sink += Parsed.Value().Get("items").Size();
    }
    // parse の経過時間。
    const u64 Elapsed = NowNs() - Started;
    // writer の入力 DOM。
    TResult<FJsonValue> WriterSource = ParseJson(Json.View());
    // writer 計測の開始時刻。
    const u64 WriterStarted = NowNs();
    // 全反復で書いた byte 数。
    usize WriterBytes = 0u;
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        // 現在反復の writer 出力。
        FString Output;
        if (WriterSource.IsOk() && TryWriteJson(WriterSource.Value(), Output)) {
            WriterBytes += Output.Size();
        }
    }
    // writer の経過時間。
    const u64 WriterElapsed = NowNs() - WriterStarted;
    std::printf("T04 json_parse ns=%llu json_write ns=%llu bytes=%llu written_bytes=%llu repeats=%llu\nT23 classifier_entries=256 parse_ns=%llu\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(WriterElapsed), static_cast<unsigned long long>(Json.Size()), static_cast<unsigned long long>(WriterBytes), static_cast<unsigned long long>(kRepeats), static_cast<unsigned long long>(Elapsed));
}

/** T05/T24 の個別・batch・typed pool 経路を計測する。 */
void BenchPool() noexcept
{
    // プールのブロック数。
    constexpr usize kCount = 4096u;
    // 確保・返却の反復数。
    constexpr usize kRepeats = 500u;
    // 現在反復で取得したブロック群。
    void* Pointers[kCount]{};
    // 個別 API を計測するプール。
    CPoolAllocator Pool(64u, kCount, 64u);

    // 個別 API 計測の開始時刻。
    const u64 Started = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        for (usize Index = 0u; Index < kCount; ++Index) {
            Pointers[Index] = Pool.Alloc(64u, 64u, FSourceLoc::Current());
        }
        for (usize Index = 0u; Index < kCount; ++Index) Pool.Free(Pointers[Index]);
    }
    // 個別 API の経過時間。
    const u64 Elapsed = NowNs() - Started;
    // 個別 API の累積ロック回数。
    const u64 IndividualLocks = Pool.LockAcquisitionCount();
    g_Sink += Pool.AllocationCount();

    // batch API を計測するプール。
    CPoolAllocator BatchPool(64u, kCount, 64u);
    // batch API 計測の開始時刻。
    const u64 BatchStarted = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        BatchPool.AllocBatch(Pointers, kCount);
        BatchPool.FreeBatch(Pointers, kCount);
    }
    // batch API の経過時間。
    const u64 BatchElapsed = NowNs() - BatchStarted;

    // compile-time layout の typed pool。
    TTypedPoolAllocator<u64, kCount> TypedPool;
    // typed pool 計測の開始時刻。
    const u64 TypedStarted = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        for (usize Index = 0u; Index < kCount; ++Index) {
            Pointers[Index] = TypedPool.Allocate();
        }
        for (usize Index = 0u; Index < kCount; ++Index) {
            TypedPool.Deallocate(static_cast<u64*>(Pointers[Index]));
        }
    }
    // typed pool の経過時間。
    const u64 TypedElapsed = NowNs() - TypedStarted;
    std::printf("T05 pool_individual ns=%llu locks=%llu pool_batch ns=%llu locks=%llu operations=%llu\nT24 typed_pool ns=%llu block_size=%llu block_count=%llu\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(IndividualLocks), static_cast<unsigned long long>(BatchElapsed), static_cast<unsigned long long>(BatchPool.LockAcquisitionCount()), static_cast<unsigned long long>(kCount * kRepeats * 2u), static_cast<unsigned long long>(TypedElapsed), static_cast<unsigned long long>(decltype(TypedPool)::BlockSize()), static_cast<unsigned long long>(decltype(TypedPool)::BlockCount()));
}

/** T25 の runtime dispatch と static policy を同一入力で計測する。 */
void BenchMath() noexcept
{
    // 1 回の点数。
    constexpr usize kCount = 1024u;
    // batch 変換の反復数。
    constexpr usize kRepeats = 20000u;
    // 変換前の点群。
    FVec3 Input[kCount]{};
    // 変換後の点群。
    FVec3 Output[kCount]{};
    for (usize Index = 0u; Index < kCount; ++Index) {
        Input[Index] = FVec3{static_cast<f32>(Index), static_cast<f32>(Index % 17u), static_cast<f32>(Index % 29u)};
    }
    // 回転と平行移動を含む変換行列。
    const FMat4 Transform = FMat4::RotationZ(0.37f) * FMat4::Translation(FVec3{3.0f, -2.0f, 7.0f});

    // runtime dispatch 計測の開始時刻。
    const u64 Started = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        TransformPoints(Input, Output, kCount, Transform);
    }
    // runtime dispatch の経過時間。
    const u64 Elapsed = NowNs() - Started;
    // static policy 計測の開始時刻。
    const u64 StaticStarted = NowNs();
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        TransformBatchStatic<EBatchTransformPolicy::Point>(Input, Output, kCount, Transform);
    }
    // static policy の経過時間。
    const u64 StaticElapsed = NowNs() - StaticStarted;
    g_Sink += static_cast<u64>(Output[kCount - 1u].x);
    std::printf("T25 math_runtime_dispatch ns=%llu math_static ns=%llu points=%llu\n", static_cast<unsigned long long>(Elapsed), static_cast<unsigned long long>(StaticElapsed), static_cast<unsigned long long>(kCount * kRepeats));
}

} // namespace

/** 全 benchmark を固定順で実行する。 */
int main()
{
    BenchArray();
    BenchHashMap();
    BenchString();
    BenchJson();
    BenchPool();
    BenchMath();
    std::printf("sink=%llu\n", static_cast<unsigned long long>(g_Sink));
    return 0;
}
