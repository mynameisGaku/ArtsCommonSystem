// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Move.h"
#include "container/String.h"
#include "container/Array.h"
#include "platform/StorageStringBatchEntry.h"

namespace acs {

/**
 * 設定・セーブデータ用の key-value ストア (INI 風、フラット、UTF-8)。
 *
 * @details
 * key=value の平坦なエントリ列を保持し、ファイル/バイト列との間で読み書きする。セクションは
 * 持たず、"audio.master_volume" のような点区切りキーで名前空間を分ける想定。値は内部で文字列
 * として保持し、Int/Float/Bool は文字列との相互変換で扱う。non-copy・move-only。
 */
class FStorage {
public:
    /** 一回の文字列一括設定と反映後のストアで許可する最大項目数。 */
    static constexpr usize kMaximumStringBatchEntryCount = 4096u;

    /** DefaultAllocator を確保元として空のストアを構築する。 */
    FStorage() noexcept : FStorage(DefaultAllocator())
    {
    }

    /** 指定した allocator を全エントリの確保元として空のストアを構築する。 */
    explicit FStorage(FAllocator& allocator) noexcept : m_Entries(allocator), m_Allocator(&allocator)
    {
    }

    /** ストアを破棄する (エントリは TArray が解放)。 */
    ~FStorage() noexcept = default;

    /** コピー禁止 (エントリ配列を単独所有するため)。 */
    FStorage(const FStorage&)            = delete;

    /** コピー代入も禁止。 */
    FStorage& operator=(const FStorage&) = delete;

    /**
     * ムーブ構築する (エントリ配列の所有権を奪う。FLocalization::Swap 等で使う)。
     *
     * @param o ムーブ元 (奪われて空になる)。
     */
    FStorage(FStorage&& o) noexcept : m_Entries(Move(o.m_Entries)), m_Allocator(o.m_Allocator)
    {
    }

    /**
     * ムーブ代入する (エントリ配列の所有権を奪う)。
     *
     * @param o ムーブ元 (奪われて空になる)。
     * @return 自身への参照。
     */
    FStorage& operator=(FStorage&& o) noexcept {
        if (this != &o) {
            m_Entries = Move(o.m_Entries);
            m_Allocator = o.m_Allocator;
        }
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
     * '#'/';'/'[' 始まりの行とキーのない行は無視する。'=' より後ろは先頭空白も値として
     * そのまま保持する。既存エントリはクリアされる。
     * @param data INI テキストの先頭 (nullptr は空扱いで成功)。
     * @param size data のバイト数。
     * @return 成功なら空の TResult。
     */
    // 同一キーは拒否し、解析・確保に失敗した場合は読み込み前の状態を保持する。
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

    /** 全エントリと予約容量を解放する (ファイルは触らない)。 */
    void Clear() noexcept
    {
        m_Entries = TArray<FEntry>(*m_Allocator);
    }

    /**
     * 文字列値を設定し、確保失敗時は既存状態を維持する。
     *
     * @param key 設定するキー (nullptr は拒否)。
     * @param value 設定する値 (nullptr は空文字列扱い)。
     * @return 設定できた場合は true。入力不正または確保失敗時は false。
     */
    bool TrySetString(const char* key, const char* value) noexcept
    {
        if (!key) return false;

        FEntry* entry = FindEntry(key);
        FString candidateValue(*m_Allocator);
        if (!candidateValue.TryAppend(
                FStringView(value ? value : ""))) {
            return false;
        }
        if (entry) {
            entry->value = Move(candidateValue);
            return true;
        }

        FString candidateKey(*m_Allocator);
        if (!candidateKey.TryAppend(FStringView(key))) return false;
        FEntry candidate;
        candidate.key = Move(candidateKey);
        candidate.value = Move(candidateValue);
        return m_Entries.TryPushBack(Move(candidate));
    }

    /**
     * 複数の文字列値を、全項目が成功した場合だけ一括反映する。
     *
     * @details key は Save 後も LoadFromBytes の trim で identity が変わらないよう、先頭と末尾の
     * ASCII space (U+0020) を許可しない。内部の ASCII space は許可する。value の nullptr は
     * TrySetString と同じく空文字列として扱い、非 nullptr は終端までの byte 列を追加検査や変換なしで
     * 保持する。同値項目は変更数へ含めず、全項目が同値なら確保せず既存状態を置き換えない。
     * 反映前に既存 key を LoadFromBytes と同じ境界 ASCII space/tab trim で比較し、既存同士、
     * または byte 列が異なる既存 key と入力 key が同じ identity になる場合は確保せず拒否する。
     * byte 列が同じ既存 key と入力 key は通常の update または no-op として扱う。
     * 衝突しない legacy key は正規化も拒否もせず、後の Save/Load で境界が trim される場合がある。
     * 入力文字列は既存内部値を指してもよい。
     * 実変更に成功すると候補全体へ置き換えるため、呼び出し前に GetString から借用した全ポインタは
     * 無効になる。失敗時と変更なしの成功時は、既存状態と借用ポインタを維持する。
     * @param entries 設定する項目列。count が 0 の場合だけ nullptr を許可する。
     * @param count 設定する項目数。kMaximumStringBatchEntryCount 以下でなければならない。
     * @return 成功時は新規または実変更した項目数。入力不正、key identity 衝突、
     * 反映後件数超過、確保失敗時はエラー。
     */
    TResult<usize> TrySetStringBatch(const FStorageStringBatchEntry* entries, usize count) noexcept;

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
    struct FEntry {
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
    FEntry*       FindEntry(const char* key) noexcept;

    /**
     * キーに一致するエントリを線形探索する (const 版)。
     *
     * @param key 探すキー。
     * @return 見つかったエントリ、無ければ nullptr。
     */
    const FEntry* FindEntry(const char* key) const noexcept;

    /** 保持している全エントリ。 */
    TArray<FEntry> m_Entries;

    /** エントリ配列と key/value 文字列を確保した allocator。 */
    FAllocator* m_Allocator = nullptr;
};

} // namespace acs
