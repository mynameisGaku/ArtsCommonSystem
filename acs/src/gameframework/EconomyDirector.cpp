// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CEconomyDirector 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・通貨 id / item_id は const char* per-byte 線形検索 (FEntitlement と同設計)。
//     件数 (通貨 2〜10 / 商品 100〜500) はオーダー的に線形で十分。
//   ・PurchaseItem は「失敗時 side effect なし」を保証するため、残高減算と
//     在庫減算の順序に注意 — 在庫先 check → 残高先 check → 両方通れば deduct +
//     stock-- を一気にやる (途中失敗が起きないため atomic と等価)。
//   ・stock_remaining == ~0u は「無制限」の哨兵値。Purchase 時には減算しない。
//   ・コールバックは `m_OnPurchase != nullptr` のときだけ呼ぶ。
//     失敗時 (item_id 不明等) でも呼出側 UI で失敗トーストを出したいので、
//     `item_id != nullptr` のときは success=false でも呼ぶ設計。
//   ・WARN は FEntitlement / CSeasonPass / CAchievementManager と同じ Log.h 経由。
#include "gameframework/EconomyDirector.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/**
 * const char* の per-byte 安全比較。
 *
 * @param a 比較文字列 1。
 * @param b 比較文字列 2。
 * @return 両者が等しければ true。どちらかが nullptr なら false。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/** 「id 未発見」を表す哨兵値。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

/** 在庫「無制限」を表す哨兵値 (Purchase 時に減算しない)。 */
constexpr u32 kStockUnlimited = ~static_cast<u32>(0);

/** 残高 (u32) の上限。AddToBalance のオーバーフロークランプで使う。 */
constexpr u32 kMaxBalance = ~static_cast<u32>(0);

} // namespace

/** currency_id に対応するスロット index を線形検索する (未発見は kNotFound)。 */
u32 CEconomyDirector::FindCurrencySlot(const char* currency_id) const noexcept {
    if (currency_id == nullptr) return kNotFound;
    const usize n = m_Currencies.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Currencies[i].id, currency_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** item_id に対応するスロット index を線形検索する (未発見は kNotFound)。 */
u32 CEconomyDirector::FindItemSlot(const char* item_id) const noexcept {
    if (item_id == nullptr) return kNotFound;
    const usize n = m_Items.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Items[i].item_id, item_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** 通貨を登録し残高を 0 で初期化する (id == nullptr / 同 id 重複は no-op)。 */
void CEconomyDirector::RegisterCurrency(const FCurrencyDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindCurrencySlot(def.id) != kNotFound) {
        ACS_LOG_WARN("CEconomyDirector: duplicate currency registration ignored ('%s')", def.id);
        return;
    }

    m_Currencies.PushBack(def);
    // 並行 TArray — 残高は 0 で初期化。
    m_Balances.PushBack(static_cast<u32>(0));
}

/** 指定通貨の残高を amount に上書きする (未登録通貨は no-op)。 */
void CEconomyDirector::SetBalance(const char* currency_id, u32 amount) noexcept {
    const u32 slot = FindCurrencySlot(currency_id);
    if (slot == kNotFound) return;
    m_Balances[slot] = amount;
}

/** 指定通貨の残高を返す (未登録通貨は 0)。 */
u32 CEconomyDirector::GetBalance(const char* currency_id) const noexcept {
    const u32 slot = FindCurrencySlot(currency_id);
    if (slot == kNotFound) return 0;
    return m_Balances[slot];
}

/** 指定通貨に delta を加算する (u32 上限でクランプ。未登録通貨は false)。 */
bool CEconomyDirector::AddToBalance(const char* currency_id, u32 delta) noexcept {
    const u32 slot = FindCurrencySlot(currency_id);
    if (slot == kNotFound) return false;

    // u32 オーバーフロー時は max にクランプ (AwardXp と同パターン)。
    u32& bal = m_Balances[slot];
    if (delta > kMaxBalance - bal) {
        bal = kMaxBalance;
    } else {
        bal += delta;
    }
    return true;
}

/** 指定通貨から delta を減算する (残高不足 / 未登録通貨は変更せず false)。 */
bool CEconomyDirector::DeductFromBalance(const char* currency_id, u32 delta) noexcept {
    const u32 slot = FindCurrencySlot(currency_id);
    if (slot == kNotFound) return false;

    u32& bal = m_Balances[slot];
    if (bal < delta) return false;  // 残高不足 — 変更しない
    bal -= delta;
    return true;
}

/** 商品を登録する (item_id == nullptr / 同 id 重複は no-op、未登録通貨参照でも受理)。 */
void CEconomyDirector::RegisterItem(const FShopItem& item) noexcept {
    // defensive: item_id == nullptr は意味を持たないので静かに弾く。
    if (item.item_id == nullptr) return;

    // 同 item_id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindItemSlot(item.item_id) != kNotFound) {
        ACS_LOG_WARN("CEconomyDirector: duplicate item registration ignored ('%s')", item.item_id);
        return;
    }

    // currency_id 未登録でも登録自体は受理する (起動順序依存を緩める)。
    // 購入時に未登録通貨を検出して失敗を返す。
    m_Items.PushBack(item);
}

/**
 * 商品を購入する。
 *
 * @details
 * 在庫・通貨・残高の全 check を済ませてから残高減算と在庫減算をまとめて行うことで
 * 「失敗時 side effect なし」を保証する。失敗時も item_id != nullptr ならコールバックを
 * success=false で呼ぶ (item_id == nullptr のみ完全 no-op)。
 * @param item_id 購入する商品 id。
 * @return 購入成功なら true。
 */
bool CEconomyDirector::PurchaseItem(const char* item_id) noexcept {
    // nullptr は完全 no-op (コールバックも呼ばない)。
    if (item_id == nullptr) return false;

    const u32 item_slot = FindItemSlot(item_id);
    if (item_slot == kNotFound) {
        // 不明 item_id でも UI で「不明な商品」エラーを出させたいので
        // コールバックは呼ぶ。生 item_id (= 呼出側が渡したポインタ) を返す。
        if (m_OnPurchase != nullptr) {
            m_OnPurchase(m_OnPurchaseUser, item_id, false);
        }
        return false;
    }

    // 以降は内部状態を参照する。失敗判定をすべて済ませてから side effect を
    // 起こすことで「失敗時 side effect なし」を保証する。
    FShopItem& item = m_Items[item_slot];

    // 在庫切れ判定 (~0u は無制限の哨兵値)。
    if (item.stock_remaining != kStockUnlimited && item.stock_remaining == 0) {
        if (m_OnPurchase != nullptr) {
            m_OnPurchase(m_OnPurchaseUser, item.item_id, false);
        }
        return false;
    }

    // 通貨登録 + 残高チェック。
    const u32 cur_slot = FindCurrencySlot(item.currency_id);
    if (cur_slot == kNotFound) {
        // 商品が未登録通貨を指していたケース (起動順序エラー等)。
        ACS_LOG_WARN(
            "CEconomyDirector: item '%s' references unknown currency '%s'",
            item.item_id,
            item.currency_id != nullptr ? item.currency_id : "<null>");
        if (m_OnPurchase != nullptr) {
            m_OnPurchase(m_OnPurchaseUser, item.item_id, false);
        }
        return false;
    }
    u32& bal = m_Balances[cur_slot];
    if (bal < item.price) {
        if (m_OnPurchase != nullptr) {
            m_OnPurchase(m_OnPurchaseUser, item.item_id, false);
        }
        return false;
    }

    // すべての check が通った — atomic に side effect を発生させる。
    bal -= item.price;
    if (item.stock_remaining != kStockUnlimited) {
        --item.stock_remaining;
    }

    if (m_OnPurchase != nullptr) {
        m_OnPurchase(m_OnPurchaseUser, item.item_id, true);
    }
    return true;
}

/** item_id に対応する商品を返す (未発見は nullptr)。 */
const FShopItem* CEconomyDirector::FindItem(const char* item_id) const noexcept {
    const u32 slot = FindItemSlot(item_id);
    if (slot == kNotFound) return nullptr;
    return &m_Items[slot];
}

/** 登録済み商品数を返す。 */
u32 CEconomyDirector::ItemCount() const noexcept {
    return static_cast<u32>(m_Items.Size());
}

/** 全商品配列の生ポインタを返す (out_count に件数を書き込む)。 */
const FShopItem* CEconomyDirector::AllItems(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Items.Size());
    return m_Items.Data();
}

/** 購入結果コールバックと user ポインタを設定する (cb == nullptr で detach)。 */
void CEconomyDirector::SetOnPurchaseCallback(PurchaseCallback cb, void* user) noexcept {
    // nullptr で detach は明示的に許可。
    m_OnPurchase      = cb;
    m_OnPurchaseUser = user;
}

/** 通貨・残高・商品・コールバックを全てリセットする。 */
void CEconomyDirector::ClearAll() noexcept {
    m_Currencies.Clear();
    m_Balances.Clear();
    m_Items.Clear();
    m_OnPurchase      = nullptr;
    m_OnPurchaseUser = nullptr;
}

} // namespace acs::game
