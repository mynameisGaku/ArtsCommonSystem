// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FSettings 実装
//
// 線形検索 + 同 key 上書き。STL 禁止方針に従い、<cstring> も避けて
// per-byte 比較ループを自前で書く (FEntitlement.cpp と同じ StrEq pattern)。
//
// Save / Load は INI 風 `<tag>:<key>=<value>\n` テキスト (UTF-8 / LF) を読み書き
// する。型は 1 文字 prefix (f/i/b/s) でディスクに残し round-trip させる。書き込みは
// FSaveArchive と同じ atomic write (`.tmp` に書いて MoveFileExW で rename) を使う。
#include "gameframework/Settings.h"

#include "foundation/Platform.h"   // <windows.h> (CreateFileW / WriteFile / MoveFileExW)
#include "memory/Memory.h"         // DefaultAllocator / MemCopy

namespace acs::game {

namespace {

// const char* の per-byte 比較。nullptr 安全。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// const char* の長さ (NUL 終端まで)。nullptr は 0。
usize StrLen(const char* s) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

// f32 を 1 文字 buffer に round-trip 可能な精度で書く。STL (snprintf) を避け、
// 自前で「整数部.小数部」を出力する。±0 / 大きい値も扱えるよう、まず整数部を
// 10 進展開し、小数部は固定 9 桁まで出して末尾の 0 を畳む (1.5 → "1.5")。
// FString に追記する。
void AppendF32(FString& out, f32 v) noexcept {
    // NaN / Inf は設定値として現実的でないので、安全側で 0 として書く
    // (Load 側でも 0 に parse される)。
    if (!(v == v) /* NaN */ || v > 3.0e38f || v < -3.0e38f) {
        out.Append(FStringView("0", 1));
        return;
    }
    if (v < 0.0f) {
        out.Append('-');
        v = -v;
    }
    // u64 に収まらない巨大値 (v >= 2^64) を static_cast<u64> すると範囲外変換で
    // 未定義動作になる。f32 は 2^24 超で整数値しか持たない (小数部は 0) ため、
    // double 空間で上位桁から 10 進展開して桁を直接出力する (u64 オーバーフロー回避)。
    const f32 kU64Max = 18446744073709551616.0f; // 2^64 (f32 で厳密表現可)
    if (v >= kU64Max) {
        double dv = static_cast<double>(v);
        double p  = 1.0;
        while (p * 10.0 <= dv) p *= 10.0;   // p = 10^k (<= dv の最大の 10 冪)
        while (p >= 1.0) {
            int digit = static_cast<int>(dv / p);
            if (digit < 0) digit = 0;
            if (digit > 9) digit = 9;        // 丸め誤差で 10 になる場合の保険
            out.Append(static_cast<char>('0' + digit));
            dv -= static_cast<double>(digit) * p;
            p  /= 10.0;
        }
        return;
    }

    // 整数部
    u64 int_part = static_cast<u64>(v);
    f32 frac     = v - static_cast<f32>(int_part);

    char digits[20];
    int  n = 0;
    if (int_part == 0) {
        digits[n++] = '0';
    } else {
        char tmp[20];
        int  t = 0;
        while (int_part > 0 && t < 20) {
            tmp[t++] = static_cast<char>('0' + static_cast<int>(int_part % 10));
            int_part /= 10;
        }
        while (t > 0) digits[n++] = tmp[--t];  // 逆順に積んだので戻す
    }
    out.Append(FStringView(digits, static_cast<usize>(n)));

    // 小数部 (固定 6 桁 → 末尾 0 を畳む)。f32 の有効精度に合わせて 6 桁で十分。
    char frac_buf[8];
    int  fn = 0;
    frac_buf[fn++] = '.';
    int last_nonzero = 0;  // '.' の index
    for (int d = 0; d < 6; ++d) {
        frac *= 10.0f;
        int digit = static_cast<int>(frac);
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        frac -= static_cast<f32>(digit);
        frac_buf[fn] = static_cast<char>('0' + digit);
        if (digit != 0) last_nonzero = fn;
        ++fn;
    }
    // last_nonzero == 0 (= 小数部すべて 0) なら小数点ごと省く ("1" のまま)。
    if (last_nonzero > 0) {
        out.Append(FStringView(frac_buf, static_cast<usize>(last_nonzero + 1)));
    }
}

// i32 を 10 進文字列として FString に追記する。
void AppendI32(FString& out, i32 v) noexcept {
    u32 mag;
    if (v < 0) {
        out.Append('-');
        // INT_MIN を含めて安全に絶対値化 (符号なしで受ける)。
        mag = static_cast<u32>(0u - static_cast<u32>(v));
    } else {
        mag = static_cast<u32>(v);
    }
    char tmp[12];
    int  t = 0;
    if (mag == 0) {
        tmp[t++] = '0';
    } else {
        while (mag > 0 && t < 12) {
            tmp[t++] = static_cast<char>('0' + static_cast<int>(mag % 10u));
            mag /= 10u;
        }
    }
    char digits[12];
    int  n = 0;
    while (t > 0) digits[n++] = tmp[--t];
    out.Append(FStringView(digits, static_cast<usize>(n)));
}

// [begin, end) を f32 に parse。"<int>.<frac>" / 先頭符号を許容。
f32 ParseF32(const char* begin, const char* end) noexcept {
    const char* p   = begin;
    f32         sign = 1.0f;
    if (p < end && (*p == '+' || *p == '-')) {
        if (*p == '-') sign = -1.0f;
        ++p;
    }
    f32 int_part = 0.0f;
    while (p < end && *p >= '0' && *p <= '9') {
        int_part = int_part * 10.0f + static_cast<f32>(*p - '0');
        ++p;
    }
    f32 frac  = 0.0f;
    f32 scale = 1.0f;
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9') {
            scale *= 0.1f;
            frac += static_cast<f32>(*p - '0') * scale;
            ++p;
        }
    }
    return sign * (int_part + frac);
}

// [begin, end) を i32 に parse。先頭符号を許容、オーバーフローは飽和しない
// (設定値が i32 範囲を超える運用は想定外)。
i32 ParseI32(const char* begin, const char* end) noexcept {
    const char* p    = begin;
    bool        neg  = false;
    if (p < end && (*p == '+' || *p == '-')) {
        neg = (*p == '-');
        ++p;
    }
    i64 acc = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        acc = acc * 10 + static_cast<i64>(*p - '0');
        ++p;
    }
    if (neg) acc = -acc;
    return static_cast<i32>(acc);
}

} // namespace

isize FSettings::FindIndex(const char* key) const noexcept {
    if (key == nullptr) return -1;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Entries[i].key, key)) return static_cast<isize>(i);
    }
    return -1;
}

FSettings::Entry& FSettings::UpsertEntry(const char* key) noexcept {
    // key == nullptr は呼び出し側でガードされている前提だが、二重防御で
    // 末尾を返す代わりに sentinel を立てる必要はない (Set* 側で弾く)。
    const isize idx = FindIndex(key);
    if (idx >= 0) {
        return m_Entries[static_cast<usize>(idx)];
    }
    Entry e;
    e.key  = key;
    e.kind = ESettingKind::None;
    m_Entries.PushBack(e);
    return m_Entries[m_Entries.Size() - 1];
}

// ============================================================================
// 書き込み
// ============================================================================
void FSettings::SetF32(const char* key, f32 v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = ESettingKind::F32;
    e.value.f  = v;
}

void FSettings::SetI32(const char* key, i32 v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = ESettingKind::I32;
    e.value.i  = v;
}

void FSettings::SetBool(const char* key, bool v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = ESettingKind::Bool;
    e.value.b  = v;
}

void FSettings::SetString(const char* key, const char* v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = ESettingKind::FString;
    e.value.s  = v;  // 非所有: 呼び出し側が寿命を保証する
}

// ============================================================================
// 読み出し (型不一致 / 未検出は default_value)
// ============================================================================
f32 FSettings::GetF32(const char* key, f32 default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = m_Entries[static_cast<usize>(idx)];
    if (e.kind != ESettingKind::F32) return default_value;
    return e.value.f;
}

i32 FSettings::GetI32(const char* key, i32 default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = m_Entries[static_cast<usize>(idx)];
    if (e.kind != ESettingKind::I32) return default_value;
    return e.value.i;
}

bool FSettings::GetBool(const char* key, bool default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = m_Entries[static_cast<usize>(idx)];
    if (e.kind != ESettingKind::Bool) return default_value;
    return e.value.b;
}

const char* FSettings::GetString(const char* key, const char* default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = m_Entries[static_cast<usize>(idx)];
    if (e.kind != ESettingKind::FString) return default_value;
    return e.value.s;
}

// ============================================================================
// メタ操作
// ============================================================================
bool FSettings::Has(const char* key) const noexcept {
    return FindIndex(key) >= 0;
}

void FSettings::Remove(const char* key) noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return;
    // 順序は保持しなくてよい (key 検索は全件走査するため、index は不変条件にない)。
    m_Entries.RemoveAtSwap(static_cast<usize>(idx));
}

void FSettings::Clear() noexcept {
    m_Entries.Clear();
}

u32 FSettings::Count() const noexcept {
    // 件数は通常 u32 範囲を超えない (UI 設定で数百個が現実的上限)。
    return static_cast<u32>(m_Entries.Size());
}

// ============================================================================
// 永続化
// ============================================================================
// サブコード: FSaveArchive と被らない独自値域 (10..) を割り当てる。
namespace {
constexpr u16 kSubIo        = 10;  // CreateFileW / WriteFile / MoveFileExW 失敗
constexpr u16 kSubFileTooLarge = 11;  // Load 対象が sanity 上限超過
constexpr u16 kSubNullPath   = 12;  // file_path == nullptr
constexpr u16 kSubOom        = 13;  // 読み込みバッファ確保失敗

// Load が読み込む settings ファイルの sanity 上限 (16 MiB)。設定ファイルが
// これを超えるのは破損か別フォーマットの誤指定なので弾く。
constexpr u64 kMaxSettingsBytes = 16ull * 1024ull * 1024ull;

// `<path>` の末尾に L".tmp" を付けた一時パスを out_buf に作る。
// out_cap は out_buf の要素数。成功で true、長さ超過で false。
bool MakeTmpPath(const wchar_t* path, wchar_t* out_buf, usize out_cap) noexcept {
    usize n = 0;
    while (path[n] != L'\0') ++n;
    const wchar_t  suffix[]  = L".tmp";
    const usize    suffix_n  = 4;
    if (n + suffix_n + 1 > out_cap) return false;  // +1 = NUL
    for (usize i = 0; i < n; ++i) out_buf[i] = path[i];
    for (usize i = 0; i < suffix_n; ++i) out_buf[n + i] = suffix[i];
    out_buf[n + suffix_n] = L'\0';
    return true;
}
} // namespace

// ----------------------------------------------------------------------------
// Save — 全 entry を `<tag>:<key>=<value>\n` で直列化し atomic write する。
//   1. FString に全行を組み立てる (UTF-8 / LF)。
//   2. `<path>.tmp` に CREATE_ALWAYS で書き、FlushFileBuffers で flush。
//   3. MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) で本パスへ rename。
// 途中で失敗しても本ファイルは旧内容のまま (atomicity)。
// ----------------------------------------------------------------------------
TResult<void> FSettings::Save(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSubNullPath, "FSettings::Save: file_path is null");
    }

    // ---- テキスト組み立て ----------------------------------------------
    FString text;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        const Entry& e = m_Entries[i];
        if (e.key == nullptr || e.kind == ESettingKind::None) continue;  // 無効 entry は skip

        switch (e.kind) {
            case ESettingKind::F32:     text.Append(FStringView("f:", 2)); break;
            case ESettingKind::I32:     text.Append(FStringView("i:", 2)); break;
            case ESettingKind::Bool:    text.Append(FStringView("b:", 2)); break;
            case ESettingKind::FString: text.Append(FStringView("s:", 2)); break;
            default: continue;
        }
        text.Append(FStringView(e.key, StrLen(e.key)));
        text.Append('=');
        switch (e.kind) {
            case ESettingKind::F32:  AppendF32(text, e.value.f); break;
            case ESettingKind::I32:  AppendI32(text, e.value.i); break;
            case ESettingKind::Bool:
                text.Append(e.value.b ? FStringView("true", 4) : FStringView("false", 5));
                break;
            case ESettingKind::FString:
                if (e.value.s != nullptr) {
                    text.Append(FStringView(e.value.s, StrLen(e.value.s)));
                }
                break;
            default: break;
        }
        text.Append('\n');
    }

    // ---- `<path>.tmp` を組む -------------------------------------------
    wchar_t tmp_path[1024];
    if (!MakeTmpPath(file_path, tmp_path, 1024)) {
        return ACS_ERR(IO, kSubIo, "FSettings::Save: file path too long for .tmp suffix");
    }

    // ---- tmp に書き込む (CREATE_ALWAYS = 既存 truncate) ----------------
    HANDLE h = ::CreateFileW(tmp_path,
                             GENERIC_WRITE,
                             0,                 // 排他: 書き込み中は他者に開かせない
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, kSubIo, "FSettings::Save: CreateFileW (.tmp) failed", err);
    }

    const char* bytes = text.Data();
    u64         remaining = static_cast<u64>(text.Size());
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!::WriteFile(h, bytes, chunk, &wrote, nullptr) || wrote != chunk) {
            const DWORD err = ::GetLastError();
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, kSubIo, "FSettings::Save: WriteFile (.tmp) failed", err);
        }
        bytes     += wrote;
        remaining -= wrote;
    }

    // ディスクまで flush してから rename することで、rename 後の本ファイルが
    // 必ず完全なデータを指すことを保証する (atomic write の肝)。
    ::FlushFileBuffers(h);
    if (!::CloseHandle(h)) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, kSubIo, "FSettings::Save: CloseHandle (.tmp) failed", err);
    }

    // ---- atomic rename: tmp → 本パス -----------------------------------
    if (!::MoveFileExW(tmp_path, file_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(tmp_path);  // 失敗した tmp は残さない (best-effort)
        return ACS_ERR_OS(IO, kSubIo, "FSettings::Save: MoveFileExW (rename) failed", err);
    }
    return Ok();
}

// ----------------------------------------------------------------------------
// Load — ファイルを全読みし、各行を parse して in-memory store に復元する。
//   行形式: `<tag>:<key>=<value>` (tag = f/i/b/s)。`#` 始まりと空行は skip。
//   未知 tag / `=` 無しの行は skip (前方互換)。
//   復元した key / string 値は m_StringPool が所有する (ファイル由来の文字列
//   寿命をストアより長く保つため複製する)。行数 ×2 を先に Reserve し、TArray
//   再確保による FString 移動 = Data() ポインタ無効化を防ぐ。
// ----------------------------------------------------------------------------
TResult<void> FSettings::Load(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSubNullPath, "FSettings::Load: file_path is null");
    }

    // ---- ファイルを開く (FSaveArchive.cpp と同じ流儀) ------------------
    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_READ,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // ファイル不在は IO カテゴリで返す (初回起動を呼び出し側が区別できる)。
        return ACS_ERR_OS(IO, kSubIo, "FSettings::Load: CreateFileW failed", err);
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(h, &size_li)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSubIo, "FSettings::Load: GetFileSizeEx failed", err);
    }
    const u64 size_u64 = static_cast<u64>(size_li.QuadPart);
    if (size_u64 == 0) {
        // 空ファイル: 復元なしで成功 (保存した設定が空だったケース)。
        ::CloseHandle(h);
        Clear();
        m_StringPool.Clear();
        return Ok();
    }
    if (size_u64 > kMaxSettingsBytes) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSubFileTooLarge,
                       "FSettings::Load: settings file exceeds 16 MiB sanity limit");
    }

    // ---- 全読み込み (+1 で末尾に番兵 NUL を置けるようにする) -----------
    const usize buf_size = static_cast<usize>(size_u64);
    FAllocator& alloc    = DefaultAllocator();
    void*       raw      = alloc.Alloc(buf_size + 1, alignof(char), FSourceLoc::Current());
    if (raw == nullptr) {
        ::CloseHandle(h);
        return ACS_ERR(Memory, kSubOom, "FSettings::Load: failed to allocate read buffer");
    }
    char* buf = static_cast<char*>(raw);

    char* p         = buf;
    u64   remaining = size_u64;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            DWORD err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            alloc.Free(raw);
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, kSubIo, "FSettings::Load: ReadFile failed", err);
        }
        p         += got;
        remaining -= got;
    }
    ::CloseHandle(h);
    buf[buf_size] = '\0';  // 番兵

    // ---- 既存値を捨ててから復元 (Load は「ファイル状態に置き換える」意味) ---
    Clear();
    m_StringPool.Clear();

    // 行数を数えて m_StringPool を一括 Reserve する (再確保 = FString 移動を封じ
    // Data() ポインタを安定させる)。1 行あたり key + (string 値) で最大 2 個。
    usize line_count = 1;
    for (usize i = 0; i < buf_size; ++i) {
        if (buf[i] == '\n') ++line_count;
    }
    m_StringPool.Reserve(line_count * 2);

    // ---- 行ごとに parse -------------------------------------------------
    usize i = 0;
    while (i < buf_size) {
        // 行の範囲 [line_begin, line_end) を切り出す (LF 区切り、CR は除去)。
        usize line_begin = i;
        while (i < buf_size && buf[i] != '\n') ++i;
        usize line_end = i;
        if (i < buf_size) ++i;  // '\n' を飛ばす
        if (line_end > line_begin && buf[line_end - 1] == '\r') --line_end;  // CRLF 対応

        // 空行 / コメント行は skip。
        if (line_end <= line_begin) continue;
        if (buf[line_begin] == '#') continue;

        // tag は先頭 2 文字 `x:`。形式不一致は skip (前方互換)。
        if (line_end - line_begin < 3 || buf[line_begin + 1] != ':') continue;
        const char tag = buf[line_begin];

        // `=` の位置を探す (key と value の境界)。
        usize eq = line_begin + 2;
        while (eq < line_end && buf[eq] != '=') ++eq;
        if (eq >= line_end) continue;  // '=' 無しは skip

        const usize key_begin   = line_begin + 2;
        const usize key_end     = eq;
        const usize val_begin   = eq + 1;
        const usize val_end     = line_end;
        if (key_end <= key_begin) continue;  // 空 key は skip

        // key を pool に複製 (NUL 終端 const char* を Set* に渡す)。
        FString key_str(FStringView(buf + key_begin, key_end - key_begin));
        m_StringPool.PushBack(Move(key_str));
        const char* key_cstr = m_StringPool.Back().Data();

        switch (tag) {
            case 'f': {
                SetF32(key_cstr, ParseF32(buf + val_begin, buf + val_end));
                break;
            }
            case 'i': {
                SetI32(key_cstr, ParseI32(buf + val_begin, buf + val_end));
                break;
            }
            case 'b': {
                // "true" のみ true、それ以外 ("false" 含む) は false。
                const usize vlen = val_end - val_begin;
                const bool  bv   = (vlen == 4 &&
                                    buf[val_begin]     == 't' &&
                                    buf[val_begin + 1] == 'r' &&
                                    buf[val_begin + 2] == 'u' &&
                                    buf[val_begin + 3] == 'e');
                SetBool(key_cstr, bv);
                break;
            }
            case 's': {
                FString val_str(FStringView(buf + val_begin, val_end - val_begin));
                m_StringPool.PushBack(Move(val_str));
                SetString(key_cstr, m_StringPool.Back().Data());
                break;
            }
            default:
                // 未知 tag: key を pool に積んでしまったが無害 (entry は作らない)。
                break;
        }
    }

    alloc.Free(raw);
    return Ok();
}

} // namespace acs::game
