// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O/G — FInventorySystem (アイテムスロット + stack 管理)
//
// プレイヤーのアイテムインベントリを「固定 slot 数 × stack 可能アイテム」モデルで
// 表現する小型マネージャ。RPG / サバイバル / クラフト系で広く使われる「N 個の
// スロットがあって、消費アイテムは max_stack まで重ねられる」スタイルを直接
// サポートする。FEconomyDirector が「通貨で買って消える」即時取引を担うのに対し、
// FInventorySystem は「拾った / 報酬で得たアイテムを、プレイヤーが持ち歩く」状態を
// 保持する責務を持つ。
//
// 想定する位置付け:
//   ・Pillar O FEconomyDirector との違い:
//     - FEconomyDirector は「ゲーム内通貨 ↔ shop 商品」の即時購入トランザクション。
//     - FInventorySystem は「プレイヤーが保持するアイテム」の slot ベース在庫。
//       購入完了後に AddItem() を呼んでもらう橋渡し設計 (本クラスはストア非依存)。
//   ・Pillar O FCharacterCustomizer との違い:
//     - FCharacterCustomizer は cosmetic (見た目装着) — slot 単位で最大 1 つ、stack なし。
//     - FInventorySystem は使用アイテム / 素材 / クエスト系 — stack あり、slot 数固定。
//   ・Pillar O Entitlement との違い:
//     - Entitlement は「DLC / FSeasonPass 等の永続権利フラグ」(持っているかの真偽)。
//     - FInventorySystem は「個数を持つ消費可能アイテム」(stack 数を持つ)。
//
// 使い方:
//   FInventorySystem inv;
//   inv.Init(/*slot_count=*/ 30);
//
//   // 起動時にアイテム定義を一度ずつ登録。
//   inv.RegisterItem({ "potion.heal",     "Heal Potion",   EItemCategory::Consumable, 99,
//                      "ui/icon/potion_heal.png", true });
//   inv.RegisterItem({ "ore.iron",        "Iron Ore",      EItemCategory::Material,   999,
//                      "ui/icon/ore_iron.png",    true });
//   inv.RegisterItem({ "quest.key.ruins", "Ruins EKey",     EItemCategory::Quest,      1,
//                      "ui/icon/key_ruins.png",   false });
//
//   // (任意) 変更通知 callback (UI 反映 / SFX 等)。
//   inv.SetOnChangeCallback(&OnSlotChanged, &ui);
//
//   // ゲーム中に追加 / 削除。
//   const u32 added = inv.AddItem("potion.heal", 5);    // 既存 stack に積む or 空 slot を使う
//   inv.RemoveItem("potion.heal", 1);
//   inv.MoveSlot(3, 7);                                  // UI ドラッグ&ドロップ
//   inv.DropSlot(7);                                     // can_drop=true のときのみ
//
// 設計選択 (Pillar O/G Phase 4):
//   ・**Item 定義は単一 TArray<FItemDef>**: アイテム数は AAA でも 500〜2000 のオーダー、
//     線形走査で十分 (Entitlement / FEconomyDirector と同じ判断)。
//   ・**FSlot data は固定長 TArray<FInventorySlot>**: Init(slot_count) で Resize し、
//     以降は伸縮しない (= UI が想定する slot grid と一致)。slot 内 item_id == nullptr
//     を「空 slot」として表す。
//   ・**所有しない const char***: id / display_name / icon_path すべて呼出側
//     (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     slot.item_id も _items[].id を直接指す (= リテラル参照、非所有)。
//   ・**AddItem は積み増し優先**: 同 id の既存 stack が max_stack 未満なら先にそちらに
//     積み、それでも残ったら空 slot を埋める。これにより inventory の fragmentation が
//     最小化される (= UI のスロットが連続して埋まる / 散らばらない)。
//   ・**RemoveItem は前方優先**: 低 index から走査して削除。これも UI の慣習
//     (前から減る方が見やすい) に合わせる。
//   ・**MoveSlot は merge or swap**: 同 item で max_stack に届かない範囲なら merge、
//     満杯 or 別 item なら swap。UI ドラッグ&ドロップ操作の自然な挙動。
//   ・**DropSlot は can_drop チェック**: quest アイテム / key アイテム等の落とせないものを
//     構造的に守る (FItemDef::can_drop = false なら DropSlot は失敗 false 返し)。
//   ・**ChangeCallback は単一購読**: STL <functional> 禁止のため C 関数ポインタ + void*
//     user. 複数 listener が必要なら呼出側で fan-out。slot 内容が変わるたびに呼ぶ
//     (Add / Remove / Move / Drop / ClearAll すべて。ClearAll は loop 防止のため呼ばない)。
//   ・**重複登録は黙って弾く + WARN**: 同 id の 2 重 RegisterItem は no-op
//     (アセット二重ロード保護)。FEconomyDirector / Entitlement と同じパターン。
//   ・**Init は冪等に再構築**: 既に Init 済みでも slot_count を変更して呼べる
//     (slot 数が違う場合は内容クリアして resize)。同じ slot_count なら no-op。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 5+ で):
//   ・永続化 (Save/Load) — Pillar J Serialize と統合。slot 内容は起動毎にリセット。
//   ・装備品の性能 / バフ — 本クラスは「持ってる個数」だけを真実とし、性能は別 Manager。
//   ・アイテムカテゴリでのフィルタリング — UI 側で実装 (本クラスは AllSlots を提供)。
//   ・craft / 合成 / 消費レシピ — 別 Manager (FCraftingSystem 等)。
//   ・サーバ側の在庫検証 — Pillar V Backend Services に委譲。
//   ・重量制限 / encumbrance — 本クラスは slot 数固定のみ。重量制は別 Manager。
//   ・並べ替え / 自動ソート — UI 側で MoveSlot を連打して実装可能。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- EItemCategory: アイテム分類 --------------------------------------------
// UI のタブフィルタや、AddItem / Drop の挙動分岐の参考に使う。
// Manager は中身を解釈しない (= 値を保存して照会できるようにするだけ)。
enum class EItemCategory : u8 {
    Consumable = 0,  // ポーション / 食料 / 弾薬等の消費アイテム
    Equipment  = 1,  // 装備品 (見た目 cosmetic は FCharacterCustomizer 側)
    Material   = 2,  // クラフト素材 / 鉱石 / ハーブ等
    Quest      = 3,  // クエスト関連アイテム (通常 can_drop = false)
    Keys       = 4,  // 鍵 / IC カード等 (通常 can_drop = false)
    Cosmetic   = 5,  // インベントリで持ち歩く cosmetic 系 (装着は別 Manager)
};

// ---- FItemDef: アイテム 1 種類の定義 (immutable) ----------------------------
// id           : 一意キー (slot.item_id から参照される)。文字列リテラル想定 (非所有)。
// display_name : UI 表示名 (非所有)。
// category     : 分類 (UI フィルタ用、Manager は値保存のみ)。
// max_stack    : 同 slot に重ねられる最大個数。1 = stack 不可。
//                0 を渡された場合は 1 にクランプして登録する (defensive)。
// icon_path    : UI アイコン画像のパス (非所有)。レンダラ / UI が解釈。
// can_drop     : DropSlot で捨てられるか。quest / key 系は false にする想定。
struct FItemDef {
    const char*  id           = nullptr;
    const char*  display_name = nullptr;
    EItemCategory category     = EItemCategory::Consumable;
    u32          max_stack    = 1;
    const char*  icon_path    = nullptr;
    bool         can_drop     = true;
};

// ---- FInventorySlot: 1 つの slot の内容 -------------------------------------
// item_id : 入っているアイテムの id (リテラル参照、非所有)。nullptr = 空 slot。
// count   : stack 数 (1〜max_stack)。空 slot のときは 0。
struct FInventorySlot {
    const char* item_id = nullptr;
    u32         count   = 0;
};

// ---- FInventorySystem -------------------------------------------------------
class FInventorySystem {
public:
    // 変更通知コールバック。STL <functional> 禁止のため C 関数ポインタ + user。
    //   user       : SetOnChangeCallback で渡したコンテキスト (Manager は所有しない)
    //   slot_index : 変化があった slot 番号
    //   item_id    : 変化後の id (リテラル参照) or nullptr (= 空になった)
    //   count      : 変化後の stack 数 (空 slot のときは 0)
    using ChangeCallback = void(*)(void* user, u32 slot_index, const char* item_id, u32 count) noexcept;

    FInventorySystem()  noexcept = default;
    ~FInventorySystem() noexcept = default;

    FInventorySystem(const FInventorySystem&)            = delete;
    FInventorySystem& operator=(const FInventorySystem&) = delete;
    FInventorySystem(FInventorySystem&&)                 = delete;
    FInventorySystem& operator=(FInventorySystem&&)      = delete;

    // ---- 初期化 ----------------------------------------------------------
    // slot 数を設定 (再呼び出しで slot_count が変わる場合は内容クリアして resize、
    // 同じ slot_count なら no-op)。slot_count == 0 は 1 にクランプ (defensive)。
    // default 30 想定 (一般的な RPG / サバイバルゲームの初期容量)。
    void Init(u32 slot_count = 30) noexcept;

    // ---- 定義登録 (起動時に 1 度ずつ) ------------------------------------
    // 同 id の 2 重登録は no-op (WARN)。`def.id == nullptr` も no-op。
    // `def.max_stack == 0` は 1 にクランプ。
    void RegisterItem(const FItemDef& def) noexcept;

    // 単一 FItemDef 取得。未登録 / nullptr は nullptr。返却ポインタは次の
    // RegisterItem() / ClearAll() で無効化される可能性がある。
    const FItemDef* FindItem(const char* item_id) const noexcept;

    // ---- 在庫操作 --------------------------------------------------------
    // 指定 item_id を count 個追加。既存 stack に積み増し優先 → なければ空 slot を使う。
    // 戻り値: 追加できた個数 (満杯で全部入らなかったら 0〜count の途中値)。
    // 未登録 item_id / nullptr / count == 0 は 0 を返す (side effect なし)。
    // Init() 前 (slot_count == 0) も 0 を返す。
    u32 AddItem(const char* item_id, u32 count) noexcept;

    // 指定 item_id を count 個削除 (全 slot 横断、前方優先で減らす)。
    // 戻り値: 実際に削除できた個数 (在庫不足なら 0〜count の途中値)。
    // 未登録 item_id / nullptr / count == 0 は 0。
    u32 RemoveItem(const char* item_id, u32 count) noexcept;

    // 指定 item_id を min_count 個以上持っているか。
    // nullptr / 未登録は false。min_count == 0 は true (defensive — 「0 個以上」)。
    bool HasItem(const char* item_id, u32 min_count = 1) const noexcept;

    // 指定 item_id を全 slot で合計何個持っているか。
    // 未登録 / nullptr は 0。
    u32 ItemTotal(const char* item_id) const noexcept;

    // ---- slot 操作 -------------------------------------------------------
    // slot 間の移動 (UI ドラッグ&ドロップ用)。
    //   ・from / to が同じ index → no-op true
    //   ・to が空 → from の内容を to に移し、from を空にする
    //   ・to に同 item_id が入っている → merge (max_stack まで)。残りは from に残す
    //   ・to に別 item_id が入っている → swap
    // 戻り値: 範囲外 / Init 前 / nullptr 系の異常は false。それ以外は true。
    // from が空 slot のときも true (操作は no-op)。
    bool MoveSlot(u32 from_index, u32 to_index) noexcept;

    // slot を空にする (= アイテムを地面に落とす想定の UI 操作)。
    // 戻り値: 範囲外 / Init 前 / 既に空 / can_drop == false の slot は false。
    // 落としたアイテムをワールドに spawn するのは呼出側の責務 (ChangeCallback で検知)。
    bool DropSlot(u32 index) noexcept;

    // ---- 照会 ------------------------------------------------------------
    // 指定 slot の内容を取得。範囲外は nullptr。
    // 返却ポインタは次の Init() / ClearAll() で無効化される可能性がある。
    // 個々の slot 操作 (Add / Remove / Move / Drop) ではポインタは有効のまま。
    const FInventorySlot* GetSlot(u32 index) const noexcept;

    // 現在の slot 数 (Init で設定した値)。
    u32 SlotCount() const noexcept;

    // 空 slot の個数 (UI の "X/Y 使用" 表示等)。
    u32 EmptySlotCount() const noexcept;

    // ---- コールバック ----------------------------------------------------
    // cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnChangeCallback(ChangeCallback cb, void* user) noexcept;

    // ---- 全リセット (デバッグ / シーン切替時) ----------------------------
    // アイテム定義 + slot 内容をすべてクリア。slot 数も 0 に戻る (再 Init 必須)。
    // ChangeCallback 設定もクリアする。loop 防止のため callback は呼ばない。
    void ClearAll() noexcept;

private:
    // item_id → _items 内 index の per-byte 線形検索。未検出は ~0u。
    u32 FindItemSlot(const char* item_id) const noexcept;

    // slot 変更通知 (callback が設定されていれば呼ぶ)。
    void NotifyChange(u32 slot_index) noexcept;

    // アイテム定義 (起動時 immutable)。
    TArray<FItemDef> _items;

    // slot data (Init で固定長 Resize、以降伸縮しない)。
    TArray<FInventorySlot> _slots;

    // 変更通知 callback (C 関数ポインタ + user)。Manager は user を所有しない。
    ChangeCallback _on_change      = nullptr;
    void*          _on_change_user = nullptr;
};

} // namespace acs::game
