// SPDX-License-Identifier: Apache-2.0
#pragma once
// GameFramework localization — CLocalizationDirector (i18n 文字列辞書)
//
// 「ロケール + 文字列 ID」を「翻訳済みの const char*」に解決する小型ストア。
// UI ラベル / セリフ / メニュー文言 / エラーメッセージ等を、ゲームロジックから
// 言語非依存に引けるようにする。出荷タイトルの多言語対応で必要になる完成度
// パーツの 1 つ。
//
// 使い方:
//   CLocalizationDirector loc;
//   英語のタイトルを登録: loc.RegisterString(ELocale::En, "ui.title", "Star Adventure");
//   loc.RegisterString(ELocale::Ja, "ui.title",      "星の冒険");
//   loc.RegisterString(ELocale::En, "ui.start",      "Start");
//   loc.RegisterString(ELocale::Ja, "ui.start",      "はじめる");
//
//   loc.SetLocale(ELocale::Ja);
//   const char* title = loc.Get("ui.title");   // -> "星の冒険"
//   const char* miss  = loc.Get("ui.missing"); // -> "ui.missing" (key 自身)
//
// 設計選択 (簡素優先):
//   ・**ロケール固定セット**: enum で 11 言語 + Default(=En) を最初から並べておく。
//     市場想定 (日本インディーが世界に出すとき) で英・日・繁中・簡中・韓・仏・独・
//     西・葡・露・伊 が現実的なカバレッジ。将来言語追加は enum 末尾に append、
//     Default = En はそのまま (バイナリ互換目的)。
//   ・**key / value とも const char* 非所有**: ACS の STL 禁止方針 + 文字列ストア
//     導入を避けるため、key と value の寿命は呼び出し側が保証する (リテラル or
//     長寿命バッファ前提)。短命バッファ渡しが dangling になる点は要注意。
//   ・**3 段フォールバック**: Get(key) は
//        現 locale で検索 → 無ければ Default(En) で検索 → それでも無ければ key 自身を返す
//     最後の「key 自身を返す」は開発中に未翻訳箇所を画面で発見しやすくするための
//     ガード (空文字や nullptr を返すと UI 側で別の null チェックが必要になる)。
//   ・**線形探索**: 文字列 ID は通常 100〜2000 のオーダー、locale 切替は起動時 +
//     設定変更時のみで頻発しないため、線形走査で十分。数千 key 規模になったら
//     THashMap 化を検討する。
//   ・**コピー / ムーブ禁止**: localization は通常 1 セッション 1 オブジェクトで
//     運用される。誤って値渡しされて分裂すると翻訳ずれを検知しづらいため、
//     最初から非コピー・非ムーブで固定する。
//   ・**全 noexcept**: 例外不使用方針 (TResult<T,E> + bool 戻り値)。
//
// 範囲外:
//   ・format 引数展開 ("Score: {0}" の {0} 置換)。`Sprintf`/`Format` 層を別途用意する想定。
//   ・複数形 (plural rules) / 性別 (gender) / ICU MessageFormat 相当
//   ・右から左 (RTL) レイアウト判定 (UI 描画層の責務)
//   ・フォントフォールバック (CJK / Cyrillic / Arabic 等のグリフセット切替)
//   ・永続化 / シリアライズ (保存 adapter から loc.json 等を流し込む)
//   ・PO/MO/CSV からの自動取り込み (ツール側で RegisterString 列に変換する想定)
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
