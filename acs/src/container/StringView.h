// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FStringView（std::string_view 代替、UTF-8 非所有ビュー）
// -----------------------------------------------------------------------------
// ポインタ + 長さで文字列を参照する軽量値型。所有権を持たないため、
// 元の文字列のライフタイムに注意。
//
// UTF-8 を前提とする（バイト単位での比較／検索のみ提供、コードポイント
// 操作は未対応）。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

class FStringView {
public:
    constexpr FStringView() noexcept = default;
    constexpr FStringView(const char* data, usize size) noexcept : _data(data), _size(size) {}

    // C 文字列からの暗黙変換（NUL 終端を見つけて長さを計算）
    FStringView(const char* cstr) noexcept : _data(cstr), _size(0) {
        if (cstr) while (cstr[_size]) ++_size;
    }

    constexpr const char* Data() const noexcept { return _data; }
    constexpr usize       Size() const noexcept { return _size; }
    constexpr bool        IsEmpty() const noexcept { return _size == 0; }

    constexpr char        operator[](usize i) const noexcept { ACS_ASSERT(i < _size); return _data[i]; }

    // 部分文字列ビュー
    constexpr FStringView SubView(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset + count <= _size);
        return FStringView(_data + offset, count);
    }

    constexpr const char* begin() const noexcept { return _data; }
    constexpr const char* end()   const noexcept { return _data + _size; }

    // バイト単位の完全一致比較
    bool Equals(FStringView other) const noexcept {
        if (_size != other._size) return false;
        for (usize i = 0; i < _size; ++i) if (_data[i] != other._data[i]) return false;
        return true;
    }

    // 接頭辞判定
    bool StartsWith(FStringView prefix) const noexcept {
        if (prefix._size > _size) return false;
        for (usize i = 0; i < prefix._size; ++i) if (_data[i] != prefix._data[i]) return false;
        return true;
    }

    // 接尾辞判定
    bool EndsWith(FStringView suffix) const noexcept {
        if (suffix._size > _size) return false;
        usize off = _size - suffix._size;
        for (usize i = 0; i < suffix._size; ++i) if (_data[off + i] != suffix._data[i]) return false;
        return true;
    }

private:
    const char* _data = nullptr;
    usize       _size = 0;
};

inline bool operator==(FStringView a, FStringView b) noexcept { return a.Equals(b); }
inline bool operator!=(FStringView a, FStringView b) noexcept { return !a.Equals(b); }

} // namespace acs
