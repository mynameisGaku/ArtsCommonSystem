// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CReplayDirector 実装
//
// Init / StartRecording / StopRecording / StartPlayback /
// PausePlayback / ResumePlayback / StopPlayback /
// SetPlaybackSpeed / SeekToTick / Tick / ProgressNormalized / SaveReplay /
// LoadReplay をすべて本実装する。SaveReplay / LoadReplay は CInputRecorder /
// CLockstep の SaveToBuffer / LoadFromBuffer を束ねた container を扱う。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartPlayback)  → Playback  → (StopPlayback)  → Idle
//                       Playback ↔ (Pause/Resume) ↔ Paused
//                       Paused → (StopPlayback) → Idle
//     Recording から Playback への直接遷移、および Idle から Pause/Resume への
//     遷移は kSub_BadMode で拒否 (一旦 Stop を挟むことを強制)。
//   ・Tick(dt) は録画中と再生中で意味が変わる:
//     - Recording: m_TickRateHz * dt 分の tick を m_CurrentTick に加算する。
//       実際の入力 capture は CInputRecorder / CLockstep 側で別途行う。
//     - Playback: dt * m_PlaybackSpeed を accumulator に貯め、tick_rate_hz 単位
//       で m_CurrentTick に加算する。duration_ticks に達したら自動 Stop。
//   ・SetPlaybackSpeed は { 0.25, 0.5, 1, 2, 4, 8, 16 } 等を想定するが、
//     UI スクラブで任意 f32 が来ても扱えるよう範囲 clamp する (0.25 未満は
//     0.25、16 超は 16)。0 や負値は 1.0 にリセットして "誤呼び出しからの自動
//     復帰" を担保する。
//   ・SeekToTick は duration_ticks を上限に clamp。Mode は変えない (Paused 中
//     の scrub もできるよう)。accumulator も 0 にリセットする (sub-tick を
//     持ち越さない)。
//   ・ProgressNormalized は duration_ticks == 0 のとき 0.0 を返す (録画開始
//     直後 / metadata 未設定の安全側)。
//
// SaveReplay / LoadReplay (本実装):
//   ・単一 container ファイル (.acsr 拡張子想定) に metadata + CInputRecorder の
//     .acsr blob + CLockstep の .acsl blob をまとめて書き出す。低レベル blob 自体は
//     CInputRecorder::SaveToBuffer / CLockstep::SaveToBuffer の real な直列化を
//     そのまま使い、本クラスは「container header + metadata + 2 blob + CRC32」を
//     被せるだけにする (二重 framing で各 blob は独立に検証可能なまま)。
//   ・Win32 I/O は CSaveArchive / CSettings と同流儀: `.tmp` に書いて
//     FlushFileBuffers → MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) で atomic
//     rename。途中失敗でも本ファイルは旧内容のまま。
//   ・byte codec は NetSnapshot.cpp / CSaveArchive.cpp と同じ LE MemCopy helper +
//     CRC32 (poly 0xEDB88320) を file-local に複製する (gameframework は assetpack を
//     依存に持たないため link 単位を独立させる方針)。
#include "gameframework/ReplayDirector.h"

#include "gameframework/InputRecorder.h"  // CInputRecorder::SaveToBuffer / LoadFromBuffer / SampleCount
#include "gameframework/Lockstep.h"       // CLockstep::SaveToBuffer / LoadFromBuffer / InputCount

#include "foundation/Platform.h"  // <windows.h> (CreateFileW / WriteFile / ReadFile / MoveFileExW)
#include "foundation/Move.h"
#include "memory/Memory.h"        // MemCopy / MemCmp / MemSet / DefaultAllocator
#include "container/Array.h"      // TArray (直列化バッファ)

#include <cstddef>

namespace acs::game {

namespace {

/** 再生速度の下限 (0.25x)。 */
constexpr f32 kMinSpeed = 0.25f;

/** 再生速度の上限 (16x)。 */
constexpr f32 kMaxSpeed = 16.0f;

/**
 * 再生速度を有効範囲に clamp し、異常値をガードする。
 *
 * @param v clamp 対象の速度。
 * @return [kMinSpeed, kMaxSpeed] に収めた速度 (0 / 負値 / NaN は 1.0)。
 */
inline f32 ClampSpeed(f32 v) noexcept {
    // 0 / 負値 / NaN は誤呼び出しとして 1.0 に強制復帰。Pause は別 API なので
    // ここで 0 を許容すると意味が二重になる。
    if (!(v > 0.0f)) {
        return 1.0f;  // NaN 比較は false になるので NaN もこの枝で 1.0 に。
    }
    if (v < kMinSpeed) return kMinSpeed;
    if (v > kMaxSpeed) return kMaxSpeed;
    return v;
}

/**
 * container 先頭の magic バイト列 'ACRP' (ACS Replay)。
 *
 * @details
 * container layout は [magic][version][metadata (seed/timestamp/duration + 4 len-prefixed 文字列)]
 * [input_blob_size + bytes][lockstep_blob_size + bytes][crc32 footer] の順。
 */
constexpr u8  kReplayMagic[4] = { 'A', 'C', 'R', 'P' };

/** container フォーマットのバージョン。 */
constexpr u32 kReplayVersion  = 1u;

/** footer の CRC32 のバイト数。 */
constexpr u32 kCrcFooterSize  = 4u;

/** metadata 数値部、各長さprefix、blob長prefix、CRCを含む空container長。 */
constexpr u64 kMinimumContainerBytes = 56ull;

/** inner recorder formatの固定値。 */
constexpr u32 kInputHeaderBytes = 16u;
constexpr u32 kInputRecordBytes = 29u;
constexpr u32 kSourceFooterBytes = 4u;

/** inner lockstep formatの固定値。 */
constexpr u32 kLockstepHeaderBytes = 16u;
constexpr u32 kLockstepRecordBytes = 17u;

/** 一意temp suffixを含むpath buffer長。 */
constexpr usize kReplayTempPathCapacity = kReplayMaximumPathChars + 64u;

/** process内でtemp pathを一意化するnonce。 */
volatile LONG64 g_ReplayTempNonce = 0;

/** pathが非空かつ公開上限内でNUL終端されているかを検証する。 */
bool IsValidReplayPath(const wchar_t* path) noexcept
{
    if (path == nullptr) return false;
    usize length = 0;
    while (length <= kReplayMaximumPathChars && path[length] != L'\0') ++length;
    return length > 0 && length <= kReplayMaximumPathChars;
}

/**
 * u32 を LE バイト列として書き込む (strict-aliasing 安全)。
 *
 * @param dst 書き込み先 (4 バイト)。
 * @param v 書き込む値。
 */
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }

/**
 * u64 を LE バイト列として書き込む (strict-aliasing 安全)。
 *
 * @param dst 書き込み先 (8 バイト)。
 * @param v 書き込む値。
 */
inline void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }

/**
 * LE バイト列から u32 を読み出す (strict-aliasing 安全)。
 *
 * @param src 読み出し元 (4 バイト)。
 * @return 復元した u32。
 */
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }

/**
 * LE バイト列から u64 を読み出す (strict-aliasing 安全)。
 *
 * @param src 読み出し元 (8 バイト)。
 * @return 復元した u64。
 */
inline u64  ReadU64LE (const u8* src) noexcept { u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v; }

/**
 * CRC32 (poly 0xEDB88320) の lookup table を返す。
 *
 * @details Meyer's singleton で thread-safe に 256 entry の table を遅延初期化する。
 * @return 256 要素の CRC32 lookup table。
 */
const u32* GetCrc32Table() noexcept
{
    struct FCrc32Table {
        FCrc32Table() noexcept
        {
            for (u32 Index = 0; Index < 256; ++Index) {
                u32 Value = Index;
                for (u32 Bit = 0; Bit < 8; ++Bit) {
                    Value = (Value & 1u) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
                }
                Values[Index] = Value;
            }
        }

        u32 Values[256] = {};
    };

    static const FCrc32Table Table;
    return Table.Values;
}

/**
 * バイト列の CRC32 を計算する (init/xorout 0xFFFFFFFF)。
 *
 * @param data 計算対象の先頭。
 * @param size バイト数。
 * @return CRC32 値。
 */
u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/** u64の10進表現をpath末尾へ追記する。 */
bool AppendDecimal(u64 value, wchar_t* out, usize& position, usize capacity) noexcept
{
    wchar_t reversed[20] = {};
    usize count = 0;
    do {
        reversed[count++] = static_cast<wchar_t>(L'0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    if (position > capacity || count >= capacity - position) return false;
    while (count > 0) out[position++] = reversed[--count];
    return true;
}

/** `<path>.tmp.<pid>.<tid>.<nonce>` を同一directoryに作る。 */
bool MakeAtomicTempPath(const wchar_t* path, wchar_t* out, usize capacity) noexcept
{
    if (path == nullptr || out == nullptr) return false;
    usize path_length = 0;
    while (path_length <= kReplayMaximumPathChars && path[path_length] != L'\0') ++path_length;
    if (path_length == 0 || path_length > kReplayMaximumPathChars) return false;

    constexpr wchar_t kSuffix[] = L".tmp.";
    constexpr usize kSuffixLength = 5u;
    if (path_length + kSuffixLength + 1u >= capacity) return false;
    for (usize i = 0; i < path_length; ++i) out[i] = path[i];
    usize position = path_length;
    for (usize i = 0; i < kSuffixLength; ++i) out[position++] = kSuffix[i];
    if (!AppendDecimal(static_cast<u64>(::GetCurrentProcessId()), out, position, capacity)) return false;
    if (position + 1u >= capacity) return false;
    out[position++] = L'.';
    if (!AppendDecimal(static_cast<u64>(::GetCurrentThreadId()), out, position, capacity)) return false;
    if (position + 1u >= capacity) return false;
    out[position++] = L'.';
    const u64 nonce = static_cast<u64>(::_InterlockedIncrement64(&g_ReplayTempNonce));
    if (!AppendDecimal(nonce, out, position, capacity) || position >= capacity) return false;
    out[position] = L'\0';
    return true;
}

/** NUL終端文字列を上限内だけ走査する。 */
bool TryStringLength(const char* text, u32 maximum, u32& out_length) noexcept
{
    out_length = 0;
    if (text == nullptr) return true;
    while (out_length <= maximum && text[out_length] != '\0') ++out_length;
    return out_length <= maximum;
}

/** checksumが空または16桁ASCII hexかを検証する。 */
bool IsCanonicalChecksum(const char* checksum, u32 length) noexcept
{
    if (length == 0) return true;
    if (checksum == nullptr || length != kReplayChecksumHexBytes) return false;
    for (u32 i = 0; i < length; ++i) {
        const char c = checksum[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

struct FMetadataLengths {
    u32 game_version = 0;
    u32 level_id = 0;
    u32 player_name = 0;
    u32 checksum = 0;
};

/** pointer metadataをbounded scanし、各lengthを返す。 */
bool MeasureMetadata(const FReplayMetadata& metadata, FMetadataLengths& lengths) noexcept
{
    return TryStringLength(metadata.game_version, kReplayMaximumGameVersionBytes, lengths.game_version) &&
           TryStringLength(metadata.level_id, kReplayMaximumLevelIdBytes, lengths.level_id) &&
           TryStringLength(metadata.player_name, kReplayMaximumPlayerNameBytes, lengths.player_name) &&
           TryStringLength(metadata.checksum_hex, kReplayChecksumHexBytes, lengths.checksum) &&
           IsCanonicalChecksum(metadata.checksum_hex, lengths.checksum);
}

/** source blobのheader、件数、完全サイズ、CRCをallocationなしでpreflightする。 */
bool ValidateSourceBlob(const u8* blob, u32 size, bool input_recorder) noexcept
{
    if (size == 0) return true;
    const u32 header_size = input_recorder ? kInputHeaderBytes : kLockstepHeaderBytes;
    const u32 record_size = input_recorder ? kInputRecordBytes : kLockstepRecordBytes;
    if (blob == nullptr || size < header_size + kSourceFooterBytes ||
        size > kReplayMaximumSourceBlobBytes) {
        return false;
    }
    const u8 expected_magic[4] = {
        'A', 'C', input_recorder ? static_cast<u8>('S') : static_cast<u8>('S'),
        input_recorder ? static_cast<u8>('R') : static_cast<u8>('L')
    };
    if (MemCmp(blob, expected_magic, sizeof(expected_magic)) != 0 || ReadU32LE(blob + 4u) != 1u) return false;
    const u32 tick_rate = ReadU32LE(blob + 8u);
    const u32 record_count = ReadU32LE(blob + 12u);
    if (tick_rate == 0 || tick_rate > 1000u || record_count > kReplayMaximumSourceRecords) return false;
    const u64 records_bytes = static_cast<u64>(record_count) * record_size;
    const u64 expected_size = static_cast<u64>(header_size) + records_bytes + kSourceFooterBytes;
    if (expected_size != size) return false;
    return ComputeCrc32(blob + header_size, records_bytes) ==
           ReadU32LE(blob + header_size + records_bytes);
}

/** Process Heap buffer。DefaultAllocator失敗注入からfile snapshotを分離する。 */
class FProcessHeapBuffer {
public:
    explicit FProcessHeapBuffer(u64 size) noexcept
    {
        if (size > 0 && size <= static_cast<u64>(~SIZE_T{0})) {
            m_Data = ::HeapAlloc(::GetProcessHeap(), 0, static_cast<SIZE_T>(size));
        }
    }

    ~FProcessHeapBuffer() noexcept
    {
        if (m_Data != nullptr) ::HeapFree(::GetProcessHeap(), 0, m_Data);
    }

    FProcessHeapBuffer(const FProcessHeapBuffer&) = delete;
    FProcessHeapBuffer& operator=(const FProcessHeapBuffer&) = delete;

    void* Data() noexcept { return m_Data; }

private:
    void* m_Data = nullptr;
};

/** FileRenameInfoExのDWORD flags layout。 */
struct FReplayRenameInfoEx {
    DWORD flags = 0;
    HANDLE root_directory = nullptr;
    DWORD file_name_length = 0;
    wchar_t file_name[1] = {};
};

/** 開いている旧snapshotと共存可能なPOSIX atomic replace fallback。 */
bool TryPosixAtomicReplace(const wchar_t* temporary_path, const wchar_t* target_path, DWORD& out_error) noexcept
{
    constexpr DWORD kReplaceIfExists = 0x00000001u;
    constexpr DWORD kPosixSemantics = 0x00000002u;
    constexpr auto kFileRenameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    out_error = 0;

    usize path_length = 0;
    while (path_length <= kReplayMaximumPathChars && target_path[path_length] != L'\0') ++path_length;
    if (path_length > kReplayMaximumPathChars) {
        out_error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    constexpr usize kPrefixBytes = offsetof(FReplayRenameInfoEx, file_name);
    const usize path_bytes = path_length * sizeof(wchar_t);
    const usize buffer_bytes = kPrefixBytes + path_bytes + sizeof(wchar_t);
    FProcessHeapBuffer storage(buffer_bytes);
    if (storage.Data() == nullptr) {
        out_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    auto* info = static_cast<FReplayRenameInfoEx*>(storage.Data());
    MemSet(info, 0, buffer_bytes);
    info->flags = kReplaceIfExists | kPosixSemantics;
    info->file_name_length = static_cast<DWORD>(path_bytes);
    if (path_bytes > 0) MemCopy(info->file_name, target_path, path_bytes);

    HANDLE source = ::CreateFileW(temporary_path, DELETE | SYNCHRONIZE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }
    const BOOL renamed = ::SetFileInformationByHandle(source, kFileRenameInfoEx, info,
                                                       static_cast<DWORD>(buffer_bytes));
    if (!renamed) out_error = ::GetLastError();
    // rename成功時点でcommit済み。後続CloseHandle診断で結果を失敗へ反転しない。
    (void)::CloseHandle(source);
    return renamed != 0;
}

/**
 * HANDLE に size バイトを chunk ループで全書き込みする。
 *
 * @param h 書き込み先のファイルハンドル。
 * @param src 書き込むデータの先頭。
 * @param size 書き込むバイト数。
 * @param err 失敗時に GetLastError を格納する出力。
 * @return 全書き込み成功で true、失敗で false。
 */
bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    const u8* p         = static_cast<const u8*>(src);
    u64       remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT;
            return false;
        }
        p         += wrote;
        remaining -= wrote;
    }
    return true;
}

/**
 * HANDLE から size バイトを chunk ループで全読み込みする。
 *
 * @param h 読み込み元のファイルハンドル。
 * @param dst 読み込み先バッファの先頭。
 * @param size 読み込むバイト数。
 * @param err 失敗時に GetLastError を格納する出力。
 * @return 全読み込み成功で true、EOF / 失敗で false。
 */
bool ReadAll(HANDLE h, void* dst, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    u8* p         = static_cast<u8*>(dst);
    u64 remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            return false;
        }
        p         += got;
        remaining -= got;
    }
    return true;
}

} // namespace

/** 録画 / 再生 state を初期値に戻す (source 結線と owned 文字列は保持)。 */
void CReplayDirector::Init() noexcept {
    m_Mode             = EReplayMode::Idle;
    m_Metadata         = FReplayMetadata{};   // 全 field を default に戻す
    m_CurrentTick     = 0;
    m_PlaybackSpeed   = 1.0f;
    m_TickAccumulator = 0.0f;
    m_TickRateHz     = 60;
    // m_Recorder / m_Lockstep (source 結線) と owned 文字列バッファは保持する。
    // Init は「録画 / 再生セッションの state リセット」であり、source の再注入を
    // 強制しない (director の寿命を通じて結線を維持したい)。
}

/** 非所有の recorder / lockstep ポインタを保持する (寿命は呼び出し側責務)。 */
void CReplayDirector::SetSources(CInputRecorder* recorder, CLockstep* lockstep) noexcept {
    // 非所有ポインタをそのまま保持する (寿命は呼び出し側責務)。nullptr 許容:
    // 片方だけ注入する / どちらも注入しない構成でも container は valid。
    m_Recorder = recorder;
    m_Lockstep = lockstep;
}

/** Idle から Recording へ遷移し metadata をコピーする (それ以外は kSub_BadMode)。 */
TResult<void> CReplayDirector::StartRecording(const FReplayMetadata& meta) noexcept {
    return TryStartRecording(meta);
}

/** metadataをbounded owned copyへstageしてから録画を開始する。 */
TResult<void> CReplayDirector::TryStartRecording(const FReplayMetadata& meta) noexcept {
    // Idle 以外からの直接遷移は禁止。Recording 中の再開や Playback 中の
    // 切り替えは意図しない上書きが起こりやすいため、明示的な Stop を強制する。
    if (m_Mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "CReplayDirector::TryStartRecording: must be Idle");
    }
    FMetadataLengths lengths{};
    if (!MeasureMetadata(meta, lengths)) {
        return ACS_ERR(IO, kSub_BadMetadata,
                       "CReplayDirector::TryStartRecording: metadata string is oversized or noncanonical");
    }

    FString game_version;
    FString level_id;
    FString player_name;
    FString checksum;
    if ((lengths.game_version > 0 &&
         !game_version.TryAppend(FStringView(meta.game_version, lengths.game_version))) ||
        (lengths.level_id > 0 &&
         !level_id.TryAppend(FStringView(meta.level_id, lengths.level_id))) ||
        (lengths.player_name > 0 &&
         !player_name.TryAppend(FStringView(meta.player_name, lengths.player_name))) ||
        (lengths.checksum > 0 &&
         !checksum.TryAppend(FStringView(meta.checksum_hex, lengths.checksum)))) {
        return ACS_ERR(Memory, kSub_Oom,
                       "CReplayDirector::TryStartRecording: metadata allocation failed");
    }

    m_GameVersionOwned = Move(game_version);
    m_LevelIdOwned = Move(level_id);
    m_PlayerNameOwned = Move(player_name);
    m_ChecksumHexOwned = Move(checksum);
    m_Mode             = EReplayMode::Recording;
    m_Metadata         = meta;
    m_Metadata.game_version = m_GameVersionOwned.Data();
    m_Metadata.level_id = m_LevelIdOwned.Data();
    m_Metadata.player_name = m_PlayerNameOwned.Data();
    m_Metadata.checksum_hex = m_ChecksumHexOwned.Data();
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    // m_PlaybackSpeed は触らない (録画中は無意味だが、StartPlayback 後にも
    // 直前の倍速を引き継ぎたい UX を許容する)。
    return TResult<void>{};
}

/** Recording から Idle へ遷移し duration_ticks を確定する (それ以外は kSub_BadMode)。 */
TResult<void> CReplayDirector::StopRecording() noexcept {
    // Recording 以外からの呼び出しは誤用扱い。Playback 中に StopRecording を
    // 呼んでしまった場合に黙って Idle にすると metadata が壊れるため明示エラー。
    if (m_Mode != EReplayMode::Recording) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "CReplayDirector::StopRecording: must be Recording");
    }
    // 録画した tick 数を確定。LoadReplay 後の再生で ProgressNormalized が
    // 正しく計算できるよう metadata に焼き込む。
    m_Metadata.duration_ticks = m_CurrentTick;
    m_Mode                    = EReplayMode::Idle;
    m_TickAccumulator        = 0.0f;
    return TResult<void>{};
}

/** Idle から Playback へ遷移し tick を 0 にリセットする (それ以外は kSub_BadMode)。 */
TResult<void> CReplayDirector::StartPlayback() noexcept {
    if (m_Mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "CReplayDirector::StartPlayback: must be Idle");
    }
    m_Mode             = EReplayMode::Playback;
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    // m_Metadata はそのまま (StartRecording 直後の再生、または LoadReplay 後の
    // 再生いずれも metadata が既に正しく設定されている前提)。
    return TResult<void>{};
}

/** Playback から Paused へ遷移する (それ以外は no-op)。 */
void CReplayDirector::PausePlayback() noexcept {
    // Playback 以外では no-op (Paused 中の再 Pause / Idle 中の誤呼び出しを許容)。
    if (m_Mode == EReplayMode::Playback) {
        m_Mode = EReplayMode::Paused;
    }
}

/** Paused から Playback へ遷移する (それ以外は no-op)。 */
void CReplayDirector::ResumePlayback() noexcept {
    // Paused 以外では no-op。
    if (m_Mode == EReplayMode::Paused) {
        m_Mode = EReplayMode::Playback;
    }
}

/** Playback / Paused から Idle へ遷移する (それ以外は冪等に no-op)。 */
void CReplayDirector::StopPlayback() noexcept {
    // Playback / Paused → Idle。Recording / Idle 中の呼び出しは黙って no-op
    // (Stop は冪等であってほしい UX)。
    if (m_Mode == EReplayMode::Playback || m_Mode == EReplayMode::Paused) {
        m_Mode             = EReplayMode::Idle;
        m_TickAccumulator = 0.0f;
    }
}

/** 再生倍速を clamp して設定する (accumulator は保持してジャンプを防ぐ)。 */
void CReplayDirector::SetPlaybackSpeed(f32 speed) noexcept {
    m_PlaybackSpeed = ClampSpeed(speed);
    // accumulator はそのまま。速度変更時に sub-tick の dt を捨てると
    // 再生が微妙にジャンプするのを防ぐ。
}

/** tick を duration_ticks 上限で clamp して current_tick にジャンプする (mode は不変)。 */
void CReplayDirector::SeekToTick(u32 tick) noexcept {
    // duration_ticks を上限に clamp。LoadReplay 前 (duration = 0) の seek は
    // 0 に張り付くが、これは UI のスクラブバーで「録画されていない」状態を
    // 明示するため意図通り。
    const u32 limit = m_Metadata.duration_ticks;
    m_CurrentTick     = (tick > limit) ? limit : tick;
    m_TickAccumulator = 0.0f;  // sub-tick を持ち越さない
}

/** current_tick / duration_ticks を [0, 1] で返す (duration 0 なら 0.0)。 */
f32 CReplayDirector::ProgressNormalized() const noexcept {
    const u32 d = m_Metadata.duration_ticks;
    if (d == 0) {
        return 0.0f;  // 録画開始直後 / metadata 未設定の安全側
    }
    const f32 p = static_cast<f32>(m_CurrentTick) / static_cast<f32>(d);
    // 上限 1.0 で clamp (Tick で duration を超えた瞬間に 1.0+ になる可能性が
    // あるため)。下限は m_CurrentTick が unsigned なので不要。
    return (p > 1.0f) ? 1.0f : p;
}

/** 現在 mode に応じて tick を進め、再生終了時は自動的に Idle へ落とす。 */
void CReplayDirector::Tick(f32 dt) noexcept {
    // 異常な dt (NaN / 負) はゲームループの早期 frame skip / pause からの復帰時に
    // 紛れ込みやすい。0 でガードして無視する。
    if (!(dt > 0.0f && dt <= 60.0f)) {
        return;
    }

    if (m_Mode == EReplayMode::Recording) {
        // 録画中: tick_rate_hz * dt 分だけ m_CurrentTick を進める。
        // 実入力 capture は CInputRecorder / CLockstep 側で別途行う前提なので、
        // ここは単純な tick カウンタの前進のみ。
        m_TickAccumulator += dt * static_cast<f32>(m_TickRateHz);
        if (m_TickAccumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(m_TickAccumulator);
            m_TickAccumulator -= static_cast<f32>(steps);
            if (steps > ~u32{0} - m_CurrentTick) {
                m_CurrentTick = ~u32{0};
                m_TickAccumulator = 0.0f;
            } else {
                m_CurrentTick += steps;
            }
        }
        return;
    }

    if (m_Mode == EReplayMode::Playback) {
        // 再生中: dt * playback_speed * tick_rate_hz 分だけ m_CurrentTick を進める。
        m_TickAccumulator += dt * m_PlaybackSpeed * static_cast<f32>(m_TickRateHz);
        if (m_TickAccumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(m_TickAccumulator);
            m_TickAccumulator -= static_cast<f32>(steps);
            const u32 duration = m_Metadata.duration_ticks;
            if (duration != 0 && m_CurrentTick < duration && steps >= duration - m_CurrentTick) {
                m_CurrentTick = duration;
            } else if (steps > ~u32{0} - m_CurrentTick) {
                m_CurrentTick = ~u32{0};
            } else {
                m_CurrentTick += steps;
            }
        }
        // duration_ticks に達したら自動的に Idle へ落とす (replay 終了)。
        // 0 のときは metadata 未設定 (load 前) と解釈し、自動停止しない。
        const u32 d = m_Metadata.duration_ticks;
        if (d != 0 && m_CurrentTick >= d) {
            m_CurrentTick     = d;
            m_Mode             = EReplayMode::Idle;
            m_TickAccumulator = 0.0f;
        }
        return;
    }

    // Paused / Idle: no-op (Tick が呼ばれても何もしない)。
}

/** metadata + 2 blob を container body に組み立て、`.tmp` 経由で atomic write する。 */
TResult<void> CReplayDirector::SaveReplay(const wchar_t* file_path) noexcept {
    return TrySaveReplay(file_path);
}

/** 上限付きcontainerを一意tempへ書き、flush/close後にatomic replaceする。 */
TResult<void> CReplayDirector::TrySaveReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "CReplayDirector::TrySaveReplay: file_path is null");
    }
    if (!IsValidReplayPath(file_path)) {
        return ACS_ERR(IO, kSub_PathTooLong,
                       "CReplayDirector::TrySaveReplay: path is empty, unterminated, or too long");
    }
    FMetadataLengths metadata_lengths{};
    if (!MeasureMetadata(m_Metadata, metadata_lengths)) {
        return ACS_ERR(IO, kSub_BadMetadata,
                       "CReplayDirector::TrySaveReplay: metadata is oversized or noncanonical");
    }

    TArray<u8> input_blob;
    if (m_Recorder != nullptr) {
        const u32 record_count = m_Recorder->SampleCount();
        if (record_count > kReplayMaximumSourceRecords) {
            return ACS_ERR(IO, kSub_LimitExceeded,
                           "CReplayDirector::TrySaveReplay: recorder sample count exceeds the limit");
        }
        const u64 required = kInputHeaderBytes +
                             static_cast<u64>(record_count) * kInputRecordBytes +
                             kSourceFooterBytes;
        if (required > kReplayMaximumSourceBlobBytes) {
            return ACS_ERR(IO, kSub_LimitExceeded,
                           "CReplayDirector::TrySaveReplay: recorder blob exceeds the limit");
        }
        if (!input_blob.TryResize(static_cast<usize>(required))) {
            return ACS_ERR(Memory, kSub_Oom,
                           "CReplayDirector::TrySaveReplay: recorder blob allocation failed");
        }
        u32 written = 0;
        TResult<void> source_result =
            m_Recorder->SaveToBuffer(input_blob.Data(), static_cast<u32>(required), written);
        if (source_result.IsErr() || written != required ||
            !ValidateSourceBlob(input_blob.Data(), written, true)) {
            return ACS_ERR(IO, kSub_BadSourceBlob,
                           "CReplayDirector::TrySaveReplay: recorder produced an invalid blob");
        }
    }

    TArray<u8> lockstep_blob;
    if (m_Lockstep != nullptr) {
        const u32 record_count = m_Lockstep->InputCount();
        if (record_count > kReplayMaximumSourceRecords) {
            return ACS_ERR(IO, kSub_LimitExceeded,
                           "CReplayDirector::TrySaveReplay: lockstep frame count exceeds the limit");
        }
        const u64 required = kLockstepHeaderBytes +
                             static_cast<u64>(record_count) * kLockstepRecordBytes +
                             kSourceFooterBytes;
        if (required > kReplayMaximumSourceBlobBytes) {
            return ACS_ERR(IO, kSub_LimitExceeded,
                           "CReplayDirector::TrySaveReplay: lockstep blob exceeds the limit");
        }
        if (!lockstep_blob.TryResize(static_cast<usize>(required))) {
            return ACS_ERR(Memory, kSub_Oom,
                           "CReplayDirector::TrySaveReplay: lockstep blob allocation failed");
        }
        u32 written = 0;
        TResult<void> source_result =
            m_Lockstep->SaveToBuffer(lockstep_blob.Data(), static_cast<u32>(required), written);
        if (source_result.IsErr() || written != required ||
            !ValidateSourceBlob(lockstep_blob.Data(), written, false)) {
            return ACS_ERR(IO, kSub_BadSourceBlob,
                           "CReplayDirector::TrySaveReplay: lockstep produced an invalid blob");
        }
    }

    const u64 total_size = kMinimumContainerBytes +
                           metadata_lengths.game_version +
                           metadata_lengths.level_id +
                           metadata_lengths.player_name +
                           metadata_lengths.checksum +
                           input_blob.Size() +
                           lockstep_blob.Size();
    if (total_size > kReplayMaximumContainerBytes || total_size > ~u32{0}) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "CReplayDirector::TrySaveReplay: container exceeds the product limit");
    }

    TArray<u8> body;
    if (!body.TryResize(static_cast<usize>(total_size))) {
        return ACS_ERR(Memory, kSub_Oom,
                       "CReplayDirector::TrySaveReplay: container allocation failed");
    }
    u64 offset = 0;
    MemCopy(body.Data() + offset, kReplayMagic, sizeof(kReplayMagic)); offset += sizeof(kReplayMagic);
    WriteU32LE(body.Data() + offset, kReplayVersion); offset += sizeof(u32);
    WriteU64LE(body.Data() + offset, m_Metadata.seed); offset += sizeof(u64);
    WriteU64LE(body.Data() + offset, m_Metadata.timestamp); offset += sizeof(u64);
    WriteU32LE(body.Data() + offset, m_Metadata.duration_ticks); offset += sizeof(u32);

    const auto write_string = [&](const char* text, u32 length) noexcept {
        WriteU32LE(body.Data() + offset, length);
        offset += sizeof(u32);
        if (length > 0) {
            MemCopy(body.Data() + offset, text, length);
            offset += length;
        }
    };
    write_string(m_Metadata.game_version, metadata_lengths.game_version);
    write_string(m_Metadata.level_id, metadata_lengths.level_id);
    write_string(m_Metadata.player_name, metadata_lengths.player_name);
    write_string(m_Metadata.checksum_hex, metadata_lengths.checksum);
    WriteU32LE(body.Data() + offset, static_cast<u32>(input_blob.Size())); offset += sizeof(u32);
    if (!input_blob.IsEmpty()) {
        MemCopy(body.Data() + offset, input_blob.Data(), input_blob.Size());
        offset += input_blob.Size();
    }
    WriteU32LE(body.Data() + offset, static_cast<u32>(lockstep_blob.Size())); offset += sizeof(u32);
    if (!lockstep_blob.IsEmpty()) {
        MemCopy(body.Data() + offset, lockstep_blob.Data(), lockstep_blob.Size());
        offset += lockstep_blob.Size();
    }
    const u32 crc = ComputeCrc32(body.Data(), offset);
    WriteU32LE(body.Data() + offset, crc);
    offset += sizeof(u32);
    if (offset != total_size) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TrySaveReplay: internal container size mismatch");
    }

    wchar_t tmp_path[kReplayTempPathCapacity] = {};
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD create_error = ERROR_FILE_EXISTS;
    for (u32 attempt = 0; attempt < 16u; ++attempt) {
        if (!MakeAtomicTempPath(file_path, tmp_path, kReplayTempPathCapacity)) {
            return ACS_ERR(IO, kSub_PathTooLong,
                           "CReplayDirector::TrySaveReplay: file path too long for atomic suffix");
        }
        h = ::CreateFileW(tmp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        create_error = ::GetLastError();
        if (create_error != ERROR_FILE_EXISTS && create_error != ERROR_ALREADY_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TrySaveReplay: CreateFileW (unique temp) failed", create_error);
    }

    DWORD err = 0;
    if (!WriteAll(h, body.Data(), total_size, err)) {
        ::CloseHandle(h);
        ::DeleteFileW(tmp_path);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TrySaveReplay: WriteFile (temp) failed", err);
    }
    if (!::FlushFileBuffers(h)) {
        const DWORD flush_error = ::GetLastError();
        ::CloseHandle(h);
        ::DeleteFileW(tmp_path);
        return ACS_ERR_OS(IO, kSub_FlushFailed,
                          "CReplayDirector::TrySaveReplay: FlushFileBuffers (temp) failed", flush_error);
    }
    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        ::DeleteFileW(tmp_path);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TrySaveReplay: CloseHandle (temp) failed", close_err);
    }

    if (!::MoveFileExW(tmp_path, file_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD move_error = ::GetLastError();
        if (!TryPosixAtomicReplace(tmp_path, file_path, move_error)) {
            ::DeleteFileW(tmp_path);
            return ACS_ERR_OS(IO, kSub_AtomicReplaceFailed,
                              "CReplayDirector::TrySaveReplay: atomic replace failed", move_error);
        }
    }
    return Ok();
}

/** container を全読みして magic/version/CRC を検証し、metadata と source blob を復元する。 */
TResult<void> CReplayDirector::LoadReplay(const wchar_t* file_path) noexcept {
    return TryLoadReplay(file_path);
}

/** file snapshotを全検証・stageし、成功時だけdirector stateへcommitする。 */
TResult<void> CReplayDirector::TryLoadReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "CReplayDirector::TryLoadReplay: file_path is null");
    }
    if (!IsValidReplayPath(file_path)) {
        return ACS_ERR(IO, kSub_PathTooLong,
                       "CReplayDirector::TryLoadReplay: path is empty, unterminated, or too long");
    }

    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: CreateFileW failed", err);
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(h, &size_li)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: GetFileSizeEx failed", err);
    }
    if (size_li.QuadPart < 0) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: negative file size");
    }
    const u64 size_u64 = static_cast<u64>(size_li.QuadPart);
    if (size_u64 < kMinimumContainerBytes) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: file smaller than minimal container");
    }
    if (size_u64 > kReplayMaximumContainerBytes) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "CReplayDirector::TryLoadReplay: container exceeds the product limit");
    }

    FProcessHeapBuffer file_storage(size_u64);
    if (file_storage.Data() == nullptr) {
        ::CloseHandle(h);
        return ACS_ERR(Memory, kSub_Oom,
                       "CReplayDirector::TryLoadReplay: failed to allocate file snapshot");
    }
    u8* buf = static_cast<u8*>(file_storage.Data());

    DWORD err = 0;
    if (!ReadAll(h, buf, size_u64, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: ReadFile failed", err);
    }
    LARGE_INTEGER final_size{};
    if (!::GetFileSizeEx(h, &final_size)) {
        const DWORD final_size_error = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: final GetFileSizeEx failed", final_size_error);
    }
    u8 extra_byte = 0;
    DWORD extra_count = 0;
    const BOOL eof_probe = ::ReadFile(h, &extra_byte, 1u, &extra_count, nullptr);
    if (!eof_probe) {
        const DWORD probe_error = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: EOF probe failed", probe_error);
    }
    if (final_size.QuadPart != size_li.QuadPart || extra_count != 0) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: file size changed during snapshot read");
    }
    if (!::CloseHandle(h)) {
        return ACS_ERR_OS(IO, kSub_Io,
                          "CReplayDirector::TryLoadReplay: CloseHandle failed", ::GetLastError());
    }

    if (MemCmp(buf, kReplayMagic, sizeof(kReplayMagic)) != 0) {
        return ACS_ERR(IO, kSub_BadMagic,
                       "CReplayDirector::TryLoadReplay: magic mismatch");
    }
    const u32 version = ReadU32LE(buf + 4);
    if (version != kReplayVersion) {
        return ACS_ERR(IO, kSub_BadVersion,
                       "CReplayDirector::TryLoadReplay: unsupported container version");
    }

    const u64 body_size = size_u64 - kCrcFooterSize;
    const u32 actual_crc = ComputeCrc32(buf, body_size);
    const u32 stored_crc = ReadU32LE(buf + body_size);
    if (actual_crc != stored_crc) {
        return ACS_ERR(IO, kSub_BadCrc,
                       "CReplayDirector::TryLoadReplay: CRC32 mismatch");
    }

    u64 off = sizeof(kReplayMagic) + sizeof(u32);
    const u64 limit = body_size;
    const auto need = [&](u64 count) noexcept -> bool {
        return off <= limit && count <= limit - off;
    };
    if (!need(8 + 8 + 4)) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: truncated numeric metadata");
    }
    const u64 seed           = ReadU64LE(buf + off); off += 8;
    const u64 timestamp      = ReadU64LE(buf + off); off += 8;
    const u32 duration_ticks = ReadU32LE(buf + off); off += 4;

    FString game_version;
    FString level_id;
    FString player_name;
    FString checksum;
    const auto read_string = [&](FString& owned, u32 maximum, bool checksum_field) noexcept -> u16 {
        if (!need(sizeof(u32))) return kSub_BadSize;
        const u32 len = ReadU32LE(buf + off);
        off += sizeof(u32);
        if (len > maximum) return kSub_BadMetadata;
        if (!need(len)) return kSub_BadSize;
        for (u32 i = 0; i < len; ++i) {
            if (buf[off + i] == 0) return kSub_BadMetadata;
        }
        if (checksum_field &&
            !IsCanonicalChecksum(reinterpret_cast<const char*>(buf + off), len)) {
            return kSub_BadMetadata;
        }
        if (len > 0) {
            if (!owned.TryAppend(FStringView(reinterpret_cast<const char*>(buf + off), len))) return kSub_Oom;
        }
        off += len;
        return 0;
    };

    u16 string_error = read_string(game_version, kReplayMaximumGameVersionBytes, false);
    if (string_error == 0) string_error = read_string(level_id, kReplayMaximumLevelIdBytes, false);
    if (string_error == 0) string_error = read_string(player_name, kReplayMaximumPlayerNameBytes, false);
    if (string_error == 0) string_error = read_string(checksum, kReplayChecksumHexBytes, true);
    if (string_error != 0) {
        if (string_error == kSub_Oom) {
            return ACS_ERR(Memory, kSub_Oom,
                           "CReplayDirector::TryLoadReplay: metadata string allocation failed");
        }
        return ACS_ERR(IO, string_error,
                       "CReplayDirector::TryLoadReplay: invalid metadata string");
    }

    if (!need(sizeof(u32))) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: truncated recorder blob size");
    }
    const u32 input_blob_size = ReadU32LE(buf + off);
    off += sizeof(u32);
    if (input_blob_size > kReplayMaximumSourceBlobBytes) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "CReplayDirector::TryLoadReplay: recorder blob exceeds the limit");
    }
    if (!need(input_blob_size)) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: recorder blob exceeds container");
    }
    const u8* input_blob_ptr = buf + off;
    off += input_blob_size;

    if (!need(sizeof(u32))) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: truncated lockstep blob size");
    }
    const u32 lockstep_blob_size = ReadU32LE(buf + off);
    off += sizeof(u32);
    if (lockstep_blob_size > kReplayMaximumSourceBlobBytes) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "CReplayDirector::TryLoadReplay: lockstep blob exceeds the limit");
    }
    if (!need(lockstep_blob_size)) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: lockstep blob exceeds container");
    }
    const u8* lockstep_blob_ptr = buf + off;
    off += lockstep_blob_size;
    if (off != limit) {
        return ACS_ERR(IO, kSub_BadSize,
                       "CReplayDirector::TryLoadReplay: trailing bytes before CRC footer");
    }
    if (!ValidateSourceBlob(input_blob_ptr, input_blob_size, true) ||
        !ValidateSourceBlob(lockstep_blob_ptr, lockstep_blob_size, false)) {
        return ACS_ERR(IO, kSub_BadSourceBlob,
                       "CReplayDirector::TryLoadReplay: inner source blob is invalid");
    }

    const bool commit_recorder = m_Recorder != nullptr && input_blob_size > 0;
    const bool commit_lockstep = m_Lockstep != nullptr && lockstep_blob_size > 0;
    IAllocator& recorder_allocator =
        m_Recorder != nullptr ? *m_Recorder->m_Samples.GetAllocator() : DefaultAllocator();
    IAllocator& lockstep_allocator =
        m_Lockstep != nullptr ? *m_Lockstep->m_Frames.GetAllocator() : DefaultAllocator();
    CInputRecorder staged_recorder(recorder_allocator);
    CLockstep staged_lockstep(lockstep_allocator);
    if (commit_recorder) {
        TResult<void> source_result = staged_recorder.TryLoadFromBuffer(input_blob_ptr, input_blob_size);
        if (source_result.IsErr()) {
            if (source_result.Error().subcode == CInputRecorder::kSub_Oom) {
                return ACS_ERR(Memory, kSub_Oom,
                               "CReplayDirector::TryLoadReplay: recorder staging allocation failed");
            }
            return ACS_ERR(IO, kSub_BadSourceBlob,
                           "CReplayDirector::TryLoadReplay: recorder rejected a prevalidated blob");
        }
    }
    if (commit_lockstep) {
        TResult<void> source_result = staged_lockstep.TryLoadFromBuffer(lockstep_blob_ptr, lockstep_blob_size);
        if (source_result.IsErr()) {
            if (source_result.Error().subcode == CLockstep::kSub_Oom) {
                return ACS_ERR(Memory, kSub_Oom,
                               "CReplayDirector::TryLoadReplay: lockstep staging allocation failed");
            }
            return ACS_ERR(IO, kSub_BadSourceBlob,
                           "CReplayDirector::TryLoadReplay: lockstep rejected a prevalidated blob");
        }
    }

    if (commit_recorder) m_Recorder->SwapLoadedState(staged_recorder);
    if (commit_lockstep) m_Lockstep->SwapLoadedState(staged_lockstep);

    m_GameVersionOwned = Move(game_version);
    m_LevelIdOwned = Move(level_id);
    m_PlayerNameOwned = Move(player_name);
    m_ChecksumHexOwned = Move(checksum);
    m_Metadata.seed           = seed;
    m_Metadata.timestamp      = timestamp;
    m_Metadata.duration_ticks = duration_ticks;
    m_Metadata.game_version   = m_GameVersionOwned.Data();
    m_Metadata.level_id       = m_LevelIdOwned.Data();
    m_Metadata.player_name    = m_PlayerNameOwned.Data();
    m_Metadata.checksum_hex   = m_ChecksumHexOwned.Data();
    m_Mode             = EReplayMode::Idle;
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    return Ok();
}

} // namespace acs::game
