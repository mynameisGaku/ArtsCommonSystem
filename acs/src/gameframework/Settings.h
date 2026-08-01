// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — CSettings (型付きゲーム設定)
//
// 音量・解像度・キーバインド等の「ゲームを跨いで永続化したい設定値」を
// 型付き key-value で保持する小型ストア。FInputMap (Pillar D) のキーコンフィグや
// AudioMixer の音量、Display の解像度・ウィンドウモード等の永続化先として使う。
//
// 使い方:
//   CSettings s;
//   s.SetF32 ("audio.master",   0.8f);
//   s.SetI32 ("display.width",  1920);
//   s.SetBool("display.vsync",  true);
//   s.SetString("locale",       "ja");
//
//   f32  master = s.GetF32 ("audio.master",  1.0f);
//   bool vsync  = s.GetBool("display.vsync", false);
//
//   s.Save(L"settings.ini");  // 次回起動時に Load() で復元
//
// 設計選択 (Pillar G):
//   ・**flat key-value**: key は `audio.master` のようなドット階層を文字列で表現する
//     だけで、ツリー構造は持たない。深いネストが必要になったら `Section`
//     型を上にかぶせる方が安全 (今は YAGNI)。
//   ・**4 型タグ付き union**: f32 / i32 / bool / const char*。FVec2/FVec3 等の複合型は
//     呼び出し側で複数 key (`pos.x`, `pos.y`) に分割するか、将来 配列型を追加。
//   ・**key / string 値は非所有 const char***: ACS の STL 禁止方針 + 文字列ストア
//     導入を避けるため、key と string 値の寿命は呼び出し側が保証する (リテラル or
//     長寿命バッファ前提)。短命バッファ渡しが dangling になる点は要注意。
//   ・**同 key の SetX は上書き**: 同名 key が既に存在すれば値と kind を上書きする。
//     ユーザーが UI で「音量」を動かす度に SetF32() が呼ばれる典型ケースに合わせる。
//   ・**線形検索**: settings 件数は通常 10〜200 程度なので TArray<FEntry> の線形走査で
//     十分。ハッシュテーブル化は計測してから検討。
//   ・**コピー / ムーブ禁止**: settings は通常 1 セッションに 1 オブジェクト
//     (グローバル所有) で運用される。誤って値渡しされて分裂すると同期ずれを
//     検知しづらいため、最初から非コピー・非ムーブで固定する。
//   ・**全 noexcept**: 例外不使用方針 (TResult<T,E> + bool 戻り値)。
//
// Save / Load (実装済み、round-trip 検証済み):
//   ・INI 風 `<tag>:<key>=<value>` テキスト (UTF-8 + LF) で読み書きする。Save は
//     CREATE_NEW の一意な一時ファイルから atomic replace して破損を防ぐ。
//   ・型は **prefix tag** (`f:`, `i:`, `b:`, `s:`) でディスクに残し、Load 時に復元する。
//
// 範囲外:
//   ・配列値 / FVec2 / FVec3 / FColor 等の複合型 (現状は 4 プリミティブのみ)
//   ・change notification (オブザーバ pattern / callback)
//   ・section / namespace ツリー
//   ・暗号化 / 改竄検知 (Pillar S Storefront / AssetPack 側で扱う)
//   ・schema migration (バージョン番号の埋め込みと変換)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/String.h"

namespace acs::game {

/**
 * 設定値 1 つの型タグ。
 *
 * @details
 * Set 系で書き込まれ、Get 系で kind 一致を確認する。不一致な型での Get は
 * default_value を返す (silent fallback)。
 */
enum class ESettingKind : u8 {
    /** 未設定 (内部用、外部からは見えない)。 */
    None,

    /** f32 値。 */
    F32,

    /** i32 値。 */
    I32,

    /** bool 値。 */
    Bool,

    /** string (const char*) 値。 */
    String,
};

/**
 * checked API が返す設定永続化エラー。
 *
 * 数値は診断契約の一部である。既存値を再利用せず、新しい値は末尾へ追加する。
 */
enum class ESettingsPersistenceError : u16 {
    None = 0,
    FileOpenFailed = 10,
    FileTooLarge = 11,
    NullPath = 12,
    AllocationFailure = 13,
    FileSizeFailed = 14,
    FileReadFailed = 15,
    FileCloseFailed = 16,
    EmbeddedNul = 17,
    LineTooLong = 18,
    EntryLimitExceeded = 19,
    MalformedRecord = 20,
    UnknownType = 21,
    EmptyKey = 22,
    KeyTooLong = 23,
    ValueTooLong = 24,
    InvalidInteger = 25,
    InvalidFloat = 26,
    NonFiniteFloat = 27,
    InvalidBool = 28,
    DuplicateKey = 29,
    InvalidInMemoryEntry = 30,
    UnrepresentableText = 31,
    OutputTooLarge = 32,
    PathTooLong = 33,
    TemporaryFileExhausted = 34,
    FileWriteFailed = 35,
    FileFlushFailed = 36,
    AtomicReplaceFailed = 37,
};

/** TryLoad/TrySave が返す、安定した allocation-free の結果。 */
struct FSettingsPersistenceResult {
    ESettingsPersistenceError Error = ESettingsPersistenceError::None;
    u32 Line = 0;
    u32 Entries = 0;
    u32 OsError = 0;

    bool Succeeded() const noexcept {
        return Error == ESettingsPersistenceError::None;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ESettingsPersistenceError に対応する安定した診断名。 */
const char* SettingsPersistenceErrorName(ESettingsPersistenceError error) noexcept;

/**
 * 型付き key-value でゲーム設定値を保持する小型ストア。
 *
 * @details
 * 音量・解像度・キーバインド等の「ゲームを跨いで永続化したい設定値」を f32 / i32 /
 * bool / const char* の 4 型で持つ。key は `audio.master` のようなドット階層を文字列で
 * 表現するだけのフラット key-value で、線形検索 + 同名 key 上書きで運用する。key /
 * string 値は非所有 const char* (寿命は呼び出し側が保証) で、コピー・ムーブ禁止。
 * Save/Load は INI 風 `<tag>:<key>=<value>` テキスト (UTF-8 / LF) で読み書きする。
 */
class CSettings {
public:
    /** 受け入れ・出力可能な settings document の最大サイズ (4 MiB)。 */
    static constexpr usize kMaxPersistenceBytes = 4u * 1024u * 1024u;

    /** 1 document から受け入れ可能な record の最大数。 */
    static constexpr u32 kMaxPersistenceEntries = 4096u;

    /** LF と任意の末尾 CR を除く、物理行の最大サイズ。 */
    static constexpr usize kMaxPersistenceLineBytes = 4096u;

    /** key の最大バイト数。 */
    static constexpr usize kMaxPersistenceKeyBytes = 255u;

    /** string value の最大バイト数。 */
    static constexpr usize kMaxPersistenceStringBytes = 4096u;

    /** 空のストアを構築する (エントリなし)。 */
    CSettings()  noexcept = default;

    /** デストラクタ (エントリ・文字列プールは TArray/FString が解放)。 */
    ~CSettings() noexcept = default;

    /** コピー禁止 (1 セッション 1 オブジェクトでの同期ずれを防ぐため)。 */
    CSettings(const CSettings&)            = delete;

    /** コピー代入も禁止。 */
    CSettings& operator=(const CSettings&) = delete;

    /** ムーブ禁止。 */
    CSettings(CSettings&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSettings& operator=(CSettings&&)      = delete;

    /**
     * f32 値を書き込む (同名 key は上書き、key == nullptr は no-op)。
     *
     * @param key 設定キー (caller 所有、リテラル想定)。
     * @param v 書き込む f32 値。
     */
    void SetF32   (const char* key, f32         v) noexcept;

    /**
     * i32 値を書き込む (同名 key は上書き、key == nullptr は no-op)。
     *
     * @param key 設定キー (caller 所有、リテラル想定)。
     * @param v 書き込む i32 値。
     */
    void SetI32   (const char* key, i32         v) noexcept;

    /**
     * bool 値を書き込む (同名 key は上書き、key == nullptr は no-op)。
     *
     * @param key 設定キー (caller 所有、リテラル想定)。
     * @param v 書き込む bool 値。
     */
    void SetBool  (const char* key, bool        v) noexcept;

    /**
     * string 値を書き込む (同名 key は上書き、key == nullptr は no-op)。
     *
     * @details string 値の生存責任は呼び出し側 (リテラル or 長寿命バッファ)。
     * @param key 設定キー (caller 所有、リテラル想定)。
     * @param v 書き込む文字列 (非所有)。
     */
    void SetString(const char* key, const char* v) noexcept;

    /**
     * f32 値を読み出す。
     *
     * @details 未設定 / 型不一致 / key == nullptr は default_value を返す。
     * @param key 設定キー。
     * @param default_value 見つからない / 型不一致時に返す値。
     * @return 登録済みの f32 値、なければ default_value。
     */
    f32         GetF32   (const char* key, f32         default_value = 0.0f) const noexcept;

    /**
     * i32 値を読み出す。
     *
     * @details 未設定 / 型不一致 / key == nullptr は default_value を返す。
     * @param key 設定キー。
     * @param default_value 見つからない / 型不一致時に返す値。
     * @return 登録済みの i32 値、なければ default_value。
     */
    i32         GetI32   (const char* key, i32         default_value = 0)    const noexcept;

    /**
     * bool 値を読み出す。
     *
     * @details 未設定 / 型不一致 / key == nullptr は default_value を返す。
     * @param key 設定キー。
     * @param default_value 見つからない / 型不一致時に返す値。
     * @return 登録済みの bool 値、なければ default_value。
     */
    bool        GetBool  (const char* key, bool        default_value = false) const noexcept;

    /**
     * string 値を読み出す。
     *
     * @details 未設定 / 型不一致 / key == nullptr は default_value を返す。
     * @param key 設定キー。
     * @param default_value 見つからない / 型不一致時に返す値。
     * @return 登録済みの文字列、なければ default_value。
     */
    const char* GetString(const char* key, const char* default_value = "")   const noexcept;

    /**
     * key が登録済みかを返す (kind 不問)。
     *
     * @param key 確認するキー (nullptr は false)。
     * @return 登録されていれば true。
     */
    bool Has   (const char* key) const noexcept;

    /**
     * key を 1 件削除する。
     *
     * @param key 削除するキー (該当なし / nullptr は no-op)。
     */
    void Remove(const char* key) noexcept;

    /** 全エントリを削除する。 */
    void Clear () noexcept;

    /**
     * 登録エントリ数を返す (kind 不問)。
     *
     * @return エントリ数。
     */
    u32  Count () const noexcept;

    /**
     * 全 entry を検証し、出力先 file を atomic に置き換える。
     *
     * 同一 directory の CREATE_NEW temporary file を使う。temporary 名の衝突時は
     * 再試行し、どの失敗でも既存の出力先を変更しない。
     */
    FSettingsPersistenceResult TrySave(const wchar_t* file_path) noexcept;

    /**
     * この object を置き換える前に settings file 全体を検証する。
     *
     * どの失敗でも entry、所有 string、GetString が以前返した全 pointer は変更しない。
     * 重複 key は拒否する。
     */
    FSettingsPersistenceResult TryLoad(const wchar_t* file_path) noexcept;

    /**
     * 全エントリを INI 風テキストに直列化し atomic write で保存する。
     *
     * @details
     * `<tag>:<key>=<value>` 形式 (UTF-8 / LF) で書き出す。tag は型を round-trip
     * させる 1 文字 prefix で、f: f32 / i: i32 / b: bool / s: string。TrySave の
     * checked validation と atomic replace を使い、途中失敗で既存ファイルが破損
     * しないようにする。
     * @param file_path 保存先パス (nullptr はエラー)。
     * @return 成功なら空の TResult、IO 失敗ならエラー。
     */
    TResult<void> Save(const wchar_t* file_path) noexcept;

    /**
     * INI 風テキストを読み込んで設定値を復元する。
     *
     * @details
     * 各行を parse し、tag に応じて Set{F32,I32,Bool,String} へ復元する。既存値は
     * 捨ててからファイル状態に置き換える。復元した key / string 値は m_StringPool が
     * 所有する (リテラル前提の Set* と違いファイル由来の文字列はストアより寿命が短い
     * ため、内部で複製する)。
     * @param file_path 読み込み元パス (nullptr はエラー)。
     * @return 成功なら空の TResult、IO 失敗 / サイズ超過ならエラー。
     */
    TResult<void> Load(const wchar_t* file_path) noexcept;

private:
    /**
     * 1 件のエントリ。
     *
     * @details
     * union で 4 種類の値を保持し、kind で実効型を区別する。key / string 値は非所有
     * const char* (寿命は呼び出し側保証)。
     */
    struct FEntry {
        /** 設定キー (非所有 const char*)。 */
        const char* key  = nullptr;

        /** 実効型を示す型タグ。 */
        ESettingKind kind = ESettingKind::None;

        /**
         * 4 型を重ねて持つ値の union。
         *
         * @details 実効的にどのメンバが有効かは外側の kind が示す。
         */
        union FValue {
            /** f32 値 (kind == F32)。 */
            f32         f;

            /** i32 値 (kind == I32)。 */
            i32         i;

            /** bool 値 (kind == Bool)。 */
            bool        b;

            /** string 値 (kind == FString、非所有)。 */
            const char* s;

            /** ゼロ初期化する既定コンストラクタ。 */
            FValue() noexcept : f(0.0f) {}
        } value;
    };

    /**
     * key 一致 entry の index を返す。
     *
     * @param key 検索するキー (nullptr は -1)。
     * @return 一致した entry の index、未検出なら -1。
     */
    isize FindIndex(const char* key) const noexcept;

    /**
     * key 一致 entry を返し、無ければ新規追加して返す。
     *
     * @param key 対象キー。
     * @return 既存または新規追加した entry への参照。
     */
    FEntry& UpsertEntry(const char* key) noexcept;

    /** 登録エントリの配列。 */
    TArray<FEntry> m_Entries;

    /**
     * Load() が複製した key / string 値の所有プール。
     *
     * @details
     * Set* に渡す const char* はここに格納した FString の Data() を指す。Load 冒頭で
     * 「行数 × 2」分を Reserve して TArray の再確保を封じるため、格納後も各 FString の
     * 位置 (= Data() が返すポインタ) は object の寿命まで安定する (dangling 回避)。
     */
    TArray<FString> m_StringPool;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSettings = CSettings;

} // namespace acs::game
