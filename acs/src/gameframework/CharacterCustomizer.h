// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * cosmetic を装着する位置を表す固定 enum。
 *
 * @details
 * 11 種類で固定。slot index は m_EquippedInSlot[] 固定長配列のキーにも使うため
 * 数値順に連続させること (中抜けすると配列が穴あきになる)。ColorPalette は
 * 色変更用の特殊 slot (UI のカラー選択を保持)。
 */
enum class ECosmeticSlot : u8 {
    /** 頭部 (帽子・ヘルメット等)。 */
    Head         = 0,

    /** 胴体 (服・スキン等)。 */
    Body         = 1,

    /** 手 (手袋等)。 */
    Hands        = 2,

    /** 脚 (ズボン等)。 */
    Legs         = 3,

    /** 足 (靴等)。 */
    Feet         = 4,

    /** 利き手の武器の見た目。 */
    MainHand     = 5,

    /** 逆手の武器・盾の見た目。 */
    OffHand      = 6,

    /** 背負い物 (バックパック等)。 */
    Backpack     = 7,

    /** ペット / 連れ。 */
    Pet          = 8,

    /** 騎乗物。 */
    Mount        = 9,

    /** 色変更用の特殊 slot (UI のカラー選択を保持)。 */
    ColorPalette = 10,
};

/** 固定長配列のサイズ定数 (ECosmeticSlot enum の総数)。slot 追加時は手で更新する。 */
inline constexpr u32 kCosmeticSlotCount = 11;

/**
 * 1 つの cosmetic 定義 (登録後は immutable)。
 *
 * @details
 * 文字列は全て非所有 (呼出側が保証する文字列リテラル / 長寿命バッファを想定)。
 * Manager は rarity / display_name 等の中身を解釈せず、そのまま保持・参照する。
 */
struct FCosmeticItem {
    /** 一意キー (ストア / 永続化 / 検索)。文字列リテラル想定 (非所有)。 */
    const char*  id           = nullptr;

    /** UI 表示名 (非所有)。 */
    const char*  display_name = nullptr;

    /** 装着可能 slot (1 つの cosmetic は 1 slot のみに装着可)。 */
    ECosmeticSlot slot         = ECosmeticSlot::Head;

    /** メッシュ / マテリアル等のアセットパス (非所有)。レンダラが解釈する。 */
    const char*  asset_path   = nullptr;

    /** 課金 / プレミアム由来かどうか (UI バッジ表示用)。 */
    bool         is_premium   = false;

    /** "common" / "rare" / "epic" / "legendary" 等のレアリティラベル (非所有)。 */
    const char*  rarity       = nullptr;
};

/**
 * 装着 / 解除のたびに呼ばれる callback の型。
 *
 * @details
 * レンダラ側でこの callback を購読し、対応する mesh / material を差し替える。
 * @param user SetOnEquipCallback で渡したポインタ (Manager は中身を解釈しない)。
 * @param slot 変更があった slot。
 * @param item_id 装着された id (リテラル) or nullptr (= 解除)。
 */
using EquipCallback = void(*)(void* user, ECosmeticSlot slot, const char* item_id) noexcept;

/**
 * プレイヤーキャラの「見た目 (cosmetic)」を slot 単位で管理する高レベルマネージャ。
 *
 * @details
 * 帽子・服・靴・武器の見た目等を slot ごとに最大 1 つ装着する id ベースの軽量
 * レジストリ。メッシュ / マテリアル差し替えの実体は EquipCallback を購読した
 * 呼出側 (レンダラ / Skeletal Mesh コンポーネント側) が担う責務分離設計。扱うのは
 * 見た目のみで、装備性能 / 戦闘パラメータは一切変更しない (pay-to-win 回避)。
 * 装着には事前の UnlockCosmetic が必須。文字列は全て const char* 非所有。
 */
class CCharacterCustomizer {
public:
    /** 全 slot を未装着で構築する (TArray は空)。 */
    CCharacterCustomizer()  noexcept;

    /** 破棄する (非所有データのみ保持のため特別な後始末なし)。 */
    ~CCharacterCustomizer() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    CCharacterCustomizer(const CCharacterCustomizer&)            = delete;

    /** コピー代入も禁止。 */
    CCharacterCustomizer& operator=(const CCharacterCustomizer&) = delete;

    /** ムーブ禁止。 */
    CCharacterCustomizer(CCharacterCustomizer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CCharacterCustomizer& operator=(CCharacterCustomizer&&)      = delete;

    /**
     * cosmetic 定義を 1 件登録する (起動時に 1 度ずつ)。
     *
     * @details
     * 同 id の 2 重登録は no-op、id == nullptr も no-op (defensive)。登録時は
     * 未 unlock 状態で並行配列に追加される。
     * @param item 登録する cosmetic 定義 (内部にコピーされる)。
     */
    void RegisterCosmetic(const FCosmeticItem& item) noexcept;

    /**
     * 指定 id を unlocked 状態に遷移させる。
     *
     * @details
     * 本 API は cosmetic 専用 (性能向上アイテムは登録しないこと)。冪等で、既に
     * unlock 済みなら新規でないため false を返す。
     * @param id unlock する cosmetic の id。
     * @return 新規 unlock したら true、既 unlock or 未登録 id なら false。
     */
    bool UnlockCosmetic(const char* id) noexcept;

    /**
     * 指定 id が unlock 済みかを返す。
     *
     * @param id 照会する cosmetic の id。
     * @return unlock 済みなら true (未登録 / nullptr は false)。
     */
    bool IsUnlocked(const char* id) const noexcept;

    /**
     * 指定 id の cosmetic を該当 slot に装着する (既装着があれば自動で外れる)。
     *
     * @details
     * 装着には unlock 済みが必須。既存解除と新規装着の両方について callback が
     * 呼ばれる (解除側は item_id=nullptr)。既に同 slot へ同 id が装着済みの場合は
     * 新規でないため false。
     * @param id 装着する cosmetic の id。
     * @return 装着成功 (新規 or 同 slot の入れ替え) なら true。未登録 / 未 unlock /
     *         nullptr / 既に同 id 装着中なら false。
     */
    bool EquipCosmetic(const char* id) noexcept;

    /**
     * 指定 slot から装着を外す。
     *
     * @details 既に空なら no-op (callback も呼ばない)。
     * @param slot 装着を外す slot。
     */
    void UnequipSlot(ECosmeticSlot slot) noexcept;

    /**
     * 指定 slot に現在装着されている cosmetic の id を返す。
     *
     * @param slot 照会する slot。
     * @return 装着中の cosmetic id (未装着 / 範囲外は nullptr)。
     */
    const char* EquippedInSlot(ECosmeticSlot slot) const noexcept;

    /**
     * 登録済 cosmetic の総件数を返す。
     *
     * @return 登録済 cosmetic 数。
     */
    u32 CosmeticCount() const noexcept;

    /**
     * unlock 済みの件数を返す。
     *
     * @details UI で "23/120 unlocked" 表示等に使う。
     * @return unlock 済み cosmetic 数。
     */
    u32 UnlockedCount() const noexcept;

    /**
     * 指定 slot に該当する登録済 cosmetic の件数を返す (装着状態とは無関係)。
     *
     * @details UI で「Head slot の選択肢を一覧表示」する際に使う。
     * @param slot 数える対象の slot。
     * @return その slot に登録された cosmetic 数。
     */
    u32 CountInSlot(ECosmeticSlot slot) const noexcept;

    /**
     * 指定 id の cosmetic 定義へのポインタを返す。
     *
     * @details 返却ポインタは次の RegisterCosmetic() / ClearAll() で無効化されうる。
     * @param id 探す cosmetic の id。
     * @return 見つかった定義へのポインタ (見つからなければ nullptr)。
     */
    const FCosmeticItem* FindCosmetic(const char* id) const noexcept;

    /**
     * 全 cosmetic 定義の生バッファを返す。
     *
     * @details
     * 返却ポインタは CosmeticCount() 件の連続バッファで、RegisterCosmetic() /
     * ClearAll() で無効化される。
     * @param out_count 件数を書き出す先。
     * @return 先頭要素へのポインタ (0 件なら out_count=0)。
     */
    const FCosmeticItem* AllCosmetics(u32& out_count) const noexcept;

    /**
     * 装着 / 解除 callback を設定する (単一購読)。
     *
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すユーザポインタ。
     */
    void SetOnEquipCallback(EquipCallback cb, void* user) noexcept;

    /**
     * 登録・unlock・装着すべてをクリアする。
     *
     * @details callback は呼ばない (loop / ノイズ防止)。Save/Load 復元前のクリーンスタート用。
     */
    void ClearAll() noexcept;

private:
    /**
     * id 文字列を全 cosmetic から線形検索する。
     *
     * @param id 探す cosmetic の id。
     * @return 見つかった index (見つからない場合 ~0u)。
     */
    u32 FindIndex(const char* id) const noexcept;

    /** 登録済 cosmetic 定義 (登録後は immutable)。 */
    TArray<FCosmeticItem> m_Items;

    /** unlock 状態 (m_Items と並行配列、1:1 対応)。 */
    TArray<bool> m_Unlocked;

    /** slot ごとの現在装着 id (m_Items の id を指す非所有参照、未装着は nullptr)。 */
    const char* m_EquippedInSlot[kCosmeticSlotCount];

    /** 装着 / 解除 callback (nullptr で未設定)。 */
    EquipCallback m_OnEquip      = nullptr;

    /** 装着 callback に渡すユーザポインタ。 */
    void*         m_OnEquipUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FCharacterCustomizer = CCharacterCustomizer;

} // namespace acs::game
