// ACS Container — Non-owning UTF-8 string view.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"

namespace acs {

class StringView {
public:
    constexpr StringView() noexcept = default;
    constexpr StringView(const char* data, usize size) noexcept : _data(data), _size(size) {}

    // From C-string. Computes length once.
    StringView(const char* cstr) noexcept : _data(cstr), _size(0) {
        if (cstr) while (cstr[_size]) ++_size;
    }

    constexpr const char* Data() const noexcept { return _data; }
    constexpr usize       Size() const noexcept { return _size; }
    constexpr bool        IsEmpty() const noexcept { return _size == 0; }

    constexpr char        operator[](usize i) const noexcept { ACS_ASSERT(i < _size); return _data[i]; }

    constexpr StringView SubView(usize offset, usize count) const noexcept {
        ACS_ASSERT(offset + count <= _size);
        return StringView(_data + offset, count);
    }

    constexpr const char* begin() const noexcept { return _data; }
    constexpr const char* end()   const noexcept { return _data + _size; }

    bool Equals(StringView other) const noexcept {
        if (_size != other._size) return false;
        for (usize i = 0; i < _size; ++i) if (_data[i] != other._data[i]) return false;
        return true;
    }

    bool StartsWith(StringView prefix) const noexcept {
        if (prefix._size > _size) return false;
        for (usize i = 0; i < prefix._size; ++i) if (_data[i] != prefix._data[i]) return false;
        return true;
    }

    bool EndsWith(StringView suffix) const noexcept {
        if (suffix._size > _size) return false;
        usize off = _size - suffix._size;
        for (usize i = 0; i < suffix._size; ++i) if (_data[off + i] != suffix._data[i]) return false;
        return true;
    }

private:
    const char* _data = nullptr;
    usize       _size = 0;
};

inline bool operator==(StringView a, StringView b) noexcept { return a.Equals(b); }
inline bool operator!=(StringView a, StringView b) noexcept { return !a.Equals(b); }

} // namespace acs
