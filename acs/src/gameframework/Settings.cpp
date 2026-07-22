// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FSettings 実装と永続化境界。
#include "gameframework/Settings.h"

#include "foundation/Platform.h"
#include "memory/Memory.h"

#include <charconv>
#include <cstddef>
#include <cfloat>
#include <cwchar>
#include <limits>
#include <system_error>

namespace acs::game {

namespace {

constexpr usize kMaxNumericBytes = 96u;
constexpr usize kPersistencePathCapacity = 1024u;
constexpr u32 kTemporaryOpenAttempts = 32u;

volatile LONG g_SettingsTemporarySerial = 0;

bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool TryBoundedLength(const char* text, usize limit, usize& out_length) noexcept {
    out_length = 0u;
    if (text == nullptr) return false;
    while (out_length <= limit && text[out_length] != '\0') ++out_length;
    return out_length <= limit;
}

bool ContainsByte(const char* text, usize length, char value) noexcept {
    for (usize i = 0u; i < length; ++i) {
        if (text[i] == value) return true;
    }
    return false;
}

bool SpanEquals(const char* a, usize a_length, const char* b, usize b_length) noexcept {
    if (a_length != b_length) return false;
    for (usize i = 0u; i < a_length; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool IsStrictFloatSyntax(const char* begin, const char* end) noexcept {
    const char* p = begin;
    if (p < end && (*p == '+' || *p == '-')) ++p;

    bool has_integer_digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_integer_digits = true;
        ++p;
    }

    bool has_fraction_digits = false;
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9') {
            has_fraction_digits = true;
            ++p;
        }
    }
    if (!has_integer_digits && !has_fraction_digits) return false;

    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        const char* exponent_begin = p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p == exponent_begin) return false;
    }
    return p == end;
}

ESettingsPersistenceError TryParseI32(
    const char* begin, const char* end, i32& out_value) noexcept {
    const char* p = begin;
    bool negative = false;
    if (p < end && (*p == '+' || *p == '-')) {
        negative = *p == '-';
        ++p;
    }
    if (p == end) return ESettingsPersistenceError::InvalidInteger;

    const u64 limit = negative ? 2147483648ull : 2147483647ull;
    u64 magnitude = 0u;
    for (; p < end; ++p) {
        if (*p < '0' || *p > '9') return ESettingsPersistenceError::InvalidInteger;
        const u32 digit = static_cast<u32>(*p - '0');
        if (magnitude > (limit - digit) / 10ull) {
            return ESettingsPersistenceError::InvalidInteger;
        }
        magnitude = magnitude * 10ull + digit;
    }

    if (negative) {
        if (magnitude == 2147483648ull) {
            out_value = static_cast<i32>(0x80000000u);
        } else {
            out_value = -static_cast<i32>(magnitude);
        }
    } else {
        out_value = static_cast<i32>(magnitude);
    }
    return ESettingsPersistenceError::None;
}

ESettingsPersistenceError TryParseF32(
    const char* begin, const char* end, f32& out_value) noexcept {
    const usize length = static_cast<usize>(end - begin);
    if (length == 0u || length > kMaxNumericBytes) {
        return ESettingsPersistenceError::InvalidFloat;
    }

    const char* unsigned_begin = begin;
    if (*unsigned_begin == '+' || *unsigned_begin == '-') ++unsigned_begin;
    const usize unsigned_length = static_cast<usize>(end - unsigned_begin);
    if (SpanEquals(unsigned_begin, unsigned_length, "nan", 3u) ||
        SpanEquals(unsigned_begin, unsigned_length, "inf", 3u) ||
        SpanEquals(unsigned_begin, unsigned_length, "infinity", 8u)) {
        return ESettingsPersistenceError::NonFiniteFloat;
    }
    if (!IsStrictFloatSyntax(begin, end)) {
        return ESettingsPersistenceError::InvalidFloat;
    }

    // from_chars は locale 非依存。標準の浮動小数点 overload は先頭 `+` を拒否するため、
    // この文法で受理した `+` は先に取り除く。
    const char* conversion_begin = *begin == '+' ? begin + 1 : begin;
    f32 value = 0.0f;
    const std::from_chars_result conversion = std::from_chars(
        conversion_begin, end, value, std::chars_format::general);
    if (conversion.ec != std::errc{} || conversion.ptr != end) {
        return ESettingsPersistenceError::InvalidFloat;
    }
    out_value = value;
    return ESettingsPersistenceError::None;
}

struct FParsedRecord {
    const char* Key = nullptr;
    usize KeyLength = 0u;
    const char* Value = nullptr;
    usize ValueLength = 0u;
    ESettingKind Kind = ESettingKind::None;
    f32 FloatValue = 0.0f;
    i32 IntegerValue = 0;
    bool BoolValue = false;
};

ESettingsPersistenceError ParseRecord(
    const char* begin, const char* end, FParsedRecord& out_record) noexcept {
    const usize length = static_cast<usize>(end - begin);
    if (length < 4u || begin[1] != ':') {
        return ESettingsPersistenceError::MalformedRecord;
    }

    switch (begin[0]) {
        case 'f': out_record.Kind = ESettingKind::F32; break;
        case 'i': out_record.Kind = ESettingKind::I32; break;
        case 'b': out_record.Kind = ESettingKind::Bool; break;
        case 's': out_record.Kind = ESettingKind::String; break;
        default: return ESettingsPersistenceError::UnknownType;
    }

    const char* equals = begin + 2;
    while (equals < end && *equals != '=') ++equals;
    if (equals == end) return ESettingsPersistenceError::MalformedRecord;

    out_record.Key = begin + 2;
    out_record.KeyLength = static_cast<usize>(equals - out_record.Key);
    out_record.Value = equals + 1;
    out_record.ValueLength = static_cast<usize>(end - out_record.Value);

    if (out_record.KeyLength == 0u) return ESettingsPersistenceError::EmptyKey;
    if (out_record.KeyLength > FSettings::kMaxPersistenceKeyBytes) {
        return ESettingsPersistenceError::KeyTooLong;
    }
    if (ContainsByte(out_record.Key, out_record.KeyLength, '\r') ||
        ContainsByte(out_record.Key, out_record.KeyLength, '\n')) {
        return ESettingsPersistenceError::UnrepresentableText;
    }

    switch (out_record.Kind) {
        case ESettingKind::F32:
            return TryParseF32(
                out_record.Value, out_record.Value + out_record.ValueLength,
                out_record.FloatValue);
        case ESettingKind::I32:
            if (out_record.ValueLength > kMaxNumericBytes) {
                return ESettingsPersistenceError::InvalidInteger;
            }
            return TryParseI32(
                out_record.Value, out_record.Value + out_record.ValueLength,
                out_record.IntegerValue);
        case ESettingKind::Bool:
            if (SpanEquals(out_record.Value, out_record.ValueLength, "true", 4u)) {
                out_record.BoolValue = true;
                return ESettingsPersistenceError::None;
            }
            if (SpanEquals(out_record.Value, out_record.ValueLength, "false", 5u)) {
                out_record.BoolValue = false;
                return ESettingsPersistenceError::None;
            }
            return ESettingsPersistenceError::InvalidBool;
        case ESettingKind::String:
            if (out_record.ValueLength > FSettings::kMaxPersistenceStringBytes) {
                return ESettingsPersistenceError::ValueTooLong;
            }
            if (ContainsByte(out_record.Value, out_record.ValueLength, '\r') ||
                ContainsByte(out_record.Value, out_record.ValueLength, '\n')) {
                return ESettingsPersistenceError::UnrepresentableText;
            }
            return ESettingsPersistenceError::None;
        default:
            return ESettingsPersistenceError::UnknownType;
    }
}

FSettingsPersistenceResult Failure(
    ESettingsPersistenceError error, u32 line = 0u, u32 entries = 0u,
    u32 os_error = 0u) noexcept {
    FSettingsPersistenceResult result{};
    result.Error = error;
    result.Line = line;
    result.Entries = entries;
    result.OsError = os_error;
    return result;
}

ESettingsPersistenceError TryAppendLimited(
    FString& output, FStringView value) noexcept {
    if (output.Size() > FSettings::kMaxPersistenceBytes) {
        return ESettingsPersistenceError::OutputTooLarge;
    }
    if (value.Size() > FSettings::kMaxPersistenceBytes - output.Size()) {
        return ESettingsPersistenceError::OutputTooLarge;
    }
    return output.TryAppend(value)
        ? ESettingsPersistenceError::None
        : ESettingsPersistenceError::AllocationFailure;
}

ESettingsPersistenceError TryAppendLimited(FString& output, char value) noexcept {
    if (output.Size() == FSettings::kMaxPersistenceBytes) {
        return ESettingsPersistenceError::OutputTooLarge;
    }
    return output.TryAppend(value)
        ? ESettingsPersistenceError::None
        : ESettingsPersistenceError::AllocationFailure;
}

ESettingsPersistenceError TryAppendI32(FString& output, i32 value) noexcept {
    char reversed[12]{};
    usize count = 0u;
    u32 magnitude = 0u;
    if (value < 0) {
        const ESettingsPersistenceError sign_error = TryAppendLimited(output, '-');
        if (sign_error != ESettingsPersistenceError::None) return sign_error;
        magnitude = 0u - static_cast<u32>(value);
    } else {
        magnitude = static_cast<u32>(value);
    }
    do {
        reversed[count++] = static_cast<char>('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);

    char digits[12]{};
    for (usize i = 0u; i < count; ++i) digits[i] = reversed[count - i - 1u];
    return TryAppendLimited(output, FStringView(digits, count));
}

ESettingsPersistenceError TryAppendF32(FString& output, f32 value) noexcept {
    if (!(value == value) || value > FLT_MAX || value < -FLT_MAX) {
        return ESettingsPersistenceError::NonFiniteFloat;
    }
    char buffer[64]{};
    const std::to_chars_result conversion = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general,
        std::numeric_limits<f32>::max_digits10);
    if (conversion.ec != std::errc{}) {
        return ESettingsPersistenceError::InvalidInMemoryEntry;
    }
    return TryAppendLimited(
        output, FStringView(
            buffer, static_cast<usize>(conversion.ptr - buffer)));
}

bool TryMakeTemporaryPath(
    const wchar_t* destination, u32 serial,
    wchar_t (&out_path)[kPersistencePathCapacity]) noexcept {
    usize destination_length = 0u;
    while (destination_length < kPersistencePathCapacity &&
           destination[destination_length] != L'\0') {
        ++destination_length;
    }
    if (destination_length == kPersistencePathCapacity) return false;

    wchar_t suffix[96]{};
    const int suffix_length = ::_snwprintf_s(
        suffix, sizeof(suffix) / sizeof(suffix[0]), _TRUNCATE,
        L".tmp.%08lX.%08lX.%08X",
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned int>(serial));
    if (suffix_length <= 0) return false;
    const usize suffix_size = static_cast<usize>(suffix_length);
    if (destination_length > kPersistencePathCapacity - suffix_size - 1u) {
        return false;
    }

    for (usize i = 0u; i < destination_length; ++i) {
        out_path[i] = destination[i];
    }
    for (usize i = 0u; i < suffix_size; ++i) {
        out_path[destination_length + i] = suffix[i];
    }
    out_path[destination_length + suffix_size] = L'\0';
    return true;
}

ESettingsPersistenceError ValidatePath(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return ESettingsPersistenceError::NullPath;
    }
    usize length = 0u;
    while (length < kPersistencePathCapacity && path[length] != L'\0') {
        ++length;
    }
    return length == kPersistencePathCapacity
        ? ESettingsPersistenceError::PathTooLong
        : ESettingsPersistenceError::None;
}

struct FFileRenameInfoEx {
    DWORD Flags = 0u;
    HANDLE RootDirectory = nullptr;
    DWORD FileNameLength = 0u;
    wchar_t FileName[1]{};
};

/**
 * FILE_SHARE_DELETE 付きで開かれている出力先を置換する。
 *
 * source/destination path はどちらも kPersistencePathCapacity の上限検証済みなので、
 * 固定 buffer を安全に使用できる。
 */
bool TryPosixAtomicReplace(
    const wchar_t* temporary_path, const wchar_t* destination,
    DWORD& out_error) noexcept {
    constexpr DWORD kRenameReplaceIfExists = 0x00000001u;
    constexpr DWORD kRenamePosixSemantics = 0x00000002u;
    constexpr auto kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

    usize destination_length = 0u;
    while (destination_length < kPersistencePathCapacity &&
           destination[destination_length] != L'\0') {
        ++destination_length;
    }
    if (destination_length == kPersistencePathCapacity) {
        out_error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }

    constexpr usize kPrefixBytes = offsetof(FFileRenameInfoEx, FileName);
    alignas(FFileRenameInfoEx)
        u8 storage[kPrefixBytes +
                   kPersistencePathCapacity * sizeof(wchar_t)]{};
    auto* const info = reinterpret_cast<FFileRenameInfoEx*>(storage);
    const usize destination_bytes = destination_length * sizeof(wchar_t);
    info->Flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->FileNameLength = static_cast<DWORD>(destination_bytes);
    for (usize i = 0u; i < destination_length; ++i) {
        info->FileName[i] = destination[i];
    }

    HANDLE source = ::CreateFileW(
        temporary_path, DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }

    const DWORD info_bytes =
        static_cast<DWORD>(kPrefixBytes + destination_bytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        source, kFileRenameInfoEx, info, info_bytes);
    if (!renamed) out_error = ::GetLastError();
    // rename 成功時点で commit 済み。close 診断によって commit 済み操作を
    // transactional failure として報告してはならない。
    (void)::CloseHandle(source);
    return renamed != 0;
}

} // namespace

const char* SettingsPersistenceErrorName(
    ESettingsPersistenceError error) noexcept {
    switch (error) {
        case ESettingsPersistenceError::None: return "None";
        case ESettingsPersistenceError::FileOpenFailed: return "FileOpenFailed";
        case ESettingsPersistenceError::FileTooLarge: return "FileTooLarge";
        case ESettingsPersistenceError::NullPath: return "NullPath";
        case ESettingsPersistenceError::AllocationFailure: return "AllocationFailure";
        case ESettingsPersistenceError::FileSizeFailed: return "FileSizeFailed";
        case ESettingsPersistenceError::FileReadFailed: return "FileReadFailed";
        case ESettingsPersistenceError::FileCloseFailed: return "FileCloseFailed";
        case ESettingsPersistenceError::EmbeddedNul: return "EmbeddedNul";
        case ESettingsPersistenceError::LineTooLong: return "LineTooLong";
        case ESettingsPersistenceError::EntryLimitExceeded: return "EntryLimitExceeded";
        case ESettingsPersistenceError::MalformedRecord: return "MalformedRecord";
        case ESettingsPersistenceError::UnknownType: return "UnknownType";
        case ESettingsPersistenceError::EmptyKey: return "EmptyKey";
        case ESettingsPersistenceError::KeyTooLong: return "KeyTooLong";
        case ESettingsPersistenceError::ValueTooLong: return "ValueTooLong";
        case ESettingsPersistenceError::InvalidInteger: return "InvalidInteger";
        case ESettingsPersistenceError::InvalidFloat: return "InvalidFloat";
        case ESettingsPersistenceError::NonFiniteFloat: return "NonFiniteFloat";
        case ESettingsPersistenceError::InvalidBool: return "InvalidBool";
        case ESettingsPersistenceError::DuplicateKey: return "DuplicateKey";
        case ESettingsPersistenceError::InvalidInMemoryEntry: return "InvalidInMemoryEntry";
        case ESettingsPersistenceError::UnrepresentableText: return "UnrepresentableText";
        case ESettingsPersistenceError::OutputTooLarge: return "OutputTooLarge";
        case ESettingsPersistenceError::PathTooLong: return "PathTooLong";
        case ESettingsPersistenceError::TemporaryFileExhausted: return "TemporaryFileExhausted";
        case ESettingsPersistenceError::FileWriteFailed: return "FileWriteFailed";
        case ESettingsPersistenceError::FileFlushFailed: return "FileFlushFailed";
        case ESettingsPersistenceError::AtomicReplaceFailed: return "AtomicReplaceFailed";
    }
    return "Unknown";
}

isize FSettings::FindIndex(const char* key) const noexcept {
    if (key == nullptr) return -1;
    const usize count = m_Entries.Size();
    for (usize i = 0u; i < count; ++i) {
        if (StrEq(m_Entries[i].key, key)) return static_cast<isize>(i);
    }
    return -1;
}

FSettings::FEntry& FSettings::UpsertEntry(const char* key) noexcept {
    const isize index = FindIndex(key);
    if (index >= 0) return m_Entries[static_cast<usize>(index)];
    FEntry entry{};
    entry.key = key;
    m_Entries.PushBack(entry);
    return m_Entries.Back();
}

void FSettings::SetF32(const char* key, f32 value) noexcept {
    if (key == nullptr) return;
    FEntry& entry = UpsertEntry(key);
    entry.kind = ESettingKind::F32;
    entry.value.f = value;
}

void FSettings::SetI32(const char* key, i32 value) noexcept {
    if (key == nullptr) return;
    FEntry& entry = UpsertEntry(key);
    entry.kind = ESettingKind::I32;
    entry.value.i = value;
}

void FSettings::SetBool(const char* key, bool value) noexcept {
    if (key == nullptr) return;
    FEntry& entry = UpsertEntry(key);
    entry.kind = ESettingKind::Bool;
    entry.value.b = value;
}

void FSettings::SetString(const char* key, const char* value) noexcept {
    if (key == nullptr) return;
    FEntry& entry = UpsertEntry(key);
    entry.kind = ESettingKind::String;
    entry.value.s = value;
}

f32 FSettings::GetF32(const char* key, f32 default_value) const noexcept {
    const isize index = FindIndex(key);
    if (index < 0) return default_value;
    const FEntry& entry = m_Entries[static_cast<usize>(index)];
    return entry.kind == ESettingKind::F32 ? entry.value.f : default_value;
}

i32 FSettings::GetI32(const char* key, i32 default_value) const noexcept {
    const isize index = FindIndex(key);
    if (index < 0) return default_value;
    const FEntry& entry = m_Entries[static_cast<usize>(index)];
    return entry.kind == ESettingKind::I32 ? entry.value.i : default_value;
}

bool FSettings::GetBool(const char* key, bool default_value) const noexcept {
    const isize index = FindIndex(key);
    if (index < 0) return default_value;
    const FEntry& entry = m_Entries[static_cast<usize>(index)];
    return entry.kind == ESettingKind::Bool ? entry.value.b : default_value;
}

const char* FSettings::GetString(
    const char* key, const char* default_value) const noexcept {
    const isize index = FindIndex(key);
    if (index < 0) return default_value;
    const FEntry& entry = m_Entries[static_cast<usize>(index)];
    return entry.kind == ESettingKind::String ? entry.value.s : default_value;
}

bool FSettings::Has(const char* key) const noexcept {
    return FindIndex(key) >= 0;
}

void FSettings::Remove(const char* key) noexcept {
    const isize index = FindIndex(key);
    if (index >= 0) m_Entries.RemoveAtSwap(static_cast<usize>(index));
}

void FSettings::Clear() noexcept {
    m_Entries.Clear();
}

u32 FSettings::Count() const noexcept {
    return m_Entries.Size() > 0xFFFFFFFFu
        ? 0xFFFFFFFFu
        : static_cast<u32>(m_Entries.Size());
}

FSettingsPersistenceResult FSettings::TrySave(
    const wchar_t* file_path) noexcept {
    const ESettingsPersistenceError path_error = ValidatePath(file_path);
    if (path_error != ESettingsPersistenceError::None) return Failure(path_error);
    if (m_Entries.Size() > kMaxPersistenceEntries) {
        return Failure(ESettingsPersistenceError::EntryLimitExceeded);
    }

    FString text;
    u32 serialized_entries = 0u;
    for (usize i = 0u; i < m_Entries.Size(); ++i) {
        const usize line_begin = text.Size();
        const FEntry& entry = m_Entries[i];
        usize key_length = 0u;
        if (entry.kind == ESettingKind::None || entry.key == nullptr) {
            return Failure(
                ESettingsPersistenceError::InvalidInMemoryEntry, 0u,
                serialized_entries);
        }
        if (!TryBoundedLength(
                entry.key, kMaxPersistenceKeyBytes, key_length)) {
            return Failure(
                ESettingsPersistenceError::KeyTooLong, 0u,
                serialized_entries);
        }
        if (key_length == 0u) {
            return Failure(
                ESettingsPersistenceError::EmptyKey, 0u, serialized_entries);
        }
        if (ContainsByte(entry.key, key_length, '=') ||
            ContainsByte(entry.key, key_length, '\r') ||
            ContainsByte(entry.key, key_length, '\n')) {
            return Failure(
                ESettingsPersistenceError::UnrepresentableText, 0u,
                serialized_entries);
        }
        for (usize prior = 0u; prior < i; ++prior) {
            if (StrEq(entry.key, m_Entries[prior].key)) {
                return Failure(
                    ESettingsPersistenceError::DuplicateKey, 0u,
                    serialized_entries);
            }
        }

        char tag = '\0';
        switch (entry.kind) {
            case ESettingKind::F32: tag = 'f'; break;
            case ESettingKind::I32: tag = 'i'; break;
            case ESettingKind::Bool: tag = 'b'; break;
            case ESettingKind::String: tag = 's'; break;
            default:
                return Failure(
                    ESettingsPersistenceError::InvalidInMemoryEntry, 0u,
                    serialized_entries);
        }

        ESettingsPersistenceError error = TryAppendLimited(text, tag);
        if (error == ESettingsPersistenceError::None) {
            error = TryAppendLimited(text, ':');
        }
        if (error == ESettingsPersistenceError::None) {
            error = TryAppendLimited(text, FStringView(entry.key, key_length));
        }
        if (error == ESettingsPersistenceError::None) {
            error = TryAppendLimited(text, '=');
        }
        if (error != ESettingsPersistenceError::None) {
            return Failure(error, 0u, serialized_entries);
        }

        switch (entry.kind) {
            case ESettingKind::F32:
                error = TryAppendF32(text, entry.value.f);
                break;
            case ESettingKind::I32:
                error = TryAppendI32(text, entry.value.i);
                break;
            case ESettingKind::Bool:
                error = TryAppendLimited(
                    text, entry.value.b
                        ? FStringView("true", 4u)
                        : FStringView("false", 5u));
                break;
            case ESettingKind::String: {
                usize value_length = 0u;
                if (!TryBoundedLength(
                        entry.value.s, kMaxPersistenceStringBytes,
                        value_length)) {
                    error = entry.value.s == nullptr
                        ? ESettingsPersistenceError::InvalidInMemoryEntry
                        : ESettingsPersistenceError::ValueTooLong;
                    break;
                }
                if (ContainsByte(entry.value.s, value_length, '\r') ||
                    ContainsByte(entry.value.s, value_length, '\n')) {
                    error = ESettingsPersistenceError::UnrepresentableText;
                    break;
                }
                error = TryAppendLimited(
                    text, FStringView(entry.value.s, value_length));
                break;
            }
            default:
                error = ESettingsPersistenceError::InvalidInMemoryEntry;
                break;
        }
        if (error == ESettingsPersistenceError::None) {
            if (text.Size() - line_begin > kMaxPersistenceLineBytes) {
                return Failure(
                    ESettingsPersistenceError::LineTooLong, 0u,
                    serialized_entries);
            }
            error = TryAppendLimited(text, '\n');
        }
        if (error != ESettingsPersistenceError::None) {
            return Failure(error, 0u, serialized_entries);
        }
        ++serialized_entries;
    }

    wchar_t temporary_path[kPersistencePathCapacity]{};
    HANDLE file = INVALID_HANDLE_VALUE;
    u32 last_collision_error = ERROR_FILE_EXISTS;
    for (u32 attempt = 0u; attempt < kTemporaryOpenAttempts; ++attempt) {
        const u32 serial = static_cast<u32>(
            ::InterlockedIncrement(&g_SettingsTemporarySerial));
        if (!TryMakeTemporaryPath(file_path, serial, temporary_path)) {
            return Failure(
                ESettingsPersistenceError::PathTooLong, 0u,
                serialized_entries);
        }
        file = ::CreateFileW(
            temporary_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;

        const DWORD os_error = ::GetLastError();
        if (os_error != ERROR_FILE_EXISTS &&
            os_error != ERROR_ALREADY_EXISTS) {
            return Failure(
                ESettingsPersistenceError::FileOpenFailed, 0u,
                serialized_entries, os_error);
        }
        last_collision_error = os_error;
    }
    if (file == INVALID_HANDLE_VALUE) {
        return Failure(
            ESettingsPersistenceError::TemporaryFileExhausted, 0u,
            serialized_entries, last_collision_error);
    }

    const char* cursor = text.Data();
    usize remaining = text.Size();
    while (remaining != 0u) {
        const DWORD chunk = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD written = 0u;
        if (!::WriteFile(file, cursor, chunk, &written, nullptr) ||
            written != chunk) {
            const DWORD os_error = ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileW(temporary_path);
            return Failure(
                ESettingsPersistenceError::FileWriteFailed, 0u,
                serialized_entries, os_error);
        }
        cursor += written;
        remaining -= written;
    }

    if (!::FlushFileBuffers(file)) {
        const DWORD os_error = ::GetLastError();
        ::CloseHandle(file);
        ::DeleteFileW(temporary_path);
        return Failure(
            ESettingsPersistenceError::FileFlushFailed, 0u,
            serialized_entries, os_error);
    }
    if (!::CloseHandle(file)) {
        const DWORD os_error = ::GetLastError();
        ::DeleteFileW(temporary_path);
        return Failure(
            ESettingsPersistenceError::FileCloseFailed, 0u,
            serialized_entries, os_error);
    }
    if (!::MoveFileExW(
            temporary_path, file_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = ::GetLastError();
        DWORD posix_error = ERROR_SUCCESS;
        if (!TryPosixAtomicReplace(
                temporary_path, file_path, posix_error)) {
            ::DeleteFileW(temporary_path);
            const DWORD reported_error =
                posix_error == ERROR_INVALID_PARAMETER ||
                posix_error == ERROR_NOT_SUPPORTED ||
                posix_error == ERROR_CALL_NOT_IMPLEMENTED
                ? move_error
                : posix_error;
            return Failure(
                ESettingsPersistenceError::AtomicReplaceFailed, 0u,
                serialized_entries, reported_error);
        }
    }

    FSettingsPersistenceResult result{};
    result.Entries = serialized_entries;
    return result;
}

FSettingsPersistenceResult FSettings::TryLoad(
    const wchar_t* file_path) noexcept {
    const ESettingsPersistenceError path_error = ValidatePath(file_path);
    if (path_error != ESettingsPersistenceError::None) return Failure(path_error);

    HANDLE file = ::CreateFileW(
        file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Failure(
            ESettingsPersistenceError::FileOpenFailed, 0u, 0u,
            ::GetLastError());
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) {
        const DWORD os_error = ::GetLastError();
        ::CloseHandle(file);
        return Failure(
            ESettingsPersistenceError::FileSizeFailed, 0u, 0u, os_error);
    }
    const u64 size = static_cast<u64>(file_size.QuadPart);
    if (size > kMaxPersistenceBytes) {
        ::CloseHandle(file);
        return Failure(ESettingsPersistenceError::FileTooLarge);
    }

    FAllocator& allocator = DefaultAllocator();
    void* raw = nullptr;
    if (size != 0u) {
        raw = allocator.Alloc(
            static_cast<usize>(size), alignof(char), FSourceLoc::Current());
        if (raw == nullptr) {
            ::CloseHandle(file);
            return Failure(ESettingsPersistenceError::AllocationFailure);
        }
    }

    char* cursor = static_cast<char*>(raw);
    u64 remaining = size;
    while (remaining != 0u) {
        const DWORD chunk = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD read = 0u;
        if (!::ReadFile(file, cursor, chunk, &read, nullptr) ||
            read == 0u || read > chunk) {
            DWORD os_error = ::GetLastError();
            if (os_error == ERROR_SUCCESS) os_error = ERROR_HANDLE_EOF;
            if (raw != nullptr) allocator.Free(raw);
            ::CloseHandle(file);
            return Failure(
                ESettingsPersistenceError::FileReadFailed, 0u, 0u, os_error);
        }
        cursor += read;
        remaining -= read;
    }

    char extra = '\0';
    DWORD extra_read = 0u;
    if (!::ReadFile(file, &extra, 1u, &extra_read, nullptr) ||
        extra_read != 0u) {
        DWORD os_error = ::GetLastError();
        if (os_error == ERROR_SUCCESS) os_error = ERROR_FILE_INVALID;
        if (raw != nullptr) allocator.Free(raw);
        ::CloseHandle(file);
        return Failure(
            ESettingsPersistenceError::FileReadFailed, 0u, 0u, os_error);
    }
    if (!::CloseHandle(file)) {
        const DWORD os_error = ::GetLastError();
        if (raw != nullptr) allocator.Free(raw);
        return Failure(
            ESettingsPersistenceError::FileCloseFailed, 0u, 0u, os_error);
    }

    const char* bytes = static_cast<const char*>(raw);
    u32 nul_line = 1u;
    for (usize i = 0u; i < static_cast<usize>(size); ++i) {
        if (bytes[i] == '\0') {
            allocator.Free(raw);
            return Failure(
                ESettingsPersistenceError::EmbeddedNul, nul_line);
        }
        if (bytes[i] == '\n') ++nul_line;
    }

    u32 record_count = 0u;
    u32 string_record_count = 0u;
    u32 line_number = 0u;
    usize position = 0u;
    while (position < static_cast<usize>(size)) {
        ++line_number;
        const usize line_begin = position;
        while (position < static_cast<usize>(size) &&
               bytes[position] != '\n') {
            ++position;
        }
        usize line_end = position;
        if (position < static_cast<usize>(size)) ++position;
        if (line_end > line_begin && bytes[line_end - 1u] == '\r') --line_end;
        const usize line_length = line_end - line_begin;
        if (line_length > kMaxPersistenceLineBytes) {
            allocator.Free(raw);
            return Failure(
                ESettingsPersistenceError::LineTooLong, line_number,
                record_count);
        }
        if (line_length == 0u || bytes[line_begin] == '#') continue;

        FParsedRecord parsed{};
        const ESettingsPersistenceError parse_error = ParseRecord(
            bytes + line_begin, bytes + line_end, parsed);
        if (parse_error != ESettingsPersistenceError::None) {
            allocator.Free(raw);
            return Failure(parse_error, line_number, record_count);
        }
        if (record_count == kMaxPersistenceEntries) {
            allocator.Free(raw);
            return Failure(
                ESettingsPersistenceError::EntryLimitExceeded, line_number,
                record_count);
        }
        ++record_count;
        if (parsed.Kind == ESettingKind::String) ++string_record_count;
    }

    TArray<FString> staged_pool;
    TArray<FEntry> staged_entries;
    const usize pool_count =
        static_cast<usize>(record_count) +
        static_cast<usize>(string_record_count);
    if (!staged_pool.TryReserve(pool_count) ||
        !staged_entries.TryReserve(record_count)) {
        if (raw != nullptr) allocator.Free(raw);
        return Failure(ESettingsPersistenceError::AllocationFailure);
    }

    line_number = 0u;
    position = 0u;
    while (position < static_cast<usize>(size)) {
        ++line_number;
        const usize line_begin = position;
        while (position < static_cast<usize>(size) &&
               bytes[position] != '\n') {
            ++position;
        }
        usize line_end = position;
        if (position < static_cast<usize>(size)) ++position;
        if (line_end > line_begin && bytes[line_end - 1u] == '\r') --line_end;
        if (line_end == line_begin || bytes[line_begin] == '#') continue;

        FParsedRecord parsed{};
        const ESettingsPersistenceError parse_error = ParseRecord(
            bytes + line_begin, bytes + line_end, parsed);
        if (parse_error != ESettingsPersistenceError::None) {
            allocator.Free(raw);
            return Failure(
                parse_error, line_number,
                static_cast<u32>(staged_entries.Size()));
        }

        for (usize prior = 0u; prior < staged_entries.Size(); ++prior) {
            usize prior_length = 0u;
            (void)TryBoundedLength(
                staged_entries[prior].key, kMaxPersistenceKeyBytes,
                prior_length);
            if (SpanEquals(
                    staged_entries[prior].key, prior_length,
                    parsed.Key, parsed.KeyLength)) {
                allocator.Free(raw);
                return Failure(
                    ESettingsPersistenceError::DuplicateKey, line_number,
                    static_cast<u32>(staged_entries.Size()));
            }
        }

        FString key;
        if (!key.TryAppend(FStringView(parsed.Key, parsed.KeyLength)) ||
            !staged_pool.TryPushBack(Move(key))) {
            allocator.Free(raw);
            return Failure(
                ESettingsPersistenceError::AllocationFailure, line_number,
                static_cast<u32>(staged_entries.Size()));
        }

        FEntry entry{};
        entry.key = staged_pool.Back().Data();
        entry.kind = parsed.Kind;
        switch (parsed.Kind) {
            case ESettingKind::F32:
                entry.value.f = parsed.FloatValue;
                break;
            case ESettingKind::I32:
                entry.value.i = parsed.IntegerValue;
                break;
            case ESettingKind::Bool:
                entry.value.b = parsed.BoolValue;
                break;
            case ESettingKind::String: {
                FString value;
                if (!value.TryAppend(
                        FStringView(parsed.Value, parsed.ValueLength)) ||
                    !staged_pool.TryPushBack(Move(value))) {
                    allocator.Free(raw);
                    return Failure(
                        ESettingsPersistenceError::AllocationFailure,
                        line_number,
                        static_cast<u32>(staged_entries.Size()));
                }
                entry.value.s = staged_pool.Back().Data();
                break;
            }
            default:
                allocator.Free(raw);
                return Failure(
                    ESettingsPersistenceError::UnknownType, line_number,
                    static_cast<u32>(staged_entries.Size()));
        }
        if (!staged_entries.TryPushBack(entry)) {
            allocator.Free(raw);
            return Failure(
                ESettingsPersistenceError::AllocationFailure, line_number,
                static_cast<u32>(staged_entries.Size()));
        }
    }

    if (raw != nullptr) allocator.Free(raw);

    // ここから先の操作は失敗しない。array の Move は backing storage を移すため、
    // staged_pool 内への pointer は commit 後も有効。
    m_Entries = Move(staged_entries);
    m_StringPool = Move(staged_pool);

    FSettingsPersistenceResult result{};
    result.Entries = record_count;
    return result;
}

TResult<void> FSettings::Save(const wchar_t* file_path) noexcept {
    const FSettingsPersistenceResult result = TrySave(file_path);
    if (result) return Ok();
    const EErrCategory category =
        result.Error == ESettingsPersistenceError::AllocationFailure
        ? EErrCategory::Memory
        : EErrCategory::IO;
    return FErrorCode(
        category, static_cast<u16>(result.Error),
        SettingsPersistenceErrorName(result.Error), FSourceLoc::Current(),
        result.OsError);
}

TResult<void> FSettings::Load(const wchar_t* file_path) noexcept {
    const FSettingsPersistenceResult result = TryLoad(file_path);
    if (result) return Ok();
    const EErrCategory category =
        result.Error == ESettingsPersistenceError::AllocationFailure
        ? EErrCategory::Memory
        : EErrCategory::IO;
    return FErrorCode(
        category, static_cast<u16>(result.Error),
        SettingsPersistenceErrorName(result.Error), FSourceLoc::Current(),
        result.OsError);
}

} // namespace acs::game
