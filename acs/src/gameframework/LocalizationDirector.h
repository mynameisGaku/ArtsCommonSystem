// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"

namespace acs::game {

/**
 * ロケール識別子 (市場想定の主要 11 言語 + Default(=En))。
 *
 * @details 値は将来の永続化 (CSettings) との互換性のため u32 で連番固定。新規追加は末尾 append。
 */
enum class ELocale : u32 {
    /** English。 */
    En      = 0,

    /** 日本語。 */
    Ja      = 1,

    /** Français (フランス語)。 */
    Fr      = 2,

    /** Deutsch (ドイツ語)。 */
    De      = 3,

    /** Español (スペイン語)。 */
    Es      = 4,

    /** 简体中文 (簡体字中国語)。 */
    ZhCn    = 5,

    /** 繁體中文 (繁体字中国語)。 */
    ZhTw    = 6,

    /** 한국어 (韓国語)。 */
    Ko      = 7,

    /** Português (ポルトガル語)。 */
    Pt      = 8,

    /** Русский (ロシア語)。 */
    Ru      = 9,

    /** Italiano (イタリア語)。 */
    It      = 10,

    /** 未翻訳時 / 起動時の既定ロケール (= En)。 */
    Default = En,
};

/**
 * 「ロケール + 文字列 ID」を「翻訳済みの const char*」に解決する小型ストア。
 *
 * @details
 * UI ラベル / セリフ / メニュー文言 / エラーメッセージ等を言語非依存に引けるようにする。
 * key / value とも非所有 const char* で寿命は呼び出し側保証。Get は現 locale → Default(En) →
 * key 自身の 3 段フォールバックで解決する。エントリは線形探索で引く。非コピー・非ムーブ、全 noexcept。
 */
class CLocalizationDirector {
public:
    /** 空のストアを構築する (current locale = Default)。 */
    CLocalizationDirector()  noexcept = default;

    /** 破棄する。 */
    ~CLocalizationDirector() noexcept = default;

    /** コピー禁止 (1 セッション 1 オブジェクト運用で翻訳ずれを避けるため)。 */
    CLocalizationDirector(const CLocalizationDirector&)            = delete;

    /** コピー代入も禁止。 */
    CLocalizationDirector& operator=(const CLocalizationDirector&) = delete;

    /** ムーブ禁止。 */
    CLocalizationDirector(CLocalizationDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CLocalizationDirector& operator=(CLocalizationDirector&&)      = delete;

    /**
     * 現在の取得対象ロケールを切り替える (UI 言語切替で呼ばれる)。
     *
     * @details 切替後 Get(key) は新 locale で検索 → Default → key の順に解決される。
     * @param loc 新しい現在ロケール。
     */
    void   SetLocale    (ELocale loc) noexcept;

    /**
     * 現在の取得対象ロケールを返す。
     *
     * @return 現在の ELocale。
     */
    ELocale CurrentLocale() const noexcept;

    /**
     * 指定 locale に key→value を 1 件登録する。
     *
     * @details
     * key/value は所有しない (寿命は呼び出し側保証、リテラル or 長寿命バッファ前提)。
     * 同 (locale, key) の重複は禁止せず後追い append する (Get は先頭一致を返す)。
     * key == nullptr は no-op で防御する。
     * @param loc 登録先ロケール。
     * @param key 文字列 ID (nullptr は no-op)。
     * @param value 翻訳済み文字列 (nullptr 許容)。
     */
    void RegisterString(ELocale loc, const char* key, const char* value) noexcept;

    /**
     * 現 locale で key を引く (見つからなければフォールバック)。
     *
     * @details
     * 現 locale → Default(En) → key 自身の順に解決する。key 自身を返すのは未翻訳箇所を
     * 画面で発見しやすくするための意図的挙動。key == nullptr は空文字 "" を返す。
     * @param key 引く文字列 ID。
     * @return 翻訳済み文字列 (未登録なら key 自身、key==nullptr なら "")。
     */
    const char* Get(const char* key) const noexcept;

    /**
     * 指定 locale で key を引く (Default フォールバックなし)。
     *
     * @details 当該 locale に無ければ key 自身を返す。key == nullptr は空文字 "" を返す。
     * @param loc 引くロケール。
     * @param key 引く文字列 ID。
     * @return 翻訳済み文字列 (未登録なら key 自身、key==nullptr なら "")。
     */
    const char* GetForLocale(ELocale loc, const char* key) const noexcept;

    /**
     * 現 locale に key が登録されているかを返す (Default フォールバックは見ない)。
     *
     * @param key 検査する文字列 ID。
     * @return 登録されていれば true (key == nullptr は false)。
     */
    bool Has(const char* key) const noexcept;

    /**
     * 指定 locale の登録件数を返す。
     *
     * @param loc 数える対象ロケール。
     * @return 当該 locale のエントリ数。
     */
    u32 KeyCount(ELocale loc) const noexcept;

    /** 全 locale の全エントリを削除する。 */
    void Clear() noexcept;

    /**
     * 指定 locale のエントリのみ削除する (他 locale は保持)。
     *
     * @param loc 削除対象ロケール。
     */
    void ClearLocale(ELocale loc) noexcept;

private:
    /** 1 件のエントリ (locale + key + value。key/value は非所有 const char*)。 */
    struct FLocaleEntry {
        /** このエントリのロケール。 */
        ELocale      locale = ELocale::Default;

        /** 文字列 ID (非所有)。 */
        const char* key    = nullptr;

        /** 翻訳済み文字列 (非所有、nullptr 許容)。 */
        const char* value  = nullptr;
    };

    /**
     * (locale, key) 一致エントリの index を返す。
     *
     * @param loc 探すロケール。
     * @param key 探す文字列 ID。
     * @return 一致エントリの index (未検出 / key==nullptr は -1)。
     */
    isize FindIndex(ELocale loc, const char* key) const noexcept;

    /** 現在の取得対象ロケール。 */
    ELocale              m_Current = ELocale::Default;

    /** 全 locale のエントリ列。 */
    TArray<FLocaleEntry>  m_Entries;
};

} // namespace acs::game
