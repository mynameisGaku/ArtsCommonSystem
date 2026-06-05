// SPDX-License-Identifier: Apache-2.0
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

/**
 * 設定・セーブデータ用の key-value ストア (INI 風、フラット、UTF-8)。
 *
 * @details
 * key=value の平坦なエントリ列を保持し、ファイル/バイト列との間で読み書きする。セクションは
 * 持たず、"audio.master_volume" のような点区切りキーで名前空間を分ける想定。値は内部で文字列
 * として保持し、Int/Float/Bool は文字列との相互変換で扱う。non-copy・move-only。
 */
class Storage {
public:
    /** 空のストアを構築する。 */
    Storage() noexcept = default;

    /** ストアを破棄する (エントリは TArray が解放)。 */
    ~Storage() noexcept = default;

    /** コピー禁止 (エントリ配列を単独所有するため)。 */
    Storage(const Storage&)            = delete;

    /** コピー代入も禁止。 */
    Storage& operator=(const Storage&) = delete;

    /**
     * ムーブ構築する (エントリ配列の所有権を奪う。Localization::Swap 等で使う)。
     *
     * @param o ムーブ元 (奪われて空になる)。
     */
    Storage(Storage&& o) noexcept : m_Entries(Move(o.m_Entries)) {}

    /**
     * ムーブ代入する (エントリ配列の所有権を奪う)。
     *
     * @param o ムーブ元 (奪われて空になる)。
     * @return 自身への参照。
     */
    Storage& operator=(Storage&& o) noexcept {
        if (this != &o) m_Entries = Move(o.m_Entries);
        return *this;
    }

    /**
     * INI 形式ファイルから読み込む (wchar_t パス版)。
     *
     * @details ファイルが存在しなければ空のまま成功扱い。既存エントリは置き換わる。
     * @param path 読み込むファイルのパス。
     * @return 成功なら空の TResult、null パスや読み取り失敗時はエラー。
     */
    TResult<void> Load(const wchar_t* path) noexcept;

    /**
     * INI 形式ファイルから読み込む (UTF-8 パス版)。
     *
     * @details MultiByteToWideChar で wchar_t 版へ委譲する。
     * @param path_utf8 読み込むファイルの UTF-8 パス。
     * @return 成功なら空の TResult、変換失敗や読み取り失敗時はエラー。
     */
    TResult<void> Load(const char*    path_utf8) noexcept;

    /**
     * INI 形式 (UTF-8、改行区切り) のバイト列から読み込む。
     *
     * @details 実行ファイル埋め込みデータ等を直接読む用途。先頭の UTF-8 BOM はスキップし、
     * '#'/';'/'[' 始まりの行とキーのない行は無視する。既存エントリはクリアされる。
     * @param data INI テキストの先頭 (nullptr は空扱いで成功)。
     * @param size data のバイト数。
     * @return 成功なら空の TResult。
     */
    TResult<void> LoadFromBytes(const u8* data, usize size) noexcept;

    /**
     * INI 形式ファイルへ保存する (wchar_t パス版)。
     *
     * @details 親ディレクトリが無ければ作成する。
     * @param path 保存先のファイルパス。
     * @return 成功なら空の TResult、null パスやディレクトリ作成・書き込み失敗時はエラー。
     */
    TResult<void> Save(const wchar_t* path) noexcept;

    /**
     * INI 形式ファイルへ保存する (UTF-8 パス版)。
     *
     * @details MultiByteToWideChar で wchar_t 版へ委譲する。
     * @param path_utf8 保存先の UTF-8 パス。
     * @return 成功なら空の TResult、変換失敗や書き込み失敗時はエラー。
     */
    TResult<void> Save(const char*    path_utf8) noexcept;

    /** 全エントリを削除する (ファイルは触らない)。 */
    void Clear() noexcept { m_Entries.Clear(); }

    /**
     * 文字列値を設定する (既存キーは上書き)。
     *
     * @param key 設定するキー (nullptr は無視)。
     * @param value 設定する値 (nullptr は空文字列扱い)。
     */
    void SetString(const char* key, const char* value) noexcept;

    /**
     * 整数値を設定する (内部では十進文字列として保持)。
     *
     * @param key 設定するキー。
     * @param value 設定する整数値。
     */
    void SetInt   (const char* key, i64 value) noexcept;

    /**
     * 浮動小数点値を設定する (内部では往復可能な精度の文字列として保持)。
     *
     * @param key 設定するキー。
     * @param value 設定する値。
     */
    void SetFloat (const char* key, f64 value) noexcept;

    /**
     * 真偽値を設定する ("true"/"false" として保持)。
     *
     * @param key 設定するキー。
     * @param value 設定する真偽値。
     */
    void SetBool  (const char* key, bool value) noexcept;

    /**
     * 文字列値を取得する。
     *
     * @details 戻り値は内部バッファへの参照で、次の Set / Load で無効化されうる。長期保持する
     * 場合は呼び出し側でコピーすること。
     * @param key 取得するキー。
     * @param default_v キーが無いときに返す既定値。
     * @return 値への参照、無ければ default_v。
     */
    const char* GetString(const char* key, const char* default_v = "") const noexcept;

    /**
     * 整数値を取得する。
     *
     * @param key 取得するキー。
     * @param default_v キーが無いときに返す既定値。
     * @return 値を十進解釈した整数、無ければ default_v。
     */
    i64         GetInt   (const char* key, i64 default_v = 0) const noexcept;

    /**
     * 浮動小数点値を取得する。
     *
     * @param key 取得するキー。
     * @param default_v キーが無いときに返す既定値。
     * @return 値を解釈した実数、無ければ default_v。
     */
    f64         GetFloat (const char* key, f64 default_v = 0.0) const noexcept;

    /**
     * 真偽値を取得する。
     *
     * @details "true"/"1"/"yes"/"on" を真、"false"/"0"/"no"/"off" を偽と解釈する。
     * @param key 取得するキー。
     * @param default_v キーが無い・解釈不能なときに返す既定値。
     * @return 解釈した真偽値、該当しなければ default_v。
     */
    bool        GetBool  (const char* key, bool default_v = false) const noexcept;

    /**
     * キーが存在するかを返す。
     *
     * @param key 確認するキー。
     * @return 存在すれば true。
     */
    bool Has(const char* key) const noexcept;

    /**
     * キーとその値を削除する (順序は保証しない)。
     *
     * @param key 削除するキー (nullptr や未存在は何もしない)。
     */
    void Remove(const char* key) noexcept;

    /**
     * 保持しているエントリ数を返す。
     *
     * @return エントリ数。
     */
    usize Count() const noexcept { return m_Entries.Size(); }

    /**
     * %APPDATA%/<sub_dir>/<file_name> へのフルパスを構築する (wchar_t 版)。
     *
     * @details FOLDERID_RoamingAppData を起点に連結し、親ディレクトリ
     * (%APPDATA%/<sub_dir>) を自動作成する。
     * @param sub_dir サブディレクトリ名 (nullptr なら "acs")。
     * @param file_name ファイル名 (nullptr なら "storage.ini")。
     * @param out パスを書き込む先のバッファ。
     * @param cap out の容量 (要素数)。
     * @return 成功なら空の TResult、引数不正・既知フォルダ取得失敗・作成失敗時はエラー。
     */
    static TResult<void> GetAppDataPath(const wchar_t* sub_dir,
                                       const wchar_t* file_name,
                                       wchar_t* out, usize cap) noexcept;

    /**
     * %APPDATA%/<sub_dir>/<file_name> へのフルパスを構築する (UTF-8 版)。
     *
     * @details 引数を MultiByteToWideChar で変換し wchar_t 版へ委譲、結果を UTF-8 に戻す。
     * @param sub_dir_utf8 サブディレクトリ名 (UTF-8、nullptr 可)。
     * @param file_name_utf8 ファイル名 (UTF-8、nullptr 可)。
     * @param out_utf8 パスを書き込む先の UTF-8 バッファ。
     * @param cap out_utf8 の容量 (バイト数)。
     * @return 成功なら空の TResult、引数不正や変換失敗時はエラー。
     */
    static TResult<void> GetAppDataPath(const char* sub_dir_utf8,
                                       const char* file_name_utf8,
                                       char* out_utf8, usize cap) noexcept;

private:
    /** 1 件の key-value エントリ。 */
    struct Entry {
        /** エントリのキー。 */
        FString key;

        /** エントリの値 (文字列として保持)。 */
        FString value;
    };

    /**
     * キーに一致するエントリを線形探索する (可変版)。
     *
     * @param key 探すキー。
     * @return 見つかったエントリ、無ければ nullptr。
     */
    Entry*       FindEntry(const char* key) noexcept;

    /**
     * キーに一致するエントリを線形探索する (const 版)。
     *
     * @param key 探すキー。
     * @return 見つかったエントリ、無ければ nullptr。
     */
    const Entry* FindEntry(const char* key) const noexcept;

    /** 保持している全エントリ。 */
    TArray<Entry> m_Entries;
};

} // namespace acs
