// SPDX-License-Identifier: Apache-2.0
// 連続メモリ領域への非所有ビュー（std::span 代替）。
// 所有権を持たないため、参照先のライフタイムに注意すること。
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

template<typename T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(T* data, usize size) noexcept : _data(data), _size(size) {}

    // C 配列からの暗黙変換（テンプレート引数 N で長さを推論）
    template<usize N>
    constexpr Span(T (&arr)[N]) noexcept : _data(arr), _size(N) {}

    constexpr T*       Data()       noexcept { return _data; }
    constexpr const T* Data() const noexcept { return _data; }
    constexpr usize    Size() const noexcept { return _size; }
    constexpr bool     IsEmpty() const noexcept { return _size == 0; }

    // 範囲外アクセスはアサート (Debug ビルドのみ検出)
    constexpr T&       operator[](usize i)       noexcept { ACS_ASSERT(i < _size); return _data[i]; }
    constexpr const T& operator[](usize i) const noexcept { ACS_ASSERT(i < _size); return _data[i]; }

    constexpr T*       begin()       noexcept { return _data; }
    constexpr T*       end()         noexcept { return _data + _size; }
    constexpr const T* begin() const noexcept { return _data; }
    constexpr const T* end()   const noexcept { return _data + _size; }

    // 部分範囲を取り出す (範囲外は ASSERT)
    constexpr Span SubSpan(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset + count <= _size);
        return Span(_data + offset, count);
    }

private:
    T*    _data = nullptr;
    usize _size = 0;
};

} // namespace acs
