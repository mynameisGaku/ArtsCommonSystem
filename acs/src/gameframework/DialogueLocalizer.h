// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム — FDialogueLocalizer (Dialogue × Localization 橋渡し)
//
// FDialogueSystem は本文 (`DialogueLine::text`) を const char* で参照のみ持つ。
// 多言語タイトルでは「同じシナリオ行を locale ごとに別文字列で出したい」
// = ロケール切替で本文 / 選択肢の表示を差し替えたい、という要件が必ず出る。
//
// FLocalizationDirector は (locale, key) → 翻訳済み const char* の単純な辞書で、
// 一方 FDialogueSystem は (line_index, speaker, text, cps) の state holder。
// この二者を直接結合すると FDialogueSystem に locale の概念が漏れて
// 設計が崩れる (= FDialogueSystem の単一責務原則違反)。
//
// FDialogueLocalizer は両者を**疎結合**で繋ぐ橋渡し helper:
//   ・「行 index → speaker_id + line_key + cps」を保持 (本文は持たない)
//   ・「行 index → 選択肢 (text_key + 次行 index) 群」を保持
//   ・StartFromLine(i, cb) で現 locale の翻訳テキストを解決し callback 発火
//   ・callback の受け手 (= UI 層 or FDialogueSystem の AddLine ラッパ) で
//     実際の本文反映を行う
//
// 設計選択:
//   ・**Director を所有しない**: SetLocalizer で外部参照を差し込み、null で detach。
//     FLocalizationDirector は通常 1 セッション 1 インスタンスで他システムと共有
//     される (Pillar S i18n の中心)。ここで所有すると共有が壊れる。
//   ・**選択肢は line_index に紐づくフラット配列**: FDialogueSystem の choices
//     データ構造と同じ形 (ChoicesAt の (start, count) スライス) を踏襲。
//     線形検索だが典型シナリオで N < 数百のため十分。
//   ・**callback 方式 (関数ポインタ + void* user)**: std::function 禁止方針に
//     従う既存 GameFramework 規約 (FCinematicsDirector / FPauseDirector 等で
//     既に同形 callback を採用)。
//   ・**speaker_id も localization key 扱い**: 発話者名も翻訳対象 (例: "char.alice"
//     → "アリス" / "Alice")。FLocalizationDirector::Get の 3 段フォールバックで
//     未翻訳時は key 自身が返るので空欄事故は起きない。
//   ・**StartFromLine は state を持たない**: 「指定行を 1 回解決して cb を呼ぶ」
//     だけ。進行状態は FDialogueSystem 側が持つ責務分離 (= 二重 state を作らない)。
//   ・**非コピー・非ムーブ + 全 noexcept**: GameFramework 規約に整合。
//
// 想定使い方:
//   FDialogueLocalizer dl;
//   LocalizedDialogueLine l0{"char.alice", "scene1.line0", 30.0f};
//   dl.RegisterLine(l0);
//   LocalizedDialogueChoice ch[2] = { {"scene1.choice0", 1}, {"scene1.choice1", 2} };
//   dl.RegisterChoice(0, ch, 2);
//
//   dl.SetLocalizer(&loc);
//   dl.StartFromLine(0, [](void*, const char* sp, const char* tx, f32 cps) noexcept {
//       // sp = "アリス", tx = "こんにちは、勇者よ。", cps = 30.0f
//       // → caller 側で FDialogueSystem::AddLine 等に流す
//   }, nullptr);
//
// 範囲外 (将来 phase で):
//   ・format 引数展開 ("{0} HP" の {0} 置換) → FLocalizationDirector 側 Phase 2 範疇
//   ・スクリプト / JSON からの自動取り込み → ツール側で Register* 列に変換する想定
//   ・フォントフォールバック / RTL → Pillar Q Polish 側の UI 描画責務
//
// 参考: gameframework/FDialogueSystem.h, gameframework/FLocalizationDirector.h
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 前方宣言: ヘッダ依存を最小化 (FLocalizationDirector.h を巻き込まない)
class FLocalizationDirector;

// 1 行ぶんのローカライズ可能ダイアログメタ。本文は持たず、key 経由で
// FLocalizationDirector から取得する。speaker_id も翻訳キー扱い。
// const char* は非所有 (literal / 長寿命バッファ前提)。
struct LocalizedDialogueLine {
    const char* speaker_id    = nullptr;   // 発話者名の翻訳 key (nullptr 可 = ナレーション)
    const char* line_key      = nullptr;   // 本文の翻訳 key (nullptr 可 = 空行)
    f32         type_speed_cps = 30.0f;    // タイプ速度 (DialogueLine と同義、<=0 で瞬時)
};

// 1 つの選択肢メタ。本文 (表示テキスト) は翻訳 key 経由。
// next_line_index が範囲外 (>= LineCount) なら「ダイアログ終了」を表現する
// (DialogueChoice::next_line_index と同契約)。
struct LocalizedDialogueChoice {
    const char* text_key        = nullptr;          // 選択肢表示の翻訳 key
    u32         next_line_index = 0xFFFFFFFFu;      // 範囲外で「終了」を表現
};

class FDialogueLocalizer {
public:
    // 現 locale で speaker / text を解決して受け取る callback。
    // std::function は使わず関数ポインタ + user pointer 形式
    // (GameFramework 既存 callback 規約と整合)。
    // speaker / text とも常に非 nullptr (FLocalizationDirector::Get の契約により
    // 未翻訳時は key 自身が返るため)。Localizer が未設定のときは
    // speaker_id / line_key そのもの (or 空文字) が返る。
    using BindCallback = void(*)(void* user, const char* speaker, const char* text, f32 cps) noexcept;

    FDialogueLocalizer()  noexcept = default;
    ~FDialogueLocalizer() noexcept = default;

    FDialogueLocalizer(const FDialogueLocalizer&)            = delete;
    FDialogueLocalizer& operator=(const FDialogueLocalizer&) = delete;
    FDialogueLocalizer(FDialogueLocalizer&&)                 = delete;
    FDialogueLocalizer& operator=(FDialogueLocalizer&&)      = delete;

    // ----- 登録 -----
    // 行を末尾に追加。挿入順に index が振られる (0,1,2,...)。
    void RegisterLine(const LocalizedDialogueLine& line) noexcept;

    // at_line_index 行に紐づく選択肢群を登録。
    // 同じ line に複数回 RegisterChoice すると 2 回目以降は無視 (= 上書き禁止)。
    // count == 0 / choices == nullptr / at_line_index 範囲外 は no-op。
    // (FDialogueSystem::AddChoices と同契約)
    void RegisterChoice(u32 at_line_index,
                        const LocalizedDialogueChoice* choices, u32 count) noexcept;

    // ----- Localizer 接続 -----
    // 翻訳ソースを差し込む。nullptr で detach (以降は key 自身が返る挙動)。
    // 所有しないので寿命は呼び出し側保証。
    void SetLocalizer(FLocalizationDirector* loc) noexcept;

    // ----- 解決 -----
    // 指定 line index の speaker / 本文を現 locale で解決し cb を発火。
    // line_index が範囲外 / cb == nullptr の場合は no-op。
    // 解決結果が空翻訳 (value == nullptr の登録) でも空文字 "" を渡し、
    // callback 側で nullptr チェックを書かなくて済むようにする。
    void StartFromLine(u32 line_index, BindCallback cb, void* user) noexcept;

    // ----- アクセサ -----
    // 登録済み行数。
    u32 LineCount() const noexcept;

    // 全行 / 全選択肢を破棄。Localizer 参照は保持する
    // (= 再利用前提、再 RegisterLine で同 localizer を使いたいケースが大半)。
    void Clear() noexcept;

private:
    // 「line_index 直後に提示する選択肢」の範囲記録
    // (FDialogueSystem::ChoicesAt と同じ形)
    struct ChoicesAt {
        u32 line_index   = 0;
        u32 choice_start = 0;
        u32 choice_count = 0;
    };

    TArray<LocalizedDialogueLine>   _lines;
    TArray<ChoicesAt>               _choices_at;     // line_index 昇順想定 (線形検索)
    TArray<LocalizedDialogueChoice> _all_choices;    // 全 choice をフラットに保持

    FLocalizationDirector* _localizer = nullptr;     // 非所有、null で detach
};

} // namespace acs::game
