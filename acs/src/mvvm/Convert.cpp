// SPDX-License-Identifier: Apache-2.0
// FString 関連の暗黙変換実装
#include "mvvm/Convert.h"
#include "container/String.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acs::mvvm {

/** i32 を "%d" で整形した FString を返す。 */
FString TDefaultConverter<i32, FString>::Convert(const i32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return FString{buf};
}

/** u32 を "%u" で整形した FString を返す。 */
FString TDefaultConverter<u32, FString>::Convert(const u32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return FString{buf};
}

/** f32 を "%g" で整形した FString を返す。 */
FString TDefaultConverter<f32, FString>::Convert(const f32& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<f64>(v));
    return FString{buf};
}

/** f64 を "%g" で整形した FString を返す。 */
FString TDefaultConverter<f64, FString>::Convert(const f64& v, void*) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return FString{buf};
}

/** bool を "true"/"false" の FString に変換して返す。 */
FString TDefaultConverter<bool, FString>::Convert(const bool& v, void*) noexcept {
    return FString{ v ? "true" : "false" };
}

/** 文字列を 10 進 i32 へパースする (null は 0)。 */
i32 TDefaultConverter<FString, i32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0;
    return static_cast<i32>(std::strtol(s, nullptr, 10));
}

/** 文字列を 10 進 u32 へパースする (null は 0)。 */
u32 TDefaultConverter<FString, u32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0;
    return static_cast<u32>(std::strtoul(s, nullptr, 10));
}

/** 文字列を f32 へパースする (null は 0.0f)。 */
f32 TDefaultConverter<FString, f32>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0.0f;
    return std::strtof(s, nullptr);
}

/** 文字列を f64 へパースする (null は 0.0)。 */
f64 TDefaultConverter<FString, f64>::Convert(const FString& v, void*) noexcept {
    const char* s = v.Data();
    if (!s) return 0.0;
    return std::strtod(s, nullptr);
}

/** 文字列を bool へパースする ("true"/"True"/"TRUE"/"1" のみ true)。 */
bool TDefaultConverter<FString, bool>::Convert(const FString& v, void*) noexcept {
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
