// SPDX-License-Identifier: Apache-2.0
// FSettings 永続化境界の厳格解析・トランザクション性・原子的保存テスト。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Settings.h"
#include "foundation/Platform.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace {

bool TextEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

struct FSettingsTempPath {
    explicit FSettingsTempPath(const wchar_t* tag) noexcept {
        usize position = ::GetTempPathW(MAX_PATH, Path);
        Append(position, L"acs_settings_safety_");
        AppendU32(position, static_cast<u32>(::GetCurrentProcessId()));
        Append(position, L"_");
        AppendU32(position, static_cast<u32>(::GetCurrentThreadId()));
        Append(position, L"_");
        Append(position, tag);
        Append(position, L".ini");
        Path[position] = L'\0';
        ::DeleteFileW(Path);

        usize legacy_position = 0u;
        while (Path[legacy_position] != L'\0') {
            LegacyTemporaryPath[legacy_position] = Path[legacy_position];
            ++legacy_position;
        }
        Append(legacy_position, L".tmp", LegacyTemporaryPath);
        LegacyTemporaryPath[legacy_position] = L'\0';
        ::DeleteFileW(LegacyTemporaryPath);
    }

    ~FSettingsTempPath() noexcept {
        ::DeleteFileW(Path);
        ::DeleteFileW(LegacyTemporaryPath);
    }

    static void Append(usize& position, const wchar_t* text, wchar_t* output) noexcept {
        while (*text != L'\0' && position + 1u < MAX_PATH + 128u) {
            output[position++] = *text++;
        }
    }

    void Append(usize& position, const wchar_t* text) noexcept {
        Append(position, text, Path);
    }

    static void AppendU32(
        usize& position, u32 value, wchar_t* output) noexcept {
        wchar_t reversed[10]{};
        usize count = 0u;
        do {
            reversed[count++] = static_cast<wchar_t>(L'0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        while (count != 0u && position + 1u < MAX_PATH + 128u) {
            output[position++] = reversed[--count];
        }
    }

    void AppendU32(usize& position, u32 value) noexcept {
        AppendU32(position, value, Path);
    }

    bool Write(const void* bytes, usize size) const noexcept {
        HANDLE file = ::CreateFileW(
            Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const u8* cursor = static_cast<const u8*>(bytes);
        usize remaining = size;
        bool succeeded = true;
        while (remaining != 0u) {
            const DWORD chunk = remaining > 0x7FFFFFFFu
                ? 0x7FFFFFFFu
                : static_cast<DWORD>(remaining);
            DWORD written = 0u;
            if (!::WriteFile(file, cursor, chunk, &written, nullptr) ||
                written != chunk) {
                succeeded = false;
                break;
            }
            cursor += written;
            remaining -= written;
        }
        succeeded = ::CloseHandle(file) != 0 && succeeded;
        return succeeded;
    }

    bool WriteLegacyCollision(const char* text) const noexcept {
        HANDLE file = ::CreateFileW(
            LegacyTemporaryPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        usize length = 0u;
        while (text[length] != '\0') ++length;
        DWORD written = 0u;
        const bool succeeded =
            ::WriteFile(
                file, text, static_cast<DWORD>(length), &written, nullptr) != 0 &&
            written == length;
        return ::CloseHandle(file) != 0 && succeeded;
    }

    bool LegacyCollisionExists() const noexcept {
        return ::GetFileAttributesW(LegacyTemporaryPath) != INVALID_FILE_ATTRIBUTES;
    }

    bool MakeSparse(usize size) const noexcept {
        HANDLE file = ::CreateFileW(
            Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(size);
        const bool succeeded =
            ::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) != 0 &&
            ::SetEndOfFile(file) != 0;
        return ::CloseHandle(file) != 0 && succeeded;
    }

    bool WriteEntryRecords(u32 count) const noexcept {
        HANDLE file = ::CreateFileW(
            Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        bool succeeded = true;
        for (u32 i = 0u; i < count && succeeded; ++i) {
            char line[64]{};
            const int length = ::snprintf(
                line, sizeof(line), "i:key%u=1\n",
                static_cast<unsigned int>(i));
            if (length <= 0 || static_cast<usize>(length) >= sizeof(line)) {
                succeeded = false;
                break;
            }
            DWORD written = 0u;
            succeeded = ::WriteFile(
                file, line, static_cast<DWORD>(length), &written, nullptr) != 0 &&
                written == static_cast<DWORD>(length);
        }
        return ::CloseHandle(file) != 0 && succeeded;
    }

    wchar_t Path[MAX_PATH + 128]{};
    wchar_t LegacyTemporaryPath[MAX_PATH + 128]{};
};

} // namespace

ACS_TEST(SettingsSafety, CheckedRoundTripPreservesAllSupportedKinds)
{
    FSettingsTempPath path(L"round_trip");
    FSettings source;
    source.SetF32("audio.master", 0.8125f);
    source.SetI32("display.width", -2147483647);
    source.SetBool("display.vsync", true);
    source.SetString("locale", "ja-JP");

    const FSettingsPersistenceResult saved = source.TrySave(path.Path);
    EXPECT_TRUE(saved.Succeeded());
    EXPECT_EQ(saved.Entries, 4u);

    FSettings loaded;
    const FSettingsPersistenceResult result = loaded.TryLoad(path.Path);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.Entries, 4u);
    EXPECT_EQ(loaded.Count(), 4u);
    EXPECT_NEAR(loaded.GetF32("audio.master"), 0.8125f, 0.000001f);
    EXPECT_EQ(loaded.GetI32("display.width"), -2147483647);
    EXPECT_TRUE(loaded.GetBool("display.vsync"));
    EXPECT_TRUE(TextEquals(loaded.GetString("locale"), "ja-JP"));
    EXPECT_TRUE(TextEquals(
        SettingsPersistenceErrorName(ESettingsPersistenceError::DuplicateKey),
        "DuplicateKey"));
}

ACS_TEST(SettingsSafety, CorruptLoadPreservesOwnedPointersAndExistingState)
{
    FSettingsTempPath path(L"transaction");
    const char valid[] = "s:name=stable\ni:count=42\n";
    EXPECT_TRUE(path.Write(valid, sizeof(valid) - 1u));

    FSettings settings;
    EXPECT_TRUE(settings.TryLoad(path.Path).Succeeded());
    const char* const stable_pointer = settings.GetString("name");
    EXPECT_TRUE(TextEquals(stable_pointer, "stable"));

    const char duplicate[] = "s:name=replaced\ni:name=7\n";
    EXPECT_TRUE(path.Write(duplicate, sizeof(duplicate) - 1u));
    const FSettingsPersistenceResult result = settings.TryLoad(path.Path);

    EXPECT_EQ(result.Error, ESettingsPersistenceError::DuplicateKey);
    EXPECT_EQ(result.Line, 2u);
    EXPECT_EQ(settings.Count(), 2u);
    EXPECT_TRUE(settings.GetString("name") == stable_pointer);
    EXPECT_TRUE(TextEquals(settings.GetString("name"), "stable"));
    EXPECT_EQ(settings.GetI32("count"), 42);
}

ACS_TEST(SettingsSafety, StrictScalarGrammarRejectsOverflowAndInvalidTokens)
{
    FSettingsTempPath path(L"strict_scalars");
    FSettings settings;
    settings.SetI32("preserved", 99);

    const char integer_overflow[] = "i:value=2147483648\n";
    EXPECT_TRUE(path.Write(integer_overflow, sizeof(integer_overflow) - 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::InvalidInteger);
    EXPECT_EQ(settings.GetI32("preserved"), 99);

    const char invalid_bool[] = "b:value=TRUE\n";
    EXPECT_TRUE(path.Write(invalid_bool, sizeof(invalid_bool) - 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::InvalidBool);

    const char non_finite[] = "f:value=1e999\n";
    EXPECT_TRUE(path.Write(non_finite, sizeof(non_finite) - 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::InvalidFloat);

    const char invalid_float[] = "f:value=nan\n";
    EXPECT_TRUE(path.Write(invalid_float, sizeof(invalid_float) - 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::NonFiniteFloat);

    const char trailing_junk[] = "f:value=1.0oops\n";
    EXPECT_TRUE(path.Write(trailing_junk, sizeof(trailing_junk) - 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::InvalidFloat);
}

ACS_TEST(SettingsSafety, EmbeddedNulAndOversizedInputAreRejectedTransactionally)
{
    FSettingsTempPath path(L"input_limits");
    FSettings settings;
    settings.SetBool("preserved", true);

    const char embedded_nul[] = {
        'i', ':', 'a', '=', '1', '\n',
        '\0',
        'i', ':', 'b', '=', '2', '\n'
    };
    EXPECT_TRUE(path.Write(embedded_nul, sizeof(embedded_nul)));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::EmbeddedNul);
    EXPECT_TRUE(settings.GetBool("preserved"));

    EXPECT_TRUE(path.MakeSparse(FSettings::kMaxPersistenceBytes + 1u));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::FileTooLarge);
    EXPECT_TRUE(settings.GetBool("preserved"));
}

ACS_TEST(SettingsSafety, LineAndKeyLimitsAreEnforced)
{
    FSettingsTempPath path(L"line_limits");
    char oversized_line[FSettings::kMaxPersistenceLineBytes + 2u]{};
    oversized_line[0] = '#';
    for (usize i = 1u; i < sizeof(oversized_line); ++i) {
        oversized_line[i] = 'x';
    }
    EXPECT_TRUE(path.Write(oversized_line, sizeof(oversized_line)));

    FSettings settings;
    const FSettingsPersistenceResult line_result = settings.TryLoad(path.Path);
    EXPECT_EQ(line_result.Error, ESettingsPersistenceError::LineTooLong);
    EXPECT_EQ(line_result.Line, 1u);

    char oversized_key[FSettings::kMaxPersistenceKeyBytes + 8u]{};
    oversized_key[0] = 'i';
    oversized_key[1] = ':';
    usize position = 2u;
    for (usize i = 0u; i <= FSettings::kMaxPersistenceKeyBytes; ++i) {
        oversized_key[position++] = 'k';
    }
    oversized_key[position++] = '=';
    oversized_key[position++] = '1';
    oversized_key[position++] = '\n';
    EXPECT_TRUE(path.Write(oversized_key, position));
    EXPECT_EQ(
        settings.TryLoad(path.Path).Error,
        ESettingsPersistenceError::KeyTooLong);
}

ACS_TEST(SettingsSafety, EntryLimitIsRejectedBeforeCommit)
{
    FSettingsTempPath path(L"entry_limit");
    EXPECT_TRUE(path.WriteEntryRecords(FSettings::kMaxPersistenceEntries + 1u));

    FSettings settings;
    settings.SetString("stable", "pointer");
    const char* const pointer = settings.GetString("stable");
    const FSettingsPersistenceResult result = settings.TryLoad(path.Path);

    EXPECT_EQ(result.Error, ESettingsPersistenceError::EntryLimitExceeded);
    EXPECT_EQ(result.Line, FSettings::kMaxPersistenceEntries + 1u);
    EXPECT_EQ(settings.Count(), 1u);
    EXPECT_TRUE(settings.GetString("stable") == pointer);
}

ACS_TEST(SettingsSafety, AtomicSaveIgnoresLegacyTempCollisionAndPreservesFileOnValidationFailure)
{
    FSettingsTempPath path(L"atomic");
    EXPECT_TRUE(path.WriteLegacyCollision("do-not-touch"));

    FSettings settings;
    settings.SetString("mode", "old");
    EXPECT_TRUE(settings.TrySave(path.Path).Succeeded());
    EXPECT_TRUE(path.LegacyCollisionExists());

    settings.SetString("mode", "invalid\nvalue");
    const FSettingsPersistenceResult rejected = settings.TrySave(path.Path);
    EXPECT_EQ(rejected.Error, ESettingsPersistenceError::UnrepresentableText);

    FSettings verifier;
    EXPECT_TRUE(verifier.TryLoad(path.Path).Succeeded());
    EXPECT_TRUE(TextEquals(verifier.GetString("mode"), "old"));
    EXPECT_TRUE(path.LegacyCollisionExists());
}

ACS_TEST(SettingsSafety, AtomicSaveKeepsOpenReaderSnapshot)
{
    FSettingsTempPath path(L"reader_snapshot");
    FSettings settings;
    settings.SetString("version", "old");
    EXPECT_TRUE(settings.TrySave(path.Path).Succeeded());

    HANDLE old_reader = ::CreateFileW(
        path.Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(old_reader != INVALID_HANDLE_VALUE);
    if (old_reader == INVALID_HANDLE_VALUE) return;

    settings.SetString("version", "new");
    const FSettingsPersistenceResult saved = settings.TrySave(path.Path);
    EXPECT_TRUE(saved.Succeeded());

    char old_bytes[64]{};
    DWORD old_size = 0u;
    EXPECT_TRUE(::ReadFile(
        old_reader, old_bytes, sizeof(old_bytes) - 1u, &old_size, nullptr) != 0);
    EXPECT_TRUE(::CloseHandle(old_reader) != 0);
    old_bytes[old_size < sizeof(old_bytes) ? old_size : sizeof(old_bytes) - 1u] = '\0';
    EXPECT_TRUE(TextEquals(old_bytes, "s:version=old\n"));

    FSettings fresh_reader;
    EXPECT_TRUE(fresh_reader.TryLoad(path.Path).Succeeded());
    EXPECT_TRUE(TextEquals(fresh_reader.GetString("version"), "new"));
}

ACS_TEST(SettingsSafety, EmptyDocumentCommitsAnEmptyStore)
{
    FSettingsTempPath path(L"empty");
    EXPECT_TRUE(path.Write(nullptr, 0u));

    FSettings settings;
    settings.SetI32("old", 1);
    EXPECT_TRUE(settings.TryLoad(path.Path).Succeeded());
    EXPECT_EQ(settings.Count(), 0u);
    EXPECT_FALSE(settings.Has("old"));
}

ACS_TEST(SettingsSafety, LegacyResultApiCarriesStableCheckedSubcode)
{
    FSettings settings;
    const TResult<void> result = settings.Load(nullptr);
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(
        result.Error().subcode,
        static_cast<u16>(ESettingsPersistenceError::NullPath));
}
