// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — FNetSnapshot 実装
//
// 構成:
//   1) NetTransportStub: 常に NotImplemented を返す null-object transport。
//   2) FNetSnapshot:
//      ・Init / Shutdown: ring buffer / pending list / 統計値の初期化と解放。
//      ・AddEntitySnapshot / CommitSnapshot: server 側で 1 frame 分の state を
//        積み、1 つの payload に concat して transport.Send。
//      ・Tick: client 側で transport.Receive を pump し、受信 snapshot を ring
//        buffer に追加 (FIFO で最古を上書き)。
//      ・TryGetInterpolatedSnapshot: 2 snapshot を取り出して entity 単位で
//        線形補間 (現時点では同 entity_id 同 component_mask の data を 2 つ
//        並べる純粋な lerp は行わず、buffer 内の "より新しい側" の data を
//        view として返す)。補間は "時刻に対応する snapshot を選ぶ" 段階まで
//        実装し、float コンポーネント単位の値補間はコンポーネントスキーマが
//        入ってから本実装する。
//
// 設計メモ:
//   ・wire format (frame): [magic 'ACSN'][version][SnapshotHeader(24B)]
//     [payload][crc32]。payload の各 entity は
//     [entity_id u32][component_mask u32][data_size u32][data]。
//   ・直列化の実体は EncodeSnapshot / DecodeSnapshot (static 純粋関数)。
//     CommitSnapshot は EncodeSnapshot の結果を transport.Send し、Tick は
//     transport.Receive した bytes を DecodeSnapshot で検証してから ring
//     buffer に積む。transport (socket) seam は INetTransport のまま外部差替で、
//     encode/decode は seam に依存しない real な byte serialization になっている。
//   ・LE 固定。MemCopy 経由で strict-aliasing 安全。
//   ・CRC32 は Zlib / PNG 規約 (poly 0xEDB88320, init/xorout 0xFFFFFFFF)。
//     FSaveArchive.cpp / FLockstep.cpp と同一の file-local ComputeCrc32 を持つ
//     (gameframework は assetpack を依存に持たないため link 単位を独立させる)。
//   ・全 noexcept。エラーは内部で握り潰す (transport.Send 失敗は packet
//     loss と等価扱い)。INetTransport API は TResult<T> だが、上位の
//     CommitSnapshot/Tick は void で「ベストエフォート」配信を表現する。
//   ・コピー / ムーブ禁止 (header で delete 済み)。

#include "gameframework/NetSnapshot.h"

#include "gameframework/NetSnapshotDiagnosticsInternal.h"

#include "foundation/Move.h" // Move (rvalue cast)
#include "memory/Memory.h"   // MemCopy / MemSet
#include "threading/ScopedLock.h"

// ---- 実 Winsock2 UDP transport (FUdpTransport) 用 ---------------------------
// <winsock2.h> は <windows.h> より先に include しないと winsock(1) と衝突する
// ため、明示的に先頭で取り込む。header (NetSnapshot.h) には漏らさない (uptr /
// raw octets で socket / endpoint を保持しているため)。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>

namespace acs::game {

namespace {

/**
 * u32 を little-endian で書き出す (strict-aliasing 安全)。
 *
 * @param dst 書き込み先。
 * @param v 書き出す値。
 */
inline void WriteU32LE(u8* dst, u32 v) noexcept
{
    MemCopy(dst, &v, sizeof(u32));
}

/**
 * u64 を little-endian で書き出す (strict-aliasing 安全)。
 *
 * @param dst 書き込み先。
 * @param v 書き出す値。
 */
inline void WriteU64LE(u8* dst, u64 v) noexcept
{
    MemCopy(dst, &v, sizeof(u64));
}

/**
 * u32 を little-endian で読み出す (strict-aliasing 安全)。
 *
 * @param src 読み込み元。
 * @return 復元した u32。
 */
inline u32 ReadU32LE(const u8* src) noexcept
{
    u32 v = 0;
    MemCopy(&v, src, sizeof(u32));
    return v;
}

/**
 * u64 を little-endian で読み出す (strict-aliasing 安全)。
 *
 * @param src 読み込み元。
 * @return 復元した u64。
 */
inline u64 ReadU64LE(const u8* src) noexcept
{
    u64 v = 0;
    MemCopy(&v, src, sizeof(u64));
    return v;
}

/** frame magic 'ACSN' (Net Snapshot。異なる wire stream の混入を弾く)。 */
constexpr u8 kFrameMagic[4] = {'A', 'C', 'S', 'N'};

/** frame の wire format バージョン。 */
constexpr u32 kFrameVersion = 1u;

/** magic フィールドのバイト数。 */
constexpr u32 kMagicSize = 4u;

/** version フィールドのバイト数。 */
constexpr u32 kVersionSize = 4u;

/** SnapshotHeader の wire 上のバイト数。 */
constexpr u32 kHeaderWireSize = 24;

/** version フィールドの frame 内オフセット。 */
constexpr u32 kVersionOffset = kMagicSize;

/** SnapshotHeader の frame 内オフセット。 */
constexpr u32 kHeaderOffset = kMagicSize + kVersionSize;

/** payload の frame 内オフセット。 */
constexpr u32 kPayloadOffset = kHeaderOffset + kHeaderWireSize;

/** crc32 footer のバイト数。 */
constexpr u32 kCrcFooterSize = 4u;

/** payload を除いた frame の固定オーバーヘッド (magic+version+header+crc32 = 36)。 */
constexpr u32 kFrameFixedOverhead = kMagicSize + kVersionSize + kHeaderWireSize + kCrcFooterSize;

/**
 * CRC32 ルックアップテーブルを返す (poly 0xEDB88320、Meyer's singleton で初期化)。
 *
 * @details FSaveArchive / FLockstep と同一実装 (gameframework は link 単位を独立させる)。
 * @return 256 要素の CRC32 テーブル先頭。
 */
const u32* GetCrc32Table() noexcept
{
    static u32 m_Table[256] = {};
    static bool m_Initialized = false;
    if (!m_Initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            m_Table[i] = c;
        }
        m_Initialized = true;
    }
    return m_Table;
}

/**
 * バイト列の CRC32 を計算する (Zlib / PNG 規約、init/xorout 0xFFFFFFFF)。
 *
 * @param data 対象バイト列。
 * @param size data のバイト数。
 * @return 計算した CRC32。
 */
u32 ComputeCrc32(const void* data, u64 size) noexcept
{
    const u32* table = GetCrc32Table();
    const u8* p = static_cast<const u8*>(data);
    u32 crc = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * SnapshotHeader を 24 byte の LE wire format で書き出す。
 *
 * @param dst 書き込み先 (24 byte 確保済み)。
 * @param h 書き出す header (crc32 フィールドも含めてそのまま書く)。
 */
void WriteHeader(u8* dst, const SnapshotHeader& h) noexcept
{
    WriteU32LE(dst + 0, h.tick);
    WriteU32LE(dst + 4, h.sequence);
    WriteU64LE(dst + 8, h.server_timestamp_us);
    WriteU32LE(dst + 16, h.payload_size);
    WriteU32LE(dst + 20, h.crc32);
}

/**
 * 24 byte の LE wire format から SnapshotHeader を読み出す。
 *
 * @param src 読み込み元 (24 byte)。
 * @param h 復元した header を書き込む先。
 */
void ReadHeader(const u8* src, SnapshotHeader& h) noexcept
{
    h.tick = ReadU32LE(src + 0);
    h.sequence = ReadU32LE(src + 4);
    h.server_timestamp_us = ReadU64LE(src + 8);
    h.payload_size = ReadU32LE(src + 16);
    h.crc32 = ReadU32LE(src + 20);
}

/**
 * WSAStartup のプロセス内参照数と lifecycle lock (FUdpTransport 用)。
 *
 * @details
 * 複数の FUdpTransport / 他モジュールの Winsock 利用と共存できるよう「初回のみ
 * WSAStartup、最後の解放で WSACleanup」とする。gameframework は Network を依存に
 * 持たないため file-local に独立した参照数を持つ (OS 側でも ref-count されるため
 * 共存しても安全)。
 */
SRWLOCK g_winsock_lifecycle_lock = SRWLOCK_INIT;
u32 g_winsock_reference_count = 0;
u32 g_winsock_cleanup_error = 0;
bool g_winsock_cleanup_debt_orphaned = false;

/** 破棄済み transport から所有権を引き継ぐ固定長 socket 回収表。 */
struct OrphanedSocketRecord {
    SOCKET socket = INVALID_SOCKET;
    bool owns_winsock_reference = false;
};

constexpr u32 kMaximumPrimaryOrphanedSocketCount = 256;
OrphanedSocketRecord g_primary_orphaned_sockets[kMaximumPrimaryOrphanedSocketCount] = {};
u32 g_primary_orphaned_socket_count = 0;

/** 固定表の飽和時に VirtualAlloc で確保する回収 record。 */
struct OverflowOrphanedSocketRecord {
    OrphanedSocketRecord resource{};
    OverflowOrphanedSocketRecord* next = nullptr;
};

OverflowOrphanedSocketRecord* g_overflow_orphaned_socket_head = nullptr;
u32 g_overflow_orphaned_socket_count = 0;

/** テスト時だけ固定表を縮小し、overflow 所有移管を少数 socket で検証する。 */
u32 g_primary_orphaned_socket_capacity = kMaximumPrimaryOrphanedSocketCount;

/** lifecycle lock 保持下で共有回収処理が所有する socket 総数を返す。 */
u32 TotalOrphanedSocketCountLocked() noexcept
{
    return g_primary_orphaned_socket_count + g_overflow_orphaned_socket_count;
}

/** プロセス寿命中に観測した Winsock 資源解放失敗の累積数。 */
volatile LONG64 g_winsock_resource_failure_count = 0;

/** テストから次の Winsock 操作だけを失敗させる原子的な残り回数。 */
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
volatile LONG g_injected_close_socket_failure_count = 0;
volatile LONG g_injected_close_socket_error = WSAEWOULDBLOCK;
volatile LONG g_injected_cleanup_failure_count = 0;
volatile LONG g_injected_cleanup_error = WSAEINPROGRESS;
#endif

/** 1 回の資源解放または所有権検証失敗を、lock 解放後に出力するための値。 */
struct WinsockFailureReport {
    const char* record = nullptr;
    const char* leak_verdict = "inconclusive";
    u32 error = 0;
    u64 failure_count = 0;
};

/** 共有 lock 内で検出した失敗を、動的確保なしで一時保持する。 */
struct PendingWinsockFailureReports {
    static constexpr u32 kCapacity = kMaximumPrimaryOrphanedSocketCount + 4;

    WinsockFailureReport reports[kCapacity] = {};
    u32 report_count = 0;
    u32 suppressed_report_count = 0;

    void Add(const char* record, u32 error, const char* leak_verdict) noexcept
    {
        const u64 failure_count = static_cast<u64>(::_InterlockedIncrement64(&g_winsock_resource_failure_count));
        if (report_count >= kCapacity) {
            ++suppressed_report_count;
            return;
        }
        reports[report_count++] = {record, leak_verdict, error, failure_count};
    }
};

/** 機械可読の 1 失敗レコードを debugger と標準エラーへ出す。 */
void EmitWinsockResourceFailure(const WinsockFailureReport& report) noexcept
{
    char message[320] = {};
    const int length = ::snprintf(message, sizeof(message),
                                  "memory_diagnostic_method=win32_resource tracker=winsock "
                                  "record=%s os_error=%u failure_count=%lld "
                                  "resource_release_failed=true leak_detected=%s\n",
                                  report.record, report.error, static_cast<long long>(report.failure_count),
                                  report.leak_verdict);
    if (length <= 0) return;

    ::OutputDebugStringA(message);
    const HANDLE error_output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error_output != nullptr && error_output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const DWORD byte_count = static_cast<DWORD>(static_cast<usize>(length) < sizeof(message) ? length
                                                                                                 : sizeof(message) - 1);
        (void)::WriteFile(error_output, message, byte_count, &written, nullptr);
    }
}

/** 固定 report buffer を超えた失敗数を、lock 解放後に集約して出力する。 */
void EmitSuppressedWinsockResourceFailures(u32 suppressed_report_count) noexcept
{
    if (suppressed_report_count == 0) return;

    const LONG64 failure_count = ::_InterlockedCompareExchange64(&g_winsock_resource_failure_count, 0, 0);
    char message[320] = {};
    const int length = ::snprintf(message, sizeof(message),
                                  "memory_diagnostic_method=win32_resource tracker=winsock "
                                  "record=diagnostic_report_buffer_overflow failure_count=%lld "
                                  "suppressed_report_count=%u resource_release_failed=false "
                                  "leak_detected=inconclusive\n",
                                  static_cast<long long>(failure_count), suppressed_report_count);
    if (length <= 0) return;

    ::OutputDebugStringA(message);
    const HANDLE error_output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error_output != nullptr && error_output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const DWORD byte_count = static_cast<DWORD>(static_cast<usize>(length) < sizeof(message) ? length
                                                                                                 : sizeof(message) - 1);
        (void)::WriteFile(error_output, message, byte_count, &written, nullptr);
    }
}

/** lock 内で蓄積した失敗をすべて lock 外から出力する。 */
void EmitWinsockResourceFailures(const PendingWinsockFailureReports& reports) noexcept
{
    for (u32 index = 0; index < reports.report_count; ++index) {
        EmitWinsockResourceFailure(reports.reports[index]);
    }
    EmitSuppressedWinsockResourceFailures(reports.suppressed_report_count);
}

/** lifecycle lock を保持していない経路から 1 失敗を即時記録する。 */
void ReportWinsockResourceFailure(const char* record, u32 error, const char* leak_verdict) noexcept
{
    PendingWinsockFailureReports reports{};
    reports.Add(record, error, leak_verdict);
    EmitWinsockResourceFailures(reports);
}

/** 共有回収状態の遷移を、失敗回数を増やさず機械可読形式で出す。 */
void EmitWinsockResourceState(const char* record, u32 error, const char* leak_verdict, bool cleanup_pending,
                              u32 orphaned_socket_count, u32 overflow_orphaned_socket_count,
                              bool cleanup_debt_orphaned) noexcept
{
    const LONG64 failure_count = ::_InterlockedCompareExchange64(&g_winsock_resource_failure_count, 0, 0);
    char message[384] = {};
    const int length = ::snprintf(message, sizeof(message),
                                  "memory_diagnostic_method=win32_resource tracker=winsock "
                                  "record=%s os_error=%u failure_count=%lld resource_release_failed=false "
                                  "cleanup_pending=%s orphaned_socket_count=%u overflow_orphaned_socket_count=%u "
                                  "cleanup_debt_orphaned=%s "
                                  "leak_detected=%s\n",
                                  record, error, static_cast<long long>(failure_count),
                                  cleanup_pending ? "true" : "false", orphaned_socket_count,
                                  overflow_orphaned_socket_count, cleanup_debt_orphaned ? "true" : "false",
                                  leak_verdict);
    if (length <= 0) return;

    ::OutputDebugStringA(message);
    const HANDLE error_output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error_output != nullptr && error_output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const DWORD byte_count = static_cast<DWORD>(static_cast<usize>(length) < sizeof(message) ? length
                                                                                                 : sizeof(message) - 1);
        (void)::WriteFile(error_output, message, byte_count, &written, nullptr);
    }
}

/** 原子的な残り回数を 1 減らし、注入すべきエラーを返す。0 は注入なし。 */
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
u32 ConsumeInjectedFailure(volatile LONG* remaining_count, volatile LONG* error) noexcept
{
    LONG remaining = ::_InterlockedCompareExchange(remaining_count, 0, 0);
    while (remaining > 0) {
        const LONG observed = ::_InterlockedCompareExchange(remaining_count, remaining - 1, remaining);
        if (observed == remaining) {
            return static_cast<u32>(::_InterlockedCompareExchange(error, 0, 0));
        }
        remaining = observed;
    }
    return 0;
}
#endif

/** 失敗注入を適用してから closesocket を呼ぶ。 */
int CloseSocketOperation(SOCKET socket) noexcept
{
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
    const u32 injected_error = ConsumeInjectedFailure(&g_injected_close_socket_failure_count,
                                                      &g_injected_close_socket_error);
    if (injected_error != 0) {
        ::WSASetLastError(static_cast<int>(injected_error));
        return SOCKET_ERROR;
    }
#endif
    return ::closesocket(socket);
}

/** 失敗注入を適用してから WSACleanup を呼ぶ。 */
int CleanupWinsockOperation() noexcept
{
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
    const u32 injected_error = ConsumeInjectedFailure(&g_injected_cleanup_failure_count, &g_injected_cleanup_error);
    if (injected_error != 0) {
        ::WSASetLastError(static_cast<int>(injected_error));
        return SOCKET_ERROR;
    }
#endif
    return ::WSACleanup();
}

/** WSA 参照解放の結果。error が非0でも released=true なら所有状態は修復済み。 */
struct WinsockReleaseResult {
    u32 error = 0;
    bool released = false;
};

/** 最終 cleanup 失敗時に、呼出元が参照所有を保持するか共有 debt へ移すか。 */
enum class WinsockReleaseOwnership : u8 {
    RetainOnFailure,
    TransferOnFailure,
};

/** lifecycle lock 保持下で WSAStartup の ref を 1 戻す。 */
WinsockReleaseResult WsaStartupReleaseReferenceLocked(WinsockReleaseOwnership ownership,
                                                      PendingWinsockFailureReports& reports) noexcept
{
    if (g_winsock_reference_count == 0) {
        reports.Add("reference_underflow", static_cast<u32>(WSANOTINITIALISED), "inconclusive");
        return {static_cast<u32>(WSANOTINITIALISED), true};
    }

    if (g_winsock_reference_count > 1) {
        --g_winsock_reference_count;
        return {0, true};
    }

    if (CleanupWinsockOperation() == 0) {
        g_winsock_reference_count = 0;
        g_winsock_cleanup_error = 0;
        g_winsock_cleanup_debt_orphaned = false;
        return {0, true};
    }

    const u32 cleanup_error = static_cast<u32>(::WSAGetLastError());
    if (cleanup_error == static_cast<u32>(WSANOTINITIALISED)) {
        // OS 側に参照が無いことが確定したため、再利用された資源へ触れないよう内部状態を修復する。
        g_winsock_reference_count = 0;
        g_winsock_cleanup_error = 0;
        g_winsock_cleanup_debt_orphaned = false;
        reports.Add("cleanup_reference_mismatch", cleanup_error, "inconclusive");
        return {cleanup_error, true};
    }

    g_winsock_cleanup_error = cleanup_error;
    g_winsock_cleanup_debt_orphaned = ownership == WinsockReleaseOwnership::TransferOnFailure;
    reports.Add("cleanup_failed", cleanup_error, "inconclusive");
    return {cleanup_error, ownership == WinsockReleaseOwnership::TransferOnFailure};
}

/** 固定回収表から index の socket を swap-remove する。 */
void RemovePrimaryOrphanedSocketLocked(u32 index) noexcept
{
    const u32 last_index = g_primary_orphaned_socket_count - 1;
    if (index != last_index) g_primary_orphaned_sockets[index] = g_primary_orphaned_sockets[last_index];
    g_primary_orphaned_sockets[last_index] = {};
    --g_primary_orphaned_socket_count;
}

/** 1 socket を閉じ、対応する WSA 参照も共有所有から解放する。 */
bool TryCloseOrphanedSocketLocked(const OrphanedSocketRecord& record, u32& first_unresolved_error,
                                  PendingWinsockFailureReports& reports) noexcept
{
    if (CloseSocketOperation(record.socket) == SOCKET_ERROR) {
        const u32 close_error = static_cast<u32>(::WSAGetLastError());
        reports.Add("orphaned_socket_close_failed", close_error, "inconclusive");
        if (close_error != static_cast<u32>(WSAENOTSOCK)) {
            if (first_unresolved_error == 0) first_unresolved_error = close_error;
            return false;
        }
    }

    if (record.owns_winsock_reference) {
        (void)WsaStartupReleaseReferenceLocked(WinsockReleaseOwnership::TransferOnFailure, reports);
    }
    return true;
}

/** lifecycle lock 保持下で、破棄済み transport の socket を可能な限り回収する。 */
u32 DrainOrphanedSocketsLocked(PendingWinsockFailureReports& reports) noexcept
{
    u32 first_unresolved_error = 0;
    u32 index = 0;
    while (index < g_primary_orphaned_socket_count) {
        const OrphanedSocketRecord record = g_primary_orphaned_sockets[index];
        if (!TryCloseOrphanedSocketLocked(record, first_unresolved_error, reports)) {
            ++index;
            continue;
        }
        RemovePrimaryOrphanedSocketLocked(index);
    }

    OverflowOrphanedSocketRecord** next_link = &g_overflow_orphaned_socket_head;
    while (*next_link != nullptr) {
        OverflowOrphanedSocketRecord* node = *next_link;
        if (!TryCloseOrphanedSocketLocked(node->resource, first_unresolved_error, reports)) {
            next_link = &node->next;
            continue;
        }

        *next_link = node->next;
        --g_overflow_orphaned_socket_count;
        if (::VirtualFree(node, 0, MEM_RELEASE) == FALSE) {
            const u32 release_error = static_cast<u32>(::GetLastError());
            reports.Add("orphaned_socket_metadata_release_failed", release_error, "true");
            if (first_unresolved_error == 0) first_unresolved_error = release_error;
        }
    }
    return first_unresolved_error;
}

/** lifecycle lock 保持下で、共有所有の WSACleanup debt だけを再試行する。 */
u32 RetryOrphanedWinsockCleanupLocked(PendingWinsockFailureReports& reports) noexcept
{
    if (g_winsock_cleanup_error == 0) return 0;
    if (!g_winsock_cleanup_debt_orphaned) return g_winsock_cleanup_error;

    if (g_winsock_reference_count != 1) {
        const u32 cleanup_error = g_winsock_cleanup_error;
        reports.Add("cleanup_retry_invalid_reference_count", cleanup_error, "inconclusive");
        return cleanup_error;
    }

    if (CleanupWinsockOperation() == 0) {
        g_winsock_reference_count = 0;
        g_winsock_cleanup_error = 0;
        g_winsock_cleanup_debt_orphaned = false;
        return 0;
    }

    const u32 cleanup_error = static_cast<u32>(::WSAGetLastError());
    if (cleanup_error == static_cast<u32>(WSANOTINITIALISED)) {
        g_winsock_reference_count = 0;
        g_winsock_cleanup_error = 0;
        g_winsock_cleanup_debt_orphaned = false;
        reports.Add("cleanup_retry_reference_mismatch", cleanup_error, "inconclusive");
        return 0;
    }

    g_winsock_cleanup_error = cleanup_error;
    reports.Add("cleanup_retry_failed", cleanup_error, "inconclusive");
    return cleanup_error;
}

/** 新しい WSA 参照を作れなかった段階。 */
enum class WinsockAcquireFailure : u8 {
    None,
    Startup,
    DeferredCleanup,
};

struct WinsockAcquireResult {
    u32 error = 0;
    WinsockAcquireFailure failure = WinsockAcquireFailure::None;
};

/** lifecycle lock 保持下で、既存 debt を回収して新しい WSA 参照を作る。 */
WinsockAcquireResult WsaStartupAddReferenceLocked(PendingWinsockFailureReports& reports) noexcept
{
    const u32 orphaned_socket_error = DrainOrphanedSocketsLocked(reports);
    if (TotalOrphanedSocketCountLocked() != 0) {
        return {orphaned_socket_error != 0 ? orphaned_socket_error : static_cast<u32>(WSAEWOULDBLOCK),
                WinsockAcquireFailure::DeferredCleanup};
    }
    if (orphaned_socket_error != 0) {
        return {orphaned_socket_error, WinsockAcquireFailure::DeferredCleanup};
    }

    if (g_winsock_cleanup_error != 0) {
        // 生存 instance が所有する debt は、別 instance が回収して参照世代を奪ってはならない。
        if (!g_winsock_cleanup_debt_orphaned) {
            return {g_winsock_cleanup_error, WinsockAcquireFailure::DeferredCleanup};
        }

        const u32 cleanup_error = RetryOrphanedWinsockCleanupLocked(reports);
        if (cleanup_error != 0) return {cleanup_error, WinsockAcquireFailure::DeferredCleanup};
    }

    if (g_winsock_reference_count == ~u32{0}) {
        return {static_cast<u32>(WSAEPROCLIM), WinsockAcquireFailure::Startup};
    }

    if (g_winsock_reference_count == 0) {
        WSADATA winsock_data{};
        const int startup_result = ::WSAStartup(MAKEWORD(2, 2), &winsock_data);
        if (startup_result != 0) {
            return {static_cast<u32>(startup_result), WinsockAcquireFailure::Startup};
        }
    }

    ++g_winsock_reference_count;
    return {};
}

/**
 * WSAStartup を ref-count して 1 回だけ実行する。
 *
 * @return 成功、WSAStartup 失敗、既存 cleanup 失敗を区別した結果。
 */
WinsockAcquireResult WsaStartupAddReference() noexcept
{
    PendingWinsockFailureReports reports{};
    bool had_deferred_resources = false;
    u32 remaining_socket_count = 0;
    u32 remaining_overflow_socket_count = 0;
    bool cleanup_debt_orphaned = false;
    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);
    had_deferred_resources = TotalOrphanedSocketCountLocked() != 0 || g_winsock_cleanup_debt_orphaned;
    const WinsockAcquireResult result = WsaStartupAddReferenceLocked(reports);
    remaining_socket_count = TotalOrphanedSocketCountLocked();
    remaining_overflow_socket_count = g_overflow_orphaned_socket_count;
    cleanup_debt_orphaned = g_winsock_cleanup_debt_orphaned;
    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);
    EmitWinsockResourceFailures(reports);

    if (had_deferred_resources) {
        const bool cleanup_pending = remaining_socket_count != 0 || cleanup_debt_orphaned;
        if (!cleanup_pending && result.failure != WinsockAcquireFailure::DeferredCleanup) {
            EmitWinsockResourceState("deferred_cleanup_resolved", 0, "false", false, 0, 0, false);
        } else {
            EmitWinsockResourceState(cleanup_pending ? "deferred_cleanup_still_pending" : "deferred_cleanup_untracked",
                                     result.error, cleanup_pending ? "inconclusive" : "true", cleanup_pending,
                                     remaining_socket_count, remaining_overflow_socket_count, cleanup_debt_orphaned);
        }
    }
    return result;
}

/** WSAStartup の ref を 1 戻す (最後の解放で WSACleanup を呼ぶ)。 */
WinsockReleaseResult WsaStartupReleaseReference() noexcept
{
    PendingWinsockFailureReports reports{};
    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);
    const WinsockReleaseResult result = WsaStartupReleaseReferenceLocked(WinsockReleaseOwnership::RetainOnFailure,
                                                                         reports);
    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);
    EmitWinsockResourceFailures(reports);
    return result;
}

/** destructor が残した socket または cleanup debt を共有回収処理へ移す。 */
struct WinsockOwnershipTransferResult {
    bool transferred = false;
    u32 orphaned_socket_count = 0;
    u32 overflow_orphaned_socket_count = 0;
    bool cleanup_debt_orphaned = false;
};

WinsockOwnershipTransferResult TransferWinsockOwnershipFromDestroyedTransport(SOCKET socket,
                                                                              bool owns_winsock_reference) noexcept
{
    WinsockOwnershipTransferResult result{};
    PendingWinsockFailureReports reports{};
    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);

    if (socket != INVALID_SOCKET) {
        if (g_primary_orphaned_socket_count < g_primary_orphaned_socket_capacity) {
            g_primary_orphaned_sockets[g_primary_orphaned_socket_count++] = {socket, owns_winsock_reference};
            result.transferred = true;
        } else {
            auto* overflow_record = static_cast<OverflowOrphanedSocketRecord*>(
                ::VirtualAlloc(nullptr, sizeof(OverflowOrphanedSocketRecord), MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE));
            if (overflow_record != nullptr) {
                overflow_record->resource = {socket, owns_winsock_reference};
                overflow_record->next = g_overflow_orphaned_socket_head;
                g_overflow_orphaned_socket_head = overflow_record;
                ++g_overflow_orphaned_socket_count;
                result.transferred = true;
            } else {
                reports.Add("orphaned_socket_registry_allocation_failed", static_cast<u32>(::GetLastError()), "true");
            }
        }
    } else if (owns_winsock_reference && g_winsock_reference_count == 1 && g_winsock_cleanup_error != 0 &&
               !g_winsock_cleanup_debt_orphaned) {
        g_winsock_cleanup_debt_orphaned = true;
        result.transferred = true;
    }

    result.orphaned_socket_count = TotalOrphanedSocketCountLocked();
    result.overflow_orphaned_socket_count = g_overflow_orphaned_socket_count;
    result.cleanup_debt_orphaned = g_winsock_cleanup_debt_orphaned;
    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);
    EmitWinsockResourceFailures(reports);
    return result;
}

} // namespace

namespace internal {

bool ConfigureUdpTransportFailureInjectionForTesting(const UdpTransportFailureInjection& injection) noexcept
{
#if !defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
    (void)injection;
    return false;
#else
    const bool close_error_is_retryable = injection.close_socket_error == static_cast<u32>(WSAEINTR) ||
                                          injection.close_socket_error == static_cast<u32>(WSAEWOULDBLOCK) ||
                                          injection.close_socket_error == static_cast<u32>(WSAEINPROGRESS);
    const bool cleanup_error_is_retryable = injection.cleanup_error == static_cast<u32>(WSAEINPROGRESS);
    if ((injection.close_socket_failure_count != 0 && !close_error_is_retryable) ||
        (injection.cleanup_failure_count != 0 && !cleanup_error_is_retryable) ||
        injection.close_socket_failure_count > static_cast<u32>(LONG_MAX) ||
        injection.cleanup_failure_count > static_cast<u32>(LONG_MAX) ||
        injection.primary_orphaned_socket_capacity > kMaximumPrimaryOrphanedSocketCount) {
        return false;
    }

    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);
    const bool ownership_is_clean = g_winsock_reference_count == 0 && g_winsock_cleanup_error == 0 &&
                                    !g_winsock_cleanup_debt_orphaned && TotalOrphanedSocketCountLocked() == 0 &&
                                    g_overflow_orphaned_socket_head == nullptr;
    if (ownership_is_clean) {
        ::_InterlockedExchange(&g_injected_close_socket_error, static_cast<LONG>(injection.close_socket_error));
        ::_InterlockedExchange(&g_injected_cleanup_error, static_cast<LONG>(injection.cleanup_error));
        ::_InterlockedExchange(&g_injected_close_socket_failure_count,
                               static_cast<LONG>(injection.close_socket_failure_count));
        ::_InterlockedExchange(&g_injected_cleanup_failure_count, static_cast<LONG>(injection.cleanup_failure_count));
        g_primary_orphaned_socket_capacity = injection.primary_orphaned_socket_capacity == 0
                                                 ? kMaximumPrimaryOrphanedSocketCount
                                                 : injection.primary_orphaned_socket_capacity;
    }
    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);
    return ownership_is_clean;
#endif
}

bool ResetUdpTransportDiagnosticsForTesting() noexcept
{
#if !defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
    return false;
#else
    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);

    // 注入だけは常に解除し、失敗テスト後の実 OS 回収を妨げない。
    ::_InterlockedExchange(&g_injected_close_socket_failure_count, 0);
    ::_InterlockedExchange(&g_injected_cleanup_failure_count, 0);

    const bool ownership_is_clean = g_winsock_reference_count == 0 && g_winsock_cleanup_error == 0 &&
                                    !g_winsock_cleanup_debt_orphaned && TotalOrphanedSocketCountLocked() == 0 &&
                                    g_overflow_orphaned_socket_head == nullptr;
    if (ownership_is_clean) {
        ::_InterlockedExchange(&g_injected_close_socket_error, WSAEWOULDBLOCK);
        ::_InterlockedExchange(&g_injected_cleanup_error, WSAEINPROGRESS);
        ::_InterlockedExchange64(&g_winsock_resource_failure_count, 0);
        g_primary_orphaned_socket_capacity = kMaximumPrimaryOrphanedSocketCount;
    }

    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);
    return ownership_is_clean;
#endif
}

} // namespace internal

/** 常に kSub_NotImplemented を返す (stub が本番に混入したケースを QA で検出可能に)。 */
TResult<void> NetTransportStub::Connect(const char* address, u16 port) noexcept
{
    (void)address;
    (void)port;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Connect is not implemented "
                   "(stub: link a concrete transport implementation)");
}

/** never-connected なので no-op。 */
void NetTransportStub::Disconnect() noexcept
{
    // stub は never-connected。no-op で安全に通す。
}

/** 常に kSub_NotImplemented を返す。 */
TResult<void> NetTransportStub::Send(const void* data, u32 size) noexcept
{
    (void)data;
    (void)size;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Send is not implemented "
                   "(stub: link a concrete transport implementation)");
}

/** 常に kSub_NotImplemented を返す。 */
TResult<u32> NetTransportStub::Receive(void* out_buffer, u32 capacity) noexcept
{
    (void)out_buffer;
    (void)capacity;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Receive is not implemented "
                   "(stub: link a concrete transport implementation)");
}

/** プロセス共有の stub を返す (function-local static で thread-safe 初期化)。 */
INetTransport& GetTransportStub() noexcept
{
    static NetTransportStub s_instance;
    return s_instance;
}

/** socket と WSA 参照の解放を試み、残存時は共有回収処理へ所有権を移して破棄する。 */
FUdpTransport::~FUdpTransport() noexcept
{
    bool cleanup_outstanding = false;
    u32 cleanup_error = 0;
    WinsockOwnershipTransferResult transfer_result{};
    {
        FScopedLock state_lock(m_StateLock);
        cleanup_error = DisconnectLocked();
        cleanup_outstanding = m_Socket != kInvalidSocket || m_WsaStarted;
        if (cleanup_outstanding) {
            // 生存期間が終わる instance から、socket と WSA token を不可分に共有回収処理へ移す。
            transfer_result = TransferWinsockOwnershipFromDestroyedTransport(
                m_Socket == kInvalidSocket ? INVALID_SOCKET : static_cast<SOCKET>(m_Socket), m_WsaStarted);
            m_Socket = kInvalidSocket;
            m_WsaStarted = false;
            m_State = EState::Disconnected;
        }
    }
    if (cleanup_outstanding) {
        EmitWinsockResourceState(transfer_result.transferred ? "transport_cleanup_deferred"
                                                             : "transport_cleanup_untracked",
                                 cleanup_error, transfer_result.transferred ? "inconclusive" : "true", true,
                                 transfer_result.orphaned_socket_count, transfer_result.overflow_orphaned_socket_count,
                                 transfer_result.cleanup_debt_orphaned);
        if (transfer_result.transferred) (void)DrainDeferredResources();
    }
}

/** 全 FUdpTransport が共有する Winsock 資源状態を一貫した時点で取得する。 */
UdpTransportDiagnostics FUdpTransport::CaptureDiagnostics() noexcept
{
    UdpTransportDiagnostics diagnostics{};
    ::AcquireSRWLockShared(&g_winsock_lifecycle_lock);
    diagnostics.active_winsock_reference_count = g_winsock_reference_count;
    diagnostics.pending_cleanup_error = g_winsock_cleanup_error;
    diagnostics.cleanup_debt_orphaned = g_winsock_cleanup_debt_orphaned;
    diagnostics.orphaned_socket_count = TotalOrphanedSocketCountLocked();
    diagnostics.overflow_orphaned_socket_count = g_overflow_orphaned_socket_count;
    diagnostics.resource_release_failure_count = static_cast<u64>(
        ::_InterlockedCompareExchange64(&g_winsock_resource_failure_count, 0, 0));
    ::ReleaseSRWLockShared(&g_winsock_lifecycle_lock);
    return diagnostics;
}

/** 共有回収処理へ移された socket / WSA 参照を明示的に drain する。 */
TResult<void> FUdpTransport::DrainDeferredResources() noexcept
{
    PendingWinsockFailureReports reports{};
    u32 first_error = 0;
    u32 remaining_socket_count = 0;
    u32 remaining_overflow_socket_count = 0;
    u32 cleanup_error = 0;
    bool cleanup_debt_orphaned = false;
    bool had_deferred_resources = false;

    ::AcquireSRWLockExclusive(&g_winsock_lifecycle_lock);
    had_deferred_resources = TotalOrphanedSocketCountLocked() != 0 || g_winsock_cleanup_debt_orphaned;
    first_error = DrainOrphanedSocketsLocked(reports);
    if (TotalOrphanedSocketCountLocked() == 0 && g_winsock_cleanup_debt_orphaned) {
        const u32 retry_error = RetryOrphanedWinsockCleanupLocked(reports);
        if (first_error == 0) first_error = retry_error;
    }
    remaining_socket_count = TotalOrphanedSocketCountLocked();
    remaining_overflow_socket_count = g_overflow_orphaned_socket_count;
    cleanup_error = g_winsock_cleanup_error;
    cleanup_debt_orphaned = g_winsock_cleanup_debt_orphaned;
    ::ReleaseSRWLockExclusive(&g_winsock_lifecycle_lock);

    EmitWinsockResourceFailures(reports);
    const bool cleanup_pending = remaining_socket_count != 0 || cleanup_debt_orphaned;
    if (first_error == 0 && !cleanup_pending) {
        if (had_deferred_resources) {
            EmitWinsockResourceState("deferred_cleanup_resolved", 0, "false", false, 0, 0, false);
        }
        return Ok();
    }

    const u32 reported_error = first_error != 0 ? first_error : cleanup_error;
    EmitWinsockResourceState(cleanup_pending ? "deferred_cleanup_still_pending" : "deferred_cleanup_untracked",
                             reported_error, cleanup_pending ? "inconclusive" : "true", cleanup_pending,
                             remaining_socket_count, remaining_overflow_socket_count, cleanup_debt_orphaned);
    return ACS_ERR_OS(IO, NetSnapshotError::kSub_CloseFailed,
                      "FUdpTransport::DrainDeferredResources: deferred Winsock cleanup failed", reported_error);
}

/** bind する local port を次回 Connect 用に設定する。 */
void FUdpTransport::SetLocalPort(u16 local_port) noexcept
{
    FScopedLock state_lock(m_StateLock);
    m_LocalPort = local_port;
}

/** 設定済みの local port を返す。 */
u16 FUdpTransport::LocalPort() const noexcept
{
    FScopedLock state_lock(m_StateLock);
    return m_LocalPort;
}

/** 完全に bind 済みで送受信可能な状態かを返す。 */
bool FUdpTransport::IsConnected() const noexcept
{
    FScopedLock state_lock(m_StateLock);
    return m_State == EState::Established;
}

/** WSAStartup → socket 生成 → 非ブロッキング化 → bind → remote endpoint 保持。 */
TResult<void> FUdpTransport::Connect(const char* address, u16 port) noexcept
{
    if (address == nullptr) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument, "FUdpTransport::Connect: address is null");
    }

    // ---- remote endpoint の IPv4 dotted-quad を先に検証 (副作用前に弾く) ----
    // inet_pton で "127.0.0.1" 等を 4 octet に変換する。失敗 = 不正なアドレス。
    in_addr remote_in{};
    if (::inet_pton(AF_INET, address, &remote_in) != 1) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadAddress,
                       "FUdpTransport::Connect: address is not a valid IPv4 dotted-quad");
    }

    FScopedLock state_lock(m_StateLock);

    // 多重 Connect と前回の cleanup debt は、完全に回収できるまで新しい所有権を作らない。
    if (m_State != EState::Disconnected || m_Socket != kInvalidSocket || m_WsaStarted) {
        const u32 cleanup_error = DisconnectLocked();
        if (cleanup_error != 0) {
            return ACS_ERR_OS(IO, NetSnapshotError::kSub_CloseFailed,
                              "FUdpTransport::Connect: previous socket cleanup failed", cleanup_error);
        }
    }
    m_State = EState::Configuring;

    // ---- WSAStartup (ref-count、初回のみ実行) -----------------------------
    const WinsockAcquireResult winsock_result = WsaStartupAddReference();
    if (winsock_result.error != 0) {
        m_State = EState::Disconnected;
        if (winsock_result.failure == WinsockAcquireFailure::DeferredCleanup) {
            return ACS_ERR_OS(IO, NetSnapshotError::kSub_CloseFailed,
                              "FUdpTransport::Connect: deferred Winsock cleanup failed", winsock_result.error);
        }
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_WsaStartup, "FUdpTransport::Connect: WSAStartup failed",
                          winsock_result.error);
    }
    m_WsaStarted = true;

    // ---- UDP socket 生成 -------------------------------------------------
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        const u32 socket_error = static_cast<u32>(::WSAGetLastError());
        m_State = EState::CleanupPending;
        (void)DisconnectLocked();
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_SocketCreate, "FUdpTransport::Connect: socket() failed",
                          socket_error);
    }
    // 以降の失敗でも ownership を失わないよう、構成完了前から内部状態へ記録する。
    m_Socket = static_cast<uptr>(socket);

    // ---- 非ブロッキング化 (FIONBIO) --------------------------------------
    u_long nonblocking = 1;
    if (::ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        const u32 socket_error = static_cast<u32>(::WSAGetLastError());
        m_State = EState::CleanupPending;
        (void)DisconnectLocked();
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_SetNonBlocking,
                          "FUdpTransport::Connect: ioctlsocket(FIONBIO) failed", socket_error);
    }

    // ---- local port に bind (m_LocalPort == 0 なら ephemeral) ------------
    // 全インターフェイス (INADDR_ANY) で受け、送信のみなら port 0 で OS 任せ。
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = ::htons(m_LocalPort);
    local.sin_addr.s_addr = ::htonl(INADDR_ANY);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        const u32 socket_error = static_cast<u32>(::WSAGetLastError());
        m_State = EState::CleanupPending;
        (void)DisconnectLocked();
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_Bind, "FUdpTransport::Connect: bind() failed (local port in use?)",
                          socket_error);
    }

    // ---- 状態確定: socket open + remote endpoint 保持 --------------------
    // inet_pton が書いた network-order 4 byte をそのまま octets に複製する。
    MemCopy(m_RemoteOctets, &remote_in, sizeof(m_RemoteOctets));
    m_RemotePort = port;
    m_State = EState::Established;
    return Ok();
}

/** instance lock 保持下で socket と WSA 参照を回収する。 */
u32 FUdpTransport::DisconnectLocked() noexcept
{
    u32 first_error = 0;

    if (m_Socket != kInvalidSocket) {
        if (CloseSocketOperation(static_cast<SOCKET>(m_Socket)) == SOCKET_ERROR) {
            const u32 close_error = static_cast<u32>(::WSAGetLastError());
            first_error = close_error;
            ReportWinsockResourceFailure("socket_close_failed", close_error, "inconclusive");

            if (close_error != static_cast<u32>(WSAENOTSOCK)) {
                // WSAEWOULDBLOCK 等では handle は依然有効なので、所有情報を保持して再試行可能にする。
                m_State = EState::CleanupPending;
                return first_error;
            }

            // WSAENOTSOCK は値が既に再利用され得る。別資源を閉じないよう数値を破棄する。
        }
        m_Socket = kInvalidSocket;
    }

    m_RemoteOctets[0] = m_RemoteOctets[1] = m_RemoteOctets[2] = m_RemoteOctets[3] = 0;
    m_RemotePort = 0;

    if (m_WsaStarted) {
        const WinsockReleaseResult release_result = WsaStartupReleaseReference();
        if (release_result.released) m_WsaStarted = false;
        if (first_error == 0) first_error = release_result.error;
        if (!release_result.released) {
            m_State = EState::CleanupPending;
            return first_error;
        }
    }

    m_State = EState::Disconnected;
    return first_error;
}

/** closesocket + WSACleanup (ref を戻す)。多重 / 未接続呼出は no-op。 */
void FUdpTransport::Disconnect() noexcept
{
    FScopedLock state_lock(m_StateLock);
    (void)DisconnectLocked();
}

/** remote endpoint へ 1 datagram を sendto() する (境界保持、1 Send = 1 packet)。 */
TResult<void> FUdpTransport::Send(const void* data, u32 size) noexcept
{
    FScopedLock state_lock(m_StateLock);
    if (m_State != EState::Established || m_Socket == kInvalidSocket) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NotConnected,
                       "FUdpTransport::Send: not connected (call Connect first)");
    }
    if (data == nullptr || size == 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument, "FUdpTransport::Send: data is null or size == 0");
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = ::htons(m_RemotePort);
    // 保持しておいた network-order 4 byte を in_addr に戻す。
    MemCopy(&remote.sin_addr, m_RemoteOctets, sizeof(m_RemoteOctets));

    const int sent = ::sendto(static_cast<SOCKET>(m_Socket), static_cast<const char*>(data), static_cast<int>(size), 0,
                              reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    if (sent == SOCKET_ERROR) {
        const int werr = ::WSAGetLastError();
        if (werr == WSAEWOULDBLOCK) {
            // 送信 buffer が一時的に満杯。BufferFull として上位に伝える。
            return ACS_ERR(IO, NetSnapshotError::kSub_BufferFull,
                           "FUdpTransport::Send: send buffer full (WSAEWOULDBLOCK)");
        }
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_SendFailed, "FUdpTransport::Send: sendto() failed",
                          static_cast<u32>(werr));
    }
    // UDP の sendto は datagram 全体を一度に送る (部分送信は起きない)。
    return Ok();
}

/** 非ブロッキング recvfrom() で 1 datagram を取り出す (受信なしは 0 byte 成功)。 */
TResult<u32> FUdpTransport::Receive(void* out_buffer, u32 capacity) noexcept
{
    FScopedLock state_lock(m_StateLock);
    if (m_State != EState::Established || m_Socket == kInvalidSocket) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NotConnected,
                       "FUdpTransport::Receive: not connected (call Connect first)");
    }
    if (out_buffer == nullptr || capacity == 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument,
                       "FUdpTransport::Receive: out_buffer is null or capacity == 0");
    }

    sockaddr_in from{};
    int from_len = static_cast<int>(sizeof(from));
    const int got = ::recvfrom(static_cast<SOCKET>(m_Socket), static_cast<char*>(out_buffer),
                               static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got == SOCKET_ERROR) {
        const int werr = ::WSAGetLastError();
        if (werr == WSAEWOULDBLOCK) {
            // 受信データなし。0 byte の成功 (= Tick の pump ループ終了条件)。
            return TResult<u32>(OkInit, 0u);
        }
        return ACS_ERR_OS(IO, NetSnapshotError::kSub_RecvFailed, "FUdpTransport::Receive: recvfrom() failed",
                          static_cast<u32>(werr));
    }
    // got >= 0。0 byte datagram (空 UDP packet) もそのまま成功で返す。
    return TResult<u32>(OkInit, static_cast<u32>(got));
}

/** recv 待ちバイト数を ioctlsocket(FIONREAD) で問い合わせる (失敗 / 未接続は 0)。 */
u32 FUdpTransport::PendingBytesIn() const noexcept
{
    FScopedLock state_lock(m_StateLock);
    if (m_State != EState::Established || m_Socket == kInvalidSocket) {
        return 0;
    }
    u_long avail = 0;
    if (::ioctlsocket(static_cast<SOCKET>(m_Socket), FIONREAD, &avail) == SOCKET_ERROR) {
        return 0;
    }
    return static_cast<u32>(avail);
}

/** 1 frame の総バイト数を返す (u64 で加算 → u32 上限超えは clamp)。 */
u32 FNetSnapshot::EncodedSnapshotSize(u32 payload_size) noexcept
{
    const u64 total = static_cast<u64>(kFrameFixedOverhead) + static_cast<u64>(payload_size);
    // payload_size は呼出側で max_payload_bytes 以下に制限済み。理論上の
    // オーバーフローは clamp で防御 (u32 上限を超えたら u32 max を返す)。
    if (total > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }
    return static_cast<u32>(total);
}

/** header + payload を frame wire layout で out_buffer に直列化する (CRC を footer に付与)。 */
TResult<void> FNetSnapshot::EncodeSnapshot(const SnapshotHeader& header, const u8* payload, u32 payload_size,
                                           u8* out_buffer, u32 out_capacity, u32& out_written) noexcept
{
    out_written = 0;
    if (out_buffer == nullptr) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NullBuffer, "FNetSnapshot::EncodeSnapshot: out_buffer is null");
    }
    // payload == nullptr は payload_size == 0 のときのみ許容。
    if (payload == nullptr && payload_size != 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument,
                       "FNetSnapshot::EncodeSnapshot: payload is null but size != 0");
    }
    // header.payload_size と引数 payload_size の不一致は wire 不整合の元なので拒否。
    if (header.payload_size != payload_size) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument,
                       "FNetSnapshot::EncodeSnapshot: header.payload_size != payload_size");
    }

    const u32 required = EncodedSnapshotSize(payload_size);
    if (out_capacity < required) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BufferTooSmall,
                       "FNetSnapshot::EncodeSnapshot: out_buffer too small for frame");
    }

    // ---- magic (4) + version (4) ----------------------------------------
    MemCopy(out_buffer, kFrameMagic, sizeof(kFrameMagic));
    WriteU32LE(out_buffer + kVersionOffset, kFrameVersion);

    // ---- SnapshotHeader (24, crc32 フィールドは 0 で書き出す) -------------
    // crc32 は frame footer に置くため、CRC 計算対象に含まれる header 内
    // フィールドは 0 として扱う (= 自己参照を避ける)。
    SnapshotHeader hdr = header;
    hdr.crc32 = 0;
    WriteHeader(out_buffer + kHeaderOffset, hdr);

    // ---- payload --------------------------------------------------------
    if (payload_size > 0) {
        MemCopy(out_buffer + kPayloadOffset, payload, payload_size);
    }

    // ---- crc32 footer ---------------------------------------------------
    // 対象 = [version .. payload 末尾) = magic を除いた本体。
    const u32 crc_region_size = kVersionSize + kHeaderWireSize + payload_size;
    const u32 crc = ComputeCrc32(out_buffer + kVersionOffset, crc_region_size);
    WriteU32LE(out_buffer + kPayloadOffset + payload_size, crc);

    out_written = required;
    return Ok();
}

/** frame bytes を magic / version / size / CRC32 で検証し、header + payload を復元する。 */
TResult<void> FNetSnapshot::DecodeSnapshot(const u8* buffer, u32 size, SnapshotHeader& out_header,
                                           TArray<u8>& out_payload) noexcept
{
    if (buffer == nullptr) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NullBuffer, "FNetSnapshot::DecodeSnapshot: buffer is null");
    }
    // ---- 最小サイズ (固定オーバーヘッド = payload 0 のケース) ------------
    if (size < kFrameFixedOverhead) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadSize,
                       "FNetSnapshot::DecodeSnapshot: buffer smaller than frame overhead");
    }
    // ---- magic 検証 ------------------------------------------------------
    if (MemCmp(buffer, kFrameMagic, sizeof(kFrameMagic)) != 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadMagic,
                       "FNetSnapshot::DecodeSnapshot: magic mismatch (not an ACSN frame)");
    }
    // ---- version 検証 ----------------------------------------------------
    const u32 version = ReadU32LE(buffer + kVersionOffset);
    if (version != kFrameVersion) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadVersion,
                       "FNetSnapshot::DecodeSnapshot: unsupported frame version");
    }

    // ---- header 読み出し + payload_size とサイズの整合 -------------------
    SnapshotHeader hdr{};
    ReadHeader(buffer + kHeaderOffset, hdr);

    // payload_size + 固定オーバーヘッドが size と完全一致することを要求する。
    // u64 で計算して 32bit 加算オーバーフローを避ける。
    const u64 expected64 = static_cast<u64>(kFrameFixedOverhead) + static_cast<u64>(hdr.payload_size);
    if (expected64 != static_cast<u64>(size)) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadSize,
                       "FNetSnapshot::DecodeSnapshot: payload_size inconsistent with buffer size");
    }

    // ---- crc32 検証 ------------------------------------------------------
    // 対象 = [version .. payload 末尾)。header 内 crc32 フィールドは encode 時に
    // 0 だったので、ここでも自然に 0 が CRC 計算へ寄与する。
    const u32 crc_region_size = kVersionSize + kHeaderWireSize + hdr.payload_size;
    const u32 actual_crc = ComputeCrc32(buffer + kVersionOffset, crc_region_size);
    const u32 stored_crc = ReadU32LE(buffer + kPayloadOffset + hdr.payload_size);
    if (actual_crc != stored_crc) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadCrc,
                       "FNetSnapshot::DecodeSnapshot: CRC32 mismatch (corrupt or tampered)");
    }

    // ---- payload を復元 (置換) ------------------------------------------
    out_payload.Clear();
    out_payload.Resize(static_cast<usize>(hdr.payload_size));
    if (hdr.payload_size > 0) {
        MemCopy(out_payload.Data(), buffer + kPayloadOffset, static_cast<usize>(hdr.payload_size));
    }

    // footer の CRC を header に復元して返す (呼出側が保持できるように)。
    hdr.crc32 = stored_crc;
    out_header = hdr;
    return Ok();
}

/** 設定をコピーし ring buffer を確保する (Standalone 以外で nullptr transport は stub に差替)。 */
void FNetSnapshot::Init(const NetSnapshotConfig& config, ENetRole role, INetTransport* transport) noexcept
{
    m_Config = config;
    m_Role = role;

    // buffer_capacity が 0 / 1 だと補間 (= 2 snapshot 必須) が成立しないため
    // 最低 2 に丸める。max_payload_bytes が 0 の場合は 1KB に。
    if (m_Config.buffer_capacity_snapshots < 2) {
        m_Config.buffer_capacity_snapshots = 2;
    }
    if (m_Config.max_payload_bytes == 0) {
        m_Config.max_payload_bytes = 1024;
    }

    // Standalone 以外で nullptr が渡されたら stub に差し替える (落ちないように)。
    if (role == ENetRole::Standalone) {
        m_Transport = transport; // nullptr 許容
    } else {
        m_Transport = (transport != nullptr) ? transport : &GetTransportStub();
    }

    // ring buffer を capacity 件で fixed-size 確保。各エントリの payload は
    // 初期サイズ 0 (CommitSnapshot で書き込まれる際に Resize される)。
    m_RingBuffer.Clear();
    m_RingBuffer.Resize(static_cast<usize>(m_Config.buffer_capacity_snapshots));
    m_RingHead = 0;
    m_RingCount = 0;

    m_PendingEntities.Clear();
    m_InterpScratch.Clear();

    m_NextSequence = 1;
    m_LastReceivedTick = 0;
    m_PacketsSent = 0;
    m_PacketsReceived = 0;
    m_BytesSent = 0;
    m_BytesReceived = 0;
}

/** ring buffer / pending / scratch を解放する (transport は外部所有なので触らない)。 */
void FNetSnapshot::Shutdown() noexcept
{
    m_RingBuffer.Clear();
    m_PendingEntities.Clear();
    m_InterpScratch.Clear();
    m_RingHead = 0;
    m_RingCount = 0;
    m_Transport = nullptr;
}

/** ring buffer に貯まっている snapshot 件数を返す。 */
u32 FNetSnapshot::BufferedSnapshotCount() const noexcept
{
    return m_RingCount;
}

/** server 側で 1 entity 分の state を pending list にコピーして積む (Client/Standalone は no-op)。 */
void FNetSnapshot::AddEntitySnapshot(u32 entity_id, u32 component_mask, const void* data, u32 data_size) noexcept
{
    // Client / Standalone は送信側ではないので no-op。
    if (m_Role == ENetRole::Client || m_Role == ENetRole::Standalone) {
        return;
    }
    // 不正な引数は黙ってスキップ (defensive、ベストエフォート方針)。
    if (data == nullptr && data_size != 0) {
        return;
    }

    PendingEntity pe;
    pe.entity_id = entity_id;
    pe.component_mask = component_mask;
    if (data_size > 0) {
        pe.data.Resize(static_cast<usize>(data_size));
        MemCopy(pe.data.Data(), data, data_size);
    }
    m_PendingEntities.PushBack(Move(pe));
}

/** pending list を 1 payload に concat し、frame を構築して best-effort 送信する。 */
void FNetSnapshot::CommitSnapshot(u32 tick) noexcept
{
    // 役割チェック。Client / Standalone は no-op。
    if (m_Role == ENetRole::Client || m_Role == ENetRole::Standalone) {
        m_PendingEntities.Clear();
        return;
    }
    if (m_Transport == nullptr) {
        m_PendingEntities.Clear();
        return;
    }

    // payload バイト数を見積もる。
    //   per-entity = kEntityHeaderSize (12) + data_size
    usize payload_size = 0;
    const usize n_ent = m_PendingEntities.Size();
    for (usize i = 0; i < n_ent; ++i) {
        payload_size += static_cast<usize>(kEntityHeaderSize) + static_cast<usize>(m_PendingEntities[i].data.Size());
    }
    if (payload_size > static_cast<usize>(m_Config.max_payload_bytes)) {
        // 上限超過。interest management / 分割送信は本クラスの範囲外なので skip。
        m_PendingEntities.Clear();
        return;
    }

    // payload を per-entity layout で 1 本に concat する。
    TArray<u8> payload_buf;
    payload_buf.Resize(payload_size);
    u8* payload_ptr = payload_buf.Data();
    usize off = 0;
    for (usize i = 0; i < n_ent; ++i) {
        const PendingEntity& pe = m_PendingEntities[i];
        const u32 data_size = static_cast<u32>(pe.data.Size());
        WriteU32LE(payload_ptr + off + 0, pe.entity_id);
        WriteU32LE(payload_ptr + off + 4, pe.component_mask);
        WriteU32LE(payload_ptr + off + 8, data_size);
        if (data_size > 0) {
            MemCopy(payload_ptr + off + kEntityHeaderSize, pe.data.Data(), data_size);
        }
        off += static_cast<usize>(kEntityHeaderSize) + static_cast<usize>(data_size);
    }

    // header を構築。crc32 は EncodeSnapshot が footer に計算して書き込む。
    SnapshotHeader hdr;
    hdr.tick = tick;
    hdr.sequence = m_NextSequence;
    hdr.server_timestamp_us = 0; // タイムスタンプ source の wire 化は caller 責務
    hdr.payload_size = static_cast<u32>(payload_size);
    hdr.crc32 = 0;

    // frame bytes = magic+version+header+payload+crc32 を 1 本にまとめる
    // (Send 1 回で送信)。EncodeSnapshot が real CRC を計算する。
    TArray<u8> wire_buf;
    const u32 frame_size = EncodedSnapshotSize(static_cast<u32>(payload_size));
    wire_buf.Resize(static_cast<usize>(frame_size));
    u32 written = 0;
    TResult<void> enc = EncodeSnapshot(hdr, payload_ptr, static_cast<u32>(payload_size), wire_buf.Data(), frame_size,
                                       written);
    if (enc.IsErr()) {
        // 直列化に失敗したら send せず skip (ベストエフォート)。
        m_PendingEntities.Clear();
        return;
    }

    // 送信 (best-effort)。失敗してもクラッシュさせず、統計だけ更新しない。
    TResult<void> r = m_Transport->Send(wire_buf.Data(), written);
    if (r.IsOk()) {
        ++m_PacketsSent;
        m_BytesSent += written;
        ++m_NextSequence;
        if (m_NextSequence == 0) {
            m_NextSequence = 1; // 0 は invalid 値として予約
        }

        // ServerListener は host が自分の画面用に補間も使うため、自分が
        // commit した snapshot を ring buffer にも積む (loopback)。送信した
        // frame を DecodeSnapshot で復元することで、受信経路と同じ検証 (CRC 等)
        // を通した上で ring buffer に積む (= encode/decode の round-trip 検証)。
        if (m_Role == ENetRole::ServerListener) {
            const u32 idx = m_RingHead;
            BufferedSnapshot& slot = m_RingBuffer[static_cast<usize>(idx)];
            SnapshotHeader decoded{};
            TResult<void> dec = DecodeSnapshot(wire_buf.Data(), written, decoded, slot.payload);
            if (dec.IsOk()) {
                slot.header = decoded;
                m_RingHead = (m_RingHead + 1u) % m_Config.buffer_capacity_snapshots;
                if (m_RingCount < m_Config.buffer_capacity_snapshots) {
                    ++m_RingCount;
                }
                m_LastReceivedTick = decoded.tick;
            }
        }
    }

    // pending list は成否によらずクリア (= 次 tick の AddEntitySnapshot は
    // 必ず空 list から積み直す)。
    m_PendingEntities.Clear();
}

/** transport.Receive を pump し、検証成功した snapshot を ring buffer に積む。 */
void FNetSnapshot::Tick(f32 dt) noexcept
{
    (void)dt;

    // Server (listen 非搭載) は受信側を持たないので何もしない。
    // Client / ServerListener / Standalone は (transport があれば) 受信する。
    if (m_Role == ENetRole::Server) {
        return;
    }
    if (m_Transport == nullptr) {
        return;
    }

    // 受信用一時 buffer。1 frame 最大長 = 固定オーバーヘッド + max_payload_bytes。
    const u32 cap = kFrameFixedOverhead + m_Config.max_payload_bytes;
    TArray<u8> rx_buf;
    rx_buf.Resize(static_cast<usize>(cap));

    // 1 Tick で取りきる安全上限 (transport が壊れてループが終わらない事故予防)。
    // ring buffer 容量の 2 倍を上限としておく (古い snapshot は捨てる流儀)。
    const u32 max_iters = m_Config.buffer_capacity_snapshots * 2u;
    for (u32 iter = 0; iter < max_iters; ++iter) {
        TResult<u32> r = m_Transport->Receive(rx_buf.Data(), cap);
        if (r.IsErr()) {
            // 受信エラーは loss と等価扱い。次 Tick で再試行。
            break;
        }
        const u32 got = r.Value();
        if (got == 0) {
            // データなし。受信側 pump 完了。
            break;
        }

        // ring buffer の挿入先 slot に直接 decode する (payload を slot に複製)。
        // FIFO で最古を上書きするため、検証成功時にのみ head を進める。
        const u32 idx = m_RingHead;
        BufferedSnapshot& slot = m_RingBuffer[static_cast<usize>(idx)];
        SnapshotHeader hdr{};
        TResult<void> dec = DecodeSnapshot(rx_buf.Data(), got, hdr, slot.payload);
        if (dec.IsErr()) {
            // magic / version / size / CRC のいずれかで弾かれた frame。破棄して
            // 統計にだけ載せる (= 破損 / 非互換 / 改竄パケットは ring に積まない)。
            ++m_PacketsReceived;
            m_BytesReceived += got;
            continue;
        }

        slot.header = hdr;
        m_RingHead = (m_RingHead + 1u) % m_Config.buffer_capacity_snapshots;
        if (m_RingCount < m_Config.buffer_capacity_snapshots) {
            ++m_RingCount;
        }
        m_LastReceivedTick = hdr.tick;
        ++m_PacketsReceived;
        m_BytesReceived += got;
    }
}

/** 最新 snapshot を payload walk して EntitySnapshot view に流す (float lerp は未実装)。 */
bool FNetSnapshot::TryGetInterpolatedSnapshot(f32 client_time_sec, EntitySnapshot* out_snapshots, u32 max_count,
                                              u32& out_actual_count) noexcept
{
    out_actual_count = 0;

    // Server は補間を持たない。
    if (m_Role == ENetRole::Server) {
        return false;
    }
    if (out_snapshots == nullptr || max_count == 0) {
        return false;
    }
    if (m_RingCount == 0) {
        return false;
    }

    // ring buffer 中の最新スロットを探す。挿入は m_RingHead に書き込み後に
    // インクリメントするので、最新エントリは (m_RingHead - 1) % capacity。
    const u32 cap = m_Config.buffer_capacity_snapshots;
    const u32 newest_idx = (m_RingHead + cap - 1u) % cap;
    const BufferedSnapshot& newest = m_RingBuffer[static_cast<usize>(newest_idx)];

    // target_seq: client_time_sec を秒として持つが、実時刻軸はまだ wire format
    // に乗っていない (server_timestamp_us は 0 固定)。よって本実装では「最新
    // snapshot を返す」シンプル選択を取る。
    //
    // server_timestamp_us を計測時刻と紐付ける場合は、ここで
    //   target_us = newest.header.server_timestamp_us
    //             - static_cast<u64>(m_Config.interpolation_delay_sec * 1e6f);
    // を計算し、target_us を挟む 2 snapshot を ring から線形探索 → lerp する。
    // 現状 client_time_sec は受け取るだけで使わない (API 後方互換のため)。
    (void)client_time_sec;

    const SnapshotHeader& hdr = newest.header;
    const u8* payload_ptr = newest.payload.Data();
    const u32 payload_size = static_cast<u32>(newest.payload.Size());

    // payload を per-entity wire format で walk して EntitySnapshot に流す。
    // out_snapshots の component_data は ring buffer の payload を直接指す
    // 非所有 view (寿命 = 次の Tick() まで)。
    u32 off = 0;
    u32 emitted = 0;
    while (static_cast<u64>(off) + kEntityHeaderSize <= payload_size && emitted < max_count) {
        const u32 entity_id = ReadU32LE(payload_ptr + off + 0);
        const u32 component_mask = ReadU32LE(payload_ptr + off + 4);
        const u32 data_size = ReadU32LE(payload_ptr + off + 8);

        // size sanity check。payload を超えるなら破損 (= 残りを捨てる)。
        // **u64 で計算する**: data_size は CRC 検証済みでも構造的には信頼できない
        // (悪意ある peer / 破損)。u32 のまま off+12+data_size を計算すると
        // data_size≈0xFFFFFFFF で加算が wrap して check をすり抜け、payload_ptr に
        // 多 GB の OOB view を返してしまう (auditor 指摘)。
        if (static_cast<u64>(off) + kEntityHeaderSize + data_size > payload_size) {
            break;
        }

        EntitySnapshot& dst = out_snapshots[emitted];
        dst.entity_id = entity_id;
        dst.component_mask = component_mask;
        dst.component_data = (data_size > 0) ? static_cast<const void*>(payload_ptr + off + kEntityHeaderSize)
                                             : nullptr;
        dst.component_data_size = data_size;

        off += kEntityHeaderSize + data_size;
        ++emitted;
    }

    out_actual_count = emitted;
    // 統計上、補間に使った snapshot の header.tick を反映。
    (void)hdr;
    return emitted > 0;
}

} // namespace acs::game
