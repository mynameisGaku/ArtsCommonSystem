// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — LocalizationDirector (i18n 文字列辞書)
//
// 「ロケール + 文字列 ID」を「翻訳済みの const char*」に解決する小型ストア。
// UI ラベル / セリフ / メニュー文言 / エラーメッセージ等を、ゲームロジックから
// 言語非依存に引けるようにする。出荷タイトルの多言語対応で必要になる完成度
// パーツの 1 つ (v7 = Accessibility / Localization 強化フェーズ)。
//
// 使い方:
//   LocalizationDirector loc;
//   loc.RegisterString(Locale::En, "ui.title",      "Adventure of Claude");
//   loc.RegisterString(Locale::Ja, "ui.title",      "クロードの冒険");
//   loc.RegisterString(Locale::En, "ui.start",      "Start");
//   loc.RegisterString(Locale::Ja, "ui.start",      "はじめる");
//
//   loc.SetLocale(Locale::Ja);
//   const char* title = loc.Get("ui.title");   // -> "クロードの冒険"
//   const char* miss  = loc.Get("ui.missing"); // -> "ui.missing" (key 自身)
//
// 設計選択 (v7 Phase 1 = 簡素優先):
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
//     設定変更時のみで頻発しないため、線形走査で十分。HashMap 化は Phase 2 で
//     計測してから検討する (TODO: 数千 key 規模になったら必要)。
//   ・**コピー / ムーブ禁止**: localization は通常 1 セッション 1 オブジェクトで
//     運用される。誤って値渡しされて分裂すると翻訳ずれを検知しづらいため、
//     最初から非コピー・非ムーブで固定する。
//   ・**全 noexcept**: 例外不使用方針 (Result<T,E> + bool 戻り値)。
//
// 範囲外 (Phase 2+ で):
//   ・format 引数展開 ("Score: {0}" の {0} 置換)。`Sprintf`/`Format` 層を別途用意する想定。
//   ・複数形 (plural rules) / 性別 (gender) / ICU MessageFormat 相当
//   ・右から左 (RTL) レイアウト判定 (Pillar Q Polish 側の UI 描画で扱う)
//   ・フォントフォールバック (CJK / Cyrillic / Arabic 等のグリフセット切替)
//   ・永続化 / シリアライズ (Pillar J Serialize 側で扱う、loc.json 等から流し込む)
//   ・PO/MO/CSV からの自動取り込み (ツール側で RegisterString 列に変換する想定)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ロケール識別子。市場想定の主要 11 言語 + Default(=En)。
// 値は将来の永続化 (Settings) との互換性のため u32 で連番固定。新規追加は末尾 append。
enum class Locale : u32 {
    En      = 0,    // English
    Ja      = 1,    // 日本語
    Fr      = 2,    // Français
    De      = 3,    // Deutsch
    Es      = 4,    // Español
    ZhCn    = 5,    // 简体中文
    ZhTw    = 6,    // 繁體中文
    Ko      = 7,    // 한국어
    Pt      = 8,    // Português
    Ru      = 9,    // Русский
    It      = 10,   // Italiano

    Default = En,   // 未翻訳時 / 起動時の既定ロケール
};

class LocalizationDirector {
public:
    LocalizationDirector()  noexcept = default;
    ~LocalizationDirector() noexcept = default;

    LocalizationDirector(const LocalizationDirector&)            = delete;
    LocalizationDirector& operator=(const LocalizationDirector&) = delete;
    LocalizationDirector(LocalizationDirector&&)                 = delete;
    LocalizationDirector& operator=(LocalizationDirector&&)      = delete;

    // ----- ロケール設定 -----
    // 現在の取得対象ロケールを切り替える (UI 言語切替で呼ばれる)。
    // 切替後 Get(key) は新 locale で検索 → Default → key の順に解決される。
    void   SetLocale    (Locale loc) noexcept;
    Locale CurrentLocale() const noexcept;

    // ----- 文字列登録 -----
    // 指定 locale に key→value を 1 件登録する。key/value は所有しない
    // (寿命は呼び出し側保証、リテラル or 長寿命バッファ前提)。
    // 同 (locale, key) の重複は禁止せず後追い append (Get は先頭一致を返す)。
    // key == nullptr は no-op で防御 (流入経路で nullptr が混じっても壊さない)。
    void RegisterString(Locale loc, const char* key, const char* value) noexcept;

    // ----- 取得 -----
    // 現 locale で key を引く。無ければ Default(En) で引く。それでも無ければ
    // key 自身を返す (nullptr / 空文字ではなく key を返すのは未翻訳箇所を
    // 画面で発見しやすくするための意図的な挙動)。
    // key == nullptr の場合は空文字 "" を返す (UI 側の null チェック省略目的)。
    const char* Get(const char* key) const noexcept;

    // 指定 locale で key を引く。当該 locale に無ければ key 自身を返す
    // (Default フォールバックは行わない、明示指定なので素直に欠落を返す)。
    // key == nullptr の場合は空文字 "" を返す。
    const char* GetForLocale(Locale loc, const char* key) const noexcept;

    // 現 locale に key が登録されているか (Default フォールバックは見ない)。
    // key == nullptr は false。
    bool Has(const char* key) const noexcept;

    // 指定 locale の登録件数。
    u32 KeyCount(Locale loc) const noexcept;

    // ----- 削除 -----
    // 全 locale の全エントリを削除。
    void Clear() noexcept;
    // 指定 locale のエントリのみ削除 (他 locale は保持)。
    void ClearLocale(Locale loc) noexcept;

private:
    // 1 件のエントリ。locale + key + value を持つ。key/value は非所有 const char*。
    struct LocaleEntry {
        Locale      locale = Locale::Default;
        const char* key    = nullptr;
        const char* value  = nullptr;
    };

    // (locale, key) 一致 entry の index を返す。未検出は -1。
    isize FindIndex(Locale loc, const char* key) const noexcept;

    Locale              _current = Locale::Default;
    Array<LocaleEntry>  _entries;
};

} // namespace acs::game
