// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CEconomyDirector (in-game 通貨 + shop 管理)
//
// 通貨残高 (soft / premium) + 固定価格 shop アイテム + 取引履歴 (callback) を
// まとめた小型マネージャ。CEntitlementRegistry / CSeasonPass と一緒に Pillar O の
// 「ストア / LiveOps 系」三兄弟を構成する。CEntitlementRegistry は「持っているか」、
// CSeasonPass は「期間内に xp で進行」、CEconomyDirector は「通貨でアイテムを
// 直接購入する」を担当する。
//
// 倫理方針 (重要):
//   ・本クラスは **cosmetic only** を強く推奨する。装備性能 / ダメージ倍率 /
//     獲得経験値ブースター等、コア体験に影響するアイテムを通貨で売る設計は
//     pay-to-win に直結するため、ACS としては明示的に反対する立場
//     (CEntitlementRegistry / CSeasonPass の方針を継承)。FShopItem::cosmetic_only は
//     「この商品が cosmetic に閉じている」ことを開発者が宣言するフラグであり、
//     強制機構ではない (Manager は値を保存して照会できるようにするだけ)。
//   ・**loot box (中身がランダムで対価が確率に依存する仕組み) は明示的に拒絶**:
//     本 API には乱数要素を一切持たせず、すべて「払うと必ず指定の cosmetic が
//     得られる、固定価格方式」に限定する。在庫 (stock_remaining) は限定販売の
//     在庫管理であり、確率ガチャではない。
//   ・**premium 通貨でゲームプレイ加速を売らない**: FCurrencyDef::is_premium は
//     「リアル課金で得られる通貨か」のメタ情報 (UI で 「¥」 アイコン表示等の
//     判定に使う) であり、premium 通貨で買えるアイテムも cosmetic_only に
//     閉じることを推奨する。
//
// 想定する位置付け:
//   ・Pillar O (CEntitlementRegistry) との違い:
//     - CEntitlementRegistry は「DLC / CSeasonPass / 引換コード」等の永続権利フラグ。
//     - CEconomyDirector は「ゲーム内通貨で売買される消費財 / cosmetic」を扱う。
//       購入結果として cosmetic を解放する場合は、PurchaseCallback で
//       CEntitlementRegistry::Add() を呼ぶ橋渡しを呼出側で実装する想定。
//   ・Pillar O (CSeasonPass) との違い:
//     - CSeasonPass は xp ベースの tier 進行 (時間と遊びで貯まる)。
//     - CEconomyDirector は通貨ベースの即時購入 (払えば即時取得)。
//   ・Pillar S (Storefront) との違い:
//     - Storefront はリアル課金プラットフォーム (Steam / EOS / 家庭機 SDK) の
//       購入トランザクション。
//     - CEconomyDirector はゲーム内通貨での売買のみを扱う。premium 通貨を
//       リアル課金で得るフローは Pillar S 側で実装し、AddToBalance() で
//       残高を増やしてもらう想定 (本クラスはストア非依存)。
//
// 使い方:
//   CEconomyDirector ed;
//
//   // 通貨定義。
//   ed.RegisterCurrency({ "gold",    "Gold",    false });  // soft (ゲーム内獲得)
//   ed.RegisterCurrency({ "gems",    "Gems",    true  });  // premium (リアル課金で増える)
//   ed.SetBalance("gold", 1000);
//
//   // shop アイテム定義。
//   ed.RegisterItem({ "skin.knight_red",  "Red Knight Skin",  "gold", 500,  ~0u, true });
//   ed.RegisterItem({ "frame.gold_star",  "Gold Star Frame",  "gems", 50,   100, true });
//
//   // 購入。
//   if (ed.PurchaseItem("skin.knight_red")) {
//       // 成功 → cosmetic 解放 (CEntitlementRegistry 側へ橋渡し)
//   }
//
//   // (任意) 購入結果コールバック。
//   ed.SetOnPurchaseCallback(&OnPurchase, user_data);
//
// 設計選択 (Pillar O):
//   ・**通貨と残高は並行 TArray**: FCurrencyDef を TArray<FCurrencyDef> に、残高を
//     TArray<u32> に同 index で 1:1 で持つ。CEntitlementRegistry の id 比較と同じく
//     const char* per-byte 線形検索。通貨種別はゲーム 1 セッションで通常 2〜5、
//     多くても 10 を超えない想定なので線形で十分。
//   ・**FShopItem は単一 TArray**: 商品数は AAA でも 100〜500 程度のオーダー、
//     線形走査で十分。検索はすべて item_id 文字列。
//   ・**所有しない const char***: id / display_name / currency_id すべて呼出側
//     (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     CEconomyDirector はコピーしない (STL <string> 禁止)。
//   ・**price は u32**: 通貨残高も u32。AAA 級でも通常範囲を超えない。
//     不足チェックは u32 同士の単純比較。
//   ・**stock_remaining は u32**: ~0u (= 0xFFFFFFFF) を「無制限」の哨兵値として
//     扱う。RegisterItem 時に ~0u を渡すと「在庫無制限」、それ以外の値は購入毎に
//     1 ずつ減算する。stock_remaining == 0 は「売り切れ」(購入失敗)。
//   ・**PurchaseItem は冪等ではない**: 残高 + 在庫を消費するため、毎回 side effect が
//     生じる。失敗時 (残高不足 / 在庫切れ / 不明 id / 通貨不明) は false を返し
//     side effect は発生させない (= atomic に成功 or 全て不変)。
//   ・**PurchaseCallback は 1 個固定 + user pointer**: STL <functional> 禁止のため、
//     C 関数ポインタ + void* user. 複数 listener が必要なら呼出側で fan-out。
//     成功・失敗両方 (bool success) で通知し、UI の「購入失敗トースト」も
//     コールバック側で出せるようにする。
//   ・**重複登録は黙って弾く + WARN**: 同 id の 2 重 RegisterCurrency /
//     RegisterItem は no-op (アセット二重ロード保護)。CEntitlementRegistry / CSeasonPass /
//     CAchievementManager と同じパターン。
//   ・**取引履歴は callback 経由のみ**: 履歴 TArray を内蔵してメモリを増やすより、
//     呼出側 (= Analytics / Pillar T Community) でログ収集する設計。
//     コア API は「現在の残高と在庫」だけを真実とし、過去ログは外部責務。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 通貨 1 種類の定義。
 *
 * @details id / display_name は所有しない const char* (文字列リテラル想定)。
 */
struct FCurrencyDef {
    /** 通貨キー (FShopItem::currency_id から参照される。非所有)。 */
    const char* id           = nullptr;

    /** UI 表示名 (非所有)。 */
    const char* display_name = nullptr;

    /** リアル課金で得る premium 通貨なら true、ゲーム内で稼ぐ soft 通貨なら false (UI 判定用メタ情報)。 */
    bool        is_premium   = false;
};

/**
 * shop に並ぶ商品 1 件の定義。
 *
 * @details id 系フィールドは所有しない const char* (文字列リテラル想定)。
 */
struct FShopItem {
    /** 商品キー (購入 / 検索のキー。非所有)。 */
    const char* item_id         = nullptr;

    /** UI 表示名 (非所有)。 */
    const char* display_name    = nullptr;

    /** 支払い通貨 (RegisterCurrency 済みの id を指す。非所有)。 */
    const char* currency_id     = nullptr;

    /** 単価。 */
    u32         price           = 0;

    /** 在庫数。~0u (= 0xFFFFFFFF) で無制限扱い、0 で売り切れ。 */
    u32         stock_remaining = 0;

    /** この商品が cosmetic に閉じていることを示す開発者宣言フラグ (Manager は強制せず、UI / Analytics の pay-to-win 検査用)。 */
    bool        cosmetic_only   = true;
};

/**
 * ゲーム内通貨残高 + 固定価格 shop + 購入コールバックをまとめる小型マネージャ。
 *
 * @details
 * 通貨定義と残高を同 index で 1:1 対応する並行 TArray に持ち、商品は単一 TArray に
 * 持つ。id 比較はすべて const char* の per-byte 線形検索 (件数が小さい前提)。
 * 乱数要素を一切持たず、購入は「払えば必ず指定 cosmetic が得られる固定価格方式」に
 * 限定する (loot box / ガチャは扱わない)。全 noexcept、非コピー・非ムーブ。
 */
class CEconomyDirector {
public:
    /**
     * 購入結果コールバックの型 (STL <functional> 禁止のため C 関数ポインタ + user)。
     *
     * @details Manager は user を所有しない。失敗時でも item_id は Find 時の生 id を渡す。
     * @param user SetOnPurchaseCallback で渡したコンテキスト (Manager は所有しない)。
     * @param item_id 購入対象の item_id (失敗時も nullptr ではない)。
     * @param success 購入成功なら true、失敗なら false。
     */
    using PurchaseCallback = void(*)(void* user, const char* item_id, bool success) noexcept;

    /** 空の director を構築する (通貨・商品なし)。 */
    CEconomyDirector()  noexcept = default;

    /** デストラクタ (非所有 const char* のみ保持するため後始末不要)。 */
    ~CEconomyDirector() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    CEconomyDirector(const CEconomyDirector&)            = delete;

    /** コピー代入も禁止。 */
    CEconomyDirector& operator=(const CEconomyDirector&) = delete;

    /** ムーブ禁止。 */
    CEconomyDirector(CEconomyDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEconomyDirector& operator=(CEconomyDirector&&)      = delete;

    /**
     * 通貨を 1 種類登録する (残高 0 で初期化)。
     *
     * @details 同 id の重複登録は WARN を出して no-op、def.id == nullptr も no-op。
     * @param def 登録する通貨定義。
     */
    void RegisterCurrency(const FCurrencyDef& def) noexcept;

    /**
     * 指定通貨の残高を絶対値で設定する。
     *
     * @details 未登録通貨 / nullptr id は no-op。
     * @param currency_id 対象通貨の id。
     * @param amount 設定する残高。
     */
    void SetBalance(const char* currency_id, u32 amount) noexcept;

    /**
     * 指定通貨の残高を取得する。
     *
     * @param currency_id 対象通貨の id。
     * @return 残高 (未登録通貨 / nullptr id は 0)。
     */
    u32 GetBalance(const char* currency_id) const noexcept;

    /**
     * 残高に delta を加算する (オーバーフロー時は最大値にクランプ)。
     *
     * @param currency_id 対象通貨の id。
     * @param delta 加算量。
     * @return 加算できれば (通貨登録済) true、未登録通貨 / nullptr id なら false。
     */
    bool AddToBalance(const char* currency_id, u32 delta) noexcept;

    /**
     * 残高から delta を減算する。
     *
     * @details 残高 < delta なら残高を変えず false を返す。通貨未登録 / nullptr id も false。
     * @param currency_id 対象通貨の id。
     * @param delta 減算量。
     * @return 減算に成功すれば true。
     */
    bool DeductFromBalance(const char* currency_id, u32 delta) noexcept;

    /**
     * 商品を 1 件登録する。
     *
     * @details
     * 同 item_id の重複登録は WARN を出して no-op、item.item_id == nullptr も no-op。
     * currency_id が未登録でも登録自体は受理する (起動順序依存を緩めるため、購入時に判定する)。
     * @param item 登録する商品定義。
     */
    void RegisterItem(const FShopItem& item) noexcept;

    /**
     * 商品を購入する (残高・在庫・通貨登録をチェックし、成功時に deduct + stock-- + callback)。
     *
     * @details
     * 失敗条件 (false を返し side effect なし): item_id 未登録 / nullptr、currency_id 未登録、
     * 在庫切れ (stock_remaining == 0)、残高不足 (balance < price)。成功時は callback(true)、
     * 失敗時も item_id != nullptr なら callback(false) を呼ぶ。
     * @param item_id 購入する商品の item_id。
     * @return 購入できれば true。
     */
    bool PurchaseItem(const char* item_id) noexcept;

    /**
     * item_id で商品を 1 件取得する。
     *
     * @details 返却ポインタは次の RegisterItem() / ClearAll() で無効化されうる。
     * @param item_id 探す商品の item_id。
     * @return 見つかった商品 (未登録 / nullptr id は nullptr)。
     */
    const FShopItem* FindItem(const char* item_id) const noexcept;

    /**
     * 登録済み商品の件数を返す。
     *
     * @return 商品件数。
     */
    u32 ItemCount() const noexcept;

    /**
     * 全商品の生バッファ先頭を返す。
     *
     * @details 返却ポインタは ItemCount() 件の連続バッファで、RegisterItem() / ClearAll() で無効化される。
     * @param out_count 商品件数を書き出す先。
     * @return 商品配列の先頭ポインタ。
     */
    const FShopItem* AllItems(u32& out_count) const noexcept;

    /**
     * 購入結果コールバックを設定する。
     *
     * @details cb = nullptr で detach。user は所有しない (呼出側の責務)。
     * @param cb 登録するコールバック (nullptr で detach)。
     * @param user コールバックに渡すコンテキスト (Manager は所有しない)。
     */
    void SetOnPurchaseCallback(PurchaseCallback cb, void* user) noexcept;

    /** 通貨定義 / 残高 / 商品 / 在庫 / コールバック設定をすべてクリアする (デバッグ・シーン切替用)。 */
    void ClearAll() noexcept;

private:
    /**
     * 通貨 id を per-element 線形検索して内部配列位置を返す。
     *
     * @param currency_id 探す通貨の id。
     * @return 内部配列の位置 (未検出は ~0u)。
     */
    u32 FindCurrencySlot(const char* currency_id) const noexcept;

    /**
     * item_id を per-element 線形検索して内部配列位置を返す。
     *
     * @param item_id 探す商品の id。
     * @return 内部配列の位置 (未検出は ~0u)。
     */
    u32 FindItemSlot(const char* item_id) const noexcept;

    /** 通貨定義 (m_Balances と同 index で 1:1 対応)。 */
    TArray<FCurrencyDef> m_Currencies;

    /** 通貨残高 (m_Currencies と同 index で 1:1 対応)。 */
    TArray<u32>         m_Balances;

    /** 商品定義。 */
    TArray<FShopItem> m_Items;

    /** 購入コールバック (C 関数ポインタ。未設定は nullptr)。 */
    PurchaseCallback m_OnPurchase      = nullptr;

    /** 購入コールバックに渡す user コンテキスト (Manager は所有しない)。 */
    void*            m_OnPurchaseUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FEconomyDirector = CEconomyDirector;

} // namespace acs::game
