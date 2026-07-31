// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Random.h"

#include "math/Math.h"
#include "platform/Time.h"

#include <float.h>

namespace acs::game {

namespace {

/** 検査済みmemory領域の半開区間を保持する値。 */
struct FRandomMemoryRange {
    /** 領域の先頭address。 */
    uptr begin = 0u;

    /** 領域の終端直後address。 */
    uptr end = 0u;

    /**
     * 別領域と1byte以上重なるかを返す。
     *
     * @param other 比較する半開区間。
     * @return 重なりがあればtrue。
     */
    bool Overlaps(const FRandomMemoryRange& other) const noexcept {
        return begin < other.end && other.begin < end;
    }
};

/**
 * 配列pointerのalignment、byte数、address加算を検査する。
 *
 * @tparam T 配列要素型。
 * @param values 検査する配列先頭。
 * @param count 要素数。0ではvaluesがnullでもよい。
 * @param out_range 成功時に得られる半開区間。
 * @return 安全に全要素へaccessできる形ならtrue。
 */
template<typename T>
bool TryMakeRandomMemoryRange(const T* values, u32 count, FRandomMemoryRange& out_range) noexcept {
    out_range = {};
    if (count == 0u) return true;
    if (values == nullptr) return false;

    /** 配列先頭の整数address。 */
    const uptr begin = reinterpret_cast<uptr>(values);
    if ((begin % alignof(T)) != 0u) return false;

    /** uptrで表せる最大address。 */
    constexpr uptr kMaximumAddress = ~static_cast<uptr>(0u);
    if (static_cast<usize>(count) > static_cast<usize>(kMaximumAddress / sizeof(T))) return false;

    /** 配列全体のbyte数。 */
    const uptr bytes = static_cast<uptr>(sizeof(T)) * static_cast<uptr>(count);
    if (begin > kMaximumAddress - bytes) return false;

    out_range.begin = begin;
    out_range.end = begin + bytes;
    return true;
}

/**
 * f32がNaNや無限大ではないかを返す。
 *
 * @param value 検査する値。
 * @return 有限値ならtrue。
 */
bool IsFiniteRandomValue(f32 value) noexcept {
    return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

/**
 * SplitMix64を1回進めてseedを拡散する。
 *
 * @param state 更新する64bit状態。
 * @return 拡散済みの64bit値。
 */
u64 SplitMix64Step(u64& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    /** 各bitへ入力差を広げる一時値。 */
    u64 value = state;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31u);
}

/**
 * 32bit値を左へ循環させる。
 *
 * @param value 循環させるbit列。
 * @param shift 左へ移すbit数。
 * @return 循環後の値。
 */
u32 RotateLeft32(u32 value, u32 shift) noexcept {
    return (value << shift) | (value >> (32u - shift));
}

/**
 * u32をlittle-endian順でFNV-1a 64bitへ混ぜる。
 *
 * @param hash 更新する検査値。
 * @param value 混ぜる32bit値。
 */
void MixRandomSnapshotWord(u64& hash, u32 value) noexcept {
    /** 1byteずつ取り出すbit位置。 */
    for (u32 shift = 0u; shift < 32u; shift += 8u) {
        hash ^= static_cast<u8>(value >> shift);
        hash *= 1099511628211ULL;
    }
}

/**
 * snapshot fieldを固定順でFNV-1a 64bitへ畳み込む。
 *
 * @param snapshot 検査値以外を設定済みのsnapshot。
 * @return little-endian field列の検査値。
 */
u64 RandomSnapshotSignature(const FRandomSnapshot& snapshot) noexcept {
    /** 標準FNV-1a 64bitの初期値。 */
    u64 hash = 14695981039346656037ULL;
    MixRandomSnapshotWord(hash, snapshot.version);
    MixRandomSnapshotWord(hash, snapshot.state0);
    MixRandomSnapshotWord(hash, snapshot.state1);
    MixRandomSnapshotWord(hash, snapshot.state2);
    MixRandomSnapshotWord(hash, snapshot.state3);
    MixRandomSnapshotWord(hash, snapshot.reserved);
    return hash;
}

/**
 * 有限な半開区間から1個のf32値を生成する。
 *
 * @param random 使用する乱数列。
 * @param min 範囲の下限。
 * @param max 範囲の上限。
 * @return 丸め後もmax未満となる値。
 */
f32 DrawCheckedRangeF32(FRandom& random, f32 min, f32 max) noexcept {
    /** 24bit精度の0以上1未満の値。 */
    const f64 unit = static_cast<f64>(random.NextF32Unit());
    /** f64でoverflowを避けて補間した値。 */
    const f64 candidate = static_cast<f64>(min) + (static_cast<f64>(max) - static_cast<f64>(min)) * unit;
    /** 公開型へ丸めた結果。 */
    f32 result = static_cast<f32>(candidate);
    if (result < min) result = min;
    if (!(result < max)) result = ::nextafterf(max, min);
    return result;
}

/**
 * 偏りのない32bit棄却法で整数を1個生成する。
 *
 * @param random 使用する乱数列。
 * @param min 両端を含む下限。
 * @param max 両端を含む上限。
 * @return 指定範囲内の整数。
 */
i32 DrawUnbiasedRangeInt(FRandom& random, i32 min, i32 max) noexcept {
    if (min == max) return min;

    /** i32全域も表せる選択肢数。 */
    const u64 selection_count = static_cast<u64>(static_cast<i64>(max) - static_cast<i64>(min)) + 1u;
    if (selection_count == 0x100000000ULL) {
        /** i32全域へ足し込む32bit値。 */
        const u32 random_bits = random.NextU32();
        return static_cast<i32>(static_cast<i64>(min) + static_cast<i64>(random_bits));
    }

    /** 32bit剰余の偏りが生じる先頭領域の長さ。 */
    const u32 rejection_threshold = (0u - static_cast<u32>(selection_count)) % static_cast<u32>(selection_count);
    /** 棄却条件を満たすまで更新する32bit値。 */
    u32 random_bits = 0u;
    do {
        random_bits = random.NextU32();
    } while (random_bits < rejection_threshold);

    /** 下限からの偏りのないoffset。 */
    const u32 offset = random_bits % static_cast<u32>(selection_count);
    return static_cast<i32>(static_cast<i64>(min) + static_cast<i64>(offset));
}

} // namespace

/** 起動時刻をseedにして乱数列を初期化する。 */
FRandom::FRandom() noexcept {
    Seed(FClock::Ticks());
}

/** 指定seedから再現可能な乱数列を初期化する。 */
FRandom::FRandom(u64 seed) noexcept {
    Seed(seed);
}

/** 指定seedから内部状態を再初期化する。 */
void FRandom::Seed(u64 seed) noexcept {
    /** SplitMix64を進める一時状態。 */
    u64 splitmix_state = seed;
    /** 第0、第1状態を含む拡散値。 */
    const u64 first = SplitMix64Step(splitmix_state);
    /** 第2、第3状態を含む拡散値。 */
    const u64 second = SplitMix64Step(splitmix_state);
    m_S0 = static_cast<u32>(first);
    m_S1 = static_cast<u32>(first >> 32u);
    m_S2 = static_cast<u32>(second);
    m_S3 = static_cast<u32>(second >> 32u);
    if ((m_S0 | m_S1 | m_S2 | m_S3) == 0u) {
        m_S0 = 0xDEADBEEFu;
        m_S1 = 0xCAFEBABEu;
        m_S2 = 0xFEEDF00Du;
        m_S3 = 0x12345678u;
    }
}

/** 乱数列から32bit値を1個生成する。 */
u32 FRandom::NextU32() noexcept {
    /** 現在状態を非線形に混ぜた出力。 */
    const u32 result = RotateLeft32(m_S1 * 5u, 7u) * 9u;
    /** 状態遷移で第2状態へ混ぜる値。 */
    const u32 shifted = m_S1 << 9u;
    m_S2 ^= m_S0;
    m_S3 ^= m_S1;
    m_S1 ^= m_S2;
    m_S0 ^= m_S3;
    m_S2 ^= shifted;
    m_S3 = RotateLeft32(m_S3, 11u);
    return result;
}

/** 指定回数だけ乱数列を進める。 */
bool FRandom::TryDiscard(u64 draw_count) noexcept {
    if (draw_count > kMaximumDiscardCount) return false;
    /** 消費済みの32bit値の個数。 */
    for (u64 index = 0u; index < draw_count; ++index) {
        (void)NextU32();
    }
    return true;
}

/** 現在の再生位置を改変検出付きの値へ保存する。 */
FRandomSnapshot FRandom::CaptureSnapshot() const noexcept {
    /** 公開fieldを固定順で設定するsnapshot。 */
    FRandomSnapshot snapshot{};
    snapshot.version = FRandomSnapshot::kCurrentVersion;
    snapshot.state0 = m_S0;
    snapshot.state1 = m_S1;
    snapshot.state2 = m_S2;
    snapshot.state3 = m_S3;
    snapshot.reserved = 0u;
    snapshot.signature = RandomSnapshotSignature(snapshot);
    return snapshot;
}

/** 検査済みの再生位置へ状態を戻す。 */
bool FRandom::TryRestoreSnapshot(const FRandomSnapshot& snapshot) noexcept {
    if (snapshot.version != FRandomSnapshot::kCurrentVersion || snapshot.reserved != 0u) return false;
    if ((snapshot.state0 | snapshot.state1 | snapshot.state2 | snapshot.state3) == 0u) return false;
    if (snapshot.signature != RandomSnapshotSignature(snapshot)) return false;
    m_S0 = snapshot.state0;
    m_S1 = snapshot.state1;
    m_S2 = snapshot.state2;
    m_S3 = snapshot.state3;
    return true;
}

/** 0以上1未満のf32値を1個生成する。 */
f32 FRandom::NextF32Unit() noexcept {
    /** f32の有効桁へ使う上位24bit。 */
    const u32 bits = NextU32() >> 8u;
    return static_cast<f32>(bits) * (1.0f / 16777216.0f);
}

/** [min, max]の整数を既存の1回剰余方式で生成する。 */
i32 FRandom::RangeInt(i32 min, i32 max) noexcept {
    if (max < min) {
        /** 交換中に保持する範囲端。 */
        const i32 temporary = min;
        min = max;
        max = temporary;
    }
    /** i32全域では0へ回る選択肢数。 */
    const u32 span = (static_cast<u32>(max) - static_cast<u32>(min)) + 1u;
    /** 既存方式で1回だけ消費する32bit値。 */
    const u32 random_bits = NextU32();
    if (span == 0u) {
        /** 正のi32範囲へ収まる生値の上限。 */
        constexpr u32 kMaximumPositiveI32 = 0x7FFFFFFFu;
        if (random_bits <= kMaximumPositiveI32) return static_cast<i32>(random_bits);
        /** 上位側を2の補数に対応する負値へ変換する係数。 */
        constexpr i64 kU32ValueCount = 4294967296LL;
        return static_cast<i32>(static_cast<i64>(random_bits) - kU32ValueCount);
    }
    /** 下限へ足す既存剰余値。 */
    const u32 offset = random_bits % span;
    return static_cast<i32>(static_cast<i64>(min) + static_cast<i64>(offset));
}

/** min以上max未満のf32値を既存方式で生成する。 */
f32 FRandom::RangeF32(f32 min, f32 max) noexcept {
    return min + (max - min) * NextF32Unit();
}

/** 指定確率でtrueを生成する。 */
bool FRandom::NextBool(f32 true_probability) noexcept {
    if (true_probability <= 0.0f) return false;
    if (true_probability >= 1.0f) return true;
    return NextF32Unit() < true_probability;
}

/** 円板内の一様な点を生成する。 */
FVec2 FRandom::PointInCircle(f32 radius) noexcept {
    for (;;) {
        /** 正方形内へ生成したx座標。 */
        const f32 x = NextF32Unit() * 2.0f - 1.0f;
        /** 正方形内へ生成したy座標。 */
        const f32 y = NextF32Unit() * 2.0f - 1.0f;
        /** 原点からの距離の2乗。 */
        const f32 distance_squared = x * x + y * y;
        if (distance_squared <= 1.0f) return FVec2{ x * radius, y * radius };
    }
}

/** 円周上の一様な点を生成する。 */
FVec2 FRandom::PointOnCircle(f32 radius) noexcept {
    /** 0以上2π未満の角度。 */
    const f32 angle = NextF32Unit() * kTwoPi;
    return FVec2{ Cos(angle) * radius, Sin(angle) * radius };
}

/** 重みに比例したindexを既存の24bit方式で生成する。 */
u32 FRandom::WeightedChoice(const f32* weights, u32 count) noexcept {
    if (count == 0u || weights == nullptr) return 0u;
    /** 正の重みだけを加えた合計。 */
    f32 total = 0.0f;
    for (u32 index = 0u; index < count; ++index) {
        /** 負値を0として扱う既存重み。 */
        const f32 weight = weights[index] > 0.0f ? weights[index] : 0.0f;
        total += weight;
    }
    if (total <= 0.0f) return 0u;
    /** 累積重みと比較する抽選位置。 */
    const f32 selection = NextF32Unit() * total;
    /** 現在までの累積重み。 */
    f32 accumulated = 0.0f;
    for (u32 index = 0u; index < count; ++index) {
        /** 負値を0として扱う既存重み。 */
        const f32 weight = weights[index] > 0.0f ? weights[index] : 0.0f;
        accumulated += weight;
        if (selection < accumulated) return index;
    }
    return count - 1u;
}

/** 検査済み重みに比例したindexを53bit精度で生成する。 */
bool FRandom::TryWeightedIndex(const f32* weights, u32 count, u32& out_index) noexcept {
    if (count == 0u || count > kMaximumBatchCount) return false;

    /** 重み配列の検査済み領域。 */
    FRandomMemoryRange weights_range{};
    /** 出力値の検査済み領域。 */
    FRandomMemoryRange output_range{};
    /** この乱数器の内部状態を含む領域。 */
    FRandomMemoryRange state_range{};
    if (!TryMakeRandomMemoryRange(weights, count, weights_range) || !TryMakeRandomMemoryRange(&out_index, 1u, output_range) || !TryMakeRandomMemoryRange(this, 1u, state_range)) return false;
    if (weights_range.Overlaps(output_range) || weights_range.Overlaps(state_range) || output_range.Overlaps(state_range)) return false;

    /** overflowを避ける正規化基準。 */
    f32 maximum_weight = 0.0f;
    for (u32 index = 0u; index < count; ++index) {
        /** 検査中の重み。 */
        const f32 weight = weights[index];
        if (!IsFiniteRandomValue(weight) || weight < 0.0f) return false;
        if (weight > maximum_weight) maximum_weight = weight;
    }
    if (maximum_weight <= 0.0f) return false;

    /** 最大値で正規化した重み合計。 */
    f64 total_weight = 0.0;
    for (u32 index = 0u; index < count; ++index) {
        total_weight += static_cast<f64>(weights[index]) / static_cast<f64>(maximum_weight);
    }

    /** 53bit値の上位へ置く最初の32bit値。 */
    const u64 high_bits = static_cast<u64>(NextU32());
    /** 53bit値の下位へ置く次の32bit値。 */
    const u64 low_bits = static_cast<u64>(NextU32());
    /** 高位から低位へ結合した64bit値。 */
    const u64 random_bits = (high_bits << 32u) | low_bits;
    /** 上位53bitを0以上1未満へ写した値。 */
    const f64 unit = static_cast<f64>(random_bits >> 11u) * (1.0 / 9007199254740992.0);
    /** 累積重みと比較する抽選位置。 */
    const f64 selection = unit * total_weight;
    /** 現在までの正規化済み累積重み。 */
    f64 accumulated = 0.0;
    /** 丸め誤差時に使う最後の正重み位置。 */
    u32 last_positive = 0u;
    for (u32 index = 0u; index < count; ++index) {
        if (weights[index] <= 0.0f) continue;
        last_positive = index;
        accumulated += static_cast<f64>(weights[index]) / static_cast<f64>(maximum_weight);
        if (selection < accumulated) {
            out_index = index;
            return true;
        }
    }
    out_index = last_positive;
    return true;
}

/** min以上max未満の有限値で出力配列を埋める。 */
bool FRandom::TryFillRangeF32(f32* values, u32 count, f32 min, f32 max) noexcept {
    if (count > kMaximumBatchCount) return false;

    /** 出力配列の検査済み領域。 */
    FRandomMemoryRange values_range{};
    if (!TryMakeRandomMemoryRange(values, count, values_range)) return false;
    if (count == 0u) return true;

    /** この乱数器の内部状態を含む領域。 */
    FRandomMemoryRange state_range{};
    if (!TryMakeRandomMemoryRange(this, 1u, state_range) || values_range.Overlaps(state_range)) return false;
    if (!IsFiniteRandomValue(min) || !IsFiniteRandomValue(max) || min > max) return false;

    if (min == max) {
        for (u32 index = 0u; index < count; ++index) values[index] = min;
        return true;
    }
    for (u32 index = 0u; index < count; ++index) values[index] = DrawCheckedRangeF32(*this, min, max);
    return true;
}

/** 偏りを除いた[min, max]の整数で出力配列を埋める。 */
bool FRandom::TryFillRangeIntUnbiased(i32* values, u32 count, i32 min, i32 max) noexcept {
    if (count > kMaximumBatchCount) return false;

    /** 出力配列の検査済み領域。 */
    FRandomMemoryRange values_range{};
    if (!TryMakeRandomMemoryRange(values, count, values_range)) return false;
    if (count == 0u) return true;

    /** この乱数器の内部状態を含む領域。 */
    FRandomMemoryRange state_range{};
    if (!TryMakeRandomMemoryRange(this, 1u, state_range) || values_range.Overlaps(state_range) || min > max) return false;

    for (u32 index = 0u; index < count; ++index) values[index] = DrawUnbiasedRangeInt(*this, min, max);
    return true;
}

/** 64bit棄却法でindex配列をその場で並べ替える。 */
bool FRandom::TryShuffleIndicesUnbiased(u32* indices, u32 count) noexcept {
    if (count > kMaximumBatchCount) return false;

    /** 並べ替え配列の検査済み領域。 */
    FRandomMemoryRange indices_range{};
    if (!TryMakeRandomMemoryRange(indices, count, indices_range)) return false;
    if (count == 0u) return true;

    /** この乱数器の内部状態を含む領域。 */
    FRandomMemoryRange state_range{};
    if (!TryMakeRandomMemoryRange(this, 1u, state_range) || indices_range.Overlaps(state_range)) return false;
    if (count == 1u) return true;

    /** 末尾から確定する配列位置。 */
    for (u32 index = count - 1u; index > 0u; --index) {
        /** 現在位置までにある選択肢数。 */
        const u64 selection_count = static_cast<u64>(index) + 1u;
        /** 64bit剰余の偏りが生じる先頭領域の長さ。 */
        const u64 rejection_threshold = (0u - selection_count) % selection_count;
        /** 棄却条件を満たすまで更新する64bit値。 */
        u64 random_bits = 0u;
        do {
            /** 上位へ置く最初の32bit値。 */
            const u64 high_bits = static_cast<u64>(NextU32());
            /** 下位へ置く次の32bit値。 */
            const u64 low_bits = static_cast<u64>(NextU32());
            random_bits = (high_bits << 32u) | low_bits;
        } while (random_bits < rejection_threshold);

        /** 現在位置までから選んだ交換位置。 */
        const u32 selected = static_cast<u32>(random_bits % selection_count);
        if (selected != index) {
            /** 交換中に保持するindex値。 */
            const u32 temporary = indices[index];
            indices[index] = indices[selected];
            indices[selected] = temporary;
        }
    }
    return true;
}

/** 呼び出し側が単一threadに閉じて使う共有乱数列を返す。 */
FRandom& FRandom::Global() noexcept {
    /** 起動時刻seedで遅延初期化する共有値。 */
    static FRandom global{};
    return global;
}

} // namespace acs::game
