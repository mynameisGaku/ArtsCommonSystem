// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (Card Game) — DeckSystem (デッキ / 手札 / 捨札 / 除外 / シャッフル)
//
// 1 プレイヤーぶんのカードゲーム状態を「デッキ + 手札 + 捨札 + 除外 (exile)」の 4 ゾーン
// モデルで保持する小型マネージャ。Magic: the Gathering / Hearthstone / Slay the Spire 等
// のデッキ構築系ジャンルが共通して持つコア状態のみを汎用的に扱う。
//
// 想定する位置付け:
//   ・**1 プレイヤー = 1 instance**: 対戦相手や複数 AI プレイヤーが居る場合は
//     `DeckSystem` を人数分作って並べる (本クラス自体は 1 人分のローカル状態のみ)。
//   ・**カード定義 (CardDef) と場の identity (CardId) を分離**:
//     - `CardDef` = 「Lightning Bolt とはどんなカードか」(コスト / レアリティ / 説明文)。
//        起動時に RegisterCard() で 1 回だけ登録、以後 immutable。
//     - `CardId`  = 「場に存在する各カードの 1 枚 1 枚」を識別する 24bit idx + 8bit gen の
//        packed handle。CardDef* を直接持たず、ID 経由で参照するパターン。
//        将来的に「同名カードでも各個に付与効果を持つ」(MtG の +1/+1 カウンタ等) 拡張への
//        ベースになる。Phase 1 (本ファイル) では gen は常に 0、idx は CardDef の登録 index
//        を入れる単純な実装としつつ、struct のレイアウトは確定させておく。
//   ・**ゾーン間遷移はメソッドで明示**: Draw / Discard / Exile / Play / DiscardAllHand 等、
//     カードがどのゾーンを通るかを API レベルで明示する (= ステートマシン的に追跡可能)。
//   ・**Shuffle は決定論再現**: `gameframework/Random.h` (xoshiro128**) を使い、seed 指定で
//     リプレイ / テストを再現可能にする。seed=0 のときは Random::Global() で時刻ベース seed。
//
// 使い方:
//   DeckSystem deck;
//
//   // 起動時にカード定義を登録。
//   deck.RegisterCard({ "card.bolt",  "Lightning Bolt", /*cost*/1, /*rarity*/2,
//                       "spell", "Deal 3 damage to any target.", "ui/card/bolt.png" });
//   deck.RegisterCard({ "card.bear",  "Grizzly Bears",  2, 1, "creature",
//                       "A 2/2 bear.", "ui/card/bears.png" });
//
//   // デッキ構築 (40 枚)。
//   deck.AddToDeck("card.bolt", 4);
//   deck.AddToDeck("card.bear", 8);
//   // ... 他のカードも追加 ...
//
//   // (任意) プレイ時の callback (アニメ起動 / 効果発動 / SE 等)。
//   deck.SetOnPlayCallback(&OnCardPlayed, &game);
//
//   // 試合開始。
//   deck.Shuffle();               // seed=0 = ランダム
//   deck.DrawN(7);                // 初手 7 枚
//
//   // ターン中。
//   deck.PlayCard(2);             // 手札 index 2 を場に出す → callback → discard へ
//   deck.DiscardFromHand(0);      // 手札 index 0 を強制捨札
//   deck.Draw();                  // 1 枚ドロー (デッキが空なら discard を shuffle して reuse)
//
// 設計選択 (Phase 1 = card game kit ベース):
//   ・**CardDef 登録は単一 Array<CardDef>**: カード種別は AAA でも 500〜2000 枚オーダー、
//     線形走査で十分 (Inventory / EconomyDirector と同じ判断)。重複登録は WARN で no-op。
//   ・**所有しない const char***: id / display_name / description / art_path / card_type すべて
//     呼出側 (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     deck / hand / discard / exile の `const char*` 要素は `_cards[].id` を直接指す (= リテラル参照、非所有)。
//   ・**4 ゾーン (deck / hand / discard / exile) は別 Array<const char*>**:
//     - `deck`    : 山札。末尾が「次に引くトップ」(PopBack で O(1) ドロー)。
//     - `hand`    : 手札。順序は登録順 (= 引いた順)、UI の表示位置に対応。
//     - `discard` : 捨札。プレイ済み or 強制捨札されたカード。Reshuffle で deck に戻る。
//     - `exile`   : 完全除外。MtG の "Exile" / Slay the Spire の "Exhaust" 相当、戻らない。
//   ・**Draw は「デッキ末尾」を取る**: 直感に反するように見えるが、`PushBack/PopBack` を使うと
//     O(1) で済む。Shuffle で順序がランダムになるので、どちらの端をトップと呼んでも UX 的に
//     違いはない (= デッキトップは末尾、デッキボトムは先頭、と内部約束)。
//   ・**Draw 自動 reshuffle**: デッキ空時に discard が残っていれば、discard を shuffle して
//     deck に戻し、もう一度 Draw する (Magic / Hearthstone の "fatigue" ではなく、Slay the
//     Spire 等のデッキ循環モデル)。両方とも空なら false。本仕様で「fatigue / Mill 敗北」が
//     必要なゲームは callback 側で `DiscardSize() == 0 && DeckSize() == 0` を検知して別途処理。
//   ・**Play = hand → callback → discard**: PlayCard() は登録された callback を呼んでから
//     discard へ送る。callback は同期実行 (= callback 内で「コストを払う」「effect を起動する」
//     等を完了させてから戻る前提)。失敗 (= 「コスト不足でプレイ不可」) は callback 側で
//     状態を巻き戻す責務とする (Manager 側は「とにかく hand → discard」)。
//   ・**Shuffle は Fisher-Yates (Random::Shuffle)**: `gameframework/Random.h` の汎用 template に
//     委譲する。seed=0 のときは `Random::Global()` を使うので、決定論再現が不要な場合は何も
//     渡さなくて OK。seed != 0 のときはローカル Random instance を作って使う (Global を汚さない)。
//   ・**PlayCallback は単一購読**: STL <functional> 禁止のため C 関数ポインタ + void* user。
//     複数 listener が必要なら呼出側で fan-out。Inventory / Economy と同じパターン。
//   ・**非コピー・非ムーブ + 全 noexcept**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 2+ で):
//   ・「同じカードが場で個別効果を持つ」(MtG カウンタ等) — CardId の gen を使う拡張で対応予定。
//   ・サーチ / scry / look-at-top-N 等の peek 系操作 — 別 API として追加可能。
//   ・複数プレイヤーをまたぐ効果 — GameFlow / 対戦 Manager の責務。
//   ・MtG 形式の「ライブラリ枚数を見せる UI」以外の高度 API — 必要に応じて拡張。
//   ・永続化 (Save/Load) — Pillar J Serialize と統合。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- CardDef: カード 1 種類の定義 (immutable) -----------------------------
// id           : 一意キー (deck/hand/discard/exile 内の const char* から参照される)。
//                文字列リテラル想定 (非所有)。
// display_name : UI 表示名 (非所有)。
// cost         : プレイコスト (マナ / エナジー等。Manager は値保存のみ、解釈しない)。
// rarity       : レアリティ (0=Common, 1=Uncommon, 2=Rare, 3=Mythic 等の慣習。値の意味は呼出側)。
// card_type    : "creature" / "spell" / "skill" 等のタイプ識別子 (非所有、UI フィルタ用)。
// description  : カード説明文 (非所有、UI 表示用)。
// art_path     : カードアートのパス (非所有、レンダラ / UI が解釈)。
struct CardDef {
    const char* id           = nullptr;
    const char* display_name = nullptr;
    u32         cost         = 0;
    u32         rarity       = 0;
    const char* card_type    = nullptr;
    const char* description  = nullptr;
    const char* art_path     = nullptr;
};

// ---- CardId: 場に存在する 1 枚の identity (24bit idx + 8bit gen) -----------
// `_packed == 0` を invalid (default) として扱う。`NodeId` / `ShapeId` と同パターンで、
// 将来的に「同名カードでも個別の付与効果 (カウンタ / 修正値) を持たせたい」拡張で
// idx を CardDef 登録 index、gen を世代カウンタとして使う土台にする。
//
// Phase 1 (本ファイル) では `DeckSystem` 内部での生成は予約のみで、現状は const char*
// ベースの API のみ公開している (PlayCallback も card_id 文字列を渡す)。
struct CardId {
    u32 _packed = 0;

    constexpr CardId() noexcept = default;
    constexpr CardId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index()      const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept { return static_cast<u8>(_packed >> 24); }
    bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(CardId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(CardId o) const noexcept { return _packed != o._packed; }
};

// ---- DeckSystem ------------------------------------------------------------
class DeckSystem {
public:
    // プレイ時 callback。
    //   user       : SetOnPlayCallback で渡したコンテキスト (Manager は所有しない)
    //   card_id    : プレイされたカードの id (リテラル参照、非所有)
    //   hand_index : 元の手札 index (PlayCard 呼び出し時点での値)
    using PlayCallback = void(*)(void* user, const char* card_id, u32 hand_index) noexcept;

    DeckSystem()  noexcept = default;
    ~DeckSystem() noexcept = default;

    DeckSystem(const DeckSystem&)            = delete;
    DeckSystem& operator=(const DeckSystem&) = delete;
    DeckSystem(DeckSystem&&)                 = delete;
    DeckSystem& operator=(DeckSystem&&)      = delete;

    // ---- カード定義 (起動時に 1 度ずつ) -----------------------------------
    // 同 id の 2 重登録は no-op (WARN)。`def.id == nullptr` も no-op。
    void RegisterCard(const CardDef& def) noexcept;

    // 単一 CardDef 取得。未登録 / nullptr は nullptr。
    // 返却ポインタは次の RegisterCard() / ClearAll() で無効化される可能性がある。
    const CardDef* FindCardDef(const char* card_id) const noexcept;

    // ---- デッキ構築 ------------------------------------------------------
    // 指定 card_id をデッキに count 枚追加 (= 山札末尾に push)。
    // 未登録 / nullptr / count==0 は no-op。Shuffle 前提のため push 順は問わない。
    void AddToDeck(const char* card_id, u32 count = 1) noexcept;

    // デッキ (= 山札) のみクリアする。hand / discard / exile はそのまま。
    void ClearDeck() noexcept;

    // ---- ゾーンサイズ照会 ------------------------------------------------
    u32 DeckSize()    const noexcept;
    u32 HandSize()    const noexcept;
    u32 DiscardSize() const noexcept;
    u32 ExileSize()   const noexcept;

    // ---- シャッフル ------------------------------------------------------
    // デッキを Fisher-Yates でシャッフル。
    //   seed == 0: `Random::Global()` を使う (= 真にランダム、時刻ベース)
    //   seed != 0: ローカルに Random instance を作って使う (= 決定論再現、Global は汚さない)
    void Shuffle(u32 seed = 0) noexcept;

    // ---- ドロー ----------------------------------------------------------
    // デッキトップ (= 内部実装は山札末尾) から 1 枚ドロー、手札末尾に追加。
    // デッキ空のときは discard を shuffle して deck に戻し、もう一度 ドロー。
    // それでも空 (= discard も空) なら false。
    bool Draw() noexcept;

    // n 枚ドロー。「引けただけ引く」設計 (= 途中で deck/discard が両方空になっても、
    // それまでに引けた枚数を保持して終了)。
    //   n == 0          : true (no-op)
    //   全部引けた     : true
    //   途中で空になった: false
    bool DrawN(u32 n) noexcept;

    // ---- ゾーン操作 ------------------------------------------------------
    // 手札 index 指定で捨札へ移動。範囲外は false。
    bool DiscardFromHand(u32 hand_index) noexcept;

    // 手札 index 指定で exile (完全除外) へ移動。範囲外は false。
    bool ExileFromHand(u32 hand_index) noexcept;

    // 手札を全部 discard。手札が空なら true (no-op 扱い)。
    bool DiscardAllHand() noexcept;

    // 手札 index のカードを「プレイ」する: PlayCallback を発火 → discard へ。
    // callback が nullptr のときも discard 移動は実行される。
    // 範囲外は false (callback も発火しない)。
    bool PlayCard(u32 hand_index) noexcept;

    // ---- 手札照会 --------------------------------------------------------
    // 手札 index のカード id。範囲外は nullptr。
    // 返却ポインタは次の RegisterCard() / ClearAll() で無効化される可能性がある。
    const char* HandCardAt(u32 index) const noexcept;

    // ---- コールバック ----------------------------------------------------
    // cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnPlayCallback(PlayCallback cb, void* user) noexcept;

    // ---- 全リセット (デバッグ / シーン切替時) ----------------------------
    // カード定義 + 全ゾーン + callback をクリア。
    void ClearAll() noexcept;

private:
    // card_id → _cards 内 index の per-byte 線形検索。未検出は ~0u。
    u32 FindCardSlot(const char* card_id) const noexcept;

    // カード定義 (起動時 immutable)。
    Array<CardDef> _cards;

    // 4 ゾーン。各要素は `_cards[].id` を直接指すリテラル参照ポインタ (非所有)。
    // deck は末尾がトップ、PushBack / PopBack で O(1) ドロー。
    Array<const char*> _deck;
    Array<const char*> _hand;
    Array<const char*> _discard;
    Array<const char*> _exile;

    // プレイ callback (C 関数ポインタ + user)。Manager は user を所有しない。
    PlayCallback _on_play      = nullptr;
    void*        _on_play_user = nullptr;
};

} // namespace acs::game
