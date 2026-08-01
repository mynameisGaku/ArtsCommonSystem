// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — CPickupSystem (ドロップアイテム = Health/Coin/Powerup 等)
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
//       通貨残高 (CEconomyDirector::AddToBalance) を増やす等を行う。
//   ・Pillar F (Collision) との違い:
//     - CCollisionWorld2D は汎用 broad-phase shape クエリ。
//     - CPickupSystem は「Circle 形状の pickup を専用に高速処理する」軽量サブセット。
//       broad-phase は持たず O(N) で player との距離判定 (典型 N=10〜100)。
//
// 使い方:
//   CPickupSystem ps;
//   ps.Init();
//
//   FPickupInfo info{};
//   info.kind          = EPickupKind::Coin;
//   info.item_id       = "coin.gold";
//   info.world_pos     = { 100.0f, 50.0f };
//   info.radius        = 12.0f;    // 拾取半径
//   info.magnet_radius = 80.0f;    // 磁石半径
//   info.lifetime_sec  = 10.0f;    // 10 秒で消滅
//   info.value         = 5;        // 通貨価値 / 回復量等
//   FPickupId id = ps.Spawn(info);
//
//   ps.SetOnPickupCallback(&OnPickup, user);
//   ps.SetOnExpireCallback(&OnExpire, user);
//
//   // 毎フレーム
//   ps.Tick(dt, player_pos, /*magnet_strength=*/200.0f);
//
// 設計選択:
//   ・**FPickupId**: 32bit packed = 24bit index + 8bit generation。CCollisionWorld2D の
//     FShapeId / FNodeId と同じパターン。slot 再利用しても古い handle は無効化される。
//   ・**FSlot TArray**: 内部 `TArray<FSlot>` に固定。index 0 は予約 (= invalid)。
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
//   ・**SpawnRandomAt**: 円内ランダムスポーン。`FRandom::Global()` を使って
//     `center + PointInCircle(spread_radius)` で位置を決定。デフォルト radius /
//     magnet_radius / lifetime_sec / value は kind 別に内部で設定する (適当な
//     既定値を割り当てる)。具体的な数値は cpp で定義。
//   ・**DespawnAllOfKind / CountOfKind**: kind 別の bulk 操作 / 統計用。線形走査。
//   ・**非コピー・非ムーブ、全 noexcept、STL 不使用、`<string>` 禁止**:
//     ACS 規約に従う。item_id は所有しない const char* (= 文字列リテラル想定)。
//
// 範囲外:
//   ・broad-phase / 空間分割: pickup 数は小規模想定 (10〜100)。万を超える場合は
//     CCollisionWorld2D 側に shape を登録して overlap クエリする設計を検討。
//   ・物理ベース吸引 (加速度 / 速度減衰): 必要なら呼出側で magnet_strength を
//     pickup 毎に変えるラッパを作る。
//   ・kind の拡張: enum に新しい値を追加する場合、SpawnRandomAt の既定値テーブルも
//     更新すること。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 拾える物の分類。
 *
 * @details 一般的な 2D ゲームの pickup 7 種 +「ユーザ定義」の計 8 種。列挙値の数値は
 * Save/Load で永続化される想定の安定値なので、途中に新しい値を挿入しないこと。
 */
enum class EPickupKind : u8 {
    /** HP 回復オーブ。 */
    HealthOrb = 0,

    /** MP 回復オーブ。 */
    ManaOrb   = 1,

    /** 通貨 (コイン)。 */
    Coin      = 2,

    /** ジェム (高価値通貨)。 */
    Gem       = 3,

    /** 弾薬箱。 */
    AmmoBox   = 4,

    /** パワーアップ。 */
    PowerUp   = 5,

    /** 鍵。 */
    Key        = 6,

    /** ユーザ定義 (既定値テーブルでは最小限の設定)。 */
    Custom    = 7,
};

/**
 * 拾取アイテムの generational handle。
 *
 * @details 32bit packed = 下位 24bit index + 上位 8bit generation。0 = invalid。
 * CCollisionWorld2D の FShapeId / ANode の FNodeId と同じパターンで、slot を再利用しても
 * 古い handle は generation 不一致で無効化される。
 */
struct FPickupId {
    /** packed 値 (下位 24bit=index、上位 8bit=generation)。 */
    u32 m_Packed = 0;

    /** invalid handle (m_Packed == 0) を構築する。 */
    constexpr FPickupId() noexcept = default;

    /**
     * index と generation から packed handle を構築する。
     *
     * @param index slot インデックス (下位 24bit に格納)。
     * @param gen generation (上位 8bit に格納)。
     */
    constexpr FPickupId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot インデックスを返す。
     *
     * @return 下位 24bit の index。
     */
    constexpr u32  Index()      const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を返す。
     *
     * @return 上位 8bit の generation。
     */
    constexpr u8   Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed が非 0 なら true。
     */
    constexpr bool IsValid()    const noexcept { return m_Packed != 0; }

    /**
     * 等価比較。
     *
     * @param o 比較相手。
     * @return packed 値が一致すれば true。
     */
    constexpr bool operator==(FPickupId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等価比較。
     *
     * @param o 比較相手。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FPickupId o) const noexcept { return m_Packed != o.m_Packed; }
};

/** pickup 1 件の定義 (生成パラメータ + Tick で更新される状態)。 */
struct FPickupInfo {
    /** 種別 (HealthOrb / Coin / ...)。 */
    EPickupKind  kind          = EPickupKind::Custom;

    /** 任意の id (文字列リテラル想定、非所有)。コールバックで素通しされる。 */
    const char* item_id       = nullptr;

    /** 世界座標。Tick() の磁石効果により更新される。 */
    FVec2        world_pos     = FVec2::Zero();

    /** 拾取半径。player との距離がこれ未満で拾取発火。 */
    f32         radius        = 0.0f;

    /** 磁石半径。player との距離がこれ未満で player へ引き寄せられる (radius < magnet_radius 想定)。 */
    f32         magnet_radius = 0.0f;

    /** 寿命 (秒)。0.0f は無期限、正値は Tick() で dt 減算され 0 以下で失効。 */
    f32         lifetime_sec  = 0.0f;

    /** 通貨価値 / 回復量等の数値。コールバックで素通しされる。 */
    u32         value         = 0;
};

/**
 * 世界に配置された「拾える物」を管理する小型マネージャ。
 *
 * @details
 * HP/MP オーブ・通貨・ジェム・弾薬箱・パワーアップ・鍵などを統一的に扱い、player 位置との
 * 距離による磁石効果・拾取判定・lifetime 切れによる消滅を 1 つの Tick() でまとめて処理する。
 * broad-phase は持たず O(N) の距離判定 (典型 N=10〜100)。内部は slot+generation の TArray で
 * handle (FPickupId) を管理し、拾取/失効時に C 関数ポインタ + user のコールバックで通知する。
 * 非コピー・非ムーブ、全 noexcept、STL 不使用。
 */
class CPickupSystem {
public:
    /**
     * 拾取コールバックの型 (STL <functional> 禁止のため C 関数ポインタ + user)。
     *
     * @param user SetOnPickupCallback で渡したコンテキスト (Manager は所有しない)。
     * @param id 拾われた FPickupId (この時点で Despawn 済み)。
     * @param kind pickup 種別。
     * @param item_id pickup の item_id (生ポインタを素通し、文字列リテラル想定)。
     * @param value pickup の value (通貨価値 / 回復量等)。
     */
    using PickupCallback = void(*)(void* user, FPickupId id, EPickupKind kind,
                                    const char* item_id, u32 value) noexcept;

    /**
     * 寿命切れコールバックの型 (拾われずに lifetime が尽きて消滅した時に呼ばれる)。
     *
     * @param user SetOnExpireCallback で渡したコンテキスト。
     * @param id 消滅した FPickupId (この時点で Despawn 済み)。
     */
    using ExpireCallback = void(*)(void* user, FPickupId id) noexcept;

    /** 空の状態で構築する (slot なし、コールバック未設定)。 */
    CPickupSystem()  noexcept = default;

    /** デストラクタ。 */
    ~CPickupSystem() noexcept = default;

    /** コピー禁止。 */
    CPickupSystem(const CPickupSystem&)            = delete;

    /** コピー代入も禁止。 */
    CPickupSystem& operator=(const CPickupSystem&) = delete;

    /** ムーブ禁止。 */
    CPickupSystem(CPickupSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPickupSystem& operator=(CPickupSystem&&)      = delete;

    /** 初期化する (複数回呼び出し可。再 Init は ClearAll と等価)。 */
    void Init() noexcept;

    /**
     * pickup を世界に登録する。
     *
     * @param info 登録する pickup の定義。
     * @return 新規に割り当てた FPickupId (失敗時 invalid)。
     */
    FPickupId Spawn(const FPickupInfo& info) noexcept;

    /**
     * pickup を消滅させる (拾取扱いではなく、コールバックも発火しない)。
     *
     * @details 無効 id / 既消滅は no-op。
     * @param id 消滅させる pickup の handle。
     */
    void Despawn(FPickupId id) noexcept;

    /**
     * 毎フレーム呼んで全 pickup を更新する。
     *
     * @details
     * pickup ごとに次の順で処理する: (1) lifetime_sec > 0 なら dt 減算し 0 以下で
     * ExpireCallback + Despawn、(2) player との距離が magnet_radius 未満なら player 方向へ
     * magnet_strength * dt だけ引き寄せ、(3) 移動後の距離が radius 未満なら PickupCallback +
     * Despawn。距離比較は LengthSq で行う。
     * @param dt 経過時間 (秒)。
     * @param player_pos プレイヤーの世界座標。
     * @param magnet_strength 磁石による吸引速度 (world unit / sec)。
     */
    void Tick(f32 dt, FVec2 player_pos, f32 magnet_strength) noexcept;

    /**
     * active な pickup の総数を返す。
     *
     * @return active pickup 数。
     */
    u32 AlivePickupCount() const noexcept;

    /**
     * pickup の FPickupInfo への生ポインタを返す。
     *
     * @details 返したポインタは Despawn / Spawn / ClearAll で無効化される。
     * @param id 取得する pickup の handle。
     * @return FPickupInfo へのポインタ (無効 id / 既消滅は nullptr)。
     */
    const FPickupInfo* GetPickup(FPickupId id) const noexcept;

    /**
     * 拾取コールバックを設定する。
     *
     * @param cb 拾取時に呼ぶコールバック (nullptr で detach)。
     * @param user コールバックへ渡すコンテキスト (Manager は所有しない)。
     */
    void SetOnPickupCallback(PickupCallback cb, void* user) noexcept;

    /**
     * 寿命切れコールバックを設定する。
     *
     * @param cb 失効時に呼ぶコールバック (nullptr で detach)。
     * @param user コールバックへ渡すコンテキスト (Manager は所有しない)。
     */
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    /**
     * 円内にランダムスポーンする。
     *
     * @details center を中心に半径 spread_radius の円板内一様サンプル
     * (FRandom::Global().PointInCircle) で位置を決め、kind 別の既定値テーブル (.cpp 内定義) で
     * Spawn する。spread_radius <= 0 のときは center 真上。
     * @param kind スポーンする pickup の種別。
     * @param center スポーン中心の世界座標。
     * @param spread_radius 散布半径。
     */
    void SpawnRandomAt(EPickupKind kind, FVec2 center, f32 spread_radius) noexcept;

    /**
     * 指定 kind の pickup を全消滅させる (コールバックは呼ばない、bulk cleanup 用)。
     *
     * @param kind 消滅させる pickup の種別。
     */
    void DespawnAllOfKind(EPickupKind kind) noexcept;

    /**
     * 指定 kind の active pickup 数を返す。
     *
     * @param kind 数える pickup の種別。
     * @return 該当する active pickup 数。
     */
    u32 CountOfKind(EPickupKind kind) const noexcept;

    /** 全 pickup を消滅させる (コールバック設定は維持される)。 */
    void ClearAll() noexcept;

private:
    /** pickup 1 件分の slot (FPickupInfo + active フラグ + generation)。 */
    struct FSlot {
        /** pickup の定義 / 状態。 */
        FPickupInfo info{};

        /** この slot が使用中かのフラグ。 */
        bool       active = false;

        /** handle 検証用の generation。 */
        u8         gen    = 0;
    };

    /**
     * 未使用 slot を 1 つ確保し index を返す。
     *
     * @details index 0 は予約 (= invalid) なので i>=1 から線形検索し、無ければ末尾に確保する。
     * @return 確保した slot の index。
     */
    u32 AcquireSlot() noexcept;

    /** pickup slot 配列 (index 0 は予約)。 */
    TArray<FSlot>     m_Slots;

    /** active pickup 数のキャッシュ。 */
    u32             m_AliveCount = 0;

    /** 拾取コールバック (未設定なら nullptr)。 */
    PickupCallback  m_OnPickup       = nullptr;

    /** 拾取コールバックへ渡す user コンテキスト。 */
    void*           m_OnPickupUser  = nullptr;

    /** 寿命切れコールバック (未設定なら nullptr)。 */
    ExpireCallback  m_OnExpire       = nullptr;

    /** 寿命切れコールバックへ渡す user コンテキスト。 */
    void*           m_OnExpireUser  = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPickupSystem = CPickupSystem;

} // namespace acs::game
