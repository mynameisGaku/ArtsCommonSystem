// SPDX-License-Identifier: Apache-2.0
// FStorage 実装（INI 形式 key=value、UTF-8）
#include "platform/Storage.h"
#include "platform/FileSystem.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <Windows.h>
#include <ShlObj.h>

// Windows.h で `CreateDirectory` が `CreateDirectoryW` に展開されると
// FileSystem::CreateDirectory もマクロ置換されて衝突する。マクロを取り消す。
#ifdef CreateDirectory
    #undef CreateDirectory
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace acs {

namespace {

/**
 * 2 つの NUL 終端文字列が等しいかを返す。
 *
 * @param a 比較する文字列 1 (nullptr 可)。
 * @param b 比較する文字列 2 (nullptr 可)。
 * @return 内容が一致すれば true (両方 nullptr も true)。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

/**
 * loader と同じ規則で key 境界の ASCII space と tab を除いたビューを返す。
 *
 * @param key 正規化する非所有 key ビュー。
 * @return key 内を指す正規化済みビュー。
 */
FStringView TrimStorageKeyForLoad(FStringView key) noexcept {
    /** 先頭の ASCII space と tab を除いた開始位置。 */
    usize start = 0u;
    while (start < key.Size() && (key.Data()[start] == ' ' || key.Data()[start] == '\t')) ++start;

    /** 末尾の ASCII space と tab を除いた終端位置。 */
    usize end = key.Size();
    while (end > start && (key.Data()[end - 1u] == ' ' || key.Data()[end - 1u] == '\t')) --end;
    return key.SubView(start, end - start);
}

/** 一括設定の入力文字列を呼び出し元から独立して保持する。 */
struct FOwnedStorageStringBatchEntry {
    /** 指定した allocator をキーと値の確保元にする。 */
    explicit FOwnedStorageStringBatchEntry(FAllocator& allocator) noexcept : key(allocator), value(allocator) {}

    /** 所有している設定キー。 */
    FString key;

    /** 所有している設定値。 */
    FString value;
};

/**
 * 一括設定キーが UTF-8 と INI の保存形式に対して安全かを返す。
 *
 * @param key 検証する終端文字列キー。
 * @return 空でなく、有効な UTF-8 で保存形式や loader の key trim と衝突しなければ true。
 */
bool IsValidStorageStringBatchKey(const char* key) noexcept {
    if (!key || key[0] == '\0') return false;

    /** INI のコメントまたはセクション開始と衝突する先頭バイト。 */
    const u8 firstByte = static_cast<u8>(key[0]);
    if (firstByte == static_cast<u8>(' ') || firstByte == static_cast<u8>('#') || firstByte == static_cast<u8>(';') || firstByte == static_cast<u8>('[')) {
        return false;
    }

    /** キー先頭の数値アドレス。 */
    const uptr baseAddress = reinterpret_cast<uptr>(key);

    /** 現在検証している UTF-8 列の先頭位置。 */
    usize offset = 0u;
    for (;;) {
        if (offset > (~uptr(0)) - baseAddress) return false;

        /** 現在の UTF-8 列を始めるバイト。 */
        const u8 lead = static_cast<u8>(key[offset]);
        if (lead == 0u) return static_cast<u8>(key[offset - 1u]) != static_cast<u8>(' ');

        /** 現在の UTF-8 列が表す Unicode 値。 */
        u32 codepoint = 0u;

        /** 現在の UTF-8 列を構成するバイト数。 */
        usize sequenceLength = 0u;
        if (lead <= 0x7Fu) {
            codepoint = lead;
            sequenceLength = 1u;
        } else if (lead >= 0xC2u && lead <= 0xDFu) {
            codepoint = static_cast<u32>(lead & 0x1Fu);
            sequenceLength = 2u;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            codepoint = static_cast<u32>(lead & 0x0Fu);
            sequenceLength = 3u;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            codepoint = static_cast<u32>(lead & 0x07u);
            sequenceLength = 4u;
        } else {
            return false;
        }

        if (sequenceLength > (~uptr(0)) - baseAddress - offset + 1u) return false;
        for (usize byteIndex = 1u; byteIndex < sequenceLength; ++byteIndex) {
            /** 現在の UTF-8 継続バイト。 */
            const u8 continuation = static_cast<u8>(key[offset + byteIndex]);
            if ((continuation & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | static_cast<u32>(continuation & 0x3Fu);
        }

        if ((sequenceLength == 3u && lead == 0xE0u && static_cast<u8>(key[offset + 1u]) < 0xA0u) || (sequenceLength == 3u && lead == 0xEDu && static_cast<u8>(key[offset + 1u]) >= 0xA0u) || (sequenceLength == 4u && lead == 0xF0u && static_cast<u8>(key[offset + 1u]) < 0x90u) || (sequenceLength == 4u && lead == 0xF4u && static_cast<u8>(key[offset + 1u]) >= 0x90u)) {
            return false;
        }

        /** ASCII と Unicode の制御文字に該当するか。 */
        const bool isControl = codepoint < 0x20u || (codepoint >= 0x7Fu && codepoint <= 0x9Fu);

        /** Unicode の行区切りまたは段落区切りに該当するか。 */
        const bool isLineSeparator = codepoint == 0x2028u || codepoint == 0x2029u;
        if (isControl || isLineSeparator || codepoint == static_cast<u32>('=')) return false;

        if (sequenceLength > (~usize(0)) - offset) return false;
        offset += sequenceLength;
    }
}

/**
 * 文字列末尾の空白・改行類 (' ' '\t' '\r' '\n') を取り除く。
 *
 * @param s 対象の文字列 (その場で短縮される)。
 */
void RTrim(FString& s) noexcept {
    while (s.Size() > 0) {
        const char c = s[s.Size() - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            // FString には Pop が無いので新しい文字列を作って置換
            FString t;
            t.Reserve(s.Size() - 1);
            t.Append(FStringView(s.Data(), s.Size() - 1));
            s = Move(t);
        } else {
            break;
        }
    }
}

/**
 * "<base>\<sub>\<file>" の形でパスを out へ書き込む。
 *
 * @details cap に収まる範囲で連結し、常に NUL 終端する。
 * @param out 書き込み先バッファ。
 * @param cap out の容量 (要素数)。
 * @param base 先頭のベースパス。
 * @param sub サブディレクトリ名。
 * @param file ファイル名。
 * @return 全要素を格納できた場合は true。出力容量不足なら false。
 */
bool Concat(wchar_t* out, usize cap, const wchar_t* base, const wchar_t* sub, const wchar_t* file) noexcept {
    if (!out || cap == 0) return false;
    usize written = 0;
    bool complete = true;
    auto append = [&](const wchar_t* text) {
        while (*text) {
            if (written + 1 >= cap) {
                complete = false;
                break;
            }
            out[written++] = *text++;
        }
    };
    append(base);
    append(L"\\");
    append(sub);
    append(L"\\");
    append(file);
    out[written] = L'\0';
    return complete;
}

/**
 * パスから末尾要素を除いたディレクトリ部分を out へ書き込む。
 *
 * @details 最後の '\\' または '/' までを切り出す。区切りが無ければ out は空文字列。
 * @param path 元のパス。
 * @param out ディレクトリ部分を書き込む先のバッファ。
 * @param cap out の容量 (要素数)。
 */
void DirOf(const wchar_t* path, wchar_t* out, usize cap) noexcept {
    usize len = 0;
    while (path[len] && len + 1 < cap) { out[len] = path[len]; ++len; }
    out[len] = 0;
    for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
        if (out[i] == L'\\' || out[i] == L'/') { out[i] = 0; return; }
    }
    out[0] = 0;  // ディレクトリ部なし
}

} // namespace

void FStorage::SetString(const char* key, const char* value) noexcept {
    if (!key) return;
    FEntry* e = FindEntry(key);
    if (e) {
        e->value = FString(value ? value : "", *m_Allocator);
    } else {
        FEntry ne;
        ne.key = FString(key, *m_Allocator);
        ne.value = FString(value ? value : "", *m_Allocator);
        m_Entries.PushBack(Move(ne));
    }
}

TResult<usize> FStorage::TrySetStringBatch(const FStorageStringBatchEntry* entries, usize count) noexcept {
    if (count == 0u) return Ok(static_cast<usize>(0u));
    if (!entries) {
        return ACS_ERR(Container, 140, "FStorage::TrySetStringBatch: null entries");
    }
    if (count > (~usize(0)) / sizeof(FStorageStringBatchEntry)) {
        return ACS_ERR(Container, 141, "FStorage::TrySetStringBatch: entry byte count overflow");
    }

    /** 入力配列全体のバイト数。 */
    const usize entryBytes = count * sizeof(FStorageStringBatchEntry);

    /** 入力配列先頭の数値アドレス。 */
    const uptr entryAddress = reinterpret_cast<uptr>(entries);
    if ((entryAddress % alignof(FStorageStringBatchEntry)) != 0u) {
        return ACS_ERR(Container, 142, "FStorage::TrySetStringBatch: misaligned entries");
    }
    if (static_cast<uptr>(entryBytes) > (~uptr(0)) - entryAddress) {
        return ACS_ERR(Container, 143, "FStorage::TrySetStringBatch: entry address overflow");
    }
    if (count > kMaximumStringBatchEntryCount) {
        return ACS_ERR(Container, 144, "FStorage::TrySetStringBatch: entry count exceeds limit");
    }

    for (usize index = 0u; index < count; ++index) {
        if (!IsValidStorageStringBatchKey(entries[index].key)) {
            return ACS_ERR(Container, 145, "FStorage::TrySetStringBatch: invalid key");
        }
        for (usize priorIndex = 0u; priorIndex < index; ++priorIndex) {
            if (StrEq(entries[index].key, entries[priorIndex].key)) {
                return ACS_ERR(Container, 146, "FStorage::TrySetStringBatch: duplicate key");
            }
        }
    }

    if (m_Entries.Size() > kMaximumStringBatchEntryCount) {
        return ACS_ERR(Container, 147, "FStorage::TrySetStringBatch: final entry count exceeds limit");
    }

    for (usize existingIndex = 0u; existingIndex < m_Entries.Size(); ++existingIndex) {
        /** loader と同じ規則で正規化した現在の既存 key。 */
        const FStringView normalizedExistingKey = TrimStorageKeyForLoad(m_Entries[existingIndex].key.View());
        for (usize priorIndex = 0u; priorIndex < existingIndex; ++priorIndex) {
            /** loader と同じ規則で正規化した検証済み既存 key。 */
            const FStringView normalizedPriorKey = TrimStorageKeyForLoad(m_Entries[priorIndex].key.View());
            if (normalizedExistingKey == normalizedPriorKey) {
                return ACS_ERR(Container, 157, "FStorage::TrySetStringBatch: existing keys collide after loader trim");
            }
        }
    }

    for (usize inputIndex = 0u; inputIndex < count; ++inputIndex) {
        /** 現在比較している入力 key。 */
        const FStringView inputKey(entries[inputIndex].key);

        /** loader と同じ規則で正規化した入力 key。 */
        const FStringView normalizedInputKey = TrimStorageKeyForLoad(inputKey);
        for (usize existingIndex = 0u; existingIndex < m_Entries.Size(); ++existingIndex) {
            /** 現在比較している既存 key。 */
            const FStringView existingKey = m_Entries[existingIndex].key.View();

            /** loader と同じ規則で正規化した既存 key。 */
            const FStringView normalizedExistingKey = TrimStorageKeyForLoad(existingKey);
            if (existingKey != inputKey && normalizedExistingKey == normalizedInputKey) {
                return ACS_ERR(Container, 158, "FStorage::TrySetStringBatch: existing and input keys collide after loader trim");
            }
        }
    }

    /** 新規追加または実変更する項目数。 */
    usize changedCount = 0u;

    /** 一括反映で新規追加する項目数。 */
    usize addedCount = 0u;
    for (usize index = 0u; index < count; ++index) {
        /** 入力キーに対応する反映前の項目。 */
        const FEntry* existingEntry = FindEntry(entries[index].key);

        /** nullptr を既存 setter と同じ空文字列へ正規化した入力値。 */
        const char* const inputValue = entries[index].value ? entries[index].value : "";
        if (!existingEntry) {
            ++changedCount;
            ++addedCount;
        } else if (!StrEq(existingEntry->value.Data(), inputValue)) {
            ++changedCount;
        }
    }
    if (addedCount > kMaximumStringBatchEntryCount - m_Entries.Size()) {
        return ACS_ERR(Container, 147, "FStorage::TrySetStringBatch: final entry count exceeds limit");
    }
    if (changedCount == 0u) return Ok(static_cast<usize>(0u));
    if (addedCount > (~usize(0)) - m_Entries.Size()) {
        return ACS_ERR(Container, 148, "FStorage::TrySetStringBatch: final entry count overflow");
    }

    /** 反映成功後に保持する項目数。 */
    const usize finalCount = m_Entries.Size() + addedCount;
    if (finalCount > (~usize(0)) / sizeof(FEntry)) {
        return ACS_ERR(Container, 149, "FStorage::TrySetStringBatch: final entry byte count overflow");
    }

    /** 呼び出し元と既存ストアの寿命から独立させた全入力。 */
    TArray<FOwnedStorageStringBatchEntry> ownedEntries(*m_Allocator);
    if (!ownedEntries.TryReserve(count)) {
        return ACS_ERR(Memory, 150, "FStorage::TrySetStringBatch: input table allocation failed");
    }
    for (usize index = 0u; index < count; ++index) {
        /** 現在所有化している入力項目。 */
        FOwnedStorageStringBatchEntry ownedEntry(*m_Allocator);
        if (!ownedEntry.key.TryAppend(FStringView(entries[index].key)) || !ownedEntry.value.TryAppend(FStringView(entries[index].value ? entries[index].value : ""))) {
            return ACS_ERR(Memory, 151, "FStorage::TrySetStringBatch: input string allocation failed");
        }
        if (!ownedEntries.TryPushBack(Move(ownedEntry))) {
            return ACS_ERR(Memory, 152, "FStorage::TrySetStringBatch: input table growth failed");
        }
    }

    /** 成功時だけ公開状態へ移す同一 allocator の候補。 */
    FStorage candidate(*m_Allocator);
    if (!candidate.m_Entries.TryReserve(finalCount)) {
        return ACS_ERR(Memory, 153, "FStorage::TrySetStringBatch: candidate table allocation failed");
    }
    for (usize index = 0u; index < m_Entries.Size(); ++index) {
        /** 複製する既存項目のキー。 */
        FString candidateKey(*m_Allocator);

        /** 複製する既存項目の値。 */
        FString candidateValue(*m_Allocator);
        if (!candidateKey.TryAppend(m_Entries[index].key.View()) || !candidateValue.TryAppend(m_Entries[index].value.View())) {
            return ACS_ERR(Memory, 154, "FStorage::TrySetStringBatch: candidate string allocation failed");
        }

        /** 候補へ追加する既存項目の複製。 */
        FEntry candidateEntry{Move(candidateKey), Move(candidateValue)};
        if (!candidate.m_Entries.TryPushBack(Move(candidateEntry))) {
            return ACS_ERR(Memory, 155, "FStorage::TrySetStringBatch: candidate table growth failed");
        }
    }

    for (usize index = 0u; index < ownedEntries.Size(); ++index) {
        /** 入力キーに対応する反映前の項目。 */
        const FEntry* existingEntry = FindEntry(ownedEntries[index].key.Data());
        if (existingEntry && StrEq(existingEntry->value.Data(), ownedEntries[index].value.Data())) {
            continue;
        }

        /** 入力キーに対応する候補内の項目。 */
        FEntry* candidateEntry = candidate.FindEntry(ownedEntries[index].key.Data());
        if (candidateEntry) {
            candidateEntry->value = Move(ownedEntries[index].value);
        } else {
            /** 候補へ新規追加する所有済み項目。 */
            FEntry addedEntry{Move(ownedEntries[index].key), Move(ownedEntries[index].value)};
            if (!candidate.m_Entries.TryPushBack(Move(addedEntry))) {
                return ACS_ERR(Memory, 156, "FStorage::TrySetStringBatch: new entry insertion failed");
            }
        }
    }

    *this = Move(candidate);
    return Ok(changedCount);
}

void FStorage::SetInt(const char* key, i64 value) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    SetString(key, buf);
}

void FStorage::SetFloat(const char* key, f64 value) noexcept {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    SetString(key, buf);
}

void FStorage::SetBool(const char* key, bool value) noexcept {
    SetString(key, value ? "true" : "false");
}

const char* FStorage::GetString(const char* key, const char* default_v) const noexcept {
    const FEntry* e = FindEntry(key);
    if (!e) return default_v;
    return e->value.Data();
}

i64 FStorage::GetInt(const char* key, i64 default_v) const noexcept {
    const FEntry* e = FindEntry(key);
    if (!e) return default_v;
    return std::strtoll(e->value.Data(), nullptr, 10);
}

f64 FStorage::GetFloat(const char* key, f64 default_v) const noexcept {
    const FEntry* e = FindEntry(key);
    if (!e) return default_v;
    return std::strtod(e->value.Data(), nullptr);
}

bool FStorage::GetBool(const char* key, bool default_v) const noexcept {
    const FEntry* e = FindEntry(key);
    if (!e) return default_v;
    const char* v = e->value.Data();
    if (StrEq(v, "true") || StrEq(v, "1") || StrEq(v, "yes") || StrEq(v, "on")) return true;
    if (StrEq(v, "false") || StrEq(v, "0") || StrEq(v, "no") || StrEq(v, "off")) return false;
    return default_v;
}

bool FStorage::Has(const char* key) const noexcept {
    return FindEntry(key) != nullptr;
}

void FStorage::Remove(const char* key) noexcept {
    if (!key) return;
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        if (StrEq(m_Entries[i].key.Data(), key)) {
            // 末尾と入替えて Pop（順序は保証しない）
            m_Entries[i] = Move(m_Entries[m_Entries.Size() - 1]);
            m_Entries.PopBack();
            return;
        }
    }
}

FStorage::FEntry* FStorage::FindEntry(const char* key) noexcept {
    if (!key) return nullptr;
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        if (StrEq(m_Entries[i].key.Data(), key)) return &m_Entries[i];
    }
    return nullptr;
}

const FStorage::FEntry* FStorage::FindEntry(const char* key) const noexcept {
    return const_cast<FStorage*>(this)->FindEntry(key);
}

TResult<void> FStorage::Load(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 100, "FStorage::Load: null or empty path");
    if (!CFileSystem::Exists(path)) {
        // 無ければ空状態のまま成功扱い
        return Ok();
    }
    auto bytes_r = CFileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<void>(bytes_r.Error());
    const TArray<byte>& bytes = bytes_r.Value();
    return LoadFromBytes(reinterpret_cast<const u8*>(bytes.Data()), bytes.Size());
}

TResult<void> FStorage::LoadFromBytes(const u8* data, usize size) noexcept {
    // 読み込み途中の失敗で公開済みの設定を壊さないよう、候補へ組み立てる。
    FStorage loaded(*m_Allocator);
    if (!data) {
        *this = Move(loaded);
        return Ok();
    }

    const char* p   = reinterpret_cast<const char*>(data);
    const char* end = p + size;
    // UTF-8 BOM をスキップ
    if (size >= 3 &&
        data[0] == 0xEF &&
        data[1] == 0xBB &&
        data[2] == 0xBF) {
        p += 3;
    }

    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n' && *p != '\r') ++p;
        FStringView line{ line_start, static_cast<usize>(p - line_start) };
        // 改行文字をスキップ
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        // コメント・空行
        if (line.Size() == 0) continue;
        if (line.Data()[0] == '#' || line.Data()[0] == ';') continue;
        if (line.Data()[0] == '[') continue;

        // key=value で分割
        usize eq = 0;
        while (eq < line.Size() && line.Data()[eq] != '=') ++eq;
        if (eq >= line.Size()) continue;

        // Save後の再読込で値を変えないため、'='より後ろは先頭空白を含めてそのまま保持する。
        const usize vs = eq + 1;
        const usize ve = line.Size();

        const FStringView key = TrimStorageKeyForLoad(line.SubView(0u, eq));
        for (usize i = 0; i < loaded.m_Entries.Size(); ++i) {
            if (loaded.m_Entries[i].key.View() == key) {
                return ACS_ERR(
                    Container, 130,
                    "FStorage::LoadFromBytes: duplicate key");
            }
        }

        FString candidateKey(*m_Allocator);
        FString candidateValue(*m_Allocator);
        if (!candidateKey.TryAppend(key) ||
            !candidateValue.TryAppend(
                FStringView(line.Data() + vs, ve - vs))) {
            return ACS_ERR(
                Memory, 131,
                "FStorage::LoadFromBytes: entry allocation failed");
        }

        FEntry entry;
        entry.key = Move(candidateKey);
        entry.value = Move(candidateValue);
        if (!loaded.m_Entries.TryPushBack(Move(entry))) {
            return ACS_ERR(
                Memory, 132,
                "FStorage::LoadFromBytes: entry table allocation failed");
        }
    }

    *this = Move(loaded);
    return Ok();
}

TResult<void> FStorage::Save(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 101, "FStorage::Save: null or empty path");

    // 親ディレクトリを作成（無ければ）
    wchar_t dir[1024];
    DirOf(path, dir, 1024);
    if (dir[0]) {
        auto mk = CFileSystem::CreateDirectory(dir);
        if (mk.IsErr()) {
            // 既存なら成功扱いになっているので、エラーは本当の失敗
            return mk;
        }
    }

    FString out("", *m_Allocator);
    out.Reserve(64 * (m_Entries.Size() + 1));
    out.Append(FStringView("# acs FStorage\n"));
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        out.Append(m_Entries[i].key.View());
        out.Append('=');
        out.Append(m_Entries[i].value.View());
        out.Append('\n');
    }
    return CFileSystem::WriteAllBytesAtomic(path, reinterpret_cast<const byte*>(out.Data()), out.Size());
}

TResult<void> FStorage::GetAppDataPath(const wchar_t* sub_dir, const wchar_t* file_name, wchar_t* out, usize cap) noexcept {
    if (!out || cap == 0) return ACS_ERR(IO, 110, "GetAppDataPath: bad args");

    // %APPDATA%（FOLDERID_RoamingAppData）を取得
    PWSTR appdata = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    if (FAILED(hr) || !appdata) {
        if (appdata) ::CoTaskMemFree(appdata);
        return ACS_ERR_OS(OS, 111, "SHGetKnownFolderPath failed", static_cast<u32>(hr));
    }

    const bool path_complete = Concat(out, cap, appdata, sub_dir ? sub_dir : L"acs", file_name ? file_name : L"storage.ini");
    ::CoTaskMemFree(appdata);
    if (!path_complete)
        return ACS_ERR(IO, 112, "GetAppDataPath: output buffer is too small");

    // 親ディレクトリを事前作成（Save で再度作るが、Load 前に呼ぶケースもあるので）
    wchar_t parent[1024];
    DirOf(out, parent, 1024);
    if (parent[0]) {
        auto mk = CFileSystem::CreateDirectory(parent);
        if (mk.IsErr()) return mk;
    }
    return Ok();
}

namespace {

/**
 * UTF-8 文字列を wchar_t 文字列へ変換する。
 *
 * @param utf8 変換元の NUL 終端 UTF-8 文字列。
 * @param out 変換結果を書き込む先のバッファ。
 * @param cap out の容量 (要素数)。
 * @return 変換に成功すれば true。
 */
bool Utf8ToWide(const char* utf8, wchar_t* out, usize cap) noexcept {
    if (!utf8 || !out || cap == 0) return false;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out,
                                  static_cast<int>(cap));
    return n > 0;
}

/**
 * wchar_t 文字列を UTF-8 文字列へ変換する。
 *
 * @param wide 変換元の NUL 終端 wchar_t 文字列。
 * @param out 変換結果を書き込む先のバッファ。
 * @param cap out の容量 (バイト数)。
 * @return 変換に成功すれば true。
 */
bool WideToUtf8(const wchar_t* wide, char* out, usize cap) noexcept {
    if (!wide || !out || cap == 0) return false;
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out,
                                  static_cast<int>(cap), nullptr, nullptr);
    return n > 0;
}

} // namespace

TResult<void> FStorage::Load(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 120, "FStorage::Load(utf8): conversion failed");
    }
    return Load(buf);
}

TResult<void> FStorage::Save(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 121, "FStorage::Save(utf8): conversion failed");
    }
    return Save(buf);
}

TResult<void> FStorage::GetAppDataPath(const char* sub_dir_utf8,
                                     const char* file_name_utf8,
                                     char* out_utf8, usize cap) noexcept {
    if (!out_utf8 || cap == 0) return ACS_ERR(IO, 122, "bad args");
    wchar_t sub[256], name[256], path[1024];
    if (sub_dir_utf8   && !Utf8ToWide(sub_dir_utf8,   sub,  256)) return ACS_ERR(IO, 123, "sub_dir utf8");
    if (file_name_utf8 && !Utf8ToWide(file_name_utf8, name, 256)) return ACS_ERR(IO, 124, "file_name utf8");
    auto r = GetAppDataPath(sub_dir_utf8 ? sub : nullptr,
                             file_name_utf8 ? name : nullptr,
                             path, 1024);
    if (r.IsErr()) return r;
    if (!WideToUtf8(path, out_utf8, cap)) return ACS_ERR(IO, 125, "result utf8");
    return Ok();
}

} // namespace acs
