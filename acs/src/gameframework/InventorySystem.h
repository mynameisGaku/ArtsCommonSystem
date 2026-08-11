// SPDX-License-Identifier: Apache-2.0
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

    /** 装備品 (見た目 cosmetic は CCharacterCustomizer 側)。 */
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
class CInventorySystem {
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
    CInventorySystem()  noexcept = default;

    /** 破棄する (内部 TArray が解放)。 */
    ~CInventorySystem() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    CInventorySystem(const CInventorySystem&)            = delete;

    /** コピー代入も禁止。 */
    CInventorySystem& operator=(const CInventorySystem&) = delete;

    /** ムーブ禁止。 */
    CInventorySystem(CInventorySystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CInventorySystem& operator=(CInventorySystem&&)      = delete;

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

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FInventorySystem = CInventorySystem;

} // namespace acs::game
