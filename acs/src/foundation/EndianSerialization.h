// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Compiler.h"
#include "foundation/TypeTraits.h"
#include "foundation/Types.h"

#include <cstring>

namespace acs {

namespace endian_detail {

/** byte 数から同幅の符号なし格納型を選ぶ補助 template。 */
template<usize Size>
struct TUnsignedStorage;

/** 1 byte 値の格納型。 */
template<>
struct TUnsignedStorage<1u> {
    using Type = u8;
};

/** 2 byte 値の格納型。 */
template<>
struct TUnsignedStorage<2u> {
    using Type = u16;
};

/** 4 byte 値の格納型。 */
template<>
struct TUnsignedStorage<4u> {
    using Type = u32;
};

/** 8 byte 値の格納型。 */
template<>
struct TUnsignedStorage<8u> {
    using Type = u64;
};

/** 直列化対象と同じ幅の符号なし格納型。 */
template<typename T>
using UnsignedStorageT = typename TUnsignedStorage<sizeof(T)>::Type;

/** 対応する固定幅 scalar または enum なら true。 */
template<typename T>
inline constexpr bool IsEndianSerializableV =
    !IsSameV<RemoveCVT<T>, bool> &&
    (IsIntegralV<T> || IsFloatingV<T> || IsEnumV<T>) &&
    (sizeof(T) == 1u || sizeof(T) == 2u || sizeof(T) == 4u || sizeof(T) == 8u);

} // namespace endian_detail

/**
 * 固定幅 scalar または enum を host endian に依存しない little endian byte 列へ書く。
 *
 * @tparam T 書き込む固定幅型。
 * @param Destination sizeof(T) byte 以上の書き込み先。
 * @param Value 書き込む値。
 */
template<typename T>
ACS_FORCEINLINE void WriteLittleEndian(u8* Destination, T Value) noexcept
{
    /** cv 修飾を除いた実値型。 */
    using ValueType = RemoveCVT<T>;
    static_assert(endian_detail::IsEndianSerializableV<ValueType>, "little endian 直列化は 1/2/4/8 byte scalar または enum だけを受け付けます");
    /** 値の object representation を保持する同幅整数。 */
    using StorageType = endian_detail::UnsignedStorageT<ValueType>;
    /** host 上の bit pattern。 */
    StorageType Bits = 0u;
    std::memcpy(&Bits, &Value, sizeof(Bits));
    /** 書き出す little endian byte 位置。 */
    for (usize Index = 0u; Index < sizeof(Bits); ++Index) {
        Destination[Index] = static_cast<u8>(Bits >> (Index * 8u));
    }
}

/**
 * little endian byte 列を host endian に依存せず固定幅 scalar または enum へ戻す。
 *
 * @tparam T 読み戻す固定幅型。
 * @param Source sizeof(T) byte 以上の読み取り元。
 * @return 復元した値。
 */
template<typename T>
ACS_FORCEINLINE T ReadLittleEndian(const u8* Source) noexcept
{
    /** cv 修飾を除いた実値型。 */
    using ValueType = RemoveCVT<T>;
    static_assert(endian_detail::IsEndianSerializableV<ValueType>, "little endian 直列化は 1/2/4/8 byte scalar または enum だけを受け付けます");
    /** 値の object representation を保持する同幅整数。 */
    using StorageType = endian_detail::UnsignedStorageT<ValueType>;
    /** little endian byte を組み立てた host 整数。 */
    StorageType Bits = 0u;
    /** 読み取る little endian byte 位置。 */
    for (usize Index = 0u; Index < sizeof(Bits); ++Index) {
        Bits |= static_cast<StorageType>(Source[Index]) << (Index * 8u);
    }
    /** 復元先の値。 */
    ValueType Value{};
    std::memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

} // namespace acs
