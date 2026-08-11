// SPDX-License-Identifier: Apache-2.0
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
