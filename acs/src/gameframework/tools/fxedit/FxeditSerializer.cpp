// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — `.fxedit` テキストシリアライザ実装
//
// テキスト形式の詳細仕様は CFxeditSerializer.h を参照。
//
// 実装の主な決定:
//   ・I/O は Win32 handle を直接使い、完全 read と durable atomic replace を保証する。
//   ・出力 buffer は `acs::TArray<char>` で動的成長させる (emitter 数に依存)。
//     `Reserve(count * kMaxBytesPerEmitter + 64)` で最初から十分な容量を確保し、
//     `AppendXxx` が再 alloc しないようにする (= STL `<sstream>` 不要)。
//   ・数値は locale 非依存の from_chars/to_chars + max_digits10 で往復する。
//   ・パースは strtok 等を使わず手書き状態機械にする (`<cstring>` の strtok は
//     非 thread-safe で warning が出るプラットフォームがある)。
//
// 注意:
//   ・本 .cpp は ParticleEmitterDef の完全型を必要とするので、ヘッダ側の
//     forward decl だけでなく `FParticleEffectSystem.h` を include する。
//   ・実 ParticleEmitterDef の color は FVec3 (alpha 無し)、gravity は FVec2。
//     テキスト形式は前方互換用に色 4 成分 / 重力 3 成分まで書く / 読むが、
//     格納できない第 4 成分 (color) / 第 3 成分 (gravity) は破棄する。
//   ・spread_radians は ParticleEmitterDef にフィールドが無いため、書き出し時は
//     0 で固定、読み込み時はパースして捨てる (将来フィールド追加時に値結合)。

#include "gameframework/tools/fxedit/FxeditSerializer.h"

#include "gameframework/ParticleEffectSystem.h"  // ParticleEmitterDef 完全型
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "container/Array.h"                     // Array<char>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>    // std::snprintf
#include <cwchar>
#include <cstring>   // std::memcpy / std::strncmp
#include <limits>

namespace acs::game::fxedit {

namespace {

/**
 * ASCII printable 範囲かを返す (制御文字を排除する用途)。
 *
 * @param c 判定する文字。
 * @return 0x20..0x7E なら true。
 */
inline bool IsAsciiPrintable(char c) noexcept {
    return c >= 0x20 && c < 0x7F;
}

/**
 * 終端文字列の長さを上限付きで計算する。
 *
 * @details `std::strlen` 相当だが上限付きで NUL なし入力の暴走を防ぐ。
 * @param s 対象文字列 (nullptr 可)。
 * @param max_len 走査する最大バイト数。
 * @return NUL または max_len までの長さ。s が nullptr なら 0。
 */
inline usize StrLenBounded(const char* s, usize max_len) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (n < max_len && s[n] != '\0') ++n;
    return n;
}

/**
 * out に len バイト分の文字列を append する (終端 NUL は書かない)。
 *
 * @param out 追記先のバイト配列。
 * @param s コピー元 (len バイト)。
 * @param len 追記するバイト数。
 * @return 追記できたら true、alloc 失敗なら false。
 */
inline bool AppendStr(TArray<char>& out, const char* s, usize len) noexcept {
    const usize old = out.Size();
    if (len > std::numeric_limits<usize>::max() - old ||
        !out.TryResize(old + len)) return false;
    std::memcpy(out.Data() + old, s, len);
    return true;
}

/**
 * "<key> v0 [v1 [v2 [v3]]]\n" 行を append する。
 *
 * @details 数値は locale 非依存の `to_chars(max_digits10)` で出力する。
 * @param out 追記先のバイト配列。
 * @param prefix non-null なら "<prefix> " を頭に付ける ("E0" など)。null なら付けない。
 * @param key 出力する key 文字列。
 * @param values 出力する数値配列 (count 個)。
 * @param count 値の個数 (1..4 にクランプ)。
 * @return 追記できたら true、整形溢れ / alloc 失敗なら false。
 */
bool AppendKeyValueLine(TArray<char>& out,
                        const char*  prefix,
                        const char*  key,
                        const f32*   values,
                        u32          count) noexcept {
    char line[160];
    int n = 0;
    if (prefix != nullptr) {
        n = std::snprintf(line, sizeof(line), "%s %s", prefix, key);
    } else {
        n = std::snprintf(line, sizeof(line), "%s", key);
    }
    if (n < 0 || static_cast<usize>(n) >= sizeof(line)) return false;
    for (u32 i = 0; i < count && i < 4u; ++i) {
        if (!std::isfinite(values[i]) || static_cast<usize>(n) + 1u >= sizeof(line)) {
            return false;
        }
        line[n++] = ' ';
        const std::to_chars_result converted = std::to_chars(
            line + n, line + sizeof(line), values[i],
            std::chars_format::general, std::numeric_limits<f32>::max_digits10);
        if (converted.ec != std::errc{}) return false;
        n = static_cast<int>(converted.ptr - line);
    }
    if (static_cast<usize>(n) + 1 >= sizeof(line)) return false;
    line[n++] = '\n';
    return AppendStr(out, line, static_cast<usize>(n));
}

/**
 * "<prefix> name \"<value>\"\n" を append する (引用符付き文字列 key 用)。
 *
 * @details name 内の非 printable 文字と `"` は `_` に置換し、kMaxEmitterName で切り詰める。
 * @param out 追記先のバイト配列。
 * @param prefix 行頭に付ける emitter prefix ("E0" など)。
 * @param name 出力する emitter 名 (nullptr は "" 扱い)。
 * @return 追記できたら true、整形溢れ / alloc 失敗なら false。
 */
bool AppendNameLine(TArray<char>& out,
                    const char*  prefix,
                    const char*  name) noexcept {
    char line[96];
    const char* safe_name = name != nullptr ? name : "";
    // 簡素化のため、name 内の `"` は `'` に置き換える防御。
    char sanitized[CFxeditSerializer::kMaxEmitterName + 1];
    usize j = 0;
    for (usize i = 0;
         safe_name[i] != '\0' && j < CFxeditSerializer::kMaxEmitterName;
         ++i) {
        char c = safe_name[i];
        if (!IsAsciiPrintable(c) || c == '"') c = '_';
        sanitized[j++] = c;
    }
    sanitized[j] = '\0';

    int n = std::snprintf(line, sizeof(line), "%s name \"%s\"\n",
                          prefix, sanitized);
    if (n < 0 || static_cast<usize>(n) >= sizeof(line)) return false;
    return AppendStr(out, line, static_cast<usize>(n));
}

/**
 * テキスト本体から 1 行を取り出す。
 *
 * @details
 * text[pos..] から `\n` または `\0` まで読み、行内容を out_line に NUL 終端付きで
 * 書き込み (末尾 \r は除去)、pos を次の行頭まで進める。out_line は out_line_capacity
 * バイト確保しておく前提。
 * @param text テキスト本体。
 * @param text_len text の長さ (バイト)。
 * @param pos 走査位置。読み終えた次の行頭に更新される。
 * @param out_line 行内容の書き込み先 (NUL 終端付き)。
 * @param out_line_capacity out_line のバイト数。
 * @return 行を取り出した (空行含む) なら true、入力が末端なら false。
 */
bool ReadOneLine(const char* text,
                 usize       text_len,
                 usize&      pos,
                 char*       out_line,
                 usize       out_line_capacity) noexcept {
    if (pos >= text_len || text[pos] == '\0') return false;
    usize n = 0;
    bool overflow = false;
    while (pos < text_len && text[pos] != '\n' && text[pos] != '\0') {
        if (n + 1 < out_line_capacity) {  // 末尾 NUL 用に 1 残す
            out_line[n++] = text[pos];
        } else {
            overflow = true;
        }
        ++pos;
    }
    if (pos < text_len && text[pos] == '\n') ++pos;
    // 末尾 \r を除去 (Windows CRLF 対応)。
    if (n > 0 && out_line[n - 1] == '\r') --n;
    out_line[n] = '\0';
    return !overflow;
}

/**
 * 行頭の "ACS_FXEDIT <v>" から version を読む。
 *
 * @param line 検査する 1 行。
 * @return 読めた version。magic 不一致 / 数値が無い場合は 0。
 */
u32 ParseMagicLine(const char* line) noexcept {
    const char* p = line;
    // 先頭の空白スキップ。
    while (*p == ' ' || *p == '\t') ++p;
    const char* magic = CFxeditSerializer::kMagic;
    usize mlen = 0;
    while (magic[mlen] != '\0') ++mlen;
    if (std::strncmp(p, magic, mlen) != 0) return 0;
    p += mlen;
    if (*p != ' ' && *p != '\t') return 0;
    while (*p == ' ' || *p == '\t') ++p;
    // 残りを十進数として読む。
    u32 v = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        const u32 digit = static_cast<u32>(*p - '0');
        if (v > (std::numeric_limits<u32>::max() - digit) / 10u) return 0u;
        v = v * 10u + digit;
        ++p;
        any = true;
    }
    while (*p == ' ' || *p == '\t') ++p;
    return any && (*p == '\0' || *p == '#') ? v : 0u;
}

/**
 * "E<digits>" prefix から emitter index を取り出す。
 *
 * @param key_begin key スライスの開始ポインタ。
 * @param key_end key スライスの終端ポインタ (exclusive)。
 * @param out_index 取り出した index の格納先。
 * @return 正しく解析できたら true、形式不正なら false。
 */
bool ParseEmitterIndex(const char* key_begin,
                      const char* key_end,
                      u32&        out_index) noexcept {
    if (key_begin >= key_end) return false;
    if (*key_begin != 'E' && *key_begin != 'e') return false;
    const char* p = key_begin + 1;
    if (p >= key_end || *p < '0' || *p > '9') return false;
    u32 v = 0;
    while (p < key_end && *p >= '0' && *p <= '9') {
        const u32 digit = static_cast<u32>(*p - '0');
        if (v > (std::numeric_limits<u32>::max() - digit) / 10u) return false;
        v = v * 10u + digit;
        ++p;
    }
    if (p != key_end) return false;
    out_index = v;
    return true;
}

/**
 * key スライスが固定リテラルと等しいかを比較する。
 *
 * @param key_begin key スライスの開始ポインタ。
 * @param key_end key スライスの終端ポインタ (exclusive)。
 * @param lit 比較対象の NUL 終端リテラル。
 * @return key スライスと lit が完全一致なら true。
 */
bool KeyEquals(const char* key_begin,
              const char* key_end,
              const char* lit) noexcept {
    usize i = 0;
    while (key_begin + i < key_end && lit[i] != '\0') {
        if (key_begin[i] != lit[i]) return false;
        ++i;
    }
    return (key_begin + i == key_end) && lit[i] == '\0';
}

enum class ENumberStatus : u8 { Ok, Invalid, OutOfRange };

void SkipHorizontalSpace(const char*& p) noexcept {
    while (*p == ' ' || *p == '\t') ++p;
}

bool AtValueEnd(const char* p) noexcept {
    SkipHorizontalSpace(p);
    return *p == '\0' || *p == '#';
}

ENumberStatus ParseU32Token(const char*& p, u32& out) noexcept {
    SkipHorizontalSpace(p);
    if (*p < '0' || *p > '9') return ENumberStatus::Invalid;
    u32 value = 0u;
    do {
        const u32 digit = static_cast<u32>(*p - '0');
        if (value > (std::numeric_limits<u32>::max() - digit) / 10u) {
            return ENumberStatus::OutOfRange;
        }
        value = value * 10u + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    out = value;
    return ENumberStatus::Ok;
}

ENumberStatus ParseFiniteFloatToken(const char*& p, f32& out) noexcept {
    SkipHorizontalSpace(p);
    if (*p == '\0' || *p == '#') return ENumberStatus::Invalid;
    const char* end = p;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '#') ++end;
    const char* conversion_begin = *p == '+' ? p + 1 : p;
    if (conversion_begin == end) return ENumberStatus::Invalid;
    f32 value = 0.0f;
    const std::from_chars_result converted = std::from_chars(
        conversion_begin, end, value, std::chars_format::general);
    if (converted.ec == std::errc::result_out_of_range) return ENumberStatus::OutOfRange;
    if (converted.ec != std::errc{} || converted.ptr != end) return ENumberStatus::Invalid;
    if (!std::isfinite(value)) return ENumberStatus::OutOfRange;
    p = end;
    out = value;
    return ENumberStatus::Ok;
}

ENumberStatus ParseFloatValues(
    const char* text, f32* values, u32 count) noexcept {
    if (text == nullptr || values == nullptr || count == 0u) return ENumberStatus::Invalid;
    const char* p = text;
    for (u32 i = 0u; i < count; ++i) {
        const ENumberStatus status = ParseFiniteFloatToken(p, values[i]);
        if (status != ENumberStatus::Ok) return status;
    }
    return AtValueEnd(p) ? ENumberStatus::Ok : ENumberStatus::Invalid;
}

bool IsBoundedWidePath(
    const wchar_t* path, usize max_chars, usize& out_length) noexcept {
    out_length = 0u;
    if (path == nullptr) return false;
    while (out_length <= max_chars && path[out_length] != L'\0') ++out_length;
    return out_length > 0u && out_length <= max_chars;
}

enum class EFxKnownKey : u8 {
    Name = 0,
    EmitRate,
    Lifetime,
    BurstCount,
    SpeedMin,
    SpeedMax,
    ScaleStart,
    ScaleEnd,
    Gravity,
    ColorStart,
    ColorEnd,
    SpreadRadians,
    Curve,
    Keyframe,
    Unknown,
};

EFxKnownKey FxKnownKey(const char* begin, const char* end) noexcept {
    if (KeyEquals(begin, end, "name")) return EFxKnownKey::Name;
    if (KeyEquals(begin, end, "emit_rate")) return EFxKnownKey::EmitRate;
    if (KeyEquals(begin, end, "lifetime_sec")) return EFxKnownKey::Lifetime;
    if (KeyEquals(begin, end, "burst_count")) return EFxKnownKey::BurstCount;
    if (KeyEquals(begin, end, "speed_min")) return EFxKnownKey::SpeedMin;
    if (KeyEquals(begin, end, "speed_max")) return EFxKnownKey::SpeedMax;
    if (KeyEquals(begin, end, "scale_start")) return EFxKnownKey::ScaleStart;
    if (KeyEquals(begin, end, "scale_end")) return EFxKnownKey::ScaleEnd;
    if (KeyEquals(begin, end, "gravity")) return EFxKnownKey::Gravity;
    if (KeyEquals(begin, end, "color_start")) return EFxKnownKey::ColorStart;
    if (KeyEquals(begin, end, "color_end")) return EFxKnownKey::ColorEnd;
    if (KeyEquals(begin, end, "spread_radians")) return EFxKnownKey::SpreadRadians;
    if (KeyEquals(begin, end, "curve")) return EFxKnownKey::Curve;
    if (KeyEquals(begin, end, "keyframe")) return EFxKnownKey::Keyframe;
    return EFxKnownKey::Unknown;
}

bool IsScalarFxKey(EFxKnownKey key) noexcept {
    return key >= EFxKnownKey::Name && key <= EFxKnownKey::SpreadRadians;
}

EFxeditSerializeError ValidateEmitterDef(const FParticleEmitterDef& def) noexcept {
    const f32 values[] = {
        def.emit_rate_per_sec, def.lifetime_sec, def.burst_count,
        def.speed_min, def.speed_max, def.scale_start, def.scale_end,
        def.gravity.x, def.gravity.y,
        def.color_start.x, def.color_start.y, def.color_start.z,
        def.color_end.x, def.color_end.y, def.color_end.z,
    };
    for (f32 value : values) {
        if (!std::isfinite(value)) return EFxeditSerializeError::ValueOutOfRange;
    }
    if (def.emit_rate_per_sec < 0.0f || def.emit_rate_per_sec > 1000000.0f ||
        def.lifetime_sec <= 0.0f || def.lifetime_sec > 86400.0f ||
        def.burst_count < 0.0f || def.burst_count > 1000000.0f ||
        def.speed_min < -1000000000.0f || def.speed_min > 1000000000.0f ||
        def.speed_max < -1000000000.0f || def.speed_max > 1000000000.0f ||
        def.speed_min > def.speed_max ||
        def.scale_start < 0.0f || def.scale_start > 1000000.0f ||
        def.scale_end < 0.0f || def.scale_end > 1000000.0f ||
        std::fabs(def.gravity.x) > 1000000000.0f ||
        std::fabs(def.gravity.y) > 1000000000.0f) {
        return EFxeditSerializeError::ValueOutOfRange;
    }
    const f32 colors[] = {
        def.color_start.x, def.color_start.y, def.color_start.z,
        def.color_end.x, def.color_end.y, def.color_end.z,
    };
    for (f32 value : colors) {
        if (value < 0.0f || value > 1.0f) return EFxeditSerializeError::ValueOutOfRange;
    }
    return EFxeditSerializeError::None;
}

EFxeditSerializeError ValidateEmitterName(const char* name) noexcept {
    if (name == nullptr) return EFxeditSerializeError::None;
    const usize length =
        StrLenBounded(name, CFxeditSerializer::kMaxEmitterName + 1u);
    if (length > CFxeditSerializer::kMaxEmitterName) {
        return EFxeditSerializeError::NameTooLong;
    }
    for (usize i = 0u; i < length; ++i) {
        if (!IsAsciiPrintable(name[i]) || name[i] == '"') {
            return EFxeditSerializeError::InvalidName;
        }
    }
    return EFxeditSerializeError::None;
}

u16 LegacySubCode(EFxeditSerializeError error) noexcept {
    switch (error) {
        case EFxeditSerializeError::None: return CFxeditSerializer::kSub_OK;
        case EFxeditSerializeError::NullArgument:
        case EFxeditSerializeError::PathTooLong:
            return CFxeditSerializer::kSub_NullArgs;
        case EFxeditSerializeError::TooManyEmitters:
            return CFxeditSerializer::kSub_TooManyEmitters;
        case EFxeditSerializeError::BufferTooSmall:
            return CFxeditSerializer::kSub_BufferOverflow;
        case EFxeditSerializeError::BadMagic:
            return CFxeditSerializer::kSub_BadMagic;
        case EFxeditSerializeError::UnsupportedVersion:
            return CFxeditSerializer::kSub_BadVersion;
        case EFxeditSerializeError::FileNotFound:
            return CFxeditSerializer::kSub_FileNotFound;
        case EFxeditSerializeError::AllocationFailure:
            return CFxeditSerializer::kSub_AllocationFailure;
        case EFxeditSerializeError::AtomicReplaceFailed:
            return CFxeditSerializer::kSub_AtomicReplace;
        case EFxeditSerializeError::FileOpenFailed:
        case EFxeditSerializeError::FileSizeFailed:
        case EFxeditSerializeError::FileChanged:
        case EFxeditSerializeError::FileReadFailed:
        case EFxeditSerializeError::FileWriteFailed:
        case EFxeditSerializeError::FileFlushFailed:
        case EFxeditSerializeError::FileCloseFailed:
            return CFxeditSerializer::kSub_IOFailure;
        default:
            return CFxeditSerializer::kSub_ValidationFailed;
    }
}

bool BuildUniqueTempPath(
    const wchar_t* destination, usize destination_length,
    wchar_t* out, usize capacity, u32 attempt) noexcept {
    wchar_t suffix[96]{};
    static volatile LONG counter = 0;
    const LONG serial = ::InterlockedIncrement(&counter);
    const int suffix_length = std::swprintf(
        suffix, sizeof(suffix) / sizeof(suffix[0]),
        L".tmp.%lu.%lu.%ld.%u",
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<long>(serial), attempt);
    if (suffix_length <= 0) return false;
    const usize suffix_size = static_cast<usize>(suffix_length);
    if (destination_length + suffix_size + 1u > capacity) return false;
    std::memcpy(out, destination, destination_length * sizeof(wchar_t));
    std::memcpy(out + destination_length, suffix, (suffix_size + 1u) * sizeof(wchar_t));
    return true;
}

struct FFileRenameInfoEx {
    DWORD flags = 0u;
    HANDLE root_directory = nullptr;
    DWORD file_name_length = 0u;
    wchar_t file_name[1]{};
};

bool TryPosixAtomicReplace(
    const wchar_t* temporary_path,
    const wchar_t* destination,
    usize destination_length,
    DWORD& out_error) noexcept {
    constexpr DWORD kRenameReplaceIfExists = 0x00000001u;
    constexpr DWORD kRenamePosixSemantics = 0x00000002u;
    constexpr auto kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    constexpr usize kPrefixBytes = offsetof(FFileRenameInfoEx, file_name);
    alignas(FFileRenameInfoEx)
        u8 storage[kPrefixBytes +
                   (CFxeditSerializer::kMaxPathChars + 1u) * sizeof(wchar_t)]{};
    auto* info = reinterpret_cast<FFileRenameInfoEx*>(storage);
    const usize destination_bytes = destination_length * sizeof(wchar_t);
    info->flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->file_name_length = static_cast<DWORD>(destination_bytes);
    std::memcpy(info->file_name, destination, destination_bytes);

    HANDLE source = ::CreateFileW(
        temporary_path, DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }
    const DWORD info_bytes =
        static_cast<DWORD>(kPrefixBytes + destination_bytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        source, kFileRenameInfoEx, info, info_bytes);
    if (!renamed) out_error = ::GetLastError();
    // rename 成功時点で commit 済み。close diagnostic で成功を失敗へ戻さない。
    (void)::CloseHandle(source);
    return renamed != 0;
}

} // namespace

/** ' ' / '\t' / '\r' / '\n' を読み飛ばす (NUL で停止)。 */
const char* CFxeditSerializer::SkipWhitespace(const char* p) noexcept {
    if (p == nullptr) return nullptr;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/** "<key> v0 [v1] [v2] [v3]" を構文解析する (空スロットは 0.0f)。 */
bool CFxeditSerializer::ParseLine(const char*  line,
                                 const char*& out_key,
                                 f32&         out_v0,
                                 f32&         out_v1,
                                 f32&         out_v2,
                                 f32&         out_v3) noexcept {
    out_key = nullptr;
    out_v0 = out_v1 = out_v2 = out_v3 = 0.0f;
    if (line == nullptr) return false;

    const char* p = SkipWhitespace(line);
    if (*p == '\0' || *p == '#') return false;  // 空行 or コメント
    out_key = p;

    // key 範囲は次の空白まで (ただし scan ループ後にトークン参照は使わない)。
    while (*p != '\0' && *p != ' ' && *p != '\t') ++p;

    // 数値トークンを最大 4 個まで読む。
    f32* slots[4] = { &out_v0, &out_v1, &out_v2, &out_v3 };
    u32 idx = 0;
    while (idx < 4u && *p != '\0' && *p != '\n') {
        p = SkipWhitespace(p);
        if (*p == '\0' || *p == '\n' || *p == '#') break;
        f32 v = 0.0f;
        if (ParseFiniteFloatToken(p, v) != ENumberStatus::Ok) break;
        *slots[idx++] = v;
    }
    return true;
}

/** 先頭の非空行で magic + version を検査し、version を返す (失敗時 0)。 */
u32 CFxeditSerializer::ParseHeaderVersion(const char* text, u32 text_len) noexcept {
    if (text == nullptr || text_len == 0u) return 0u;
    if (std::memchr(text, '\0', text_len) != nullptr) return 0u;
    char line[kMaxLineLength + 1];
    usize pos = 0;
    while (ReadOneLine(text, text_len, pos, line, sizeof(line))) {
        const char* p = SkipWhitespace(line);
        if (*p == '\0' || *p == '#') continue;  // 空行 / コメントは飛ばす
        return ParseMagicLine(line);
    }
    return 0u;
}

const char* FFxeditSerializeResult::ErrorName(EFxeditSerializeError error) noexcept {
    switch (error) {
        case EFxeditSerializeError::None: return "None";
        case EFxeditSerializeError::NullArgument: return "NullArgument";
        case EFxeditSerializeError::PathTooLong: return "PathTooLong";
        case EFxeditSerializeError::InputTooLarge: return "InputTooLarge";
        case EFxeditSerializeError::EmbeddedNul: return "EmbeddedNul";
        case EFxeditSerializeError::TooManyLines: return "TooManyLines";
        case EFxeditSerializeError::LineTooLong: return "LineTooLong";
        case EFxeditSerializeError::BadMagic: return "BadMagic";
        case EFxeditSerializeError::UnsupportedVersion: return "UnsupportedVersion";
        case EFxeditSerializeError::MissingEmitterCount: return "MissingEmitterCount";
        case EFxeditSerializeError::DuplicateEmitterCount: return "DuplicateEmitterCount";
        case EFxeditSerializeError::TooManyEmitters: return "TooManyEmitters";
        case EFxeditSerializeError::BufferTooSmall: return "BufferTooSmall";
        case EFxeditSerializeError::InvalidSyntax: return "InvalidSyntax";
        case EFxeditSerializeError::InvalidEmitterIndex: return "InvalidEmitterIndex";
        case EFxeditSerializeError::DuplicateKey: return "DuplicateKey";
        case EFxeditSerializeError::InvalidValue: return "InvalidValue";
        case EFxeditSerializeError::ValueOutOfRange: return "ValueOutOfRange";
        case EFxeditSerializeError::NameTooLong: return "NameTooLong";
        case EFxeditSerializeError::InvalidName: return "InvalidName";
        case EFxeditSerializeError::TooManyCurves: return "TooManyCurves";
        case EFxeditSerializeError::TooManyKeyframes: return "TooManyKeyframes";
        case EFxeditSerializeError::AllocationFailure: return "AllocationFailure";
        case EFxeditSerializeError::FileNotFound: return "FileNotFound";
        case EFxeditSerializeError::FileOpenFailed: return "FileOpenFailed";
        case EFxeditSerializeError::FileSizeFailed: return "FileSizeFailed";
        case EFxeditSerializeError::FileChanged: return "FileChanged";
        case EFxeditSerializeError::FileReadFailed: return "FileReadFailed";
        case EFxeditSerializeError::FileWriteFailed: return "FileWriteFailed";
        case EFxeditSerializeError::FileFlushFailed: return "FileFlushFailed";
        case EFxeditSerializeError::FileCloseFailed: return "FileCloseFailed";
        case EFxeditSerializeError::AtomicReplaceFailed: return "AtomicReplaceFailed";
    }
    return "Unknown";
}

FFxeditSerializeResult CFxeditSerializer::TryParseText(
    const char* text,
    usize text_size,
    FParticleEmitterDef* out_defs,
    char* out_name_buffer,
    usize name_buffer_capacity,
    u32 max_emitters) noexcept {
    FFxeditSerializeResult result{};
    result.bytes_processed = static_cast<u64>(text_size);
    if (text == nullptr || out_defs == nullptr || out_name_buffer == nullptr ||
        max_emitters == 0u || name_buffer_capacity == 0u) {
        result.error = EFxeditSerializeError::NullArgument;
        return result;
    }
    if (max_emitters > kMaxEmitterCount) {
        result.error = EFxeditSerializeError::TooManyEmitters;
        return result;
    }
    if (text_size > kMaxInputBytes) {
        result.error = EFxeditSerializeError::InputTooLarge;
        return result;
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        result.error = EFxeditSerializeError::EmbeddedNul;
        return result;
    }

    TArray<FParticleEmitterDef> staged_defs;
    TArray<char> staged_names;
    TArray<u32> seen_keys;
    TArray<u8> curve_defined;
    TArray<u16> keyframe_counts;
    u32 declared_count = 0u;
    bool saw_header = false;
    bool saw_count = false;
    usize offset = 0u;
    u32 line_number = 0u;
    char line[kMaxLineLength + 1u]{};

    auto fail = [&](EFxeditSerializeError error) noexcept -> FFxeditSerializeResult {
        result.error = error;
        result.line = line_number;
        return result;
    };

    while (offset < text_size) {
        if (++line_number > kMaxLineCount) return fail(EFxeditSerializeError::TooManyLines);
        const usize begin = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize length = offset - begin;
        if (offset < text_size) ++offset;
        if (length > kMaxLineLength) return fail(EFxeditSerializeError::LineTooLong);
        std::memcpy(line, text + begin, length);
        if (length > 0u && line[length - 1u] == '\r') --length;
        line[length] = '\0';
        char* current = line;
        while (*current == ' ' || *current == '\t') ++current;
        char* line_end = current + std::strlen(current);
        while (line_end > current && (line_end[-1] == ' ' || line_end[-1] == '\t')) --line_end;
        *line_end = '\0';
        if (*current == '\0' || *current == '#') continue;

        if (!saw_header) {
            constexpr usize magic_length = 10u;
            if (std::strncmp(current, kMagic, magic_length) != 0 ||
                (current[magic_length] != ' ' && current[magic_length] != '\t')) {
                return fail(EFxeditSerializeError::BadMagic);
            }
            const char* version_text = current + magic_length;
            u32 version = 0u;
            const ENumberStatus status = ParseU32Token(version_text, version);
            if (status != ENumberStatus::Ok || !AtValueEnd(version_text)) {
                return fail(status == ENumberStatus::OutOfRange
                    ? EFxeditSerializeError::ValueOutOfRange
                    : EFxeditSerializeError::BadMagic);
            }
            if (version != kCurrentVersion) {
                return fail(EFxeditSerializeError::UnsupportedVersion);
            }
            saw_header = true;
            continue;
        }

        if (std::strncmp(current, "EMITTER", 7u) == 0 &&
            (current[7] == ' ' || current[7] == '\t')) {
            if (saw_count) return fail(EFxeditSerializeError::DuplicateEmitterCount);
            const char* count_text = current + 7u;
            SkipHorizontalSpace(count_text);
            if (std::strncmp(count_text, "count", 5u) != 0 ||
                (count_text[5] != ' ' && count_text[5] != '\t')) {
                return fail(EFxeditSerializeError::InvalidSyntax);
            }
            count_text += 5u;
            const ENumberStatus status = ParseU32Token(count_text, declared_count);
            if (status != ENumberStatus::Ok || !AtValueEnd(count_text)) {
                return fail(status == ENumberStatus::OutOfRange
                    ? EFxeditSerializeError::ValueOutOfRange
                    : EFxeditSerializeError::InvalidValue);
            }
            if (declared_count > kMaxEmitterCount || declared_count > max_emitters) {
                return fail(EFxeditSerializeError::TooManyEmitters);
            }
            const usize required_names =
                static_cast<usize>(declared_count) * (kMaxEmitterName + 1u);
            if (required_names > name_buffer_capacity) {
                return fail(EFxeditSerializeError::BufferTooSmall);
            }
            const usize curve_slots =
                static_cast<usize>(declared_count) * kMaxCurvesPerEmitter;
            if (!staged_defs.TryResize(declared_count) ||
                !staged_names.TryResize(required_names) ||
                !seen_keys.TryResize(declared_count) ||
                !curve_defined.TryResize(curve_slots) ||
                !keyframe_counts.TryResize(curve_slots)) {
                return fail(EFxeditSerializeError::AllocationFailure);
            }
            saw_count = true;
            continue;
        }
        if (!saw_count) return fail(EFxeditSerializeError::MissingEmitterCount);

        const char* emitter_begin = current;
        const char* emitter_end = emitter_begin;
        while (*emitter_end != '\0' && *emitter_end != ' ' && *emitter_end != '\t') ++emitter_end;
        u32 emitter_index = 0u;
        if (!ParseEmitterIndex(emitter_begin, emitter_end, emitter_index) ||
            emitter_index >= declared_count) {
            return fail(EFxeditSerializeError::InvalidEmitterIndex);
        }
        const char* key_begin = emitter_end;
        SkipHorizontalSpace(key_begin);
        if (*key_begin == '\0') return fail(EFxeditSerializeError::InvalidSyntax);
        const char* key_end = key_begin;
        while (*key_end != '\0' && *key_end != ' ' && *key_end != '\t') ++key_end;
        if (static_cast<usize>(key_end - key_begin) > 63u) {
            return fail(EFxeditSerializeError::InvalidSyntax);
        }
        const char* value_text = key_end;
        SkipHorizontalSpace(value_text);
        const EFxKnownKey key = FxKnownKey(key_begin, key_end);
        if (key == EFxKnownKey::Unknown) continue; // v1 の前方互換。

        if (IsScalarFxKey(key)) {
            const u32 mask = u32{1} << static_cast<u32>(key);
            if ((seen_keys[emitter_index] & mask) != 0u) {
                return fail(EFxeditSerializeError::DuplicateKey);
            }
            seen_keys[emitter_index] |= mask;
        }

        FParticleEmitterDef& def = staged_defs[emitter_index];
        if (key == EFxKnownKey::Name) {
            if (*value_text != '"') return fail(EFxeditSerializeError::InvalidName);
            ++value_text;
            const char* name_begin = value_text;
            while (*value_text != '\0' && *value_text != '"') ++value_text;
            if (*value_text != '"') return fail(EFxeditSerializeError::InvalidName);
            const usize name_length = static_cast<usize>(value_text - name_begin);
            if (name_length > kMaxEmitterName) return fail(EFxeditSerializeError::NameTooLong);
            for (usize i = 0u; i < name_length; ++i) {
                if (!IsAsciiPrintable(name_begin[i]) || name_begin[i] == '"') {
                    return fail(EFxeditSerializeError::InvalidName);
                }
            }
            ++value_text;
            if (!AtValueEnd(value_text)) return fail(EFxeditSerializeError::InvalidName);
            char* destination = staged_names.Data() +
                static_cast<usize>(emitter_index) * (kMaxEmitterName + 1u);
            std::memcpy(destination, name_begin, name_length);
            destination[name_length] = '\0';
            continue;
        }

        if (key == EFxKnownKey::Curve) {
            u32 curve_index = 0u;
            const ENumberStatus status = ParseU32Token(value_text, curve_index);
            if (status != ENumberStatus::Ok || !AtValueEnd(value_text)) {
                return fail(status == ENumberStatus::OutOfRange
                    ? EFxeditSerializeError::ValueOutOfRange
                    : EFxeditSerializeError::InvalidValue);
            }
            if (curve_index >= kMaxCurvesPerEmitter) {
                return fail(EFxeditSerializeError::TooManyCurves);
            }
            const usize slot = static_cast<usize>(emitter_index) * kMaxCurvesPerEmitter +
                curve_index;
            if (curve_defined[slot] != 0u) return fail(EFxeditSerializeError::DuplicateKey);
            curve_defined[slot] = 1u;
            continue;
        }

        if (key == EFxKnownKey::Keyframe) {
            u32 curve_index = 0u;
            ENumberStatus status = ParseU32Token(value_text, curve_index);
            f32 time = 0.0f;
            f32 value = 0.0f;
            if (status == ENumberStatus::Ok) status = ParseFiniteFloatToken(value_text, time);
            if (status == ENumberStatus::Ok) status = ParseFiniteFloatToken(value_text, value);
            if (status != ENumberStatus::Ok || !AtValueEnd(value_text)) {
                return fail(status == ENumberStatus::OutOfRange
                    ? EFxeditSerializeError::ValueOutOfRange
                    : EFxeditSerializeError::InvalidValue);
            }
            if (curve_index >= kMaxCurvesPerEmitter) {
                return fail(EFxeditSerializeError::TooManyCurves);
            }
            if (time < 0.0f || time > 1.0f || std::fabs(value) > 1000000000.0f) {
                return fail(EFxeditSerializeError::ValueOutOfRange);
            }
            const usize slot = static_cast<usize>(emitter_index) * kMaxCurvesPerEmitter +
                curve_index;
            if (keyframe_counts[slot] >= kMaxKeyframesPerCurve) {
                return fail(EFxeditSerializeError::TooManyKeyframes);
            }
            ++keyframe_counts[slot];
            continue;
        }

        f32 values[4]{};
        u32 value_count = 1u;
        if (key == EFxKnownKey::Gravity) value_count = 3u;
        if (key == EFxKnownKey::ColorStart || key == EFxKnownKey::ColorEnd) value_count = 4u;
        const ENumberStatus number_status =
            ParseFloatValues(value_text, values, value_count);
        if (number_status != ENumberStatus::Ok) {
            return fail(number_status == ENumberStatus::OutOfRange
                ? EFxeditSerializeError::ValueOutOfRange
                : EFxeditSerializeError::InvalidValue);
        }
        switch (key) {
            case EFxKnownKey::EmitRate: def.emit_rate_per_sec = values[0]; break;
            case EFxKnownKey::Lifetime: def.lifetime_sec = values[0]; break;
            case EFxKnownKey::BurstCount: def.burst_count = values[0]; break;
            case EFxKnownKey::SpeedMin: def.speed_min = values[0]; break;
            case EFxKnownKey::SpeedMax: def.speed_max = values[0]; break;
            case EFxKnownKey::ScaleStart: def.scale_start = values[0]; break;
            case EFxKnownKey::ScaleEnd: def.scale_end = values[0]; break;
            case EFxKnownKey::Gravity:
                if (std::fabs(values[2]) > 1000000000.0f) {
                    return fail(EFxeditSerializeError::ValueOutOfRange);
                }
                def.gravity = FVec2{values[0], values[1]};
                break;
            case EFxKnownKey::ColorStart:
            case EFxKnownKey::ColorEnd:
                for (f32 value : values) {
                    if (value < 0.0f || value > 1.0f) {
                        return fail(EFxeditSerializeError::ValueOutOfRange);
                    }
                }
                if (key == EFxKnownKey::ColorStart) {
                    def.color_start = FVec3{values[0], values[1], values[2]};
                } else {
                    def.color_end = FVec3{values[0], values[1], values[2]};
                }
                break;
            case EFxKnownKey::SpreadRadians:
                if (values[0] < 0.0f || values[0] > 6.2831855f) {
                    return fail(EFxeditSerializeError::ValueOutOfRange);
                }
                break;
            default: break;
        }
    }

    if (!saw_header) {
        result.error = EFxeditSerializeError::BadMagic;
        return result;
    }
    if (!saw_count) {
        result.error = EFxeditSerializeError::MissingEmitterCount;
        return result;
    }
    for (u32 i = 0u; i < declared_count; ++i) {
        const EFxeditSerializeError validation = ValidateEmitterDef(staged_defs[i]);
        if (validation != EFxeditSerializeError::None) {
            result.error = validation;
            return result;
        }
    }

    for (u32 i = 0u; i < max_emitters; ++i) out_defs[i] = FParticleEmitterDef{};
    std::memset(out_name_buffer, 0, name_buffer_capacity);
    for (u32 i = 0u; i < declared_count; ++i) out_defs[i] = staged_defs[i];
    if (!staged_names.IsEmpty()) {
        std::memcpy(out_name_buffer, staged_names.Data(), staged_names.Size());
    }
    result.emitter_count = declared_count;
    return result;
}

FFxeditSerializeResult CFxeditSerializer::TryLoad(
    const wchar_t* file_path,
    FParticleEmitterDef* out_defs,
    char* out_name_buffer,
    usize name_buffer_capacity,
    u32 max_emitters) noexcept {
    FFxeditSerializeResult result{};
    usize path_length = 0u;
    if (file_path == nullptr || out_defs == nullptr || out_name_buffer == nullptr ||
        max_emitters == 0u || name_buffer_capacity == 0u) {
        result.error = EFxeditSerializeError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(file_path, kMaxPathChars, path_length)) {
        result.error = EFxeditSerializeError::PathTooLong;
        return result;
    }

    HANDLE file = ::CreateFileW(
        file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.os_error = ::GetLastError();
        result.error = result.os_error == ERROR_FILE_NOT_FOUND ||
                       result.os_error == ERROR_PATH_NOT_FOUND
            ? EFxeditSerializeError::FileNotFound
            : EFxeditSerializeError::FileOpenFailed;
        return result;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        result.os_error = ::GetLastError();
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::FileSizeFailed;
        return result;
    }
    if (static_cast<u64>(size.QuadPart) > static_cast<u64>(kMaxInputBytes)) {
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::InputTooLarge;
        return result;
    }
    TArray<char> text;
    if (!text.TryResize(static_cast<usize>(size.QuadPart))) {
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::AllocationFailure;
        return result;
    }
    usize total = 0u;
    while (total < text.Size()) {
        const usize remaining = text.Size() - total;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7ffff000u ? 0x7ffff000u : remaining);
        DWORD read = 0u;
        if (!::ReadFile(file, text.Data() + total, chunk, &read, nullptr) || read == 0u) {
            result.os_error = ::GetLastError();
            ::CloseHandle(file);
            result.error = EFxeditSerializeError::FileReadFailed;
            result.bytes_processed = static_cast<u64>(total);
            return result;
        }
        total += read;
    }
    char probe = '\0';
    DWORD probe_read = 0u;
    if (!::ReadFile(file, &probe, 1u, &probe_read, nullptr)) {
        result.os_error = ::GetLastError();
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::FileReadFailed;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (probe_read != 0u) {
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    LARGE_INTEGER final_size{};
    if (!::GetFileSizeEx(file, &final_size)) {
        result.os_error = ::GetLastError();
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::FileSizeFailed;
        return result;
    }
    if (final_size.QuadPart != size.QuadPart) {
        ::CloseHandle(file);
        result.error = EFxeditSerializeError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (!::CloseHandle(file)) {
        result.os_error = ::GetLastError();
        result.error = EFxeditSerializeError::FileCloseFailed;
        return result;
    }
    result = TryParseText(
        text.IsEmpty() ? "" : text.Data(), text.Size(),
        out_defs, out_name_buffer, name_buffer_capacity, max_emitters);
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

FFxeditSerializeResult CFxeditSerializer::TrySave(
    const wchar_t* file_path,
    const FParticleEmitterDef* defs,
    const char* const* names,
    u32 count) noexcept {
    FFxeditSerializeResult result{};
    usize path_length = 0u;
    if (file_path == nullptr || (count > 0u && (defs == nullptr || names == nullptr))) {
        result.error = EFxeditSerializeError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(file_path, kMaxPathChars, path_length)) {
        result.error = EFxeditSerializeError::PathTooLong;
        return result;
    }
    if (count > kMaxEmitterCount) {
        result.error = EFxeditSerializeError::TooManyEmitters;
        return result;
    }
    for (u32 i = 0u; i < count; ++i) {
        result.error = ValidateEmitterDef(defs[i]);
        if (result.error != EFxeditSerializeError::None) return result;
        result.error = ValidateEmitterName(names[i]);
        if (result.error != EFxeditSerializeError::None) return result;
    }

    const usize reserve_size =
        static_cast<usize>(count) * kMaxBytesPerEmitter + 128u;
    TArray<char> out;
    if (!out.TryReserve(reserve_size)) {
        result.error = EFxeditSerializeError::AllocationFailure;
        return result;
    }
    char header[64]{};
    int written = std::snprintf(
        header, sizeof(header), "%s %u\n", kMagic, kCurrentVersion);
    if (written < 0 || static_cast<usize>(written) >= sizeof(header) ||
        !AppendStr(out, header, static_cast<usize>(written))) {
        result.error = EFxeditSerializeError::AllocationFailure;
        return result;
    }
    written = std::snprintf(header, sizeof(header), "EMITTER count %u\n", count);
    if (written < 0 || static_cast<usize>(written) >= sizeof(header) ||
        !AppendStr(out, header, static_cast<usize>(written))) {
        result.error = EFxeditSerializeError::AllocationFailure;
        return result;
    }
    for (u32 i = 0u; i < count; ++i) {
        char prefix[16]{};
        const int prefix_length = std::snprintf(prefix, sizeof(prefix), "E%u", i);
        if (prefix_length < 0 || static_cast<usize>(prefix_length) >= sizeof(prefix) ||
            !AppendNameLine(out, prefix, names[i])) {
            result.error = EFxeditSerializeError::AllocationFailure;
            return result;
        }
        const FParticleEmitterDef& def = defs[i];
        bool append_ok = true;
        f32 value = def.emit_rate_per_sec;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "emit_rate", &value, 1u);
        value = def.lifetime_sec;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "lifetime_sec", &value, 1u);
        value = def.burst_count;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "burst_count", &value, 1u);
        value = def.speed_min;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "speed_min", &value, 1u);
        value = def.speed_max;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "speed_max", &value, 1u);
        value = def.scale_start;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "scale_start", &value, 1u);
        value = def.scale_end;
        append_ok = append_ok && AppendKeyValueLine(out, prefix, "scale_end", &value, 1u);
        {
            const f32 gravity[3] = {def.gravity.x, def.gravity.y, 0.0f};
            append_ok = append_ok && AppendKeyValueLine(out, prefix, "gravity", gravity, 3u);
        }
        {
            const f32 color[4] = {
                def.color_start.x, def.color_start.y, def.color_start.z, 1.0f};
            append_ok = append_ok && AppendKeyValueLine(out, prefix, "color_start", color, 4u);
        }
        {
            const f32 color[4] = {
                def.color_end.x, def.color_end.y, def.color_end.z, 1.0f};
            append_ok = append_ok && AppendKeyValueLine(out, prefix, "color_end", color, 4u);
        }
        value = 0.0f;
        append_ok = append_ok &&
            AppendKeyValueLine(out, prefix, "spread_radians", &value, 1u);
        if (!append_ok) {
            result.error = EFxeditSerializeError::AllocationFailure;
            return result;
        }
    }
    if (out.Size() > kMaxInputBytes) {
        result.error = EFxeditSerializeError::InputTooLarge;
        return result;
    }

    constexpr usize kTempPathCapacity = kMaxPathChars + 97u;
    wchar_t temp_path[kTempPathCapacity]{};
    HANDLE temp = INVALID_HANDLE_VALUE;
    for (u32 attempt = 0u; attempt < 8u; ++attempt) {
        if (!BuildUniqueTempPath(
                file_path, path_length, temp_path,
                sizeof(temp_path) / sizeof(temp_path[0]), attempt)) {
            result.error = EFxeditSerializeError::PathTooLong;
            return result;
        }
        temp = ::CreateFileW(
            temp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temp != INVALID_HANDLE_VALUE) break;
        result.os_error = ::GetLastError();
        if (result.os_error != ERROR_FILE_EXISTS &&
            result.os_error != ERROR_ALREADY_EXISTS) {
            result.error = EFxeditSerializeError::FileOpenFailed;
            return result;
        }
    }
    if (temp == INVALID_HANDLE_VALUE) {
        result.error = EFxeditSerializeError::FileOpenFailed;
        return result;
    }
    usize total = 0u;
    while (total < out.Size()) {
        const usize remaining = out.Size() - total;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7ffff000u ? 0x7ffff000u : remaining);
        DWORD bytes_written = 0u;
        if (!::WriteFile(temp, out.Data() + total, chunk, &bytes_written, nullptr) ||
            bytes_written == 0u) {
            result.os_error = ::GetLastError();
            ::CloseHandle(temp);
            ::DeleteFileW(temp_path);
            result.error = EFxeditSerializeError::FileWriteFailed;
            result.bytes_processed = static_cast<u64>(total);
            return result;
        }
        total += bytes_written;
    }
    if (!::FlushFileBuffers(temp)) {
        result.os_error = ::GetLastError();
        ::CloseHandle(temp);
        ::DeleteFileW(temp_path);
        result.error = EFxeditSerializeError::FileFlushFailed;
        return result;
    }
    if (!::CloseHandle(temp)) {
        result.os_error = ::GetLastError();
        ::DeleteFileW(temp_path);
        result.error = EFxeditSerializeError::FileCloseFailed;
        return result;
    }
    if (!::MoveFileExW(
            temp_path, file_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = ::GetLastError();
        DWORD posix_error = 0u;
        if (!TryPosixAtomicReplace(
                temp_path, file_path, path_length, posix_error)) {
            result.os_error = posix_error != 0u ? posix_error : move_error;
            ::DeleteFileW(temp_path);
            result.error = EFxeditSerializeError::AtomicReplaceFailed;
            return result;
        }
    }
    result.emitter_count = count;
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

TResult<void, FErrorCode> CFxeditSerializer::Save(
    const wchar_t* file_path,
    const FParticleEmitterDef* defs,
    const char* const* names,
    u32 count) noexcept {
    const FFxeditSerializeResult result = TrySave(file_path, defs, names, count);
    if (result.Succeeded()) return Ok();
    return ACS_ERR_OS(
        IO, LegacySubCode(result.error),
        "FFxeditSerializer::Save: checked save failed", result.os_error);
}

TResult<u32, FErrorCode> CFxeditSerializer::Load(
    const wchar_t* file_path,
    FParticleEmitterDef* out_defs,
    char* out_name_buffer,
    u32 name_buffer_capacity,
    u32 max_emitters) noexcept {
    const FFxeditSerializeResult result = TryLoad(
        file_path, out_defs, out_name_buffer,
        static_cast<usize>(name_buffer_capacity), max_emitters);
    if (!result.Succeeded()) {
        return ACS_ERR_OS(
            IO, LegacySubCode(result.error),
            "FFxeditSerializer::Load: checked load failed", result.os_error);
    }
    u32 count = result.emitter_count;
    return TResult<u32, FErrorCode>(OkInit, Move(count));
}

} // namespace acs::game::fxedit
