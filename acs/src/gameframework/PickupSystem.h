// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — PickupSystem (ドロップアイテム = Health/Coin/Powerup 等)
//
// 世界に配置された「拾える物」を管理する小型マネージャ。HP オーブ / 通貨 / ジェム /
// 弾薬箱 / パワーアップ / 鍵などを統一的に扱い、プレイヤー位置との距離による
// 「磁石効果 (magnet)」「拾取 (pickup) 判定」「lifetime 切れによる消滅」を
// 1 つの Tick() でまとめて処理する。
//
// 想定する位置付け:
//   ・Pillar R (Items / Inventory) 系と Pillar I (Interaction) 系の橋渡し:
//     - 「持ち物」(Inventory) は別 Manager。本クラスは「世界に転がっている拾取」を
//       Pickup として表現し、拾うと PickupCallback で呼出側へ通知する。
//     - 呼出側はコールバックを受けて Inventory に AddItem する / HP を回復する /
//       通貨残高 (EconomyDirector::AddToBalance) を増やす等を行う。
//   ・Pillar F (Collision) との違い:
//     - CollisionWorld2D は汎用 broad-phase shape クエリ。
//     - PickupSystem は「Circle 形状の pickup を専用に高速処理する」軽量サブセット。
//       broad-phase は持たず O(N) で player との距離判定 (典型 N=10〜100)。
//
// 使い方:
//   PickupSystem ps;
//   ps.Init();
//
//   PickupInfo info{};
//   info.kind          = EPickupKind::Coin;
//   info.item_id       = "coin.gold";
//   info.world_pos     = { 100.0f, 50.0f };
//   info.radius        = 12.0f;    // 拾取半径
//   info.magnet_radius = 80.0f;    // 磁石半径
//   info.lifetime_sec  = 10.0f;    // 10 秒で消滅
//   info.value         = 5;        // 通貨価値 / 回復量等
//   PickupId id = ps.Spawn(info);
//
//   ps.SetOnPickupCallback(&OnPickup, user);
//   ps.SetOnExpireCallback(&OnExpire, user);
//
//   // 毎フレーム
//   ps.Tick(dt, player_pos, /*magnet_strength=*/200.0f);
//
// 設計選択:
//   ・**PickupId**: 32bit packed = 24bit index + 8bit generation。CollisionWorld2D の
//     ShapeId / NodeId と同じパターン。slot 再利用しても古い handle は無効化される。
//   ・**Slot Array**: 内部 `Array<Slot>` に固定。index 0 は予約 (= invalid)。
//     Spawn 時に inactive slot を線形検索 (典型 N が小さいので十分)、無ければ
//     末尾に PushBack。Despawn は active=false、slot は再利用。
//   ・**磁石効果は equal-step**: `|player - pickup| < magnet_radius` のとき
//     `pickup_pos += normalize(player - pickup) * magnet_strength * dt`。
//     物理的な近似 (加速度ベース) ではなく、「シンプルで予測可能な吸引」を採用。
//     これが Vampire Survivors / Hades / 90% のローグライクの標準的挙動。
//   ・**拾取判定**: 磁石移動後の pickup_pos と player の距離が `radius` 未満で発火。
//     近接条件は LengthSq で比較 (sqrt 不要)。
//   ・**lifetime**: 0.0f より大きいとき毎フレーム dt 減算。0 以下に落ちたら
//     ExpireCallback 発火 + Despawn。lifetime_sec == 0.0f は「無期限」として扱う
//     (= 永続 pickup、明示 Despawn / ClearAll() まで残る)。
//   ・**コールバックは 1 個固定 + user pointer**: STL <functional> 禁止のため。
//     PickupCallback / ExpireCallback はそれぞれ独立に設定可能 (片方だけ使う
//     ユースケースが多い)。
//   ・**SpawnRandomAt**: 円内ランダムスポーン。`Random::Global()` を使って
//     `center + PointInCircle(spread_radius)` で位置を決定。デフォルト radius /
//     magnet_radius / lifetime_sec / value は kind 別に内部で設定する (適当な
//     既定値を割り当てる)。具体的な数値は cpp で定義。
//   ・**DespawnAllOfKind / CountOfKind**: kind 別の bulk 操作 / 統計用。線形走査。
//   ・**非コピー・非ムーブ、全 noexcept、STL 不使用、`<string>` 禁止**:
//     ACS 規約に従う。item_id は所有しない const char* (= 文字列リテラル想定)。
//
// 範囲外:
//   ・broad-phase / 空間分割: pickup 数は小規模想定 (10〜100)。万を超える場合は
//     CollisionWorld2D 側に shape を登録して overlap クエリする設計を検討。
//   ・物理ベース吸引 (加速度 / 速度減衰): 必要なら呼出側で magnet_strength を
//     pickup 毎に変えるラッパを作る。
//   ・kind の拡張: enum に新しい値を追加する場合、SpawnRandomAt の既定値テーブルも
//     更新すること。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// ---- EPickupKind: 拾える物の分類 -------------------------------------------
// 一般的な 2D ゲームで使われる pickup の種類を 7 個と「ユーザ定義」を含めて 8 個。
// 列挙値の数値は安定値 (Save/Load で永続化される想定。途中に挿入しない)。
enum class EPickupKind : u8 {
    HealthOrb = 0,
    ManaOrb   = 1,
    Coin      = 2,
    Gem       = 3,
    AmmoBox   = 4,
    PowerUp   = 5,
    EKey       = 6,
    Custom    = 7,
};

// ---- PickupId: 拾取アイテムの handle ---------------------------------------
// 32bit packed = 24bit index + 8bit generation。0 = invalid。
// CollisionWorld2D::ShapeId / Node2D の NodeId と同パターン。
struct PickupId {
    u32 _packed = 0;

    constexpr PickupId() noexcept = default;
    constexpr PickupId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index()      const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept { return static_cast<u8>(_packed >> 24); }
    constexpr bool IsValid()    const noexcept { return _packed != 0; }

    constexpr bool operator==(PickupId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(PickupId o) const noexcept { return _packed != o._packed; }
};

// ---- PickupInfo: pickup 1 件の定義 -----------------------------------------
// kind          : 種別 (HealthOrb / Coin / ...)
// item_id       : 任意の id (文字列リテラル想定、非所有)。コールバックで素通しされる。
// world_pos     : 世界座標。Tick() で磁石効果により更新される。
// radius        : 拾取半径。player との距離がこれ未満で拾取発火。
// magnet_radius : 磁石半径。player との距離がこれ未満で player へ引き寄せられる。
//                 radius < magnet_radius を想定 (radius 内では磁石は既に効いている)。
// lifetime_sec  : 寿命 (秒)。0.0f は「無期限」。0 より大きい値は Tick() で dt 減算され
//                 0 以下になった瞬間に ExpireCallback 発火 + Despawn。
// value         : 通貨価値 / 回復量等の数値。コールバックで素通しされる。
struct PickupInfo {
    EPickupKind  kind          = EPickupKind::Custom;
    const char* item_id       = nullptr;
    Vec2        world_pos     = Vec2::Zero();
    f32         radius        = 0.0f;
    f32         magnet_radius = 0.0f;
    f32         lifetime_sec  = 0.0f;
    u32         value         = 0;
};

// ---- PickupSystem ----------------------------------------------------------
class PickupSystem {
public:
    // 拾取コールバック。STL <functional> 禁止のため C 関数ポインタ + user。
    //   user    : SetOnPickupCallback で渡したコンテキスト (Manager は所有しない)
    //   id      : 拾われた PickupId (この時点で Despawn 済み)
    //   kind    : pickup 種別
    //   item_id : pickup の item_id (生ポインタを素通し、文字列リテラル想定)
    //   value   : pickup の value (通貨価値 / 回復量等)
    using PickupCallback = void(*)(void* user, PickupId id, EPickupKind kind,
                                    const char* item_id, u32 value) noexcept;

    // 寿命切れコールバック。拾われずに lifetime が尽きて消滅した時に呼ばれる。
    //   user : SetOnExpireCallback で渡したコンテキスト
    //   id   : 消滅した PickupId (この時点で Despawn 済み)
    using ExpireCallback = void(*)(void* user, PickupId id) noexcept;

    PickupSystem()  noexcept = default;
    ~PickupSystem() noexcept = default;

    PickupSystem(const PickupSystem&)            = delete;
    PickupSystem& operator=(const PickupSystem&) = delete;
    PickupSystem(PickupSystem&&)                 = delete;
    PickupSystem& operator=(PickupSystem&&)      = delete;

    // 初期化。複数回呼び出し可 (再 Init は ClearAll と等価)。
    void Init() noexcept;

    // pickup を世界に登録。戻り値は新規 PickupId (失敗時 invalid)。
    PickupId Spawn(const PickupInfo& info) noexcept;

    // pickup を消滅させる (拾取扱いではなく、コールバックも発火しない)。
    // 無効 id / 既消滅は no-op。
    void Despawn(PickupId id) noexcept;

    // 毎フレーム呼ぶ。
    //   dt              : 経過時間 (秒)
    //   player_pos      : プレイヤー世界座標
    //   magnet_strength : 磁石による吸引速度 (world unit / sec)
    //
    // 処理順 (per pickup):
    //   1) lifetime_sec > 0 なら lifetime_sec -= dt。0 以下なら ExpireCallback + Despawn。
    //   2) |player - pickup| < magnet_radius なら pickup_pos を player 方向へ
    //      magnet_strength * dt だけ動かす。
    //   3) |player - pickup| < radius なら PickupCallback + Despawn。
    void Tick(f32 dt, Vec2 player_pos, f32 magnet_strength) noexcept;

    // active pickup の総数。
    u32 AlivePickupCount() const noexcept;

    // PickupInfo の生ポインタ取得 (Despawn / Spawn / ClearAll で無効化される)。
    // 無効 id / 既消滅は nullptr。
    const PickupInfo* GetPickup(PickupId id) const noexcept;

    // 拾取コールバック設定。cb = nullptr で detach。
    void SetOnPickupCallback(PickupCallback cb, void* user) noexcept;

    // 寿命切れコールバック設定。cb = nullptr で detach。
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    // 円内ランダムスポーン。center を中心に半径 spread_radius の円板内一様サンプル
    // (Random::Global().PointInCircle) で位置を決め、kind 別の既定値で Spawn する。
    // 既定値テーブルは .cpp 内で定義。
    void SpawnRandomAt(EPickupKind kind, Vec2 center, f32 spread_radius) noexcept;

    // 指定 kind の pickup を全消滅させる (コールバックは呼ばない、bulk cleanup 用)。
    void DespawnAllOfKind(EPickupKind kind) noexcept;

    // 指定 kind の active pickup 数を返す。
    u32 CountOfKind(EPickupKind kind) const noexcept;

    // 全 pickup を消滅。コールバック設定は維持される。
    void ClearAll() noexcept;

private:
    struct Slot {
        PickupInfo info{};
        bool       active = false;
        u8         gen    = 0;
    };

    // 未使用 slot を 1 つ確保し index を返す。index 0 は予約 (= invalid)。
    u32 AcquireSlot() noexcept;

    Array<Slot>     _slots;
    u32             _alive_count = 0;

    PickupCallback  _on_pickup       = nullptr;
    void*           _on_pickup_user  = nullptr;
    ExpireCallback  _on_expire       = nullptr;
    void*           _on_expire_user  = nullptr;
};

} // namespace acs::game
