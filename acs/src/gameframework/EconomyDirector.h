// SPDX-License-Identifier: Apache-2.0
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
