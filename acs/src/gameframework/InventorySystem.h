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
//   ・Pillar O FEntitlementRegistry との違い:
//     - FEntitlementRegistry は「DLC / FSeasonPass 等の永続権利フラグ」(持っているかの真偽)。
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
// 設計選択 (Pillar O/G):
//   ・**Item 定義は単一 TArray<FItemDef>**: アイテム数は AAA でも 500〜2000 のオーダー、
//     線形走査で十分 (FEntitlementRegistry / FEconomyDirector と同じ判断)。
//   ・**FInventorySlot data は固定長 TArray<FInventorySlot>**: Init(slot_count) で Resize し、
//     以降は伸縮しない (= UI が想定する slot grid と一致)。slot 内 item_id == nullptr
//     を「空 slot」として表す。
//   ・**所有しない const char***: id / display_name / icon_path すべて呼出側
//     (= ゲームコード or リソースバンドル) が長寿命を保証する文字列リテラル想定。
//     slot.item_id も m_Items[].id を直接指す (= リテラル参照、非所有)。
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
//     (アセット二重ロード保護)。FEconomyDirector / FEntitlementRegistry と同じパターン。
//   ・**Init は冪等に再構築**: 既に Init 済みでも slot_count を変更して呼べる
//     (slot 数が違う場合は内容クリアして resize)。同じ slot_count なら no-op。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外:
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

/**
 * アイテム分類。
 *
 * @details
 * UI のタブフィルタや、AddItem / Drop の挙動分岐の参考に使う。Manager は中身を
 * 解釈しない (= 値を保存して照会できるようにするだけ)。
 */
enum class EItemCategory : u8 {
    /** ポーション / 食料 / 弾薬等の消費アイテム。 */
    Consumable = 0,

    /** 装備品 (見た目 cosmetic は FCharacterCustomizer 側)。 */
    Equipment  = 1,

    /** クラフト素材 / 鉱石 / ハーブ等。 */
    Material   = 2,

    /** クエスト関連アイテム (通常 can_drop = false)。 */
    Quest      = 3,

    /** 鍵 / IC カード等 (通常 can_drop = false)。 */
    Keys       = 4,

    /** インベントリで持ち歩く cosmetic 系 (装着は別 Manager)。 */
    Cosmetic   = 5,
};

/**
 * アイテム 1 種類の定義 (immutable)。
 */
struct FItemDef {
    /** 一意キー (slot.item_id から参照される)。文字列リテラル想定 (非所有)。 */
    const char*  id           = nullptr;

    /** UI 表示名 (非所有)。 */
    const char*  display_name = nullptr;

    /** 分類 (UI フィルタ用、Manager は値保存のみ)。 */
    EItemCategory category     = EItemCategory::Consumable;

    /** 同 slot に重ねられる最大個数 (1 = stack 不可。0 は 1 にクランプ)。 */
    u32          max_stack    = 1;

    /** UI アイコン画像のパス (非所有)。レンダラ / UI が解釈。 */
    const char*  icon_path    = nullptr;

    /** DropSlot で捨てられるか (quest / key 系は false にする想定)。 */
    bool         can_drop     = true;
};

/**
 * 1 つの slot の内容。
 */
struct FInventorySlot {
    /** 入っているアイテムの id (リテラル参照、非所有)。nullptr = 空 slot。 */
    const char* item_id = nullptr;

    /** stack 数 (1〜max_stack)。空 slot のときは 0。 */
    u32         count   = 0;
};

/**
 * アイテムスロット + stack 管理を行うインベントリマネージャ。
 *
 * @details
 * 固定 slot 数 × stack 可能アイテムモデルで、プレイヤーが持ち歩くアイテムを保持する。
 * 全 noexcept、非コピー・非ムーブ。const char* は非所有 (呼出側が長寿命を保証)。
 */
class FInventorySystem {
public:
    /**
     * slot 変更通知コールバック (C 関数ポインタ + user)。
     *
     * @details STL <functional> 禁止のため関数ポインタ + void* user 形式。
     * @param user SetOnChangeCallback で渡したコンテキスト (Manager は所有しない)。
     * @param slot_index 変化があった slot 番号。
     * @param item_id 変化後の id (リテラル参照) or nullptr (= 空になった)。
     * @param count 変化後の stack 数 (空 slot のときは 0)。
     */
    using ChangeCallback = void(*)(void* user, u32 slot_index, const char* item_id, u32 count) noexcept;

    /** 空状態 (slot 数 0) で構築する。 */
    FInventorySystem()  noexcept = default;

    /** 破棄する (内部 TArray が解放)。 */
    ~FInventorySystem() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    FInventorySystem(const FInventorySystem&)            = delete;

    /** コピー代入も禁止。 */
    FInventorySystem& operator=(const FInventorySystem&) = delete;

    /** ムーブ禁止。 */
    FInventorySystem(FInventorySystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FInventorySystem& operator=(FInventorySystem&&)      = delete;

    /**
     * slot 数を設定する (冪等に再構築)。
     *
     * @details
     * 再呼び出しで slot_count が変わる場合は内容クリアして resize、同じ slot_count
     * なら no-op。slot_count == 0 は 1 にクランプ (defensive)。
     * @param slot_count 確保する slot 数 (既定 30)。
     */
    void Init(u32 slot_count = 30) noexcept;

    /**
     * アイテム定義を登録する (起動時に 1 度ずつ)。
     *
     * @details
     * 同 id の 2 重登録は no-op (WARN)。`def.id == nullptr` も no-op。
     * `def.max_stack == 0` は 1 にクランプ。
     * @param def 登録するアイテム定義。
     */
    void RegisterItem(const FItemDef& def) noexcept;

    /**
     * 単一 FItemDef を取得する。
     *
     * @details 返却ポインタは次の RegisterItem() / ClearAll() で無効化され得る。
     * @param item_id 探すアイテム id。
     * @return 見つかった FItemDef へのポインタ (未登録 / nullptr なら nullptr)。
     */
    const FItemDef* FindItem(const char* item_id) const noexcept;

    /**
     * 指定アイテムを count 個追加する。
     *
     * @details 既存 stack への積み増し優先 → なければ空 slot を使う。
     * @param item_id 追加するアイテム id。
     * @param count 追加したい個数。
     * @return 実際に追加できた個数 (満杯 / 未登録 / nullptr / Init 前は 0〜count の途中値)。
     */
    u32 AddItem(const char* item_id, u32 count) noexcept;

    /**
     * 指定アイテムを count 個削除する (全 slot 横断、前方優先)。
     *
     * @param item_id 削除するアイテム id。
     * @param count 削除したい個数。
     * @return 実際に削除できた個数 (在庫不足 / 未登録 / nullptr は 0〜count の途中値)。
     */
    u32 RemoveItem(const char* item_id, u32 count) noexcept;

    /**
     * 指定アイテムを min_count 個以上持っているかを返す。
     *
     * @param item_id 調べるアイテム id。
     * @param min_count 必要な最小個数 (既定 1。0 は defensive に true)。
     * @return min_count 個以上あれば true (nullptr / 未登録は false)。
     */
    bool HasItem(const char* item_id, u32 min_count = 1) const noexcept;

    /**
     * 指定アイテムを全 slot 合計で何個持っているかを返す。
     *
     * @param item_id 集計するアイテム id。
     * @return 合計 stack 数 (未登録 / nullptr は 0)。
     */
    u32 ItemTotal(const char* item_id) const noexcept;

    /**
     * slot 間で内容を移動する (UI ドラッグ&ドロップ用)。
     *
     * @details
     * from / to が同 index は no-op で成功扱い。to が空なら移動、to に同 item_id
     * なら merge (max_stack まで、残りは from に残す)、別 item_id なら swap。
     * @param from_index 移動元 slot の添字。
     * @param to_index 移動先 slot の添字。
     * @return 範囲外 / Init 前 / nullptr 系の異常は false、それ以外は true。
     */
    bool MoveSlot(u32 from_index, u32 to_index) noexcept;

    /**
     * slot を空にする (アイテムを地面に落とす想定の UI 操作)。
     *
     * @details
     * 落としたアイテムをワールドに spawn するのは呼出側の責務 (ChangeCallback で検知)。
     * @param index 空にする slot の添字。
     * @return 範囲外 / Init 前 / 既に空 / can_drop == false なら false、成功なら true。
     */
    bool DropSlot(u32 index) noexcept;

    /**
     * 指定 slot の内容を取得する。
     *
     * @details
     * 返却ポインタは次の Init() / ClearAll() で無効化され得る。個々の slot 操作
     * (Add / Remove / Move / Drop) ではポインタは有効のまま。
     * @param index 取得する slot の添字。
     * @return slot へのポインタ (範囲外なら nullptr)。
     */
    const FInventorySlot* GetSlot(u32 index) const noexcept;

    /**
     * 現在の slot 数を返す。
     *
     * @return Init で設定した slot 数。
     */
    u32 SlotCount() const noexcept;

    /**
     * 空 slot の個数を返す (UI の "X/Y 使用" 表示等)。
     *
     * @return item_id == nullptr の slot 数。
     */
    u32 EmptySlotCount() const noexcept;

    /**
     * slot 変更通知コールバックを設定する。
     *
     * @details cb = nullptr で detach。user は所有しない (呼出側の責務)。
     * @param cb 変更時に呼ぶコールバック (nullptr で解除)。
     * @param user コールバックに渡すコンテキスト。
     */
    void SetOnChangeCallback(ChangeCallback cb, void* user) noexcept;

    /**
     * アイテム定義 + slot 内容 + コールバック設定をすべてクリアする。
     *
     * @details
     * slot 数も 0 に戻る (再 Init 必須)。loop 防止のため callback は呼ばない。
     */
    void ClearAll() noexcept;

private:
    /**
     * item_id から m_Items 内 index を線形検索する。
     *
     * @param item_id 探すアイテム id。
     * @return 見つかった index、未検出は ~0u。
     */
    u32 FindItemSlot(const char* item_id) const noexcept;

    /**
     * slot 変更を通知する (callback が設定されていれば呼ぶ)。
     *
     * @param slot_index 変化があった slot 番号。
     */
    void NotifyChange(u32 slot_index) noexcept;

    /** アイテム定義 (起動時 immutable)。 */
    TArray<FItemDef> m_Items;

    /** slot data (Init で固定長 Resize、以降伸縮しない)。 */
    TArray<FInventorySlot> m_Slots;

    /** 変更通知 callback (C 関数ポインタ)。未設定は nullptr。 */
    ChangeCallback m_OnChange      = nullptr;

    /** ChangeCallback に渡すユーザコンテキスト (Manager は所有しない)。 */
    void*          m_OnChangeUser = nullptr;
};

} // namespace acs::game
