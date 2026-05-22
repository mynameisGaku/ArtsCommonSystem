// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — EconomyDirector (in-game 通貨 + shop 管理)
//
// 通貨残高 (soft / premium) + 固定価格 shop アイテム + 取引履歴 (callback) を
// まとめた小型マネージャ。Entitlement / SeasonPass と一緒に Pillar O の
// 「ストア / LiveOps 系」三兄弟を構成する。Entitlement は「持っているか」、
// SeasonPass は「期間内に xp で進行」、EconomyDirector は「通貨でアイテムを
// 直接購入する」を担当する。
//
// 倫理方針 (重要):
//   ・本クラスは **cosmetic only** を強く推奨する。装備性能 / ダメージ倍率 /
//     獲得経験値ブースター等、コア体験に影響するアイテムを通貨で売る設計は
//     pay-to-win に直結するため、ACS としては明示的に反対する立場
//     (Entitlement / SeasonPass の方針を継承)。ShopItem::cosmetic_only は
//     「この商品が cosmetic に閉じている」ことを開発者が宣言するフラグであり、
//     強制機構ではない (Manager は値を保存して照会できるようにするだけ)。
//   ・**loot box (中身がランダムで対価が確率に依存する仕組み) は明示的に拒絶**:
//     本 API には乱数要素を一切持たせず、すべて「払うと必ず指定の cosmetic が
//     得られる、固定価格方式」に限定する。在庫 (stock_remaining) は限定販売の
//     在庫管理であり、確率ガチャではない。
//   ・**premium 通貨でゲームプレイ加速を売らない**: CurrencyDef::is_premium は
//     「リアル課金で得られる通貨か」のメタ情報 (UI で 「¥」 アイコン表示等の
//     判定に使う) であり、premium 通貨で買えるアイテムも cosmetic_only に
//     閉じることを推奨する。
//
// 想定する位置付け:
//   ・Pillar O (Entitlement) との違い:
//     - Entitlement は「DLC / SeasonPass / 引換コード」等の永続権利フラグ。
//     - EconomyDirector は「ゲーム内通貨で売買される消費財 / cosmetic」を扱う。
//       購入結果として cosmetic を解放する場合は、PurchaseCallback で
//       Entitlement::Add() を呼ぶ橋渡しを呼出側で実装する想定。
//   ・Pillar O (SeasonPass) との違い:
//     - SeasonPass は xp ベースの tier 進行 (時間と遊びで貯まる)。
//     - EconomyDirector は通貨ベースの即時購入 (払えば即時取得)。
//   ・Pillar S (Storefront) との違い:
//     - Storefront はリアル課金プラットフォーム (Steam / EOS / 家庭機 SDK) の
//       購入トランザクション。
//     - EconomyDirector はゲーム内通貨での売買のみを扱う。premium 通貨を
//       リアル課金で得るフローは Pillar S 側で実装し、AddToBalance() で
//       残高を増やしてもらう想定 (本クラスはストア非依存)。
//
// 使い方:
//   EconomyDirector ed;
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
//       // 成功 → cosmetic 解放 (Entitlement 側へ橋渡し)
//   }
//
//   // (任意) 購入結果コールバック。
//   ed.SetOnPurchaseCallback(&OnPurchase, user_data);
//
// 設計選択 (Pillar O Phase 3):
//   ・**通貨と残高は並行 Array**: CurrencyDef を Array<CurrencyDef> に、残高を
//     Array<u32> に同 index で 1:1 で持つ。Entitlement の id 比較と同じく
//     const char* per-byte 線形検索。通貨種別はゲーム 1 セッションで通常 2〜5、
//     多くても 10 を超えない想定なので線形で十分。
//   ・**ShopItem は単一 Array**: 商品数は AAA でも 100〜500 程度のオーダー、
//     線形走査で十分。検索はすべて item_id 文字列。
//   ・**所有しない const char***: id / display_name / currency_id すべて呼出側
//     (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     EconomyDirector はコピーしない (STL <string> 禁止)。
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
//     RegisterItem は no-op (アセット二重ロード保護)。Entitlement / SeasonPass /
//     AchievementManager と同じパターン。
//   ・**取引履歴は callback 経由のみ**: 履歴 Array を内蔵してメモリを増やすより、
//     呼出側 (= Analytics / Pillar T Community) でログ収集する設計。
//     コア API は「現在の残高と在庫」だけを真実とし、過去ログは外部責務。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 4+ で):
//   ・永続化 (Save/Load) — Pillar J Serialize と統合。残高 / 在庫は起動毎にリセット。
//   ・リアル課金トランザクション — Pillar S Storefront に委譲。
//   ・サーバ側の残高検証 / 不正対策 — Pillar V Backend Services に委譲。
//   ・割引 / セール / 時限価格変動 — Phase 4+ で別 Manager を検討。
//   ・通貨間レート / 両替 — 不要であれば実装しない。実装する場合は別 API で。
//   ・取引履歴の内蔵保持 — callback 経由のみ。Analytics 統合で外部蓄積。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- CurrencyDef: 通貨 1 種類の定義 ----------------------------------------
// id            : 通貨キー (ShopItem::currency_id から参照される)。文字列リテラル想定。
// display_name  : UI 表示名 (非所有)。
// is_premium    : リアル課金で得られる通貨か (true) / ゲーム内で稼ぐ soft 通貨か (false)。
//                 UI で 「¥」 アイコンを出すか等の判定用メタ情報。
struct CurrencyDef {
    const char* id           = nullptr;
    const char* display_name = nullptr;
    bool        is_premium   = false;
};

// ---- ShopItem: shop に並ぶ商品 1 件 ----------------------------------------
// item_id          : 商品キー (購入 / 検索のキー)。文字列リテラル想定。
// display_name     : UI 表示名 (非所有)。
// currency_id      : 支払い通貨 (RegisterCurrency 済みの id を指す)。
// price            : 単価。u32。
// stock_remaining  : 在庫数。~0u (= 0xFFFFFFFF) で「無制限」扱い。0 で売り切れ。
// cosmetic_only    : 「この商品が cosmetic に閉じている」開発者宣言フラグ。
//                    Manager は強制しないが、UI / Analytics でこのフラグを見て
//                    pay-to-win 検査を実装できるようにしておく。
struct ShopItem {
    const char* item_id         = nullptr;
    const char* display_name    = nullptr;
    const char* currency_id     = nullptr;
    u32         price           = 0;
    u32         stock_remaining = 0;
    bool        cosmetic_only   = true;
};

// ---- EconomyDirector ------------------------------------------------------
class EconomyDirector {
public:
    // 購入結果コールバック。STL <functional> 禁止のため C 関数ポインタ + user。
    //   user      : SetOnPurchaseCallback で渡したコンテキスト (Manager は所有しない)
    //   item_id   : 購入対象の item_id (失敗時でも nullptr ではない、Find 時の生 id を渡す)
    //   success   : 購入成功 (true) / 失敗 (false)
    using PurchaseCallback = void(*)(void* user, const char* item_id, bool success) noexcept;

    EconomyDirector()  noexcept = default;
    ~EconomyDirector() noexcept = default;

    EconomyDirector(const EconomyDirector&)            = delete;
    EconomyDirector& operator=(const EconomyDirector&) = delete;
    EconomyDirector(EconomyDirector&&)                 = delete;
    EconomyDirector& operator=(EconomyDirector&&)      = delete;

    // ---- 通貨定義 / 残高 -------------------------------------------------
    // 同 id 重複は no-op (WARN)。`def.id == nullptr` も no-op。
    // 残高は 0 で初期化される。
    void RegisterCurrency(const CurrencyDef& def) noexcept;

    // 指定通貨の残高を絶対値で設定。未登録通貨 / nullptr id は no-op。
    void SetBalance(const char* currency_id, u32 amount) noexcept;

    // 指定通貨の残高を取得。未登録通貨 / nullptr id は 0。
    u32 GetBalance(const char* currency_id) const noexcept;

    // 残高に delta を加算。オーバーフロー時は max クランプ。
    // 戻り値: true if 処理した (= 通貨登録済), false if 未登録通貨 / nullptr id。
    bool AddToBalance(const char* currency_id, u32 delta) noexcept;

    // 残高から delta を減算。残高 < delta なら false を返し、残高は変更しない。
    // 通貨未登録 / nullptr id も false。成功時のみ true。
    bool DeductFromBalance(const char* currency_id, u32 delta) noexcept;

    // ---- 商品定義 ---------------------------------------------------------
    // 同 item_id 重複は no-op (WARN)。`item.item_id == nullptr` も no-op。
    // currency_id 未登録でも登録自体は受理する (購入時に判定して失敗を返す方が、
    // 起動順序依存 — 通貨と商品の登録順 — の縛りを緩められて呼出側に優しい)。
    void RegisterItem(const ShopItem& item) noexcept;

    // ---- 購入 -------------------------------------------------------------
    // 残高 + 在庫 + 通貨登録チェック → 成功時 deduct + stock-- + callback(true)。
    // 失敗条件 (false 戻り、side effect なし):
    //   ・item_id が未登録 / nullptr
    //   ・currency_id が未登録 (商品登録時に未定義通貨を指定していた等)
    //   ・在庫切れ (stock_remaining == 0)
    //   ・残高不足 (balance < price)
    // 成功時は callback(true) を、失敗時も呼出側に「失敗したよ」UI を出させたい
    // ケースを考慮して callback(false) を呼ぶ (`item_id == nullptr` は除く)。
    bool PurchaseItem(const char* item_id) noexcept;

    // ---- 商品照会 ---------------------------------------------------------
    // item_id で 1 件取得。未登録 / nullptr は nullptr。返却ポインタは次の
    // RegisterItem() / ClearAll() で無効化される可能性がある。
    const ShopItem* FindItem(const char* item_id) const noexcept;

    // 商品件数。
    u32 ItemCount() const noexcept;

    // 全商品の生バッファ。`out_count` に件数を書き出す。
    // 返却ポインタは ItemCount() 件の連続バッファ、RegisterItem() / ClearAll()
    // で無効化される。
    const ShopItem* AllItems(u32& out_count) const noexcept;

    // ---- コールバック -----------------------------------------------------
    // cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnPurchaseCallback(PurchaseCallback cb, void* user) noexcept;

    // ---- 全リセット (デバッグ / シーン切替時) -----------------------------
    // 通貨定義 / 残高 / 商品 / 在庫すべてクリア。コールバック設定もクリアする。
    void ClearAll() noexcept;

private:
    // 通貨 id → 内部配列位置の per-element 線形検索。未検出は ~0u。
    u32 FindCurrencySlot(const char* currency_id) const noexcept;

    // item_id → 内部配列位置の per-element 線形検索。未検出は ~0u。
    u32 FindItemSlot(const char* item_id) const noexcept;

    // 通貨定義 + 残高 (同 index で 1:1 対応の並行 Array)。
    Array<CurrencyDef> _currencies;
    Array<u32>         _balances;

    // 商品定義。
    Array<ShopItem> _items;

    // 購入コールバック (C 関数ポインタ + user)。Manager は user を所有しない。
    PurchaseCallback _on_purchase      = nullptr;
    void*            _on_purchase_user = nullptr;
};

} // namespace acs::game
