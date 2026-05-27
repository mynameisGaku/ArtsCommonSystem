// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — FEntitlement (DLC / FSeasonPass / Cosmetic 権利チェック)
//
// プレイヤーが「持っているかどうか」をゲームロジック側から問い合わせる窓口。
// DLC、シーズンパス、バトルパス、コスメティックパック、グッズ同梱の引換コード等の
// **権利情報** (entitlement) をローカルに保持し、ストアからの取得結果や
// プラットフォーム SDK (Pillar S = Steamworks / EOS / 各家庭機 SDK) の問い合わせ結果を
// Add() で流し込んでもらう想定。EntitlementRegistry 自体は **ストア非依存** であり、
// 配信プラットフォームに紐付かない (Pillar S 側がアダプタ層になる)。
//
// 設計上の倫理方針 (LiveOps と pay-to-win の境界):
//   ・本レジストリは **cosmetic 中心** で運用することを推奨する。装備性能や
//     ゲームプレイのコア体験を金銭で買わせる pay-to-win 設計は、開発者の責任で
//     避けるべきという立場 (ACS の出荷フィルタ層には強制機構を置かないが、
//     IsActive() を見て「攻撃力 +10」のようなコードを書くと簡単に踏み外せる)。
//   ・GoodsRedeemCode は物販同梱のリアル商品コード等を想定。再現性ある安全な
//     redeem フローは Pillar S 側で実装し、ここには結果だけが流れてくる。
//
// 使い方:
//   EntitlementRegistry reg;
//   reg.Add({ "dlc.expansion_1",  EntitlementKind::Dlc,           true  });
//   reg.Add({ "cosmetic.hat_red", EntitlementKind::CosmeticPack,  true  });
//
//   if (reg.IsActive("dlc.expansion_1")) UnlockExpansionMap();
//   if (reg.HasAny(EntitlementKind::FSeasonPass)) ShowSeasonPassBadge();
//
// 設計選択:
//   ・id は **const char* 非所有**: ACS の STL 禁止方針 + 寿命管理単純化のため、
//     呼び出し側が文字列リテラル or 長寿命バッファを保証する。短命バッファを
//     渡すと dangling になる点は API ドキュメントで明示。
//   ・コピー / ムーブ禁止: registry は通常 1 つの長寿命オブジェクトで運用される。
//     誤って値渡しされた結果として entitlement が分裂すると検知しづらいため、
//     最初から非コピー・非ムーブで固定する。
//   ・全 noexcept: 例外不使用方針 (TResult<T,E> + bool 戻り値)。
//
// 範囲外 (本フェーズでは扱わない):
//   ・永続化 / シリアライズ (Pillar J Serialize 側で扱う)
//   ・プラットフォーム SDK 連携の具象 (Pillar S Storefront 側で実装、こちらは依存しない)
//   ・期限付き entitlement (期限切れ判定や残時間照会) — `active` フラグの更新を
//     ストア側で行う前提とし、本レジストリは時間概念を持たない
//   ・サーバ検証 (Pillar V Backend Services 側で実装)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 権利種別。Pillar S 側のアダプタが分類してから Add() に渡す前提で、
// レジストリは単に分類タグとして保持・検索キーに使う。
enum class EntitlementKind : u8 {
    Dlc,                // 追加コンテンツ (マップ / シナリオ / キャラ等)
    FSeasonPass,         // 一定期間 / シーズン束ねの権利
    BattlePass,         // tier 進行型 (cosmetic 中心を推奨)
    CosmeticPack,       // 見た目だけのスキン・装飾
    GoodsRedeemCode,    // 物販同梱コード等から redeem された結果
};

// 1 つの権利情報。`id` は registry 側で所有しない (呼び出し側保証)。
struct EntitlementInfo {
    const char*     id     = nullptr;
    EntitlementKind kind   = EntitlementKind::Dlc;
    bool            active = false;  // false で「持ってはいるが現在無効」を表現可
};

class EntitlementRegistry {
public:
    EntitlementRegistry()  noexcept = default;
    ~EntitlementRegistry() noexcept = default;

    EntitlementRegistry(const EntitlementRegistry&)            = delete;
    EntitlementRegistry& operator=(const EntitlementRegistry&) = delete;
    EntitlementRegistry(EntitlementRegistry&&)                 = delete;
    EntitlementRegistry& operator=(EntitlementRegistry&&)      = delete;

    // 新規 entitlement を登録。同一 id の重複は禁止せず、後追いで上書き挙動には
    // しない (Pillar S 側で dedup する想定)。`id == nullptr` は no-op で防御。
    void Add(EntitlementInfo info) noexcept;

    // id が登録済みか (active 不問)。`id == nullptr` は false。
    bool Has(const char* id) const noexcept;

    // id が登録済み **かつ** active か。`id == nullptr` は false。
    bool IsActive(const char* id) const noexcept;

    // 指定 kind の active な entitlement が 1 つでもあるか。
    // 「シーズンパス所持者向け UI を出すか」等の判定に使う。
    bool HasAny(EntitlementKind k) const noexcept;

    // 全削除 (ストア再同期時に呼ばれる想定)。
    void Clear() noexcept;

    // 件数 (active / inactive 含む)。
    u32 Count() const noexcept;

    // 生バッファ getter。デバッグ表示 / イテレーション用。
    // 戻り値は Count() 件の連続バッファ、Clear() / Add() で無効化される。
    const EntitlementInfo* AllInfos() const noexcept;

private:
    TArray<EntitlementInfo> m_Infos;
};

} // namespace acs::game
