// SPDX-License-Identifier: Apache-2.0
// FSaveArchive の往復・破損/改竄検知テスト
//
// .acssave はユーザーが改竄し得る外部入力なので、正常往復だけでなく
// «壊れた/悪意あるファイルを安全に拒否できるか» を重点的に検証する:
//   ・payload CRC 改竄 → kSubChecksumFail
//     (呼び出し側 buffer/object は未変更)
//   ・header の payload_size を巨大値に改竄 → PeekPayloadSize/ReadFromFile が拒否
//     (呼び出し側が申告値で巨大確保して OOM/DoS しないための回帰テスト)
//   ・header 申告より短い/長い実データ → kSubSizeMismatch
//   ・version 不一致 → kSubMigrationNeeded (buffer 未書き込み)
//   ・容量不足 → kSubBufferTooSmall
//   ・header 未満に切り詰め → kSubBadMagic
//   ・atomic write の一時ファイル作成失敗 → 既存 save は保持
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SaveArchive.h"
#include "foundation/Platform.h"   // <windows.h> (テスト用ファイル操作)
#include "memory/Memory.h"         // MemCmp

using namespace acs;
using namespace acs::game;

namespace {

/** subcode 比較用の縮約 (実装側 SubU16 と同じ)。 */
constexpr u16 Sub(ESaveArchiveSubCode sc) noexcept
{
    return static_cast<u16>(static_cast<u32>(sc));
}

/** テスト用の一時 .acssave パスを組み立てる (%TEMP%\acs_save_test_<pid>_<tag>.acssave)。 */
struct FTempSavePath {
    explicit FTempSavePath(const wchar_t* tag) noexcept
    {
        // CRT の swprintf 系は include しない方針なので手で連結する。
        usize pos = 0;
        // 失敗時は 0 文字 (カレントディレクトリ相対) に落ちる (テストでは許容)。
        pos = ::GetTempPathW(MAX_PATH, m_Path);
        AppendW(pos, L"acs_save_test_");
        AppendU32(pos, static_cast<u32>(::GetCurrentProcessId()));
        AppendW(pos, L"_");
        AppendW(pos, tag);
        AppendW(pos, L".acssave");
        m_Path[pos] = L'\0';
        ::DeleteFileW(m_Path);
    }

    /** 終端まで文字列を追記する (バッファ末尾で切り詰め)。 */
    void AppendW(usize& pos, const wchar_t* s) noexcept
    {
        while (*s != L'\0' && pos + 1 < sizeof(m_Path) / sizeof(m_Path[0])) {
            m_Path[pos++] = *s++;
        }
    }

    /** u32 を 10 進で追記する。 */
    void AppendU32(usize& pos, u32 v) noexcept
    {
        wchar_t digits[10] = {};
        usize n = 0;
        do {
            digits[n++] = static_cast<wchar_t>(L'0' + (v % 10u));
            v /= 10u;
        } while (v != 0u);
        while (n > 0) {
            if (pos + 1 >= sizeof(m_Path) / sizeof(m_Path[0])) break;
            m_Path[pos++] = digits[--n];
        }
    }

    ~FTempSavePath() noexcept
    {
        ::DeleteFileW(m_Path);
    }

    const wchar_t* Get() const noexcept
    {
        return m_Path;
    }

    wchar_t m_Path[MAX_PATH + 64] = {};
};

/** ファイル先頭から Offset の位置へ Size バイトを上書きする (改竄シミュレーション)。 */
bool PatchFileBytes(const wchar_t* path, u64 offset, const void* bytes, u32 size) noexcept
{
    HANDLE h = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    bool ok = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
    if (ok) {
        DWORD wrote = 0;
        ok = ::WriteFile(h, bytes, size, &wrote, nullptr) != 0 && wrote == size;
    }
    ::CloseHandle(h);
    return ok;
}

/** ファイルを new_size バイトへ切り詰める。 */
bool TruncateFile(const wchar_t* path, u64 new_size) noexcept
{
    HANDLE h = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(new_size);
    bool ok = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0
           && ::SetEndOfFile(h) != 0;
    ::CloseHandle(h);
    return ok;
}

/** ファイル末尾へ 1 byte 追記する (余分なデータの改竄シミュレーション)。 */
bool AppendFileByte(const wchar_t* path, u8 value) noexcept
{
    HANDLE h = ::CreateFileW(path, FILE_APPEND_DATA, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = ::WriteFile(h, &value, 1u, &wrote, nullptr) != 0 && wrote == 1u;
    ::CloseHandle(h);
    return ok;
}

/** 実装と同じ `<path>.tmp.<pid>.<tid>` を atomicity 妨害テスト用に組み立てる。 */
bool MakeExpectedTempPath(const wchar_t* path, wchar_t* out, usize cap) noexcept
{
    usize pos = 0;
    while (path[pos] != L'\0' && pos + 1 < cap) {
        out[pos] = path[pos];
        ++pos;
    }
    if (path[pos] != L'\0') return false;

    const wchar_t suffix[] = L".tmp.";
    for (usize i = 0; suffix[i] != L'\0'; ++i) {
        if (pos + 1 >= cap) return false;
        out[pos++] = suffix[i];
    }

    const auto append_u32 = [out, cap](usize& write_pos, u32 value) noexcept {
        wchar_t digits[10] = {};
        usize count = 0;
        do {
            digits[count++] = static_cast<wchar_t>(L'0' + (value % 10u));
            value /= 10u;
        } while (value != 0u);
        if (write_pos >= cap || count >= cap - write_pos) return false;
        while (count > 0) out[write_pos++] = digits[--count];
        return true;
    };

    if (!append_u32(pos, static_cast<u32>(::GetCurrentProcessId()))) return false;
    if (pos + 1 >= cap) return false;
    out[pos++] = L'.';
    if (!append_u32(pos, static_cast<u32>(::GetCurrentThreadId()))) return false;
    out[pos] = L'\0';
    return true;
}

/** テストで往復させる代表 payload。 */
struct FTestProfile {
    u32 Gold      = 0;
    u32 Level     = 0;
    u8  Name[16]  = {};
};

} // namespace

ACS_TEST(SaveArchive, RoundTripPreservesPayloadVersionAndPeeks)
{
    FTempSavePath path(L"roundtrip");

    FTestProfile out{};
    out.Gold  = 12345u;
    out.Level = 42u;
    out.Name[0] = 'G'; out.Name[1] = 'a'; out.Name[2] = 'k'; out.Name[3] = 'u';

    const auto wr = FSaveArchive::WriteToFile(path.Get(), 3u, &out, sizeof(out));
    EXPECT_TRUE(wr.IsOk());

    // Peek 系は header のみで正しい値を返す。
    const auto ver = FSaveArchive::PeekVersion(path.Get());
    EXPECT_TRUE(ver.IsOk());
    if (ver.IsOk()) EXPECT_EQ(ver.Value(), 3u);
    const auto sz = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(sz.IsOk());
    if (sz.IsOk()) EXPECT_EQ(sz.Value(), static_cast<u64>(sizeof(out)));

    FTestProfile in{};
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &in, sizeof(in), 3u, actual);
    EXPECT_TRUE(rd.IsOk());
    EXPECT_EQ(actual, static_cast<u64>(sizeof(out)));
    EXPECT_TRUE(MemCmp(&in, &out, sizeof(out)) == 0);

    wchar_t temp_path[MAX_PATH + 128] = {};
    EXPECT_TRUE(MakeExpectedTempPath(path.Get(), temp_path,
                                     sizeof(temp_path) / sizeof(temp_path[0])));
    EXPECT_EQ(::GetFileAttributesW(temp_path), INVALID_FILE_ATTRIBUTES);
}

ACS_TEST(SaveArchive, TamperedPayloadFailsCrc)
{
    FTempSavePath path(L"crc");

    FTestProfile out{};
    out.Gold = 100u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &out, sizeof(out)).IsOk());

    // payload 先頭 (offset 24 = header 直後) の Gold を 1 バイト改竄する。
    const u8 evil = 0xFFu;
    EXPECT_TRUE(PatchFileBytes(path.Get(), FSaveArchive::kHeaderSize, &evil, 1u));

    FTestProfile in{};
    in.Gold = 0xDEADBEEFu;
    in.Level = 0xCAFEBABEu;
    FTestProfile before = in;
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &in, sizeof(in), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubChecksumFail));
    EXPECT_TRUE(MemCmp(&in, &before, sizeof(in)) == 0);
    EXPECT_EQ(actual, static_cast<u64>(sizeof(out)));
}

ACS_TEST(SaveArchive, HugeClaimedPayloadSizeIsRejectedBeforeAllocation)
{
    FTempSavePath path(L"hugesize");

    FTestProfile out{};
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &out, sizeof(out)).IsOk());

    // header の payload_size (offset 0x0C, u64 LE) を 2^62 に改竄する。
    // PeekPayloadSize がこの申告値をそのまま返すと、呼び出し側 (FProgression::Load 等) が
    // 「確保するサイズ」として信じて巨大確保 → OOM/DoS になる。実ファイルサイズとの
    // 照合で弾くことを検証する回帰テスト。
    const u64 huge = 1ull << 62;
    u8 huge_le[8] = {};
    for (u32 i = 0; i < 8; ++i) huge_le[i] = static_cast<u8>((huge >> (8u * i)) & 0xFFu);
    EXPECT_TRUE(PatchFileBytes(path.Get(), 12u, huge_le, 8u));

    const auto sz = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(sz.IsErr());
    if (sz.IsErr()) EXPECT_EQ(sz.Error().subcode, Sub(ESaveArchiveSubCode::kSubPayloadTooLarge));

    // ReadFromFile 側も同じ理由で拒否する (out_capacity 検査より前のサイズ整合検査)。
    FTestProfile in{};
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &in, sizeof(in), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubPayloadTooLarge));
}

ACS_TEST(SaveArchive, VersionMismatchReturnsMigrationNeededWithoutWriting)
{
    FTempSavePath path(L"version");

    FTestProfile out{};
    out.Gold = 777u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &out, sizeof(out)).IsOk());

    // 期待 version=2 で読む → migration 要求。buffer は書き込まれない。
    FTestProfile in{};
    in.Gold = 0xDEADBEEFu;
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &in, sizeof(in), 2u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubMigrationNeeded));
    EXPECT_EQ(in.Gold, 0xDEADBEEFu);                   // buffer 未変更
    EXPECT_EQ(actual, static_cast<u64>(sizeof(out)));  // サイズは報告される (migration 用)
}

ACS_TEST(SaveArchive, BufferTooSmallIsRejected)
{
    FTempSavePath path(L"toosmall");

    FTestProfile out{};
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &out, sizeof(out)).IsOk());

    u8 tiny[4] = {0xA5u, 0xA5u, 0xA5u, 0xA5u};
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), tiny, sizeof(tiny), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubBufferTooSmall));
    EXPECT_EQ(tiny[0], 0xA5u);
    EXPECT_EQ(tiny[1], 0xA5u);
    EXPECT_EQ(tiny[2], 0xA5u);
    EXPECT_EQ(tiny[3], 0xA5u);
    EXPECT_EQ(actual, static_cast<u64>(sizeof(out)));
}

ACS_TEST(SaveArchive, TruncatedFileIsRejectedAsBadMagic)
{
    FTempSavePath path(L"truncated");

    FTestProfile out{};
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &out, sizeof(out)).IsOk());
    EXPECT_TRUE(TruncateFile(path.Get(), FSaveArchive::kHeaderSize - 1u));

    FTestProfile in{};
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &in, sizeof(in), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubBadMagic));

    const auto sz = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(sz.IsErr());
    if (sz.IsErr()) EXPECT_EQ(sz.Error().subcode, Sub(ESaveArchiveSubCode::kSubBadMagic));
}

ACS_TEST(SaveArchive, TruncatedPayloadIsRejectedWithoutChangingOutput)
{
    FTempSavePath path(L"short_payload");

    FTestProfile saved{};
    saved.Gold = 91u;
    saved.Level = 7u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &saved, sizeof(saved)).IsOk());
    EXPECT_TRUE(TruncateFile(path.Get(),
                             FSaveArchive::kHeaderSize + sizeof(saved) - 1u));

    FTestProfile output{};
    output.Gold = 0x12345678u;
    output.Level = 0x87654321u;
    const FTestProfile before = output;
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &output, sizeof(output), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) {
        EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubSizeMismatch));
    }
    EXPECT_TRUE(MemCmp(&output, &before, sizeof(output)) == 0);
    EXPECT_EQ(actual, 0ull);

    const auto size = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(size.IsErr());
    if (size.IsErr()) {
        EXPECT_EQ(size.Error().subcode, Sub(ESaveArchiveSubCode::kSubSizeMismatch));
    }
}

ACS_TEST(SaveArchive, TrailingDataIsRejectedWithoutChangingOutput)
{
    FTempSavePath path(L"trailing_payload");

    FTestProfile saved{};
    saved.Gold = 300u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &saved, sizeof(saved)).IsOk());
    EXPECT_TRUE(AppendFileByte(path.Get(), 0xCCu));

    FTestProfile output{};
    output.Gold = 0xA5A5A5A5u;
    const FTestProfile before = output;
    u64 actual = 0;
    const auto rd = FSaveArchive::ReadFromFile(path.Get(), &output, sizeof(output), 1u, actual);
    EXPECT_TRUE(rd.IsErr());
    if (rd.IsErr()) {
        EXPECT_EQ(rd.Error().subcode, Sub(ESaveArchiveSubCode::kSubSizeMismatch));
    }
    EXPECT_TRUE(MemCmp(&output, &before, sizeof(output)) == 0);
    EXPECT_EQ(actual, 0ull);

    const auto size = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(size.IsErr());
    if (size.IsErr()) {
        EXPECT_EQ(size.Error().subcode, Sub(ESaveArchiveSubCode::kSubSizeMismatch));
    }
}

ACS_TEST(SaveArchive, AtomicWriteFailurePreservesExistingSave)
{
    FTempSavePath path(L"atomic_preserve");

    FTestProfile original{};
    original.Gold = 111u;
    original.Level = 2u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &original, sizeof(original)).IsOk());

    // 実装が使う temp path を directory で占有し、CreateFileW を確実に失敗させる。
    wchar_t temp_path[MAX_PATH + 128] = {};
    EXPECT_TRUE(MakeExpectedTempPath(path.Get(), temp_path,
                                     sizeof(temp_path) / sizeof(temp_path[0])));
    ::RemoveDirectoryW(temp_path);
    const bool made_blocking_directory = ::CreateDirectoryW(temp_path, nullptr) != 0;
    EXPECT_TRUE(made_blocking_directory);

    FTestProfile replacement{};
    replacement.Gold = 999u;
    replacement.Level = 50u;
    const auto write = FSaveArchive::WriteToFile(path.Get(), 2u,
                                                 &replacement, sizeof(replacement));
    EXPECT_TRUE(write.IsErr());

    if (made_blocking_directory) {
        EXPECT_TRUE(::RemoveDirectoryW(temp_path) != 0);
    }

    FTestProfile loaded{};
    u64 actual = 0;
    const auto read = FSaveArchive::ReadFromFile(path.Get(), &loaded, sizeof(loaded), 1u, actual);
    EXPECT_TRUE(read.IsOk());
    EXPECT_EQ(actual, static_cast<u64>(sizeof(original)));
    EXPECT_TRUE(MemCmp(&loaded, &original, sizeof(original)) == 0);
}

ACS_TEST(SaveArchive, AtomicReplaceKeepsExistingReaderSnapshotConsistent)
{
    FTempSavePath path(L"atomic_reader");

    FTestProfile original{};
    original.Gold = 10u;
    original.Level = 1u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &original, sizeof(original)).IsOk());

    HANDLE old_reader = ::CreateFileW(path.Get(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(old_reader != INVALID_HANDLE_VALUE);

    FTestProfile replacement{};
    replacement.Gold = 20u;
    replacement.Level = 2u;
    const auto write = FSaveArchive::WriteToFile(path.Get(), 2u,
                                                 &replacement, sizeof(replacement));
    EXPECT_TRUE(write.IsOk());

    // 置換前に開いた handle は古い file object の完全な snapshot を読み続ける。
    FTestProfile old_snapshot{};
    LARGE_INTEGER payload_offset{};
    payload_offset.QuadPart = static_cast<LONGLONG>(FSaveArchive::kHeaderSize);
    EXPECT_TRUE(::SetFilePointerEx(old_reader, payload_offset, nullptr, FILE_BEGIN) != 0);
    DWORD got = 0;
    EXPECT_TRUE(::ReadFile(old_reader, &old_snapshot,
                           static_cast<DWORD>(sizeof(old_snapshot)), &got, nullptr) != 0);
    EXPECT_EQ(got, static_cast<DWORD>(sizeof(old_snapshot)));
    EXPECT_TRUE(MemCmp(&old_snapshot, &original, sizeof(original)) == 0);
    if (old_reader != INVALID_HANDLE_VALUE) {
        EXPECT_TRUE(::CloseHandle(old_reader) != 0);
    }

    FTestProfile current{};
    u64 actual = 0;
    const auto read = FSaveArchive::ReadFromFile(path.Get(), &current, sizeof(current), 2u, actual);
    EXPECT_TRUE(read.IsOk());
    EXPECT_TRUE(MemCmp(&current, &replacement, sizeof(replacement)) == 0);
}

ACS_TEST(SaveArchive, InvalidWriteDoesNotTruncateExistingSave)
{
    FTempSavePath path(L"invalid_preserve");

    FTestProfile original{};
    original.Gold = 444u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &original, sizeof(original)).IsOk());

    const auto write = FSaveArchive::WriteToFile(path.Get(), 2u, nullptr, 1u);
    EXPECT_TRUE(write.IsErr());
    if (write.IsErr()) {
        EXPECT_EQ(write.Error().subcode, Sub(ESaveArchiveSubCode::kSubInvalidArgument));
    }

    FTestProfile loaded{};
    u64 actual = 0;
    const auto read = FSaveArchive::ReadFromFile(path.Get(), &loaded, sizeof(loaded), 1u, actual);
    EXPECT_TRUE(read.IsOk());
    EXPECT_TRUE(MemCmp(&loaded, &original, sizeof(original)) == 0);
}

ACS_TEST(SaveArchive, OversizedWriteIsRejectedBeforeReadingPayload)
{
    FTempSavePath path(L"oversized_write");

    FTestProfile original{};
    original.Gold = 808u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u, &original, sizeof(original)).IsOk());

    // 1 byte しかない入力 pointer と上限超過 size。size を先に拒否できれば範囲外を読まない。
    const u8 one_byte = 0x5Au;
    const auto write = FSaveArchive::WriteToFile(path.Get(), 2u, &one_byte,
                                                 FSaveArchive::kMaxPayloadSize + 1u);
    EXPECT_TRUE(write.IsErr());
    if (write.IsErr()) {
        EXPECT_EQ(write.Error().subcode, Sub(ESaveArchiveSubCode::kSubPayloadTooLarge));
    }

    FTestProfile loaded{};
    u64 actual = 0;
    const auto read = FSaveArchive::ReadFromFile(path.Get(), &loaded, sizeof(loaded), 1u, actual);
    EXPECT_TRUE(read.IsOk());
    EXPECT_TRUE(MemCmp(&loaded, &original, sizeof(original)) == 0);
}

ACS_TEST(SaveArchive, EmptyPayloadRoundTripsWithoutOutputBuffer)
{
    FTempSavePath path(L"empty");

    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 9u, nullptr, 0u).IsOk());

    const auto size = FSaveArchive::PeekPayloadSize(path.Get());
    EXPECT_TRUE(size.IsOk());
    if (size.IsOk()) EXPECT_EQ(size.Value(), 0ull);

    u64 actual = 99u;
    const auto read = FSaveArchive::ReadFromFile(path.Get(), nullptr, 0u, 9u, actual);
    EXPECT_TRUE(read.IsOk());
    EXPECT_EQ(actual, 0ull);
}
