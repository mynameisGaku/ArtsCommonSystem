// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (Card CGame) — CDeckSystem (デッキ / 手札 / 捨札 / 除外 / シャッフル)
//
// 1 プレイヤーぶんのカードゲーム状態を「デッキ + 手札 + 捨札 + 除外 (exile)」の 4 ゾーン
// モデルで保持する小型マネージャ。Magic: the Gathering / Hearthstone / Slay the Spire 等
// のデッキ構築系ジャンルが共通して持つコア状態のみを汎用的に扱う。
//
// 想定する位置付け:
//   ・**1 プレイヤー = 1 instance**: 対戦相手や複数 AI プレイヤーが居る場合は
//     `CDeckSystem` を人数分作って並べる (本クラス自体は 1 人分のローカル状態のみ)。
//   ・**カード定義 (FCardDef) と場の identity (FCardId) を分離**:
//     - `FCardDef` = 「Lightning Bolt とはどんなカードか」(コスト / レアリティ / 説明文)。
//        起動時に RegisterCard() で 1 回だけ登録、以後 immutable。
//     - `FCardId`  = 「場に存在する各カードの 1 枚 1 枚」を識別する 24bit idx + 8bit gen の
//        packed handle。FCardDef* を直接持たず、ID 経由で参照するパターン。
//        将来的に「同名カードでも各個に付与効果を持つ」(MtG の +1/+1 カウンタ等) 拡張への
//        ベースになる。現状は gen は常に 0、idx は FCardDef の登録 index
//        を入れる単純な実装としつつ、struct のレイアウトは確定させておく。
//   ・**ゾーン間遷移はメソッドで明示**: Draw / Discard / Exile / Play / DiscardAllHand 等、
//     カードがどのゾーンを通るかを API レベルで明示する (= ステートマシン的に追跡可能)。
//   ・**Shuffle は決定論再現**: `gameframework/Random.h` (xoshiro128**) を使い、seed 指定で
//     リプレイ / テストを再現可能にする。seed=0 のときは FRandom::Global() で時刻ベース seed。
//
// 使い方:
//   CDeckSystem deck;
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
// 設計選択 (card game kit ベース):
//   ・**FCardDef 登録は単一 TArray<FCardDef>**: カード種別は AAA でも 500〜2000 枚オーダー、
//     線形走査で十分 (Inventory / CEconomyDirector と同じ判断)。重複登録は WARN で no-op。
//   ・**所有しない const char***: id / display_name / description / art_path / card_type すべて
//     呼出側 (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     deck / hand / discard / exile の `const char*` 要素は `m_Cards[].id` を直接指す (= リテラル参照、非所有)。
//   ・**4 ゾーン (deck / hand / discard / exile) は別 TArray<const char*>**:
//     - `deck`    : 山札。末尾が「次に引くトップ」(Pop で O(1) ドロー)。
//     - `hand`    : 手札。順序は登録順 (= 引いた順)、UI の表示位置に対応。
//     - `discard` : 捨札。プレイ済み or 強制捨札されたカード。Reshuffle で deck に戻る。
//     - `exile`   : 完全除外。MtG の "Exile" / Slay the Spire の "Exhaust" 相当、戻らない。
//   ・**Draw は「デッキ末尾」を取る**: 直感に反するように見えるが、`Add/Pop` を使うと
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
//   ・**Shuffle は Fisher-Yates (FRandom::Shuffle)**: `gameframework/Random.h` の汎用 template に
//     委譲する。seed=0 のときは `FRandom::Global()` を使うので、決定論再現が不要な場合は何も
//     渡さなくて OK。seed != 0 のときはローカル FRandom instance を作って使う (Global を汚さない)。
//   ・**PlayCallback は単一購読**: STL <functional> 禁止のため C 関数ポインタ + void* user。
//     複数 listener が必要なら呼出側で fan-out。Inventory / Economy と同じパターン。
//   ・**非コピー・非ムーブ + 全 noexcept**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外:
//   ・「同じカードが場で個別効果を持つ」(MtG カウンタ等) — FCardId の gen を使う拡張で対応予定。
//   ・サーチ / scry / look-at-top-N 等の peek 系操作 — 別 API として追加可能。
//   ・複数プレイヤーをまたぐ効果 — CGameFlow / 対戦 Manager の責務。
//   ・MtG 形式の「ライブラリ枚数を見せる UI」以外の高度 API — 必要に応じて拡張。
//   ・永続化 (Save/Load) — Pillar J Serialize と統合。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * カード 1 種類の定義 (immutable)。
 *
 * @details
 * RegisterCard() で起動時に 1 回だけ登録し、以後不変。const char* 系メンバは
 * すべて呼出側 (ゲームコード / リソースバンドル) 所有の文字列リテラル想定 (非所有)。
 */
struct FCardDef {
    /** 一意キー (deck/hand/discard/exile 内の const char* から参照される、非所有)。 */
    const char* id           = nullptr;

    /** UI 表示名 (非所有)。 */
    const char* display_name = nullptr;

    /** プレイコスト (マナ / エナジー等。Manager は値保存のみで解釈しない)。 */
    u32         cost         = 0;

    /** レアリティ (0=Common, 1=Uncommon … の慣習。値の意味は呼出側が定義)。 */
    u32         rarity       = 0;

    /** タイプ識別子 ("creature" / "spell" / "skill" 等。非所有、UI フィルタ用)。 */
    const char* card_type    = nullptr;

    /** カード説明文 (非所有、UI 表示用)。 */
    const char* description  = nullptr;

    /** カードアートのパス (非所有、レンダラ / UI が解釈)。 */
    const char* art_path     = nullptr;
};

/**
 * 場に存在する 1 枚の identity (24bit idx + 8bit gen の packed handle)。
 *
 * @details
 * m_Packed == 0 を invalid (default) として扱う。FNodeId / FShapeId と同パターンで、
 * 将来「同名カードでも個別の付与効果 (カウンタ / 修正値) を持たせたい」拡張で idx を
 * FCardDef 登録 index、gen を世代カウンタとして使う土台にする。現状は内部での生成は
 * 予約のみで、公開 API は const char* ベース (PlayCallback も card_id 文字列を渡す)。
 */
struct FCardId {
    /** 24bit index + 8bit generation を packed した値 (0 = invalid)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な FCardId を構築する。 */
    constexpr FCardId() noexcept = default;

    /**
     * index と generation から FCardId を構築する。
     *
     * @param index 下位 24bit に格納する index (上位はマスクで切り捨て)。
     * @param gen 上位 8bit に格納する世代カウンタ。
     */
    constexpr FCardId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * 下位 24bit の index を返す。
     *
     * @return index 部。
     */
    constexpr u32  Index()      const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * 上位 8bit の世代カウンタを返す。
     *
     * @return generation 部。
     */
    constexpr u8   Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 同一 handle かを比較する。
     *
     * @param o 比較対象。
     * @return packed 値が等しければ true。
     */
    constexpr bool operator==(FCardId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 異なる handle かを比較する。
     *
     * @param o 比較対象。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FCardId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * 1 プレイヤーぶんのカードゲーム状態を 4 ゾーンモデルで保持する小型マネージャ。
 *
 * @details
 * デッキ + 手札 + 捨札 + 除外 (exile) の 4 ゾーンを管理し、Draw / Discard / Exile /
 * Play / DiscardAllHand などゾーン間遷移を API レベルで明示する。カード定義 (FCardDef)
 * は RegisterCard で登録、各ゾーンは m_Cards[].id を指す非所有 const char* を保持する。
 * Shuffle は Fisher-Yates (FRandom) で seed 指定により決定論再現可能。非コピー・非ムーブ。
 */
class CDeckSystem {
public:
    /**
     * カードがプレイされたときに呼ばれる callback の型。
     *
     * @param user SetOnPlayCallback で渡したコンテキスト (Manager は所有しない)。
     * @param card_id プレイされたカードの id (リテラル参照、非所有)。
     * @param hand_index PlayCard 呼び出し時点での元の手札 index。
     */
    using PlayCallback = void(*)(void* user, const char* card_id, u32 hand_index) noexcept;

    /** 空状態で構築する。 */
    CDeckSystem()  noexcept = default;

    /** 破棄する (TArray が内部バッファを解放、const char* は非所有なので Free 不要)。 */
    ~CDeckSystem() noexcept = default;

    /** コピー禁止。 */
    CDeckSystem(const CDeckSystem&)            = delete;

    /** コピー代入も禁止。 */
    CDeckSystem& operator=(const CDeckSystem&) = delete;

    /** ムーブ禁止。 */
    CDeckSystem(CDeckSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDeckSystem& operator=(CDeckSystem&&)      = delete;

    /**
     * カード定義を登録する (起動時に 1 度ずつ)。
     *
     * @details 同 id の 2 重登録は WARN ログを出して no-op。def.id == nullptr も no-op。
     * @param def 登録するカード定義 (値コピーで保持)。
     */
    void RegisterCard(const FCardDef& def) noexcept;

    /**
     * card_id から単一カード定義を取得する。
     *
     * @details 返却ポインタは次の RegisterCard() / ClearAll() で無効化される可能性がある。
     * @param card_id 検索するカード id。
     * @return 見つかった FCardDef へのポインタ。未登録 / nullptr なら nullptr。
     */
    const FCardDef* FindCardDef(const char* card_id) const noexcept;

    /**
     * 指定 card_id をデッキ (= 山札末尾) に count 枚追加する。
     *
     * @details Shuffle 前提のため push 順は問わない。未登録 / nullptr / count==0 は no-op。
     * @param card_id 追加するカード id (登録済みであること)。
     * @param count 追加枚数 (既定 1)。
     */
    void AddToDeck(const char* card_id, u32 count = 1) noexcept;

    /** デッキ (= 山札) のみクリアする (hand / discard / exile はそのまま)。 */
    void ClearDeck() noexcept;

    /**
     * デッキ (山札) の枚数を返す。
     *
     * @return 山札の枚数。
     */
    u32 DeckSize()    const noexcept;

    /**
     * 手札の枚数を返す。
     *
     * @return 手札の枚数。
     */
    u32 HandSize()    const noexcept;

    /**
     * 捨札の枚数を返す。
     *
     * @return 捨札の枚数。
     */
    u32 DiscardSize() const noexcept;

    /**
     * 除外 (exile) の枚数を返す。
     *
     * @return 除外ゾーンの枚数。
     */
    u32 ExileSize()   const noexcept;

    /**
     * デッキを Fisher-Yates でシャッフルする。
     *
     * @details seed == 0 は FRandom::Global() を使い真にランダム (時刻ベース)、seed != 0 は
     * ローカル FRandom instance を作って決定論再現する (Global は汚さない)。
     * @param seed シャッフル用 seed (既定 0 = ランダム)。
     */
    void Shuffle(u32 seed = 0) noexcept;

    /**
     * デッキトップ (= 内部実装は山札末尾) から 1 枚ドローし、手札末尾に追加する。
     *
     * @details デッキ空のときは discard を shuffle して deck に戻し、もう一度ドローする。
     * @return ドローできたら true。deck も discard も空なら false。
     */
    bool Draw() noexcept;

    /**
     * n 枚ドローする (引けただけ引く設計)。
     *
     * @details 途中で deck/discard が両方空になっても、それまでに引けた枚数は保持して終了する。
     * @param n ドロー枚数 (0 は no-op で true)。
     * @return 全部引けたら true。途中で空になったら false。
     */
    bool DrawN(u32 n) noexcept;

    /**
     * 手札 index 指定のカードを捨札へ移動する。
     *
     * @param hand_index 移動する手札 index。
     * @return 移動できたら true。範囲外なら false。
     */
    bool DiscardFromHand(u32 hand_index) noexcept;

    /**
     * 手札 index 指定のカードを exile (完全除外) へ移動する。
     *
     * @param hand_index 移動する手札 index。
     * @return 移動できたら true。範囲外なら false。
     */
    bool ExileFromHand(u32 hand_index) noexcept;

    /**
     * 手札を全部 discard へ移動する。
     *
     * @details 手札が空でも no-op 扱いで成功させる (ターン終了処理を統一して書きやすくする)。
     * @return 常に true。
     */
    bool DiscardAllHand() noexcept;

    /**
     * 手札 index のカードを「プレイ」する (PlayCallback を発火 → discard へ)。
     *
     * @details callback が nullptr のときも discard 移動は実行される。callback 内で hand が
     * 変動する可能性に備え、移動前に範囲を再チェックする。
     * @param hand_index プレイする手札 index。
     * @return プレイできたら true。範囲外なら false (callback も発火しない)。
     */
    bool PlayCard(u32 hand_index) noexcept;

    /**
     * 手札 index のカード id を返す。
     *
     * @details 返却ポインタは次の RegisterCard() / ClearAll() で無効化される可能性がある。
     * @param index 参照する手札 index。
     * @return カード id (リテラル参照、非所有)。範囲外なら nullptr。
     */
    const char* HandCardAt(u32 index) const noexcept;

    /**
     * プレイ callback を設定する。
     *
     * @details cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
     * @param cb 発火する callback (nullptr で無効化)。
     * @param user callback に渡すコンテキスト (非所有)。
     */
    void SetOnPlayCallback(PlayCallback cb, void* user) noexcept;

    /** カード定義 + 全ゾーン + callback をクリアする (デバッグ / シーン切替時)。 */
    void ClearAll() noexcept;

private:
    /**
     * card_id を m_Cards から per-byte 線形検索する。
     *
     * @param card_id 検索するカード id。
     * @return 見つかった index。未検出 / nullptr は ~0u (kNotFound)。
     */
    u32 FindCardSlot(const char* card_id) const noexcept;

    /** カード定義 (起動時 immutable、値で所有)。 */
    TArray<FCardDef> m_Cards;

    /** 山札 (末尾がトップ、Add / Pop で O(1) ドロー、要素は非所有)。 */
    TArray<const char*> m_Deck;

    /** 手札 (順序は引いた順、要素は m_Cards[].id を指す非所有参照)。 */
    TArray<const char*> m_Hand;

    /** 捨札 (Reshuffle で deck に戻る、要素は非所有参照)。 */
    TArray<const char*> m_Discard;

    /** 除外ゾーン (戻らない、要素は非所有参照)。 */
    TArray<const char*> m_Exile;

    /** プレイ callback (C 関数ポインタ、nullptr で無効)。 */
    PlayCallback m_OnPlay      = nullptr;

    /** callback に渡すコンテキスト (Manager は所有しない)。 */
    void*        m_OnPlayUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDeckSystem = CDeckSystem;

} // namespace acs::game
