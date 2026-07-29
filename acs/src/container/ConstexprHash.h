// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

namespace hash_detail {

/** 64bit ハッシュの第1混合定数。 */
inline constexpr u64 kPrime1 = 0x9E3779B185EBCA87ull;

/** 64bit ハッシュの第2混合定数。 */
inline constexpr u64 kPrime2 = 0xC2B2AE3D27D4EB4Full;

/** 64bit ハッシュの第3混合定数。 */
inline constexpr u64 kPrime3 = 0x165667B19E3779F9ull;

/** 64bit ハッシュの第4混合定数。 */
inline constexpr u64 kPrime4 = 0x85EBCA77C2B2AE63ull;

/** 64bit ハッシュの第5混合定数。 */
inline constexpr u64 kPrime5 = 0x27D4EB2F165667C5ull;

/**
 * 64bit 値を左へ循環移動する。
 *
 * @param Value 移動する値。
 * @param Shift 1 から 63 までの移動 bit 数。
 * @return 循環移動後の値。
 */
constexpr u64 RotateLeft(u64 Value, u32 Shift) noexcept
{
    return (Value << Shift) | (Value >> (64u - Shift));
}

/**
 * リテラルの指定位置から little-endian 64bit 値を読む。
 *
 * @param Data 8 byte 以上を読める入力。
 * @param Offset 読み取り開始位置。
 * @return little-endian 64bit 値。
 */
constexpr u64 ReadU64Literal(const char* Data, usize Offset) noexcept
{
    // 読み取った little-endian 値。
    u64 Value = 0u;
    for (u32 Index = 0u; Index < 8u; ++Index) {
        Value |= static_cast<u64>(static_cast<u8>(Data[Offset + Index])) << (Index * 8u);
    }
    return Value;
}

/**
 * リテラルの指定位置から little-endian 32bit 値を読む。
 *
 * @param Data 4 byte 以上を読める入力。
 * @param Offset 読み取り開始位置。
 * @return little-endian 32bit 値。
 */
constexpr u64 ReadU32Literal(const char* Data, usize Offset) noexcept
{
    // 読み取った little-endian 値。
    u64 Value = 0u;
    for (u32 Index = 0u; Index < 4u; ++Index) {
        Value |= static_cast<u64>(static_cast<u8>(Data[Offset + Index])) << (Index * 8u);
    }
    return Value;
}

} // namespace hash_detail

/**
 * HashBytes と同じアルゴリズムをコンパイル時評価可能な形で実行する。
 *
 * @param Data 入力 byte 列。nullptr かつ Length>0 は 0 を返す。
 * @param Length 入力 byte 数。埋め込み NUL も通常の byte として数える。
 * @param Seed 初期 seed。
 * @return HashBytes と bit 一致する 64bit hash。
 */
constexpr u64 HashBytesConstexpr(const char* Data, usize Length, u64 Seed = 0xCBF29CE484222325ull) noexcept
{
    using namespace hash_detail;
    if (Data == nullptr && Length != 0u) return 0u;

    // 次に処理する入力 byte の位置。
    usize Offset = 0u;
    // 混合途中または完成済みの hash 値。
    u64 Hash = 0u;

    if (Length >= 32u) {
        // 32 byte 区間の第1累積値。
        u64 V1 = Seed + kPrime1 + kPrime2;
        // 32 byte 区間の第2累積値。
        u64 V2 = Seed + kPrime2;
        // 32 byte 区間の第3累積値。
        u64 V3 = Seed;
        // 32 byte 区間の第4累積値。
        u64 V4 = Seed - kPrime1;
        do {
            V1 += ReadU64Literal(Data, Offset) * kPrime2;
            V1 = RotateLeft(V1, 31u) * kPrime1;
            Offset += 8u;
            V2 += ReadU64Literal(Data, Offset) * kPrime2;
            V2 = RotateLeft(V2, 31u) * kPrime1;
            Offset += 8u;
            V3 += ReadU64Literal(Data, Offset) * kPrime2;
            V3 = RotateLeft(V3, 31u) * kPrime1;
            Offset += 8u;
            V4 += ReadU64Literal(Data, Offset) * kPrime2;
            V4 = RotateLeft(V4, 31u) * kPrime1;
            Offset += 8u;
        } while (Offset <= Length - 32u);
        Hash = RotateLeft(V1, 1u) + RotateLeft(V2, 7u) + RotateLeft(V3, 12u) + RotateLeft(V4, 18u);
    } else {
        Hash = Seed + kPrime5;
    }
    Hash += static_cast<u64>(Length);

    while (Offset + 8u <= Length) {
        // 8 byte 端数を混合した一時値。
        u64 Mixed = ReadU64Literal(Data, Offset) * kPrime2;
        Mixed = RotateLeft(Mixed, 31u) * kPrime1;
        Hash ^= Mixed;
        Hash = RotateLeft(Hash, 27u) * kPrime1 + kPrime4;
        Offset += 8u;
    }
    if (Offset + 4u <= Length) {
        Hash ^= ReadU32Literal(Data, Offset) * kPrime1;
        Hash = RotateLeft(Hash, 23u) * kPrime2 + kPrime3;
        Offset += 4u;
    }
    while (Offset < Length) {
        Hash ^= static_cast<u64>(static_cast<u8>(Data[Offset])) * kPrime5;
        Hash = RotateLeft(Hash, 11u) * kPrime1;
        ++Offset;
    }
    Hash ^= Hash >> 33u;
    Hash *= kPrime2;
    Hash ^= Hash >> 29u;
    Hash *= kPrime3;
    Hash ^= Hash >> 32u;
    return Hash;
}

/**
 * 終端 NUL を除いた文字列リテラルの hash をコンパイル時に求める。
 *
 * @tparam N 終端 NUL を含むリテラル配列長。
 * @param Literal hash 化する文字列リテラル。内部 NUL も対象に含む。
 * @param Seed 初期 seed。
 * @return HashBytes と bit 一致する 64bit hash。
 */
template<usize N>
constexpr u64 HashLiteral(const char (&Literal)[N], u64 Seed = 0xCBF29CE484222325ull) noexcept
{
    static_assert(N > 0u, "文字列リテラルは終端 NUL を必要とする");
    return HashBytesConstexpr(Literal, N - 1u, Seed);
}

} // namespace acs
