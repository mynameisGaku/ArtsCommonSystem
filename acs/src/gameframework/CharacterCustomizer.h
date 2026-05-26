// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CharacterCustomizer (cosmetic 装備管理)
//
// プレイヤーキャラの「見た目」を装備する高レベルマネージャ。帽子・服・靴・武器の
// 見た目 (=  cosmetic) 等を slot 単位で 1 つずつ装着する。実装的には id ベースの
// 軽量レジストリで、メッシュ / マテリアル差し替えの実体は呼出側 (レンダラ / Skeletal
// Mesh コンポーネント側) が EquipCallback を購読して反応する責務分離設計。
//
// 倫理方針 (Entitlement.h / SeasonPass.h と一貫):
//   ・本クラスが扱うのは **見た目のみ (cosmetic)**。装備性能 / 戦闘パラメータの
//     書き換えは行わない。pay-to-win 設計を構造的に避けるための型レベル分離。
//   ・装備性能の変動が必要な場合はゲーム側で別レジストリ (e.g. 装備ステータス
//     マネージャ) を用意し、本クラスとは独立に動かす。
//
// 使い方:
//   CharacterCustomizer cc;
//
//   // 起動時にゲーム or アセットバンドル側で全 cosmetic を登録。
//   cc.RegisterCosmetic({ "hat.red_cap",   "Red Cap",   ECosmeticSlot::Head,
//                         "art/cosmetic/hat_red.fbx", false, "common" });
//   cc.RegisterCosmetic({ "skin.gold",     "Gold Skin", ECosmeticSlot::Body,
//                         "art/cosmetic/skin_gold.fbx", true,  "rare"   });
//   // ...
//
//   // ストア / クエスト報酬経由で unlock。実 entitlement 検証は呼出側 (Pillar O
//   // EntitlementRegistry / Pillar S Storefront) で済ませてから本 API に通知。
//   cc.UnlockCosmetic("hat.red_cap");
//
//   // 装着 callback を購読 (レンダラ側で見た目差し替えを反映する用)。
//   cc.SetOnEquipCallback(&OnEquipChanged, /*user*/ &renderer);
//
//   // UI からプレイヤーが選択。
//   if (cc.EquipCosmetic("hat.red_cap")) {
//       // renderer 側で対応 mesh をロード済みなら自動で見た目反映される。
//   }
//
// 設計選択 (Pillar O Phase 3):
//   ・**id は const char* 非所有**: ACS の STL 禁止方針 + Entitlement / Achievement
//     と一貫。文字列リテラル or 長寿命バッファ前提 (呼出側保証)。
//   ・**slot は固定 enum**: ECosmeticSlot は 11 種類で固定。slot ごとに最大 1 つの
//     cosmetic が装着可能 (装着すると同 slot の既存装着は自動で外れる)。
//     ColorPalette は色変更用の特殊 slot (UI のカラー選択を保持)。
//   ・**Def + Unlocked 状態を並行 TArray で持つ**: AchievementManager と同じ Def/State
//     分離。CosmeticItem は immutable な定義、unlocked は実行時 bool。1:1 対応で
//     同 index を共有 (TArray<CosmeticItem> + TArray<bool>)。
//   ・**装着状態は slot indexed const char* 配列**: 線形検索を避けるため、slot を
//     index にした固定長 const char*[kSlotCount] を持つ。各エントリは「現在装着
//     されている cosmetic の id」(未装着なら nullptr)。 EquipCosmetic / UnequipSlot
//     で O(1) 更新できる。
//   ・**EquipCallback は単一購読**: レンダラ側の差し替え駆動用に raw 関数ポインタ
//     + user data。複数購読は本フェーズでは扱わない (将来必要なら EventBus 経由
//     に切り替える)。装着 / 解除いずれも同 callback を呼び、item_id == nullptr で
//     「解除」を表現する。
//   ・**Unlock は冪等 + bool 返し**: 既 unlock 済 id に再 Unlock は false (新規でない)。
//     未登録 id への Unlock も false。新規 unlock 成功時のみ true。
//   ・**Equip は unlock 必須**: IsUnlocked(id) == false の cosmetic は装着拒否。
//     UI 側で「グレーアウト + locked 表示」を実装する前提。
//   ・**線形検索**: cosmetic 件数は AAA タイトルでも通常 200〜1000 のオーダー。
//     per-byte 文字列比較で十分 (Entitlement / AchievementManager と同じ判断)。
//   ・**ClearAll は登録も装着も両方リセット**: Save/Load 復元前のクリーンスタート用。
//     callback は呼ばない (loop / ノイズ防止)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 3+ で):
//   ・永続化 (Save/Load) — Pillar J Serialize と統合
//   ・複数装着 (e.g. アクセサリ slot に同時 3 つ) — 現状は slot あたり最大 1
//   ・cosmetic の合成・染色 (ColorPalette と base アイテムの合算) — Phase 3+
//   ・サーバ側 unlock 検証 — Pillar V Backend Services に委譲
//   ・ストア連携 (購入トランザクション) — Pillar S Storefront に委譲
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- ECosmeticSlot: 装着位置 ------------------------------------------------
// 11 種類で固定 (Phase 3)。slot index は固定長配列のキーとしても使うため、
// 数値順に連続させること (中抜けすると _equipped_in_slot[] が穴あきになる)。
// ColorPalette は色変更用の特殊 slot (UI のカラー選択を保持)。
enum class ECosmeticSlot : u8 {
    Head         = 0,
    Body         = 1,
    Hands        = 2,
    Legs         = 3,
    Feet         = 4,
    MainHand     = 5,
    OffHand      = 6,
    Backpack     = 7,
    Pet          = 8,
    Mount        = 9,
    ColorPalette = 10,
};

// 固定長配列のサイズ定数 (ECosmeticSlot enum の総数)。
// 新規 slot 追加時はここも更新する (static_assert はないので手で守る)。
inline constexpr u32 kCosmeticSlotCount = 11;

// ---- CosmeticItem: 1 つの cosmetic 定義 (immutable) ------------------------
// id          : 一意キー (ストア / 永続化 / 検索)。文字列リテラル想定 (非所有)。
// display_name: UI 表示名 (非所有)。
// slot        : 装着可能 slot (1 つの cosmetic は 1 slot のみに装着可)。
// asset_path  : メッシュ / マテリアル等のアセットパス (非所有)。レンダラが解釈。
// is_premium  : 課金 / プレミアム由来かどうか (UI バッジ表示用)。
// rarity      : "common" / "rare" / "epic" / "legendary" 等のレアリティラベル。
//               非所有文字列。UI 側で色分けに使う想定 (Manager は中身を解釈しない)。
struct CosmeticItem {
    const char*  id           = nullptr;
    const char*  display_name = nullptr;
    ECosmeticSlot slot         = ECosmeticSlot::Head;
    const char*  asset_path   = nullptr;
    bool         is_premium   = false;
    const char*  rarity       = nullptr;
};

// ---- EquipCallback ---------------------------------------------------------
// 装着 / 解除のたびに呼ばれる。
//   user    : SetOnEquipCallback で渡したポインタ (Manager は中身を解釈しない)。
//   slot    : 変更があった slot。
//   item_id : 装着された id (リテラル) or nullptr (= 解除)。
// レンダラ側でこの callback を購読し、対応する mesh / material を差し替える。
using EquipCallback = void(*)(void* user, ECosmeticSlot slot, const char* item_id) noexcept;

// ---- CharacterCustomizer ---------------------------------------------------
class CharacterCustomizer {
public:
    CharacterCustomizer()  noexcept;
    ~CharacterCustomizer() noexcept = default;

    CharacterCustomizer(const CharacterCustomizer&)            = delete;
    CharacterCustomizer& operator=(const CharacterCustomizer&) = delete;
    CharacterCustomizer(CharacterCustomizer&&)                 = delete;
    CharacterCustomizer& operator=(CharacterCustomizer&&)      = delete;

    // ---- 定義登録 (起動時に 1 度ずつ) ------------------------------------
    // 同 id の 2 重登録は no-op、`id == nullptr` も no-op (defensive)。
    // 未知の slot enum 値も拒否しないが、kCosmeticSlotCount 内のものだけ
    // 装着できる (内部 cast 時にクランプ)。
    void RegisterCosmetic(const CosmeticItem& item) noexcept;

    // ---- Unlock 管理 -----------------------------------------------------
    // 指定 id を unlocked 状態に遷移させる。
    // 戻り値: 新規 unlock したら true、既 unlock or 未登録 id なら false。
    // 倫理: 本 API は cosmetic 専用。性能向上 cosmetic を登録しないこと。
    bool UnlockCosmetic(const char* id) noexcept;

    // 指定 id が unlock 済みか。未登録 / nullptr は false。
    bool IsUnlocked(const char* id) const noexcept;

    // ---- 装着 / 解除 -----------------------------------------------------
    // 指定 id の cosmetic を該当 slot に装着する。既装着があれば自動で外れる。
    // 戻り値: 装着成功 (新規 or 同 slot の入れ替え) → true。
    // 失敗 (false): 未登録 / 未 unlock / nullptr / 既に同 slot に同 id が装着中。
    // callback は新規装着 + 既存解除の両方について呼ばれる (解除側は item_id=nullptr)。
    bool EquipCosmetic(const char* id) noexcept;

    // 指定 slot から装着を外す。既に空なら no-op (callback も呼ばない)。
    void UnequipSlot(ECosmeticSlot slot) noexcept;

    // 指定 slot に現在装着されている cosmetic の id を返す。未装着 / 範囲外は nullptr。
    const char* EquippedInSlot(ECosmeticSlot slot) const noexcept;

    // ---- 照会 ------------------------------------------------------------
    // 登録済 cosmetic の総件数。
    u32 CosmeticCount() const noexcept;

    // unlock 済みの件数。UI で "23/120 unlocked" 表示等に使う。
    u32 UnlockedCount() const noexcept;

    // 指定 slot に該当する登録済 cosmetic の件数。装着状態とは無関係。
    // UI で「Head slot の選択肢を一覧表示」する際に使う。
    u32 CountInSlot(ECosmeticSlot slot) const noexcept;

    // 単一 cosmetic 取得。見つからなければ nullptr。返却ポインタは次の
    // RegisterCosmetic() / ClearAll() で無効化される可能性がある。
    const CosmeticItem* FindCosmetic(const char* id) const noexcept;

    // 全 cosmetic の生バッファ。`out_count` に件数を書き出して返す。
    // 返却ポインタは CosmeticCount() 件の連続バッファ、RegisterCosmetic() /
    // ClearAll() で無効化される。
    const CosmeticItem* AllCosmetics(u32& out_count) const noexcept;

    // ---- Callback --------------------------------------------------------
    // 装着 / 解除 callback を設定 (単一購読)。cb = nullptr で解除。
    void SetOnEquipCallback(EquipCallback cb, void* user) noexcept;

    // ---- リセット --------------------------------------------------------
    // 登録・unlock・装着すべてをクリア。callback は呼ばない (loop 防止)。
    void ClearAll() noexcept;

private:
    // id 文字列を全 cosmetic から線形検索。見つからない場合 ~0u。
    u32 FindIndex(const char* id) const noexcept;

    // 登録済 cosmetic 定義 (immutable)。
    TArray<CosmeticItem> _items;

    // unlock 状態 (_items と並行配列、1:1 対応)。
    TArray<bool> _unlocked;

    // slot ごとの現在装着 id (kCosmeticSlotCount 個の固定長配列)。
    // 値は _items の id をそのまま指すリテラル参照 (非所有)。
    const char* _equipped_in_slot[kCosmeticSlotCount];

    // 装着 callback (関数ポインタ + user data)。両方 nullptr で未設定。
    EquipCallback _on_equip      = nullptr;
    void*         _on_equip_user = nullptr;
};

} // namespace acs::game
