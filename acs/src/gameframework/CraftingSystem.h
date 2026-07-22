// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット — FCraftingSystem (レシピ + 素材消費 + 成果物生成)
//
// サバイバル / クラフト系ジャンル (Minecraft, Valheim, Subnautica, Terraria 等) の中核を
// なす「素材を消費して時間経過後に成果物を得る」クラフト挙動を、レシピ登録 + 進行タイマ +
// インベントリ adapter callback だけで扱う小型マネージャ。
//
// 想定する位置付け:
//   ・Pillar O FInventorySystem との違い:
//     - FInventorySystem は「持っているアイテムの slot 在庫」を保持する。
//     - FCraftingSystem は「レシピ定義 + 現在クラフト中の進行状態」を保持し、
//       素材の消費 / 成果物の付与は FInventorySystem 等への callback 経由で行う
//       (= 在庫表現に非依存。テストでは fake adapter を差し込める)。
//   ・Pillar O FEconomyDirector との違い:
//     - FEconomyDirector は「通貨で即座に商品を購入」する取引マネージャ。
//     - FCraftingSystem は「素材で時間をかけて成果物を生成」する production マネージャ。
//       時間 (craft_duration_sec) と前提条件 (workbench / level) を持つ点が決定的に違う。
//   ・Pillar G FHealthSystem / FBuffSystem との関係:
//     - 直接の依存はないが、料理レシピが FBuffSystem の食事バフを与える等の連携は、
//       CompleteCallback 経由で呼出側が組む想定 (本クラスは何も知らない)。
//
// 使い方:
//   FCraftingSystem cs;
//
//   // レシピ定義 (素材配列は呼出側が長寿命を保証する static / global 想定)。
//   static const FIngredient kIronAxeIngredients[] = {
//       { "wood",     3 },
//       { "iron_ore", 2 },
//   };
//   cs.RegisterRecipe({
//       /* recipe_id        */ "craft.iron_axe",
//       /* display_name     */ "Iron Axe",
//       /* result_item_id   */ "tool.iron_axe",
//       /* result_count     */ 1,
//       /* ingredient_count */ 2,
//       /* ingredients      */ kIronAxeIngredients,
//       /* craft_duration   */ 4.0f,
//       /* required_workbench*/ "workbench.forge",
//       /* unlock_level     */ 3,
//   });
//
//   // インベントリ adapter (素材数 query / 消費 / 成果物付与) を差し込む。
//   cs.SetInventoryAdapter(&QueryInv, &ConsumeInv, &GrantInv, &inv);
//
//   // (任意) 完了コールバック (SFX / トースト等)。
//   cs.SetOnCompleteCallback(&OnCraftDone, &ui);
//
//   // クラフト開始 → Tick で進行 → 完了で result_item が grant される。
//   if (cs.CanCraft("craft.iron_axe", "workbench.forge", player_level)) {
//       cs.StartCraft("craft.iron_axe", "workbench.forge", player_level);
//   }
//   // ゲームループ毎:
//   cs.Tick(dt);
//
// 設計選択 (ジャンルキット survival / crafting):
//   ・**レシピは単一 TArray<FCraftRecipe>**: ジャンルキット規模 (200〜500 recipe) で
//     線形検索で十分。FEconomyDirector / FInventorySystem と同じ判断。
//   ・**FIngredient 配列は非所有**: FCraftRecipe::ingredients は呼出側が長寿命を保証する
//     生バッファ (static / global 配列、リソースバンドル) を指す。文字列 id も同様
//     (Manager は何もコピーしない。STL <string> 禁止 / <vector> 禁止)。
//   ・**Inventory adapter は C 関数ポインタ + user**: STL <functional> 禁止のため。
//     在庫表現に依存しない設計 (FInventorySystem を直接保持しないので、テストで
//     fake adapter を差し込める / 別 inventory システムにも繋げる)。
//   ・**同時クラフトは 1 件のみ**: 「ワークベンチに 1 つ載せて待つ」スタイルに統一。
//     並列クラフトキューが必要なゲームは、FCraftingSystem を複数インスタンス保持する
//     (= ベンチ毎 / プレイヤー毎)。Manager が queue を内蔵すると複雑化するため意図的に省く。
//   ・**ingredient 消費はクラフト「開始」時**: 中断時の返却を容易にするため、StartCraft
//     時点で全 ingredient を一括消費し、CancelCraft で同じ量を grant し戻す
//     (素材が「在庫から失われた = クラフト中」の状態を明示化する)。
//   ・**完了時の成果物 grant は Tick 内で**: Tick(dt) のフレームで完了判定が真になったら
//     即座に grant + CompleteCallback を発火。Status() は Tick の直後に Completed を
//     返し、次の StartCraft (or ClearAll) まで保持する。これにより UI が完了を 1 フレーム
//     確実に観測できる (callback だけだとフレームを逃すと取り逃す)。
//   ・**CanCraft は side effect なし const**: ingredient + workbench + level チェックの
//     pure な事前判定。CanCraft() が true なら StartCraft() も成功する不変条件を保つ。
//   ・**adapter 未設定でも CanCraft は workbench + level のみで判定**: query == nullptr
//     のケースは「在庫無制限 (デバッグ / テスト用)」と解釈し、ingredient チェックを
//     スキップする (= 同 Manager をプロトタイピング段階で使いやすくする)。
//     StartCraft の consume は adapter があるときだけ呼ぶ。
//   ・**required_workbench は nullptr / "" で「不要」**: 素手で作れる recipe を表現可能。
//   ・**重複登録は黙って弾く + WARN**: FEconomyDirector / FInventorySystem と同パターン。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有 + acs::TArray のみ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * レシピ 1 件分の素材エントリ。
 */
struct FIngredient {
    /** 素材アイテム id (FInventorySystem の item_id と一致する想定。文字列リテラル、非所有)。 */
    const char* item_id = nullptr;

    /** 必要個数。0 は「実質常に満たす」素材として扱う。 */
    u32         count   = 0;
};

/**
 * 1 件のクラフトレシピ定義 (immutable)。
 */
struct FCraftRecipe {
    /** 一意キー (FindRecipe / StartCraft のキー。文字列リテラル想定)。 */
    const char*       recipe_id          = nullptr;

    /** UI 表示名 (非所有)。 */
    const char*       display_name       = nullptr;

    /** 成果物の item_id (FInventorySystem の item_id 想定)。 */
    const char*       result_item_id     = nullptr;

    /** 1 回のクラフトで得られる成果物の個数 (1 以上)。 */
    u32               result_count       = 1;

    /** ingredients 配列の要素数。 */
    u32               ingredient_count   = 0;

    /** 素材配列 (呼出側が長寿命を保証する非所有バッファ。要素数 0 のとき nullptr 許容)。 */
    const FIngredient* ingredients        = nullptr;

    /** クラフト所要秒数。0 以下は「即座に完了」(次の Tick で grant)。 */
    f32               craft_duration_sec = 0.0f;

    /** 必要なワークベンチ id (StartCraft の current_workbench と一致しないと失敗。nullptr / "" で不要)。 */
    const char*       required_workbench = nullptr;

    /** この recipe を解放するプレイヤー必要レベル (player_level >= unlock_level で許可。0 で常に解放)。 */
    u32               unlock_level       = 0;
};

/**
 * 現在のクラフト状態。
 */
enum class ECraftStatus : u8 {
    /** 何もクラフトしていない (初期 / Cancel 後 / Completed の次の StartCraft 後)。 */
    Idle      = 0,

    /** Tick で進行中。 */
    Crafting  = 1,

    /** 直前の Tick で完了 → result が grant 済 (次の StartCraft / ClearAll まで保持)。 */
    Completed = 2,

    /** Crafting 中に CancelCraft で取り消し → ingredient 返却済。 */
    Cancelled = 3,
};

/**
 * レシピ登録 + 進行タイマ + インベントリ adapter で「素材を消費し時間経過後に成果物を得る」
 * クラフト挙動を扱う小型マネージャ。
 *
 * @details
 * レシピ定義と現在クラフト中の進行状態を保持し、素材の消費 / 成果物の付与は C 関数ポインタの
 * adapter 経由で行う (在庫表現に非依存)。同時クラフトは 1 件のみ。ingredient はクラフト開始時に
 * 一括消費し、CancelCraft で grant し戻す。完了は Tick 内で判定し grant + CompleteCallback を
 * 発火、Status() は次の StartCraft / ClearAll まで Completed を保持する。
 */
class FCraftingSystem {
public:
    /**
     * 在庫の現個数を取得する adapter の型。
     *
     * @param user SetInventoryAdapter で渡したコンテキスト (Manager は所有しない)。
     * @param item_id 対象アイテム id (リテラル参照)。
     * @return 在庫の現在個数 (0 以上)。
     */
    using InventoryQueryFn   = u32  (*)(void* user, const char* item_id) noexcept;

    /**
     * 在庫から count 個 consume する adapter の型。
     *
     * @param user SetInventoryAdapter で渡したコンテキスト。
     * @param item_id 対象アイテム id (リテラル参照)。
     * @param count 消費個数。
     * @return 全量消費成功なら true、不足等で失敗なら false。
     */
    using InventoryConsumeFn = bool (*)(void* user, const char* item_id, u32 count) noexcept;

    /**
     * 在庫に count 個 grant する adapter の型。
     *
     * @param user SetInventoryAdapter で渡したコンテキスト。
     * @param item_id 対象アイテム id (リテラル参照)。
     * @param count 付与個数。
     * @return 付与成功なら true、在庫満杯等で失敗なら false。
     */
    using InventoryGrantFn   = bool (*)(void* user, const char* item_id, u32 count) noexcept;

    /**
     * クラフト完了コールバックの型 (Tick 内で完了したフレームに発火)。
     *
     * @param user SetOnCompleteCallback で渡したコンテキスト。
     * @param recipe_id 完了したレシピ id (リテラル参照、登録時の id)。
     * @param result_item_id 成果物 id (リテラル参照)。
     * @param result_count 付与した個数。
     */
    using CompleteCallback = void (*)(void* user, const char* recipe_id,
                                      const char* result_item_id, u32 result_count) noexcept;

    /** 空のマネージャを構築する。 */
    FCraftingSystem()  noexcept = default;

    /** 破棄する。 */
    ~FCraftingSystem() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    FCraftingSystem(const FCraftingSystem&)            = delete;

    /** コピー代入も禁止。 */
    FCraftingSystem& operator=(const FCraftingSystem&) = delete;

    /** ムーブ禁止。 */
    FCraftingSystem(FCraftingSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FCraftingSystem& operator=(FCraftingSystem&&)      = delete;

    /**
     * レシピを登録する (起動時に 1 度ずつ)。
     *
     * @details
     * 同 recipe_id の 2 重登録は no-op (WARN)。recipe.recipe_id == nullptr も no-op。
     * result_count == 0 は 1 にクランプする。ingredient_count > 0 で ingredients == nullptr
     * の場合は ingredient_count を 0 に補正して受理する (配列指定漏れの救済)。
     * @param recipe 登録するレシピ定義 (内部にコピーされる)。
     */
    void RegisterRecipe(const FCraftRecipe& recipe) noexcept;

    /**
     * recipe_id で単一レシピを取得する。
     *
     * @details 返却ポインタは次の RegisterRecipe() / ClearAll() で無効化される可能性がある。
     * @param recipe_id 探すレシピ id。
     * @return 見つかったレシピへのポインタ (未登録 / nullptr は nullptr)。
     */
    const FCraftRecipe* FindRecipe(const char* recipe_id) const noexcept;

    /**
     * 登録済みレシピ件数を返す。
     *
     * @return 登録レシピ数。
     */
    u32 RecipeCount() const noexcept;

    /**
     * 全レシピの連続バッファを返す。
     *
     * @details 返却ポインタは RecipeCount() 件の連続バッファで、RegisterRecipe() / ClearAll()
     * で無効化される。
     * @param out_count レシピ件数を書き出す出力先。
     * @return 全レシピを指す先頭ポインタ。
     */
    const FCraftRecipe* AllRecipes(u32& out_count) const noexcept;

    /**
     * インベントリ adapter を設定する。
     *
     * @details
     * すべて nullptr でも動作する (在庫無制限デバッグモード)。部分設定も許容し、consume / grant
     * が nullptr の場合は「在庫操作なし成功」として扱う (テスト容易性)。
     * @param query 在庫個数取得 adapter (nullptr 可)。
     * @param consume 在庫消費 adapter (nullptr 可)。
     * @param grant 在庫付与 adapter (nullptr 可)。
     * @param user 各 adapter に渡すコンテキスト (所有しない)。
     */
    void SetInventoryAdapter(InventoryQueryFn   query,
                             InventoryConsumeFn consume,
                             InventoryGrantFn   grant,
                             void*              user) noexcept;

    /**
     * クラフト可能かを判定する (side effect なし)。
     *
     * @details
     * recipe_id が登録され、required_workbench が一致 (nullptr / "" は不要扱い)、
     * player_level >= unlock_level、かつ query adapter があるとき全 ingredient が必要数以上
     * 揃っていれば true。CanCraft() が true なら StartCraft() も成功する不変条件を保つ。
     * @param recipe_id クラフトするレシピ id。
     * @param current_workbench 現在のワークベンチ id (nullptr / "" はワークベンチ無し)。
     * @param player_level プレイヤーレベル (既定 999 = レベル制約なしのショートカット)。
     * @return クラフト可能なら true。
     */
    bool CanCraft(const char* recipe_id,
                  const char* current_workbench = nullptr,
                  u32         player_level      = 999) const noexcept;

    /**
     * クラフトを開始する (ingredient を一括 consume → Status = Crafting)。
     *
     * @details
     * 失敗時 (CanCraft が false / 既に Crafting / consume が途中失敗) は false を返し、途中失敗
     * 時は消費したぶんを巻き戻して side effect なしにする。既に Completed / Cancelled なら上書き
     * 許可。
     * @param recipe_id クラフトするレシピ id。
     * @param current_workbench 現在のワークベンチ id。
     * @param player_level プレイヤーレベル。
     * @return 開始できたら true。
     */
    bool StartCraft(const char* recipe_id,
                    const char* current_workbench,
                    u32         player_level) noexcept;

    /**
     * 進行中クラフトを中断する。
     *
     * @details Status == Crafting のときだけ動作 (それ以外は no-op)。ingredient を全量 grant し
     * 戻して Status = Cancelled に遷移する。
     */
    void CancelCraft() noexcept;

    /**
     * 現在のクラフト状態を返す。
     *
     * @return 現在の ECraftStatus。
     */
    ECraftStatus Status() const noexcept;

    /**
     * クラフト進行率を返す。
     *
     * @details Crafting 時のみ意味を持つ (それ以外は 0)。craft_duration_sec <= 0 のレシピでは 1.0。
     * @return 進行率 [0,1]。
     */
    f32 CraftProgress() const noexcept;

    /**
     * クラフトの残り秒数を返す。
     *
     * @details Crafting 時のみ意味を持つ (それ以外は 0)。
     * @return 残り秒数。
     */
    f32 CraftRemainingSec() const noexcept;

    /**
     * 現在のクラフト対象 recipe_id を返す。
     *
     * @details Crafting / Completed / Cancelled 時に有効。返却ポインタは登録時の生 id (リテラル参照)。
     * @return クラフト対象の recipe_id (Idle / 未開始は nullptr)。
     */
    const char* CurrentRecipeId() const noexcept;

    /**
     * 時間を進める。
     *
     * @details Crafting 中なら remaining_sec を dt 減算し、<= 0 で完了 → grant + CompleteCallback
     * 発火 + Status = Completed に遷移する。Crafting 以外や dt <= 0 は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 完了コールバックを設定する。
     *
     * @param cb 発火させる callback (nullptr で detach)。
     * @param user callback に渡すコンテキスト (所有しない)。
     */
    void SetOnCompleteCallback(CompleteCallback cb, void* user) noexcept;

    /**
     * レシピ + 現在状態 + adapter + callback をすべてクリアする (デバッグ / シーン切替時)。
     *
     * @details ingredient の返却は行わない (シーン切替で在庫ごと再構築する想定)。
     */
    void ClearAll() noexcept;

private:
    /**
     * recipe_id を per-byte 線形検索して m_Recipes 内 index を返す。
     *
     * @param recipe_id 探すレシピ id。
     * @return 見つかった index、未検出は kNotFound (~0u)。
     */
    u32 FindRecipeSlot(const char* recipe_id) const noexcept;

    /** レシピ定義 (起動時 immutable)。 */
    TArray<FCraftRecipe> m_Recipes;

    /** 在庫個数取得 adapter。 */
    InventoryQueryFn   m_Query   = nullptr;

    /** 在庫消費 adapter。 */
    InventoryConsumeFn m_Consume = nullptr;

    /** 在庫付与 adapter。 */
    InventoryGrantFn   m_Grant   = nullptr;

    /** 各 adapter に渡すコンテキスト。 */
    void*              m_InvUser = nullptr;

    /** 完了通知 callback。 */
    CompleteCallback m_OnComplete      = nullptr;

    /** 完了通知 callback に渡すコンテキスト。 */
    void*            m_OnCompleteUser = nullptr;

    /** 現在のクラフト状態。 */
    ECraftStatus _status               = ECraftStatus::Idle;

    /** 現在のクラフト対象 recipe (m_Recipes の要素を指す。Idle / ClearAll で nullptr)。 */
    const FCraftRecipe* m_CurrentRecipe = nullptr;

    /** 現在クラフトの所要秒数 (進行率計算用に保持。recipe 差し替えに対し安定)。 */
    f32 m_CurrentDurationSec          = 0.0f;

    /** 現在クラフトの残り秒数 (Tick で減算。完了 / Cancel で 0)。 */
    f32 m_CurrentRemainingSec         = 0.0f;
};

} // namespace acs::game
