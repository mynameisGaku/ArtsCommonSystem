// 設定 / セーブデータ用 key-value ストア（INI 形式）
//
// 用途: ゲーム設定（音量・解像度）、セーブデータ（ハイスコア・進行度）の
//       人間可読な永続化。
//
// 使い方:
//   Storage cfg;
//   wchar_t path[260];
//   Storage::GetAppDataPath(L"MyGame", L"settings.ini", path, 260);
//
//   cfg.Load(path);                       // 既存があれば読み込む（無ければ空）
//   if (!cfg.Has("master_volume")) {
//       cfg.SetFloat("master_volume", 0.8f);
//   }
//   f64 vol = cfg.GetFloat("master_volume", 0.8);
//   cfg.SetInt("high_score", 12345);
//   cfg.Save(path);                       // 保存
//
// ファイル形式 (INI 風、フラット):
//   master_volume=0.8
//   resolution_w=1920
//   high_score=12345
//   player_name=タロウ
//
// セクションは v1 では持たず、平坦なキー名（例 "audio.master_volume"）で
// 名前空間分けする想定。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Move.h"
#include "container/String.h"
#include "container/Array.h"

namespace acs {

class Storage {
public:
    Storage() noexcept = default;
    ~Storage() noexcept = default;

    Storage(const Storage&)            = delete;
    Storage& operator=(const Storage&) = delete;

    // ムーブは可（Localization::Swap 等で使う）
    Storage(Storage&& o) noexcept : _entries(Move(o._entries)) {}
    Storage& operator=(Storage&& o) noexcept {
        if (this != &o) _entries = Move(o._entries);
        return *this;
    }

    // INI 形式ファイルから読み込む。ファイルが無ければ空のままで成功扱い。
    // (Windows 既存の wchar_t API + UTF-8 const char* 受口の両方を提供)
    Result<void> Load(const wchar_t* path) noexcept;
    Result<void> Load(const char*    path_utf8) noexcept;

    // バイト列（INI 形式 UTF-8、改行区切り）から読み込む。実行ファイル埋め込みなど。
    Result<void> LoadFromBytes(const u8* data, usize size) noexcept;

    // INI 形式ファイルへ保存。親ディレクトリは作成する。
    Result<void> Save(const wchar_t* path) noexcept;
    Result<void> Save(const char*    path_utf8) noexcept;

    // 全エントリ削除（ファイルは触らない）
    void Clear() noexcept { _entries.Clear(); }

    // ===== Set =====
    void SetString(const char* key, const char* value) noexcept;
    void SetInt   (const char* key, i64 value) noexcept;
    void SetFloat (const char* key, f64 value) noexcept;
    void SetBool  (const char* key, bool value) noexcept;

    // ===== Get =====
    // 文字列は内部バッファへの参照。次の Set / Load でポインタは無効化される
    // ことがあるので、長く保持したい場合は呼び出し側でコピーすること。
    const char* GetString(const char* key, const char* default_v = "") const noexcept;
    i64         GetInt   (const char* key, i64 default_v = 0) const noexcept;
    f64         GetFloat (const char* key, f64 default_v = 0.0) const noexcept;
    bool        GetBool  (const char* key, bool default_v = false) const noexcept;

    // ===== その他 =====
    bool Has(const char* key) const noexcept;
    void Remove(const char* key) noexcept;
    usize Count() const noexcept { return _entries.Size(); }

    // ===== ユーティリティ =====
    // %APPDATA%/<sub_dir>/<file_name> へのフルパスを out に書き込む。
    // 親ディレクトリ（%APPDATA%/<sub_dir>）も自動作成する。
    static Result<void> GetAppDataPath(const wchar_t* sub_dir,
                                       const wchar_t* file_name,
                                       wchar_t* out, usize cap) noexcept;

    // UTF-8 受口版。中で MultiByteToWideChar / std::filesystem を呼んで処理する。
    // Linux / macOS では XDG_DATA_HOME / ~/Library/Application Support を見る。
    static Result<void> GetAppDataPath(const char* sub_dir_utf8,
                                       const char* file_name_utf8,
                                       char* out_utf8, usize cap) noexcept;

private:
    struct Entry {
        String key;
        String value;
    };
    Entry*       FindEntry(const char* key) noexcept;
    const Entry* FindEntry(const char* key) const noexcept;

    Array<Entry> _entries;
};

} // namespace acs
