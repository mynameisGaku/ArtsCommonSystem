// SPDX-License-Identifier: Apache-2.0
// FString 関連の暗黙変換実装
#include "mvvm/Convert.h"
#include "container/String.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acs::mvvm {

// ---------- 数値 → FString ----------
FString DefaultConverter<i32, FString>::Convert(const i32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return FString{buf};
}
FString DefaultConverter<u32, FString>::Convert(const u32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return FString{buf};
}
FString DefaultConverter<f32, FString>::Convert(const f32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<f64>(v));
    return FString{buf};
}
FString DefaultConverter<f64, FString>::Convert(const f64& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return FString{buf};
}
FString DefaultConverter<bool, FString>::Convert(const bool& v, void*) noexcept {
    return FString{ v ? "true" : "false" };
}

// ---------- FString → 数値 (パース失敗時は 0/false) ----------
i32 DefaultConverter<FString, i32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0;
    return static_cast<i32>(std::strtol(s, nullptr, 10));
}
u32 DefaultConverter<FString, u32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0;
    return static_cast<u32>(std::strtoul(s, nullptr, 10));
}
f32 DefaultConverter<FString, f32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0.0f;
    return std::strtof(s, nullptr);
}
f64 DefaultConverter<FString, f64>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0.0;
    return std::strtod(s, nullptr);
}
bool DefaultConverter<FString, bool>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return false;
    // "true" / "True" / "TRUE" / "1" を true、それ以外を false
    if (std::strcmp(s, "true") == 0 ||
        std::strcmp(s, "True") == 0 ||
        std::strcmp(s, "TRUE") == 0 ||
        std::strcmp(s, "1")    == 0) return true;
    return false;
}

} // namespace acs::mvvm
