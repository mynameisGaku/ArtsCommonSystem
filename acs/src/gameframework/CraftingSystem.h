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
//   static const Ingredient kIronAxeIngredients[] = {
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
//   ・**レシピは単一 TArray<CraftRecipe>**: ジャンルキット規模 (200〜500 recipe) で
//     線形検索で十分。FEconomyDirector / FInventorySystem と同じ判断。
//   ・**Ingredient 配列は非所有**: CraftRecipe::ingredients は呼出側が長寿命を保証する
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
//
// 範囲外 (ジャンルキット Phase 2+ で):
//   ・並列クラフトキュー — 必要ならインスタンスを複数持つ。
//   ・確率成果物 / クラフト品質ロール — 本クラスは決定的に固定数を grant する。
//   ・レシピ習得アンロック (実績 / 進行) — FProgression / FAchievementManager と組み合わせて
//     unlock_level を更新する形で呼出側が表現する。
//   ・永続化 — Pillar J Serialize と統合 (本クラスはセッション内のみ)。
//   ・craft 中の中断状態 (再開可能 in-progress) — 仕様上 Cancel = 素材返却 + Idle へ統一。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- Ingredient: レシピ 1 つの素材エントリ ----------------------------------
// item_id : 素材アイテム id (FInventorySystem 側の item_id と一致する想定)。
//           文字列リテラル想定 (非所有)。
// count   : 必要個数 (1 以上)。0 を渡された場合は「実質常に満たす」素材として扱う。
struct Ingredient {
    const char* item_id = nullptr;
    u32         count   = 0;
};

// ---- CraftRecipe: 1 件のレシピ定義 (immutable) -----------------------------
// recipe_id          : 一意キー (FindRecipe / StartCraft のキー)。文字列リテラル想定。
// display_name       : UI 表示名 (非所有)。
// result_item_id     : 成果物の item_id (FInventorySystem の item_id 想定)。
// result_count       : 1 回のクラフトで得られる成果物の個数 (1 以上)。
// ingredient_count   : ingredients 配列の要素数。
// ingredients        : 素材配列 (呼出側が長寿命を保証する非所有バッファ)。
//                      ingredient_count == 0 のとき nullptr 許容 (素材不要 recipe)。
// craft_duration_sec : クラフト所要秒数。0 以下は「即座に完了」(次の Tick で grant)。
// required_workbench : クラフトに必要なワークベンチ id (StartCraft の current_workbench と
//                      一致しないと失敗)。nullptr / "" で「ワークベンチ不要」。
// unlock_level       : この recipe を解放するプレイヤー必要レベル (StartCraft の
//                      player_level >= unlock_level で許可)。0 で常に解放。
struct CraftRecipe {
    const char*       recipe_id          = nullptr;
    const char*       display_name       = nullptr;
    const char*       result_item_id     = nullptr;
    u32               result_count       = 1;
    u32               ingredient_count   = 0;
    const Ingredient* ingredients        = nullptr;
    f32               craft_duration_sec = 0.0f;
    const char*       required_workbench = nullptr;
    u32               unlock_level       = 0;
};

// ---- ECraftStatus: 現在のクラフト状態 ---------------------------------------
// Idle      : 何もクラフトしていない (初期 / Cancel 後 / Completed の次の StartCraft 後)。
// Crafting  : Tick で進行中。
// Completed : 直前の Tick で完了 → result が grant 済。次の StartCraft / ClearAll まで保持。
// Cancelled : Crafting 中に CancelCraft で取り消し → ingredient 返却済。
//             次の StartCraft で Idle 経由せず Crafting に上書きされる。
enum class ECraftStatus : u8 {
    Idle      = 0,
    Crafting  = 1,
    Completed = 2,
    Cancelled = 3,
};

// ---- FCraftingSystem ---------------------------------------------------------
class FCraftingSystem {
public:
    // インベントリ adapter (C 関数ポインタ + user)。STL <functional> 禁止のため。
    //   user    : SetInventoryAdapter で渡したコンテキスト (Manager は所有しない)
    //   item_id : 対象アイテム id (リテラル参照)
    //   count   : 個数 (consume / grant のときのみ意味を持つ)

    // 「在庫の現個数を取得」 — 戻り値: 現在の個数 (0 以上)。
    using InventoryQueryFn   = u32  (*)(void* user, const char* item_id) noexcept;

    // 「在庫から count 個 consume」 — 戻り値: 全量消費成功 true / 不足等で失敗 false。
    using InventoryConsumeFn = bool (*)(void* user, const char* item_id, u32 count) noexcept;

    // 「在庫に count 個 grant」 — 戻り値: 付与成功 true / 在庫満杯等で失敗 false。
    using InventoryGrantFn   = bool (*)(void* user, const char* item_id, u32 count) noexcept;

    // 完了コールバック。Tick(dt) 内で craft 完了したフレームに発火する。
    //   user           : SetOnCompleteCallback で渡したコンテキスト
    //   recipe_id      : 完了したレシピ id (リテラル参照、登録時の id を返す)
    //   result_item_id : 成果物 id (リテラル参照)
    //   result_count   : 付与した個数
    using CompleteCallback = void (*)(void* user, const char* recipe_id,
                                      const char* result_item_id, u32 result_count) noexcept;

    FCraftingSystem()  noexcept = default;
    ~FCraftingSystem() noexcept = default;

    FCraftingSystem(const FCraftingSystem&)            = delete;
    FCraftingSystem& operator=(const FCraftingSystem&) = delete;
    FCraftingSystem(FCraftingSystem&&)                 = delete;
    FCraftingSystem& operator=(FCraftingSystem&&)      = delete;

    // ---- レシピ定義 (起動時に 1 度ずつ) ----------------------------------
    // 同 recipe_id の 2 重登録は no-op (WARN)。`recipe.recipe_id == nullptr` も no-op。
    // `recipe.result_count == 0` は 1 にクランプ (defensive)。
    // ingredient_count > 0 で ingredients == nullptr の場合は ingredient_count を 0 に
    // 補正して受理 (起動順序エラーで配列指定漏れを救済)。
    void RegisterRecipe(const CraftRecipe& recipe) noexcept;

    // 単一 CraftRecipe 取得。未登録 / nullptr は nullptr。
    // 返却ポインタは次の RegisterRecipe() / ClearAll() で無効化される可能性がある。
    const CraftRecipe* FindRecipe(const char* recipe_id) const noexcept;

    // 登録 recipe 件数。
    u32 RecipeCount() const noexcept;

    // 全 recipe の生バッファ。`out_count` に件数を書き出す。
    // 返却ポインタは RecipeCount() 件の連続バッファ。RegisterRecipe() / ClearAll() で
    // 無効化される。
    const CraftRecipe* AllRecipes(u32& out_count) const noexcept;

    // ---- インベントリ adapter ---------------------------------------------
    // すべて nullptr でも動作する (= 在庫無制限デバッグモード)。
    // 部分設定 (query だけ等) は意図的に許容するが、StartCraft の consume / 完了時の
    // grant が nullptr の関数を踏むと「在庫操作なし成功」として扱われる (=テスト容易)。
    void SetInventoryAdapter(InventoryQueryFn   query,
                             InventoryConsumeFn consume,
                             InventoryGrantFn   grant,
                             void*              user) noexcept;

    // ---- クラフト判定 / 実行 ----------------------------------------------
    // 事前判定 (side effect なし):
    //   ・recipe_id が登録されていること
    //   ・required_workbench が一致 (nullptr / "" なら不要扱い)
    //   ・player_level >= unlock_level
    //   ・(query adapter があるとき) すべての ingredient が必要数以上揃っていること
    // current_workbench == nullptr / "" は「ワークベンチ無し」を意味する。
    // player_level のデフォルト 999 は「レベル制約なし」想定のショートカット (テスト等)。
    bool CanCraft(const char* recipe_id,
                  const char* current_workbench = nullptr,
                  u32         player_level      = 999) const noexcept;

    // クラフト開始 — ingredient を一括 consume → Status = Crafting に遷移。
    // 失敗時 (CanCraft が false / 既に Crafting / consume が途中失敗) は false を返し
    // side effect なし (途中失敗時は既に consume したぶんを巻き戻す)。
    // 既に Completed / Cancelled なら上書き許可 (= 次のクラフトに進める)。
    bool StartCraft(const char* recipe_id,
                    const char* current_workbench,
                    u32         player_level) noexcept;

    // クラフト中断 — Status == Crafting のときだけ動作 (Idle / Completed / Cancelled では no-op)。
    // ingredient を全量 grant し戻して Status = Cancelled に遷移。
    void CancelCraft() noexcept;

    // ---- 状態取得 ----------------------------------------------------------
    // 現在のクラフト状態。
    ECraftStatus Status() const noexcept;

    // クラフト進行率 [0, 1]。Crafting 時のみ意味を持つ (それ以外は 0)。
    // craft_duration_sec <= 0 のレシピでは Tick 後 1.0 を返す (即時完了)。
    f32 CraftProgress() const noexcept;

    // 残り秒数。Crafting 時のみ意味を持つ (それ以外は 0)。
    f32 CraftRemainingSec() const noexcept;

    // 現在のクラフト対象 recipe_id (Crafting / Completed / Cancelled 時に有効)。
    // Idle / 未開始は nullptr。返却ポインタは登録時の生 id (リテラル参照)。
    const char* CurrentRecipeId() const noexcept;

    // ---- 進行 --------------------------------------------------------------
    // 時間進行。Crafting 中なら remaining_sec を dt 減算し、<= 0 で完了 → grant +
    // CompleteCallback 発火 + Status = Completed に遷移。
    // Crafting 以外は no-op。dt <= 0 も no-op (defensive)。
    void Tick(f32 dt) noexcept;

    // ---- コールバック ------------------------------------------------------
    // cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnCompleteCallback(CompleteCallback cb, void* user) noexcept;

    // ---- 全リセット (デバッグ / シーン切替時) -----------------------------
    // レシピ + 現在状態 + adapter + callback をすべてクリア。
    // ingredient の返却は行わない (シーン切替で在庫ごと再構築する想定)。
    void ClearAll() noexcept;

private:
    // recipe_id → m_Recipes 内 index の per-byte 線形検索。未検出は ~0u。
    u32 FindRecipeSlot(const char* recipe_id) const noexcept;

    // レシピ定義 (起動時 immutable)。
    TArray<CraftRecipe> m_Recipes;

    // インベントリ adapter (C 関数ポインタ + user)。
    InventoryQueryFn   m_Query   = nullptr;
    InventoryConsumeFn m_Consume = nullptr;
    InventoryGrantFn   m_Grant   = nullptr;
    void*              m_InvUser = nullptr;

    // 完了通知 callback (C 関数ポインタ + user)。
    CompleteCallback m_OnComplete      = nullptr;
    void*            m_OnCompleteUser = nullptr;

    // 現在のクラフト状態。
    ECraftStatus _status               = ECraftStatus::Idle;
    // 現在のクラフト対象 recipe (= m_Recipes の要素を指す or nullptr)。
    // Crafting / Completed / Cancelled の間は有効、Idle / ClearAll で nullptr。
    const CraftRecipe* m_CurrentRecipe = nullptr;
    // craft_duration_sec — 進行率計算用に保持 (recipe を差し替えても安定)。
    f32 m_CurrentDurationSec          = 0.0f;
    // 残り秒数 (Tick で減算)。完了で 0、Cancel でも 0 にリセット。
    f32 m_CurrentRemainingSec         = 0.0f;
};

} // namespace acs::game
