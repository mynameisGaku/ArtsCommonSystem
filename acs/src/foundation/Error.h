// ACS Foundation — Error categorization for Result<T,E>.
//
// ErrorCode is a small POD with a category, a numeric code, and an optional
// message pointer. It is meant to be embedded inside Result<T, ErrorCode> and
// surfaces enough information for callers to either log or recover.
#pragma once

#include "foundation/Types.h"
#include "foundation/SourceLoc.h"

namespace acs {

enum class ErrCategory : u16 {
    None        = 0,
    Generic     = 1,
    Memory      = 2,    // allocation failure, OOM, alignment violation
    OS          = 3,    // OS / Win32 syscall failure
    IO          = 4,    // file / network IO failure
    Container   = 5,    // out-of-bounds, capacity overflow, key not found
    Threading   = 6,    // mutex contention, deadlock detected, thread spawn fail
    Math        = 7,    // domain error, NaN, divide by zero
    Render      = 8,    // graphics backend failure (DX12 etc.)
    Asset       = 9,    // asset load / decode failure
    UserCancel  = 10,
};

struct ErrorCode {
    ErrCategory category   = ErrCategory::None;
    u16         subcode    = 0;       // category-specific
    u32         os_error   = 0;       // GetLastError() / HRESULT (if applicable)
    const char* message    = nullptr; // static-storage string literal preferred
    SourceLoc   loc        = {};

    constexpr ErrorCode() noexcept = default;

    constexpr ErrorCode(ErrCategory c,
                        u16 sub,
                        const char* msg,
                        SourceLoc l = SourceLoc::Current(),
                        u32 os = 0) noexcept
        : category(c), subcode(sub), os_error(os), message(msg), loc(l) {}

    constexpr bool IsOk() const noexcept { return category == ErrCategory::None; }
    constexpr explicit operator bool() const noexcept { return !IsOk(); }
};

constexpr const char* ToString(ErrCategory c) noexcept {
    switch (c) {
        case ErrCategory::None:       return "None";
        case ErrCategory::Generic:    return "Generic";
        case ErrCategory::Memory:     return "Memory";
        case ErrCategory::OS:         return "OS";
        case ErrCategory::IO:         return "IO";
        case ErrCategory::Container:  return "Container";
        case ErrCategory::Threading:  return "Threading";
        case ErrCategory::Math:       return "Math";
        case ErrCategory::Render:     return "Render";
        case ErrCategory::Asset:      return "Asset";
        case ErrCategory::UserCancel: return "UserCancel";
    }
    return "Unknown";
}

// Convenience constructors used throughout the codebase.
#define ACS_ERR(cat, sub, msg) \
    ::acs::ErrorCode(::acs::ErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::SourceLoc::Current())

#define ACS_ERR_OS(cat, sub, msg, os_err) \
    ::acs::ErrorCode(::acs::ErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::SourceLoc::Current(), (os_err))

} // namespace acs
