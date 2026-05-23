// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット — CraftingSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・recipe_id / item_id は const char* per-byte 線形検索 (Entitlement / EconomyDirector /
//     InventorySystem と同設計)。レシピ件数 (200〜500) なら線形で十分。
//   ・StartCraft は「ingredient 一括消費 → Crafting 遷移」の 1 動作にまとめる。
//     consume が途中失敗したケースを救済するため、消費した数を覚えておいて巻き戻し、
//     atomic に「全成功 or 全部もとのまま」を保証する。
//   ・CancelCraft は ingredient を grant し戻して Cancelled へ。grant が満杯等で失敗
//     しても進行は止め、状態は Cancelled に確定させる (= 中断は必ず成功する不変条件)。
//     満杯で grant に失敗するケースはレアだが、起きた場合は素材を失う代わりに進行は
//     確実に停止することを優先する (これがバグなのか仕様なのかは Manager の責務外)。
//   ・Tick は dt <= 0 と Crafting 以外を no-op で弾く。完了フレームでは grant + Status
//     を Completed に遷移させ、CompleteCallback を 1 回だけ呼ぶ。
//   ・StrEq は他 Manager と同じ per-byte 比較。required_workbench == nullptr / "" は
//     「ワークベンチ不要」として扱う (空文字とリテラルの両方を許容)。
//   ・adapter が一部 nullptr の場合は「該当操作はスキップして成功扱い」(=テスト容易性)。
//     query が nullptr なら CanCraft の ingredient チェックを飛ばし、
//     consume が nullptr なら StartCraft の消費を飛ばし、
//     grant が nullptr なら CancelCraft の返却と Tick 完了時の付与を飛ばす。
#include "gameframework/CraftingSystem.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

// const char* の per-byte 安全比較 (Entitlement / EconomyDirector / InventorySystem と同設計)。
// どちらかが nullptr なら false。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「ワークベンチ不要」を意味する文字列か (nullptr / 空文字)。
bool IsNoWorkbench(const char* s) noexcept {
    return s == nullptr || s[0] == '\0';
}

// 「id 未発見」を表す哨兵値。
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

// =============================================================================
// 内部検索
// =============================================================================

u32 CraftingSystem::FindRecipeSlot(const char* recipe_id) const noexcept {
    if (recipe_id == nullptr) return kNotFound;
    const usize n = _recipes.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_recipes[i].recipe_id, recipe_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// レシピ定義
// =============================================================================

void CraftingSystem::RegisterRecipe(const CraftRecipe& recipe) noexcept {
    // defensive: recipe_id == nullptr は意味を持たないので静かに弾く。
    if (recipe.recipe_id == nullptr) return;

    // 同 recipe_id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindRecipeSlot(recipe.recipe_id) != kNotFound) {
        ACS_LOG_WARN("CraftingSystem: duplicate recipe registration ignored ('%s')",
                     recipe.recipe_id);
        return;
    }

    CraftRecipe copy = recipe;
    // result_count == 0 は意味を持たないので 1 にクランプ。
    if (copy.result_count == 0) copy.result_count = 1;
    // ingredients が nullptr で ingredient_count > 0 の場合は count を 0 に補正
    // (起動順序エラーで配列指定漏れを救済 — 素材不要 recipe として登録する)。
    if (copy.ingredient_count > 0 && copy.ingredients == nullptr) {
        ACS_LOG_WARN(
            "CraftingSystem: recipe '%s' has ingredient_count=%u but ingredients=nullptr; "
            "treating as 0 ingredients",
            copy.recipe_id, copy.ingredient_count);
        copy.ingredient_count = 0;
    }

    _recipes.PushBack(copy);
}

const CraftRecipe* CraftingSystem::FindRecipe(const char* recipe_id) const noexcept {
    const u32 slot = FindRecipeSlot(recipe_id);
    if (slot == kNotFound) return nullptr;
    return &_recipes[slot];
}

u32 CraftingSystem::RecipeCount() const noexcept {
    return static_cast<u32>(_recipes.Size());
}

const CraftRecipe* CraftingSystem::AllRecipes(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_recipes.Size());
    return _recipes.Data();
}

// =============================================================================
// インベントリ adapter
// =============================================================================

void CraftingSystem::SetInventoryAdapter(InventoryQueryFn   query,
                                        InventoryConsumeFn consume,
                                        InventoryGrantFn   grant,
                                        void*              user) noexcept {
    _query    = query;
    _consume  = consume;
    _grant    = grant;
    _inv_user = user;
}

// =============================================================================
// クラフト判定 / 実行
// =============================================================================

bool CraftingSystem::CanCraft(const char* recipe_id,
                              const char* current_workbench,
                              u32         player_level) const noexcept {
    const u32 slot = FindRecipeSlot(recipe_id);
    if (slot == kNotFound) return false;
    const CraftRecipe& r = _recipes[slot];

    // unlock_level 判定。
    if (player_level < r.unlock_level) return false;

    // workbench 一致判定。required_workbench == nullptr / "" は「不要」扱い。
    if (!IsNoWorkbench(r.required_workbench)) {
        if (IsNoWorkbench(current_workbench)) return false;
        if (!StrEq(r.required_workbench, current_workbench)) return false;
    }

    // ingredient 充足判定 (query adapter があるときだけ)。
    // adapter なし = 「無制限デバッグモード」として ingredient チェックをスキップ。
    if (_query != nullptr && r.ingredient_count > 0 && r.ingredients != nullptr) {
        for (u32 i = 0; i < r.ingredient_count; ++i) {
            const Ingredient& ing = r.ingredients[i];
            if (ing.count == 0) continue;            // 0 個必要 = 常に満たす (defensive)
            if (ing.item_id == nullptr) continue;    // id 未指定 = スキップ (defensive)
            const u32 have = _query(_inv_user, ing.item_id);
            if (have < ing.count) return false;
        }
    }

    return true;
}

bool CraftingSystem::StartCraft(const char* recipe_id,
                                const char* current_workbench,
                                u32         player_level) noexcept {
    // 既に Crafting 中は上書き不可 (= 並列クラフト禁止、明示的に CancelCraft が必要)。
    if (_status == ECraftStatus::Crafting) return false;

    // CanCraft と同じ条件で事前判定。CanCraft が true なら StartCraft も成功する不変条件。
    if (!CanCraft(recipe_id, current_workbench, player_level)) return false;

    const u32 slot = FindRecipeSlot(recipe_id);
    if (slot == kNotFound) return false;  // CanCraft の判定で弾かれている筈だが defensive
    const CraftRecipe& r = _recipes[slot];

    // ingredient 一括 consume。途中失敗時は atomic に巻き戻す。
    // adapter consume == nullptr の場合は「消費スキップして進める」(=テスト容易性)。
    if (_consume != nullptr && r.ingredient_count > 0 && r.ingredients != nullptr) {
        u32 consumed_upto = 0;  // 巻き戻し用に「ここまで消費した」index を保持
        bool ok = true;
        for (u32 i = 0; i < r.ingredient_count; ++i) {
            const Ingredient& ing = r.ingredients[i];
            if (ing.count == 0 || ing.item_id == nullptr) {
                consumed_upto = i + 1;
                continue;
            }
            if (!_consume(_inv_user, ing.item_id, ing.count)) {
                ok = false;
                break;
            }
            consumed_upto = i + 1;
        }
        if (!ok) {
            // 巻き戻し — 既に消費したぶんを grant し戻す (adapter grant があれば)。
            if (_grant != nullptr) {
                for (u32 j = 0; j < consumed_upto; ++j) {
                    const Ingredient& ing = r.ingredients[j];
                    if (ing.count == 0 || ing.item_id == nullptr) continue;
                    _grant(_inv_user, ing.item_id, ing.count);
                }
            }
            ACS_LOG_WARN(
                "CraftingSystem: StartCraft('%s') aborted — consume failed at ingredient %u",
                r.recipe_id, consumed_upto);
            return false;
        }
    }

    // 状態遷移。
    _current_recipe         = &_recipes[slot];
    _current_duration_sec   = (r.craft_duration_sec > 0.0f) ? r.craft_duration_sec : 0.0f;
    _current_remaining_sec  = _current_duration_sec;
    _status                 = ECraftStatus::Crafting;
    return true;
}

void CraftingSystem::CancelCraft() noexcept {
    if (_status != ECraftStatus::Crafting) return;
    if (_current_recipe == nullptr) {
        // 不整合 (Crafting なのに recipe が nullptr) — defensive に Idle に戻す。
        _status                = ECraftStatus::Idle;
        _current_remaining_sec = 0.0f;
        _current_duration_sec  = 0.0f;
        return;
    }

    // ingredient 返却。grant adapter があるときだけ実行。
    // adapter なし or 満杯で grant 失敗の場合は素材を失うが、進行は確実に停止する
    // (= 中断は必ず Cancelled で確定させる不変条件を優先)。
    const CraftRecipe& r = *_current_recipe;
    if (_grant != nullptr && r.ingredient_count > 0 && r.ingredients != nullptr) {
        for (u32 i = 0; i < r.ingredient_count; ++i) {
            const Ingredient& ing = r.ingredients[i];
            if (ing.count == 0 || ing.item_id == nullptr) continue;
            _grant(_inv_user, ing.item_id, ing.count);
        }
    }

    _status                = ECraftStatus::Cancelled;
    _current_remaining_sec = 0.0f;
    // _current_recipe / _current_duration_sec は CurrentRecipeId() で参照できるよう保持。
}

// =============================================================================
// 状態取得
// =============================================================================

ECraftStatus CraftingSystem::Status() const noexcept {
    return _status;
}

f32 CraftingSystem::CraftProgress() const noexcept {
    if (_status != ECraftStatus::Crafting) return 0.0f;
    if (_current_duration_sec <= 0.0f) return 1.0f;  // 即時完了 recipe
    const f32 done = _current_duration_sec - _current_remaining_sec;
    const f32 p    = done / _current_duration_sec;
    if (p < 0.0f) return 0.0f;
    if (p > 1.0f) return 1.0f;
    return p;
}

f32 CraftingSystem::CraftRemainingSec() const noexcept {
    if (_status != ECraftStatus::Crafting) return 0.0f;
    return _current_remaining_sec;
}

const char* CraftingSystem::CurrentRecipeId() const noexcept {
    if (_current_recipe == nullptr) return nullptr;
    return _current_recipe->recipe_id;
}

// =============================================================================
// 進行
// =============================================================================

void CraftingSystem::Tick(f32 dt) noexcept {
    if (_status != ECraftStatus::Crafting) return;
    if (dt <= 0.0f)                       return;
    if (_current_recipe == nullptr) {
        // 不整合 — defensive に停止。
        _status = ECraftStatus::Idle;
        return;
    }

    // 残り秒数を進める (即時完了 recipe は最初から remaining <= 0 で即完了)。
    _current_remaining_sec -= dt;

    if (_current_remaining_sec <= 0.0f) {
        _current_remaining_sec = 0.0f;

        // 成果物 grant。adapter があるときだけ実行 (= 失敗しても完了は確定させる)。
        const CraftRecipe& r = *_current_recipe;
        if (_grant != nullptr && r.result_item_id != nullptr && r.result_count > 0) {
            const bool grant_ok = _grant(_inv_user, r.result_item_id, r.result_count);
            if (!grant_ok) {
                ACS_LOG_WARN(
                    "CraftingSystem: grant failed for recipe '%s' result '%s' x%u "
                    "(inventory full?) — completing anyway",
                    r.recipe_id, r.result_item_id, r.result_count);
            }
        }

        // 状態遷移 → Completed。callback は遷移後に呼ぶ (callback 内で状態取得しても
        // Completed が見えるようにする)。
        _status = ECraftStatus::Completed;
        if (_on_complete != nullptr) {
            _on_complete(_on_complete_user, r.recipe_id, r.result_item_id, r.result_count);
        }
    }
}

// =============================================================================
// コールバック
// =============================================================================

void CraftingSystem::SetOnCompleteCallback(CompleteCallback cb, void* user) noexcept {
    // nullptr で detach は明示的に許可。
    _on_complete      = cb;
    _on_complete_user = user;
}

// =============================================================================
// 全リセット
// =============================================================================

void CraftingSystem::ClearAll() noexcept {
    _recipes.Clear();
    _query            = nullptr;
    _consume          = nullptr;
    _grant            = nullptr;
    _inv_user         = nullptr;
    _on_complete      = nullptr;
    _on_complete_user = nullptr;
    _status                = ECraftStatus::Idle;
    _current_recipe        = nullptr;
    _current_duration_sec  = 0.0f;
    _current_remaining_sec = 0.0f;
}

} // namespace acs::game
