// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar W — FStudioWorkflow stub 実装 (CAssetLockingStub / CBuildFarmStub)
//
// 本ファイルは FStudioWorkflow.h で宣言した 2 つの interface に対し、
// 「常に NotImplemented を返すだけ」の defensive stub を提供する。
//
// 目的:
//   ・ACS 本体 / エディタが Perforce / Plastic / Jenkins SDK の有無に関わらず
//     リンクを通せるようにする (Pillar W の seam 要件)。
//   ・タイトル側 / エディタ側が
//       `IAssetLockingBackend* p = &acs::game::GetAssetLockingStub();`
//     のように null-object パターンで保持し、後から具象実装に差し替える経路を
//     確保する。
//   ・stub に対する LockAsset / SubmitBuild 等の呼び出しは「成功扱いで黙る」ではなく
//     **必ず TResult<...> Err を返す** ことで、本番ビルドに stub が紛れ込んだ
//     ケースを QA 工程で検出可能にしておく。
//
// 設計メモ:
//   ・Stub は **process-wide singleton** で十分。`static` ローカル変数で
//     thread-safe initialization (C++11 magic statics) を活用する。
//   ・コピー/ムーブは IAssetLockingBackend / IBuildFarmBackend 側で delete 済み
//     なので、stub 派生クラスも自然に non-copy / non-movable。
//   ・全関数 noexcept。stub なので分岐も最小限。
//   ・引数バリデーション (nullptr 等) は本実装ではしない: NotImplemented を
//     先に返してしまうため。具象実装側で kSub_BadArgument を返す責務になる。
#include "gameframework/StudioWorkflow.h"

#include "foundation/Error.h"
#include "foundation/Platform.h"   // <windows.h> (CreateFileW / CreateProcessW 等)

namespace acs::game {

/** stub: 常に NotImplemented を返す (具象アセットロックバックエンドを link せよ)。 */
TResult<void> CAssetLockingStub::LockAsset(const char* asset_path, const char* user) noexcept {
    (void)asset_path;
    (void)user;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::LockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

/** stub: 常に NotImplemented を返す。 */
TResult<void> CAssetLockingStub::UnlockAsset(const char* asset_path) noexcept {
    (void)asset_path;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IAssetLockingBackend::UnlockAsset is not implemented "
                   "(stub: link a concrete asset locking backend such as Perforce/Plastic)");
}

/** stub: 常に NotImplemented を返す。 */
TResult<FAssetLockInfo> CAssetLockingStub::QueryLock(const char* asset_path) noexcept {
    (void)asset_path;
    return TResult<FAssetLockInfo>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IAssetLockingBackend::QueryLock is not implemented "
                "(stub: link a concrete asset locking backend such as Perforce/Plastic)"));
}

/** stub: 常に NotImplemented を返す (具象ビルドファームバックエンドを link せよ)。 */
TResult<u64> CBuildFarmStub::SubmitBuild(const FBuildRequest& req) noexcept {
    (void)req;
    return TResult<u64>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::SubmitBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

/** stub: 常に NotImplemented を返す。 */
TResult<IBuildFarmBackend::FBuildResult> CBuildFarmStub::PollBuild(u64 build_id) noexcept {
    (void)build_id;
    return TResult<IBuildFarmBackend::FBuildResult>(
        ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                "IBuildFarmBackend::PollBuild is not implemented "
                "(stub: link a concrete build farm backend such as Jenkins/TeamCity)"));
}

/** stub: 常に NotImplemented を返す。 */
TResult<void> CBuildFarmStub::CancelBuild(u64 build_id) noexcept {
    (void)build_id;
    return ACS_ERR(Generic, FStudioWorkflowError::kSub_NotImplemented,
                   "IBuildFarmBackend::CancelBuild is not implemented "
                   "(stub: link a concrete build farm backend such as Jenkins/TeamCity)");
}

namespace {

/**
 * UTF-8 文字列を UTF-16 へ変換する (FHotReload と同流儀)。
 *
 * @details u8 (NUL 終端) を out_w に NUL 終端付きで変換する。
 * @param u8 入力 UTF-8 文字列 (NUL 終端)。
 * @param out_w 変換結果を書き込む UTF-16 バッファ。
 * @param out_cap out_w の要素数。
 * @return 成功で true、入力 null / バッファ不足 / 変換失敗で false。
 */
bool Utf8ToUtf16(const char* u8, wchar_t* out_w, int out_cap) noexcept {
    if (u8 == nullptr || out_w == nullptr || out_cap <= 0) {
        return false;
    }
    // -1 を渡して入力 NUL 終端を含めて変換させる (出力も NUL 終端になる)。
    const int got = ::MultiByteToWideChar(CP_UTF8, 0, u8, -1, out_w, out_cap);
    return got > 0;
}

/**
 * char バッファへ NUL 終端付きで安全コピーする (STL 非依存)。
 *
 * @details src が長すぎれば dst の容量に合わせて切り詰める。
 * @param dst コピー先バッファ。
 * @param cap dst の要素数。
 * @param src コピー元文字列 (nullptr 可)。
 */
void CopyCStr(char* dst, int cap, const char* src) noexcept {
    if (dst == nullptr || cap <= 0) return;
    int i = 0;
    if (src != nullptr) {
        for (; i < cap - 1 && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/**
 * NUL 終端 C 文字列の一致を判定する (strcmp 相当, STL 非依存)。
 *
 * @param a 比較する文字列 1 (nullptr 可)。
 * @param b 比較する文字列 2 (nullptr 可)。
 * @return 内容が一致すれば true (両方 nullptr も一致扱い)。
 */
bool CStrEqual(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

/**
 * NUL 終端済み wchar_t バッファの末尾に suffix を連結する (NUL 終端維持)。
 *
 * @details 連結後に容量を超える場合は何もしない。
 * @param dst 連結先バッファ (NUL 終端済み)。
 * @param cap dst の要素数。
 * @param suffix 連結する UTF-16 文字列。
 * @return 連結に成功すれば true、溢れ/不正引数なら false。
 */
bool AppendWStr(wchar_t* dst, int cap, const wchar_t* suffix) noexcept {
    if (dst == nullptr || suffix == nullptr || cap <= 0) return false;
    int len = 0;
    while (len < cap && dst[len] != L'\0') ++len;
    if (len >= cap) return false;  // 終端が無い (壊れている)
    int j = 0;
    while (suffix[j] != L'\0') {
        if (len + 1 >= cap) return false;  // NUL の分を残せない
        dst[len++] = suffix[j++];
    }
    dst[len] = L'\0';
    return true;
}

/**
 * 現在時刻を UNIX epoch 秒で取得する。
 *
 * @details Win32 FILETIME (1601-01-01 起点, 100ns 単位) を UNIX epoch 秒へ変換する。
 * @return 現在時刻の UNIX epoch 秒。
 */
u64 NowUnixSeconds() noexcept {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    u64 ticks = (static_cast<u64>(ft.dwHighDateTime) << 32) |
                static_cast<u64>(ft.dwLowDateTime);
    // 1601→1970 のオフセット (秒) = 11644473600。100ns → 秒は /10,000,000。
    constexpr u64 kEpochOffsetSec = 11644473600ull;
    return (ticks / 10000000ull) - kEpochOffsetSec;
}

/**
 * u64 を 10 進 ASCII 文字列に変換する (sprintf 非使用)。
 *
 * @param v 変換する値。
 * @param out 結果を書き込むバッファ (NUL 終端付き)。
 * @param cap out の要素数。
 * @return 書いたバイト数 (NUL 除く)。
 */
int U64ToDec(u64 v, char* out, int cap) noexcept {
    if (out == nullptr || cap <= 0) return 0;
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10ull));
        v /= 10ull;
    } while (v != 0ull && n < 24);
    int w = 0;
    while (n > 0 && w < cap - 1) {
        out[w++] = tmp[--n];
    }
    out[w] = '\0';
    return w;
}

struct FParsedLocalLock {
    char                 owner[CLocalFileAssetLocking::kMaxUserChars] = {};
    FLocalAssetLockToken token = {};
    u64                  lock_time = 0;
};

FLocalAssetLockResult LocalLockError(ELocalAssetLockError error,
                                     DWORD os_error = 0) noexcept {
    FLocalAssetLockResult result{};
    result.error = error;
    result.os_error = static_cast<u32>(os_error);
    return result;
}

bool TokensEqual(FLocalAssetLockToken a, FLocalAssetLockToken b) noexcept {
    return a.high == b.high && a.low == b.low;
}

bool BoundedLength(const char* value, int capacity, int& out_length) noexcept {
    out_length = 0;
    if (value == nullptr || capacity <= 0) return false;
    while (out_length < capacity && value[out_length] != '\0') ++out_length;
    return out_length < capacity;
}

bool IsStrictUtf8(const char* value, int length) noexcept {
    if (value == nullptr || length <= 0) return false;
    wchar_t scratch[CLocalFileAssetLocking::kMaxPathChars] = {};
    if (length >= CLocalFileAssetLocking::kMaxPathChars) return false;
    const int got = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length,
                                           scratch,
                                           CLocalFileAssetLocking::kMaxPathChars);
    return got > 0;
}

ELocalAssetLockError ValidateOwner(const char* owner, int& out_length) noexcept {
    if (owner == nullptr || owner[0] == '\0') return ELocalAssetLockError::BadArgument;
    if (!BoundedLength(owner, CLocalFileAssetLocking::kMaxUserChars, out_length)) {
        return ELocalAssetLockError::OwnerTooLong;
    }
    for (int i = 0; i < out_length; ++i) {
        const unsigned char c = static_cast<unsigned char>(owner[i]);
        if (c < 0x20u || c == 0x7fu) return ELocalAssetLockError::InvalidOwner;
    }
    return IsStrictUtf8(owner, out_length)
        ? ELocalAssetLockError::None
        : ELocalAssetLockError::InvalidUtf8;
}

ELocalAssetLockError MakeStrictLockPath(const char* asset_path,
                                        wchar_t* out_w,
                                        int out_cap) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        return ELocalAssetLockError::BadArgument;
    }
    int path_length = 0;
    if (!BoundedLength(asset_path, CLocalFileAssetLocking::kMaxPathChars,
                       path_length)) {
        return ELocalAssetLockError::PathTooLong;
    }
    for (int i = 0; i < path_length; ++i) {
        if (static_cast<unsigned char>(asset_path[i]) < 0x20u) {
            return ELocalAssetLockError::BadArgument;
        }
    }
    if (!IsStrictUtf8(asset_path, path_length)) {
        return ELocalAssetLockError::InvalidUtf8;
    }
    const int got = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           asset_path, path_length, out_w,
                                           out_cap - 1);
    if (got <= 0) return ELocalAssetLockError::PathTooLong;
    out_w[got] = L'\0';
    if (!AppendWStr(out_w, out_cap, L".lock")) {
        return ELocalAssetLockError::PathTooLong;
    }
    return ELocalAssetLockError::None;
}

u64 MixLockToken(u64 value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31;
    return value;
}

FLocalAssetLockToken GenerateLockToken() noexcept {
    alignas(8) static volatile LONG64 sequence = 0;
    LARGE_INTEGER counter{};
    FILETIME now{};
    ::QueryPerformanceCounter(&counter);
    ::GetSystemTimeAsFileTime(&now);
    const u64 time_bits = (static_cast<u64>(now.dwHighDateTime) << 32) |
                          static_cast<u64>(now.dwLowDateTime);
    const u64 seq = static_cast<u64>(::InterlockedIncrement64(&sequence));
    const u64 process_bits =
        (static_cast<u64>(::GetCurrentProcessId()) << 32) |
        static_cast<u64>(::GetCurrentThreadId());
    FLocalAssetLockToken token{};
    token.high = MixLockToken(time_bits ^ process_bits ^ seq);
    token.low = MixLockToken(static_cast<u64>(counter.QuadPart) ^
                             (seq * 0x9e3779b97f4a7c15ull) ^
                             (process_bits << 1));
    if (!token.IsValid()) token.low = 1;
    return token;
}

char HexDigit(unsigned value) noexcept {
    return static_cast<char>(value < 10u ? ('0' + value) : ('A' + value - 10u));
}

void U64ToHex16(u64 value, char* out) noexcept {
    for (int i = 15; i >= 0; --i) {
        out[i] = HexDigit(static_cast<unsigned>(value & 0xfull));
        value >>= 4;
    }
}

int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int BuildLockRecord(const char* owner,
                    int owner_length,
                    FLocalAssetLockToken token,
                    u64 lock_time,
                    char* out,
                    int capacity) noexcept {
    constexpr char kMagic[] = "ACSLOCK/1\nOWNER:";
    constexpr char kToken[] = "\nTOKEN:";
    constexpr char kTime[] = "\nTIME:";
    int offset = 0;
    for (int i = 0; kMagic[i] != '\0'; ++i) out[offset++] = kMagic[i];
    for (int i = 0; i < owner_length; ++i) out[offset++] = owner[i];
    for (int i = 0; kToken[i] != '\0'; ++i) out[offset++] = kToken[i];
    U64ToHex16(token.high, out + offset);
    offset += 16;
    U64ToHex16(token.low, out + offset);
    offset += 16;
    for (int i = 0; kTime[i] != '\0'; ++i) out[offset++] = kTime[i];
    offset += U64ToDec(lock_time, out + offset, capacity - offset);
    out[offset++] = '\n';
    return offset;
}

bool ConsumeLiteral(const char* raw, int length, int& at,
                    const char* literal) noexcept {
    for (int i = 0; literal[i] != '\0'; ++i) {
        if (at >= length || raw[at] != literal[i]) return false;
        ++at;
    }
    return true;
}

bool ParseLockRecord(const char* raw, int length,
                     FParsedLocalLock& out_record) noexcept {
    if (raw == nullptr || length <= 0) return false;
    int at = 0;
    if (!ConsumeLiteral(raw, length, at, "ACSLOCK/1\nOWNER:")) return false;

    FParsedLocalLock parsed{};
    int owner_length = 0;
    while (at < length && raw[at] != '\n') {
        const unsigned char c = static_cast<unsigned char>(raw[at]);
        if (c == 0 || c < 0x20u || c == 0x7fu ||
            owner_length >= CLocalFileAssetLocking::kMaxUserChars - 1) {
            return false;
        }
        parsed.owner[owner_length++] = raw[at++];
    }
    if (owner_length == 0 || at >= length || raw[at++] != '\n') return false;
    parsed.owner[owner_length] = '\0';
    if (!IsStrictUtf8(parsed.owner, owner_length)) return false;

    if (!ConsumeLiteral(raw, length, at, "TOKEN:")) return false;
    for (int i = 0; i < 32; ++i) {
        if (at >= length) return false;
        const int digit = HexValue(raw[at++]);
        if (digit < 0) return false;
        u64& half = i < 16 ? parsed.token.high : parsed.token.low;
        half = (half << 4) | static_cast<u64>(digit);
    }
    if (!parsed.token.IsValid() || at >= length || raw[at++] != '\n') return false;
    if (!ConsumeLiteral(raw, length, at, "TIME:")) return false;

    int digits = 0;
    constexpr u64 kMaxU64 = ~static_cast<u64>(0);
    while (at < length && raw[at] >= '0' && raw[at] <= '9') {
        const u64 digit = static_cast<u64>(raw[at++] - '0');
        if (parsed.lock_time > (kMaxU64 - digit) / 10ull) return false;
        parsed.lock_time = parsed.lock_time * 10ull + digit;
        ++digits;
    }
    if (digits == 0 || at >= length || raw[at++] != '\n') return false;
    if (at != length) return false; // trailing byte/NUL/second record は拒否

    out_record = parsed;
    return true;
}

FLocalAssetLockResult ReadStrictLockRecord(const wchar_t* lock_path,
                                            DWORD desired_access,
                                            DWORD share_mode,
                                            HANDLE& out_handle,
                                            FParsedLocalLock& out_record) noexcept {
    out_handle = INVALID_HANDLE_VALUE;
    HANDLE handle = ::CreateFileW(lock_path, desired_access, share_mode, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return LocalLockError(ELocalAssetLockError::NotFound, error);
        }
        return LocalLockError(ELocalAssetLockError::OpenFailed, error);
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::SizeFailed, error);
    }
    if (size.QuadPart <= 0 ||
        size.QuadPart > CLocalFileAssetLocking::kMaxRecordBytes) {
        ::CloseHandle(handle);
        return LocalLockError(size.QuadPart > CLocalFileAssetLocking::kMaxRecordBytes
                                  ? ELocalAssetLockError::RecordTooLarge
                                  : ELocalAssetLockError::CorruptRecord);
    }

    char raw[CLocalFileAssetLocking::kMaxRecordBytes] = {};
    DWORD total = 0;
    const DWORD expected = static_cast<DWORD>(size.QuadPart);
    while (total < expected) {
        DWORD got = 0;
        if (!::ReadFile(handle, raw + total, expected - total, &got, nullptr)) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(handle);
            return LocalLockError(ELocalAssetLockError::ReadFailed, error);
        }
        if (got == 0) {
            ::CloseHandle(handle);
            return LocalLockError(ELocalAssetLockError::CorruptRecord,
                                  ERROR_HANDLE_EOF);
        }
        total += got;
    }
    char extra = 0;
    DWORD extra_count = 0;
    if (!::ReadFile(handle, &extra, 1, &extra_count, nullptr)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::ReadFailed, error);
    }
    if (extra_count != 0) {
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::CorruptRecord);
    }

    FParsedLocalLock parsed{};
    if (!ParseLockRecord(raw, static_cast<int>(total), parsed)) {
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::CorruptRecord);
    }
    out_record = parsed;
    out_handle = handle;
    FLocalAssetLockResult result{};
    result.token = parsed.token;
    result.lock_time = parsed.lock_time;
    return result;
}

bool MarkOpenFileForDelete(HANDLE handle, DWORD& out_error) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!::SetFileInformationByHandle(handle, FileDispositionInfo,
                                      &disposition, sizeof(disposition))) {
        out_error = ::GetLastError();
        return false;
    }
    out_error = 0;
    return true;
}

class FLocalLockStateGuard {
public:
    explicit FLocalLockStateGuard(volatile long& state) noexcept : m_State(state) {
        while (::InterlockedCompareExchange(&m_State, 1, 0) != 0) {
            ::SwitchToThread();
        }
    }
    ~FLocalLockStateGuard() noexcept { ::InterlockedExchange(&m_State, 0); }

    FLocalLockStateGuard(const FLocalLockStateGuard&) = delete;
    FLocalLockStateGuard& operator=(const FLocalLockStateGuard&) = delete;

private:
    volatile long& m_State;
};

u16 LegacyLockSubCode(ELocalAssetLockError error) noexcept {
    switch (error) {
    case ELocalAssetLockError::BadArgument:
    case ELocalAssetLockError::PathTooLong:
    case ELocalAssetLockError::OwnerTooLong:
    case ELocalAssetLockError::InvalidUtf8:
    case ELocalAssetLockError::InvalidOwner:
        return FStudioWorkflowError::kSub_BadArgument;
    case ELocalAssetLockError::AlreadyLocked:
        return FStudioWorkflowError::kSub_AlreadyLocked;
    case ELocalAssetLockError::NotFound:
        return FStudioWorkflowError::kSub_NotFound;
    default:
        return FStudioWorkflowError::kSub_PermissionDenied;
    }
}

} // namespace

const char* LocalAssetLockErrorName(ELocalAssetLockError error) noexcept {
    switch (error) {
    case ELocalAssetLockError::None:             return "None";
    case ELocalAssetLockError::BadArgument:      return "BadArgument";
    case ELocalAssetLockError::PathTooLong:      return "PathTooLong";
    case ELocalAssetLockError::OwnerTooLong:     return "OwnerTooLong";
    case ELocalAssetLockError::InvalidUtf8:      return "InvalidUtf8";
    case ELocalAssetLockError::InvalidOwner:     return "InvalidOwner";
    case ELocalAssetLockError::AlreadyLocked:    return "AlreadyLocked";
    case ELocalAssetLockError::NotFound:         return "NotFound";
    case ELocalAssetLockError::RecordTooLarge:   return "RecordTooLarge";
    case ELocalAssetLockError::CorruptRecord:    return "CorruptRecord";
    case ELocalAssetLockError::OpenFailed:       return "OpenFailed";
    case ELocalAssetLockError::SizeFailed:       return "SizeFailed";
    case ELocalAssetLockError::ReadFailed:       return "ReadFailed";
    case ELocalAssetLockError::WriteFailed:      return "WriteFailed";
    case ELocalAssetLockError::FlushFailed:      return "FlushFailed";
    case ELocalAssetLockError::CloseFailed:      return "CloseFailed";
    case ELocalAssetLockError::OwnerMismatch:    return "OwnerMismatch";
    case ELocalAssetLockError::TokenMismatch:    return "TokenMismatch";
    case ELocalAssetLockError::NotOwned:         return "NotOwned";
    case ELocalAssetLockError::DeleteFailed:     return "DeleteFailed";
    case ELocalAssetLockError::CapacityExceeded: return "CapacityExceeded";
    }
    return "Unknown";
}

FLocalAssetLockResult
CLocalFileAssetLocking::TryLockAsset(const char* asset_path,
                                     const char* user) noexcept {
    wchar_t lock_path[kMaxPathChars] = {};
    const ELocalAssetLockError path_error =
        MakeStrictLockPath(asset_path, lock_path, kMaxPathChars);
    if (path_error != ELocalAssetLockError::None) return LocalLockError(path_error);

    int owner_length = 0;
    const ELocalAssetLockError owner_error = ValidateOwner(user, owner_length);
    if (owner_error != ELocalAssetLockError::None) return LocalLockError(owner_error);

    FLocalLockStateGuard state_guard(m_StateGuard);
    int free_slot = -1;
    for (int i = 0; i < kMaxHeldLocks; ++i) {
        if (!m_HeldLocks[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) return LocalLockError(ELocalAssetLockError::CapacityExceeded);

    const FLocalAssetLockToken token = GenerateLockToken();
    const u64 lock_time = NowUnixSeconds();
    char record[kMaxRecordBytes] = {};
    const int record_length =
        BuildLockRecord(user, owner_length, token, lock_time, record, kMaxRecordBytes);

    HANDLE handle = ::CreateFileW(lock_path, GENERIC_WRITE | DELETE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return LocalLockError(ELocalAssetLockError::AlreadyLocked, error);
        }
        return LocalLockError(ELocalAssetLockError::OpenFailed, error);
    }

    DWORD total = 0;
    while (total < static_cast<DWORD>(record_length)) {
        DWORD wrote = 0;
        const BOOL write_ok =
            ::WriteFile(handle, record + total,
                        static_cast<DWORD>(record_length) - total, &wrote, nullptr);
        if (!write_ok ||
            wrote == 0) {
            const DWORD error = write_ok ? ERROR_WRITE_FAULT : ::GetLastError();
            DWORD ignored = 0;
            MarkOpenFileForDelete(handle, ignored);
            ::CloseHandle(handle);
            return LocalLockError(ELocalAssetLockError::WriteFailed, error);
        }
        total += wrote;
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD error = ::GetLastError();
        DWORD ignored = 0;
        MarkOpenFileForDelete(handle, ignored);
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::FlushFailed, error);
    }
    if (!::CloseHandle(handle)) {
        FLocalAssetLockResult result =
            LocalLockError(ELocalAssetLockError::CloseFailed, ::GetLastError());
        result.token = token; // close 成否が不明なため、呼び出し側が検査付き回復を試せるよう残す
        result.lock_time = lock_time;
        return result;
    }

    FHeldLock& held = m_HeldLocks[free_slot];
    CopyCStr(held.path, kMaxPathChars, asset_path);
    CopyCStr(held.owner, kMaxUserChars, user);
    held.token = token;
    held.in_use = true;

    FLocalAssetLockResult result{};
    result.token = token;
    result.lock_time = lock_time;
    return result;
}

FLocalAssetLockResult
CLocalFileAssetLocking::TryUnlockAsset(const char* asset_path,
                                       const char* user,
                                       FLocalAssetLockToken token) noexcept {
    wchar_t lock_path[kMaxPathChars] = {};
    const ELocalAssetLockError path_error =
        MakeStrictLockPath(asset_path, lock_path, kMaxPathChars);
    if (path_error != ELocalAssetLockError::None) return LocalLockError(path_error);
    int owner_length = 0;
    const ELocalAssetLockError owner_error = ValidateOwner(user, owner_length);
    if (owner_error != ELocalAssetLockError::None) return LocalLockError(owner_error);
    (void)owner_length;
    if (!token.IsValid()) return LocalLockError(ELocalAssetLockError::BadArgument);

    HANDLE handle = INVALID_HANDLE_VALUE;
    FParsedLocalLock parsed{};
    FLocalAssetLockResult read =
        ReadStrictLockRecord(lock_path, GENERIC_READ | DELETE, FILE_SHARE_READ,
                             handle, parsed);
    if (!read.Succeeded()) return read;

    if (!CStrEqual(parsed.owner, user)) {
        const DWORD close_error = ::CloseHandle(handle) ? 0 : ::GetLastError();
        return close_error == 0
            ? LocalLockError(ELocalAssetLockError::OwnerMismatch)
            : LocalLockError(ELocalAssetLockError::CloseFailed, close_error);
    }
    if (!TokensEqual(parsed.token, token)) {
        const DWORD close_error = ::CloseHandle(handle) ? 0 : ::GetLastError();
        return close_error == 0
            ? LocalLockError(ELocalAssetLockError::TokenMismatch)
            : LocalLockError(ELocalAssetLockError::CloseFailed, close_error);
    }

    DWORD delete_error = 0;
    if (!MarkOpenFileForDelete(handle, delete_error)) {
        ::CloseHandle(handle);
        return LocalLockError(ELocalAssetLockError::DeleteFailed, delete_error);
    }
    if (!::CloseHandle(handle)) {
        return LocalLockError(ELocalAssetLockError::CloseFailed, ::GetLastError());
    }

    {
        FLocalLockStateGuard state_guard(m_StateGuard);
        for (int i = 0; i < kMaxHeldLocks; ++i) {
            FHeldLock& held = m_HeldLocks[i];
            if (held.in_use && CStrEqual(held.path, asset_path) &&
                CStrEqual(held.owner, user) && TokensEqual(held.token, token)) {
                held = FHeldLock{};
                break;
            }
        }
    }
    FLocalAssetLockResult result{};
    result.token = token;
    result.lock_time = parsed.lock_time;
    return result;
}

FLocalAssetLockResult
CLocalFileAssetLocking::TryQueryLock(const char* asset_path,
                                     FAssetLockInfo& out_info) noexcept {
    wchar_t lock_path[kMaxPathChars] = {};
    const ELocalAssetLockError path_error =
        MakeStrictLockPath(asset_path, lock_path, kMaxPathChars);
    if (path_error != ELocalAssetLockError::None) return LocalLockError(path_error);

    HANDLE handle = INVALID_HANDLE_VALUE;
    FParsedLocalLock parsed{};
    FLocalAssetLockResult result =
        ReadStrictLockRecord(lock_path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, handle, parsed);
    if (!result.Succeeded()) return result;
    if (!::CloseHandle(handle)) {
        return LocalLockError(ELocalAssetLockError::CloseFailed, ::GetLastError());
    }

    FLocalLockStateGuard state_guard(m_StateGuard);
    CopyCStr(m_QueryPathBuf, kMaxPathChars, asset_path);
    CopyCStr(m_QueryUserBuf, kMaxUserChars, parsed.owner);
    FAssetLockInfo committed{};
    committed.asset_path = m_QueryPathBuf;
    committed.locker_user = m_QueryUserBuf;
    committed.lock_time = parsed.lock_time;
    out_info = committed;
    result.token = parsed.token;
    result.lock_time = parsed.lock_time;
    return result;
}

TResult<void> CLocalFileAssetLocking::LockAsset(const char* asset_path,
                                                const char* user) noexcept {
    const FLocalAssetLockResult result = TryLockAsset(asset_path, user);
    if (result.Succeeded()) return Ok();
    return ACS_ERR_OS(IO, LegacyLockSubCode(result.error),
                      "CLocalFileAssetLocking::LockAsset failed",
                      result.os_error);
}

TResult<void> CLocalFileAssetLocking::UnlockAsset(const char* asset_path) noexcept {
    wchar_t validated_path[kMaxPathChars] = {};
    const ELocalAssetLockError validation =
        MakeStrictLockPath(asset_path, validated_path, kMaxPathChars);
    if (validation != ELocalAssetLockError::None) {
        return ACS_ERR_OS(IO, LegacyLockSubCode(validation),
                          "CLocalFileAssetLocking::UnlockAsset: invalid path", 0);
    }
    char owner[kMaxUserChars] = {};
    FLocalAssetLockToken token{};
    {
        FLocalLockStateGuard state_guard(m_StateGuard);
        for (int i = 0; i < kMaxHeldLocks; ++i) {
            const FHeldLock& held = m_HeldLocks[i];
            if (held.in_use && CStrEqual(held.path, asset_path)) {
                CopyCStr(owner, kMaxUserChars, held.owner);
                token = held.token;
                break;
            }
        }
    }
    if (!token.IsValid()) {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_PermissionDenied,
                       "CLocalFileAssetLocking::UnlockAsset: this instance does not own lock");
    }
    const FLocalAssetLockResult result = TryUnlockAsset(asset_path, owner, token);
    if (result.Succeeded()) return Ok();
    return ACS_ERR_OS(IO, LegacyLockSubCode(result.error),
                      "CLocalFileAssetLocking::UnlockAsset failed",
                      result.os_error);
}

TResult<void> CLocalFileAssetLocking::UnlockAssetAs(const char* asset_path,
                                                    const char* user) noexcept {
    wchar_t validated_path[kMaxPathChars] = {};
    const ELocalAssetLockError path_validation =
        MakeStrictLockPath(asset_path, validated_path, kMaxPathChars);
    int owner_length = 0;
    const ELocalAssetLockError owner_validation = ValidateOwner(user, owner_length);
    (void)owner_length;
    if (path_validation != ELocalAssetLockError::None ||
        owner_validation != ELocalAssetLockError::None) {
        const ELocalAssetLockError validation =
            path_validation != ELocalAssetLockError::None
                ? path_validation : owner_validation;
        return ACS_ERR_OS(IO, LegacyLockSubCode(validation),
                          "CLocalFileAssetLocking::UnlockAssetAs: invalid argument", 0);
    }
    FLocalAssetLockToken token{};
    {
        FLocalLockStateGuard state_guard(m_StateGuard);
        for (int i = 0; i < kMaxHeldLocks; ++i) {
            const FHeldLock& held = m_HeldLocks[i];
            if (held.in_use && CStrEqual(held.path, asset_path) &&
                CStrEqual(held.owner, user)) {
                token = held.token;
                break;
            }
        }
    }
    if (!token.IsValid()) {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_PermissionDenied,
                       "CLocalFileAssetLocking::UnlockAssetAs: owner/token not held");
    }
    const FLocalAssetLockResult result = TryUnlockAsset(asset_path, user, token);
    if (result.Succeeded()) return Ok();
    return ACS_ERR_OS(IO, LegacyLockSubCode(result.error),
                      "CLocalFileAssetLocking::UnlockAssetAs failed",
                      result.os_error);
}

TResult<FAssetLockInfo>
CLocalFileAssetLocking::QueryLock(const char* asset_path) noexcept {
    FAssetLockInfo info{};
    const FLocalAssetLockResult result = TryQueryLock(asset_path, info);
    if (result.Succeeded()) return TResult<FAssetLockInfo>(OkInit, info);
    return TResult<FAssetLockInfo>(
        ACS_ERR_OS(IO, LegacyLockSubCode(result.error),
                   "CLocalFileAssetLocking::QueryLock failed",
                   result.os_error));
}

/** 追跡中のプロセス HANDLE をすべて閉じて破棄する (プロセス自体は kill しない)。 */
CLocalBuildRunner::~CLocalBuildRunner() noexcept {
    // 追跡中のプロセス HANDLE をすべて閉じる (プロセス自体は kill しない)。
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId != 0) {
            CloseJob(m_Jobs[i]);
        }
    }
}

/** build_id でジョブを引く (無ければ nullptr)。 */
CLocalBuildRunner::FJob* CLocalBuildRunner::FindJob(u64 build_id) noexcept {
    if (build_id == 0) return nullptr;
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId == build_id) return &m_Jobs[i];
    }
    return nullptr;
}

/** プロセス HANDLE を閉じてスロットを空きに戻す。 */
void CLocalBuildRunner::CloseJob(FJob& job) noexcept {
    if (job.m_Process != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(job.m_Process));
        job.m_Process = nullptr;
    }
    job.m_BuildId = 0;
    job.m_Finished = false;
    job.m_Success = false;
    job.m_ExitCode = 0;
    job.m_Artifact[0] = '\0';
}

/** command_line を CreateProcessW で起動し、完了まで待って終了コードを回収する。 */
TResult<void> CLocalBuildRunner::RunBuild(const wchar_t* command_line,
                                          u32&           out_exit_code,
                                          u32            timeout_ms) noexcept {
    out_exit_code = 0;
    if (command_line == nullptr || command_line[0] == L'\0') {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                       "CLocalBuildRunner::RunBuild: command_line is null/empty");
    }

    // CreateProcessW は lpCommandLine を書き換える可能性があるため可変バッファへ複写。
    wchar_t cmd[kMaxCmdChars];
    {
        int n = 0;
        for (; command_line[n] != L'\0' && n < kMaxCmdChars - 1; ++n) {
            cmd[n] = command_line[n];
        }
        if (command_line[n] != L'\0') {
            return ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                           "CLocalBuildRunner::RunBuild: command_line too long");
        }
        cmd[n] = L'\0';
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, FStudioWorkflowError::kSub_NotFound,
                          "CLocalBuildRunner::RunBuild: CreateProcessW failed", err);
    }

    const DWORD wait_ms = (timeout_ms == 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, wait_ms);
    if (wr == WAIT_TIMEOUT) {
        // タイムアウト時は kill して HANDLE を閉じる。
        ::TerminateProcess(pi.hProcess, 1u);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return ACS_ERR(IO, FStudioWorkflowError::kSub_PermissionDenied,
                       "CLocalBuildRunner::RunBuild: build timed out");
    }
    if (wr != WAIT_OBJECT_0) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return ACS_ERR_OS(IO, FStudioWorkflowError::kSub_PermissionDenied,
                          "CLocalBuildRunner::RunBuild: WaitForSingleObject failed", err);
    }

    DWORD code = 0;
    const BOOL gok = ::GetExitCodeProcess(pi.hProcess, &code);
    const DWORD gerr = gok ? 0u : ::GetLastError();
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (!gok) {
        return ACS_ERR_OS(IO, FStudioWorkflowError::kSub_PermissionDenied,
                          "CLocalBuildRunner::RunBuild: GetExitCodeProcess failed", gerr);
    }

    out_exit_code = static_cast<u32>(code);
    return Ok();
}

/** command_line を UTF-8 で受け、UTF-16 へ変換して RunBuild に委譲する。 */
TResult<void> CLocalBuildRunner::RunBuildUtf8(const char* command_line,
                                              u32&        out_exit_code,
                                              u32         timeout_ms) noexcept {
    out_exit_code = 0;
    wchar_t cmd_w[kMaxCmdChars];
    if (!Utf8ToUtf16(command_line, cmd_w, kMaxCmdChars)) {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                       "CLocalBuildRunner::RunBuildUtf8: command_line null / too long / bad UTF-8");
    }
    return RunBuild(cmd_w, out_exit_code, timeout_ms);
}

/** preset を起動コマンドラインとして解釈し、非同期にビルドジョブを起動する。 */
TResult<u64> CLocalBuildRunner::SubmitBuild(const FBuildRequest& req) noexcept {
    // preset を「起動するコマンドライン」として解釈する (ローカルファーム規約)。
    if (req.preset == nullptr || req.preset[0] == '\0') {
        return TResult<u64>(
            ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                    "CLocalBuildRunner::SubmitBuild: req.preset (command line) is null/empty"));
    }

    // 空きスロットを探す。
    FJob* slot = nullptr;
    for (int i = 0; i < kMaxJobs; ++i) {
        if (m_Jobs[i].m_BuildId == 0) { slot = &m_Jobs[i]; break; }
    }
    if (slot == nullptr) {
        return TResult<u64>(
            ACS_ERR(IO, FStudioWorkflowError::kSub_PermissionDenied,
                    "CLocalBuildRunner::SubmitBuild: job table full"));
    }

    wchar_t cmd[kMaxCmdChars];
    if (!Utf8ToUtf16(req.preset, cmd, kMaxCmdChars)) {
        return TResult<u64>(
            ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                    "CLocalBuildRunner::SubmitBuild: preset too long / bad UTF-8"));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        return TResult<u64>(
            ACS_ERR_OS(IO, FStudioWorkflowError::kSub_NotFound,
                       "CLocalBuildRunner::SubmitBuild: CreateProcessW failed", err));
    }
    ::CloseHandle(pi.hThread);  // メインスレッド HANDLE は不要。

    const u64 id = m_NextBuildId++;
    slot->m_BuildId  = id;
    slot->m_Process  = static_cast<void*>(pi.hProcess);
    slot->m_Finished = false;
    slot->m_Success  = false;
    slot->m_ExitCode = 0;
    // 疑似 artifact パス: branch/commit を含めて識別子化 (情報のみ)。
    {
        int off = 0;
        const char prefix[] = "local-build://job/";
        for (int i = 0; prefix[i] != '\0' && off < kMaxArtifactLen - 1; ++i) {
            slot->m_Artifact[off++] = prefix[i];
        }
        off += U64ToDec(id, slot->m_Artifact + off, kMaxArtifactLen - off);
        slot->m_Artifact[off] = '\0';
    }
    return TResult<u64>(OkInit, id);
}

/** ジョブの完了状態を非ブロッキングに確認し、結果を返す。 */
TResult<IBuildFarmBackend::FBuildResult>
CLocalBuildRunner::PollBuild(u64 build_id) noexcept {
    if (build_id == 0) {
        return TResult<FBuildResult>(
            ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                    "CLocalBuildRunner::PollBuild: build_id == 0 is reserved/invalid"));
    }
    FJob* job = FindJob(build_id);
    if (job == nullptr) {
        return TResult<FBuildResult>(
            ACS_ERR(IO, FStudioWorkflowError::kSub_NotFound,
                    "CLocalBuildRunner::PollBuild: unknown build_id"));
    }

    // 未だラッチしていなければプロセス状態を非ブロッキングに確認する。
    if (!job->m_Finished) {
        HANDLE h = static_cast<HANDLE>(job->m_Process);
        const DWORD wr = ::WaitForSingleObject(h, 0);  // 0 = poll
        if (wr == WAIT_OBJECT_0) {
            DWORD code = 0;
            const BOOL got_code = ::GetExitCodeProcess(h, &code);
            const DWORD code_error = got_code ? 0u : ::GetLastError();

            // 終了済みプロセスの exit code 取得を試みた後、不要な HANDLE を閉じる。
            ::CloseHandle(h);
            job->m_Process = nullptr;
            if (got_code) {
                job->m_ExitCode = static_cast<u32>(code);
                job->m_Success  = (code == 0);
            }
            job->m_Finished = true;
            if (!got_code) {
                return TResult<FBuildResult>(ACS_ERR_OS(IO, FStudioWorkflowError::kSub_PermissionDenied,
                                                       "CLocalBuildRunner::PollBuild: GetExitCodeProcess failed",
                                                       code_error));
            }
        } else if (wr == WAIT_FAILED) {
            const DWORD wait_error = ::GetLastError();
            // 壊れた HANDLE を永続追跡しない。スロットには失敗結果をラッチし、
            // OS 資源だけはこの呼び出しで確実に手放す。
            ::CloseHandle(h);
            job->m_Process = nullptr;
            job->m_Finished = true;
            job->m_Success = false;
            return TResult<FBuildResult>(ACS_ERR_OS(IO, FStudioWorkflowError::kSub_PermissionDenied,
                                                   "CLocalBuildRunner::PollBuild: WaitForSingleObject failed",
                                                   wait_error));
        }
        // WAIT_TIMEOUT = まだ実行中。
    }

    FBuildResult r{};
    r.build_id     = build_id;
    r.success      = job->m_Finished && job->m_Success;
    r.artifact_url = (job->m_Finished && job->m_Success) ? job->m_Artifact : nullptr;

    if (!job->m_Finished) {
        // 進行中は IsErr で返す (ヘッダが許容するポリシー)。Generic + NotFound 以外。
        return TResult<FBuildResult>(
            ACS_ERR(Generic, FStudioWorkflowError::kSub_PermissionDenied,
                    "CLocalBuildRunner::PollBuild: build still running"));
    }
    return TResult<FBuildResult>(OkInit, r);
}

/** 進行中のジョブを kill してスロットを解放する。 */
TResult<void> CLocalBuildRunner::CancelBuild(u64 build_id) noexcept {
    if (build_id == 0) {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_BadArgument,
                       "CLocalBuildRunner::CancelBuild: build_id == 0 is reserved/invalid");
    }
    FJob* job = FindJob(build_id);
    if (job == nullptr) {
        return ACS_ERR(IO, FStudioWorkflowError::kSub_NotFound,
                       "CLocalBuildRunner::CancelBuild: unknown build_id");
    }
    if (job->m_Finished) {
        // 既に完了済みのジョブはキャンセル不可 (NotFound 扱い)。
        return ACS_ERR(IO, FStudioWorkflowError::kSub_NotFound,
                       "CLocalBuildRunner::CancelBuild: build already finished");
    }

    HANDLE h = static_cast<HANDLE>(job->m_Process);
    if (h != nullptr) {
        // まだ実行中なら kill。
        const DWORD wr = ::WaitForSingleObject(h, 0);
        if (wr == WAIT_TIMEOUT) {
            ::TerminateProcess(h, 1u);
        }
    }
    CloseJob(*job);
    return Ok();
}

/** function-local static で process-wide singleton の stub locking backend を返す。 */
IAssetLockingBackend& GetAssetLockingStub() noexcept {
    static CAssetLockingStub s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の stub build farm backend を返す。 */
IBuildFarmBackend& GetBuildFarmStub() noexcept {
    static CBuildFarmStub s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の実ローカル locking backend を返す。 */
CLocalFileAssetLocking& GetLocalFileAssetLocking() noexcept {
    static CLocalFileAssetLocking s_instance;
    return s_instance;
}

/** function-local static で process-wide singleton の実ローカル build runner を返す。 */
CLocalBuildRunner& GetLocalBuildRunner() noexcept {
    static CLocalBuildRunner s_instance;
    return s_instance;
}

} // namespace acs::game
