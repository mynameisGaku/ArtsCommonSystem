// ACS Foundation — Result<T, E>.
//
// Exception-free error propagation. Matches the spirit of std::expected /
// Rust's Result. Constraints:
//   * No STL.
//   * Trivially destructible types pay zero overhead in storage.
//   * Movable-only types (Ptr<T>, ThreadHandle...) work without copy.
//   * E defaults to ErrorCode but any type works.
//
// Usage:
//   Result<File, ErrorCode> r = OpenFile("foo");
//   if (!r) { Logger::Error("open failed: {}", r.Error().message); return; }
//   File& f = r.Value();
#pragma once

#include "foundation/Types.h"
#include "foundation/TypeTraits.h"
#include "foundation/Move.h"
#include "foundation/Error.h"

namespace acs {

namespace detail {
struct OkTag    {};
struct ErrTag   {};
struct EmptyTag {};
} // namespace detail

inline constexpr detail::OkTag    OkInit {};
inline constexpr detail::ErrTag   ErrInit{};

template<typename T, typename E = ErrorCode>
class Result {
public:
    using ValueType = T;
    using ErrorType = E;

    // ---- Construction ------------------------------------------------------
    template<typename U = T>
    Result(U&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Forward<U>(v));
    }

    Result(detail::OkTag, T&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Move(v));
    }

    Result(detail::ErrTag, E&& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }

    // Implicit conversion from an ErrorCode (or any E) marks failure.
    Result(const E& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    Result(const Result& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }

    Result(Result&& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }

    Result& operator=(const Result& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }

    ~Result() noexcept { Destroy(); }

    // ---- Inspect -----------------------------------------------------------
    bool IsOk()  const noexcept { return _has_value; }
    bool IsErr() const noexcept { return !_has_value; }
    explicit operator bool() const noexcept { return _has_value; }

    T&       Value()       noexcept { return _storage._value; }
    const T& Value() const noexcept { return _storage._value; }

    E&       Error()       noexcept { return _storage._error; }
    const E& Error() const noexcept { return _storage._error; }

    T ValueOr(T fallback) const noexcept {
        return _has_value ? _storage._value : fallback;
    }

private:
    void Destroy() noexcept {
        if (_has_value) {
            if constexpr (!IsTriviallyDestructibleV<T>) _storage._value.~T();
        } else {
            if constexpr (!IsTriviallyDestructibleV<E>) _storage._error.~E();
        }
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        T _value;
        E _error;
    } _storage;
    bool _has_value;
};

// Specialization for Result<void, E> — used by functions that return only success/error.
template<typename E>
class Result<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    Result() noexcept : _has_value(true) {}
    Result(detail::OkTag) noexcept : _has_value(true) {}
    Result(detail::ErrTag, E&& e) noexcept : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }
    Result(const E& e) noexcept : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    Result(const Result& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }
    Result(Result&& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }
    Result& operator=(const Result& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }
    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }
    ~Result() noexcept { Destroy(); }

    bool IsOk()  const noexcept { return _has_value; }
    bool IsErr() const noexcept { return !_has_value; }
    explicit operator bool() const noexcept { return _has_value; }

    E&       Error()       noexcept { return _storage._error; }
    const E& Error() const noexcept { return _storage._error; }

private:
    void Destroy() noexcept {
        if (!_has_value) if constexpr (!IsTriviallyDestructibleV<E>) _storage._error.~E();
    }
    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        u8 _pad;
        E  _error;
    } _storage;
    bool _has_value;
};

// ---- Helpers --------------------------------------------------------------

template<typename T> Result<T> Ok(T v) noexcept { return Result<T>(OkInit, Move(v)); }
inline Result<void> Ok() noexcept { return Result<void>(OkInit); }

template<typename T = void>
Result<T> Err(ErrorCode e) noexcept { return Result<T>(e); }

// Macro: propagate error if Result is Err.
//   ACS_TRY(result_expr);                  // for Result<void>
//   ACS_TRY_ASSIGN(local_name, expr);      // for Result<T> — binds value
#define ACS_TRY(expr)                                                         \
    do {                                                                      \
        auto _acs_try_r = (expr);                                             \
        if (_acs_try_r.IsErr()) return ::acs::Err(_acs_try_r.Error());        \
    } while (0)

#define ACS_TRY_ASSIGN(name, expr)                                            \
    auto ACS_CONCAT(_acs_tmp_, __LINE__) = (expr);                            \
    if (ACS_CONCAT(_acs_tmp_, __LINE__).IsErr())                              \
        return ::acs::Err(ACS_CONCAT(_acs_tmp_, __LINE__).Error());           \
    auto&& name = ACS_CONCAT(_acs_tmp_, __LINE__).Value()

} // namespace acs
