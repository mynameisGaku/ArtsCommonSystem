// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — FPickupSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・slot+gen pattern: FCollisionWorld2D と統一。index 0 は予約。
//   ・Tick の処理順は「lifetime 進行 → 磁石 → 拾取判定」。lifetime expire と
//     pickup 拾取が同フレームに起こり得るが、lifetime expire を先に処理することで
//     「期限切れと同時に拾った」エッジケースを ExpireCallback に倒す。
//     プレイヤーは「期限切れの瞬間に拾った」より「期限切れで失った」の方を
//     許容する文化が一般的 (= 取り損ね演出で煽れる)。
//   ・LengthSq 比較で sqrt 回避。磁石移動は Normalize() を 1 回だけ実行 (Vec.h)。
//   ・SpawnRandomAt の kind 別既定値テーブル: gameplay 設計者向けの「すぐ動く既定」。
//     呼出側が細かく制御したい場合は直接 Spawn(info) を使うこと。
#include "gameframework/PickupSystem.h"
#include "gameframework/Random.h"
#include "math/Math.h"

namespace acs::game {

namespace {

/**
 * SpawnRandomAt 用の kind 別既定値。
 *
 * @details 数値は AAA でも indie でも違和感のない一般的なバランスを狙う。
 * 単位は world unit (サンプル既定で 1px 想定)。
 */
struct KindDefaults {
    /** 拾取半径。 */
    f32 radius;

    /** 磁石半径。 */
    f32 magnet_radius;

    /** 寿命 (秒、0 で無期限)。 */
    f32 lifetime_sec;

    /** 通貨価値 / 回復量等の数値。 */
    u32 value;
};

/**
 * kind 別の既定値テーブル。
 *
 * @details 列挙順は EPickupKind と一致し、インデックスは static_cast<u8>(kind)。
 * サイズは Custom を含む enum 値の数 (8 個)。
 */
constexpr KindDefaults kKindDefaults[8] = {
    // HealthOrb : 小さめ・吸引強め・短寿命・回復量 10
    { 10.0f,  60.0f,  8.0f, 10u },
    // ManaOrb   : 小さめ・吸引強め・短寿命・MP 10
    { 10.0f,  60.0f,  8.0f, 10u },
    // Coin      : 小・吸引強・短寿命・通貨 1
    {  8.0f,  80.0f, 10.0f,  1u },
    // Gem       : 中・吸引中・中寿命・通貨 10
    { 12.0f,  60.0f, 15.0f, 10u },
    // AmmoBox   : 大・吸引なし・長寿命・弾薬 30
    { 20.0f,  20.0f, 30.0f, 30u },
    // PowerUp   : 大・吸引なし・長寿命・パワー id (value で区別想定)
    { 18.0f,  20.0f, 20.0f,  1u },
    // EKey       : 中・吸引なし・無期限・id 1
    { 14.0f,   0.0f,  0.0f,  1u },
    // Custom    : 既定値は呼出側に任せる (radius のみ確保、magnet 0、無期限、value 0)
    { 10.0f,   0.0f,  0.0f,  0u },
};

} // namespace

/** 初期化する (再 Init は ClearAll と等価)。 */
void FPickupSystem::Init() noexcept {
    // 再 Init は ClearAll と等価 (ヘッダの仕様コメントと対応)。
    ClearAll();
}

/**
 * 未使用 slot を確保して index を返す。
 *
 * @details index 0 は予約なので i>=1 から inactive slot を線形検索し、初回は 0 番に dummy を
 * 置き、無ければ末尾に PushBack する。
 * @return 確保した slot の index。
 */
u32 FPickupSystem::AcquireSlot() noexcept {
    // index 0 は予約 (= invalid)。i >= 1 から線形検索で inactive slot を探す。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    // 初回は dummy slot を 0 番に置く (FCollisionWorld2D と同パターン)。
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/**
 * pickup を slot に登録して handle を返す。
 *
 * @details slot を確保し info をコピー、generation をインクリメント (0 はスキップ) して active 化する。
 * @param info 登録する pickup の定義。
 * @return 新規 PickupId。
 */
PickupId FPickupSystem::Spawn(const PickupInfo& info) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.info   = info;
    // gen をインクリメント。0 はスキップ (= invalid id 生成を回避)。
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_AliveCount;
    return PickupId{idx, s.gen};
}

/**
 * pickup を消滅させる (handle 検証あり、コールバックは発火しない)。
 *
 * @param id 消滅させる pickup の handle。
 */
void FPickupSystem::Despawn(PickupId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return;
    Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    if (m_AliveCount > 0) --m_AliveCount;
}

/**
 * 全 pickup の lifetime 進行・磁石効果・拾取判定を 1 パスで処理する。
 *
 * @param dt 経過時間 (秒)。
 * @param player_pos プレイヤーの世界座標。
 * @param magnet_strength 磁石による吸引速度 (world unit / sec)。
 */
void FPickupSystem::Tick(f32 dt, FVec2 player_pos, f32 magnet_strength) noexcept {
    const usize n = m_Slots.Size();
    // index 0 は予約なのでスキップ。
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;

        // ---- 1) lifetime 進行 ----------------------------------------------
        // lifetime_sec == 0.0f は「無期限」(= dt を進めない)。
        // > 0 のときだけ減算し、0 以下に落ちたら expire 処理。
        if (s.info.lifetime_sec > 0.0f) {
            s.info.lifetime_sec -= dt;
            if (s.info.lifetime_sec <= 0.0f) {
                // expire: コールバック発火 → Despawn。
                // 既消滅扱いになるので、後続の磁石 / 拾取は実行しない。
                const PickupId id{static_cast<u32>(i), s.gen};
                // ★ コールバック前に状態更新を完了させる。コールバック内で Spawn() が
                //   m_Slots を Grow→旧バッファ Free すると `s` が dangling になるため、
                //   callback 復帰後に s を触ってはいけない (UAF)。
                s.active = false;
                if (m_AliveCount > 0) --m_AliveCount;
                if (m_OnExpire != nullptr) {
                    m_OnExpire(m_OnExpireUser, id);
                }
                continue;
            }
        }

        // ---- 2) 磁石効果 ---------------------------------------------------
        // |player - pickup| < magnet_radius のとき player 方向へ引き寄せる。
        // LengthSq で比較して sqrt を 1 回減らす。
        const FVec2 to_player = player_pos - s.info.world_pos;
        const f32  dist_sq   = LengthSq(to_player);
        const f32  mr        = s.info.magnet_radius;
        if (mr > 0.0f && dist_sq < mr * mr) {
            // Normalize は 0 ベクトルで {0,0} を返す safe 実装 (math/Vec.h 参照)。
            const FVec2 dir = Normalize(to_player);
            s.info.world_pos += dir * (magnet_strength * dt);
        }

        // ---- 3) 拾取判定 ---------------------------------------------------
        // 磁石移動後の最新位置で再評価する (1 フレームで吸い込まれて拾える)。
        const FVec2 to_player2 = player_pos - s.info.world_pos;
        const f32  dist_sq2   = LengthSq(to_player2);
        const f32  r          = s.info.radius;
        if (r > 0.0f && dist_sq2 < r * r) {
            // pickup: コールバック発火 → Despawn。
            const PickupId id{static_cast<u32>(i), s.gen};
            // ★ コールバックに渡す値を事前にコピーし、状態更新も callback 前に完了させる
            //   (callback 内 Spawn() の m_Slots 再確保で s が dangling になる UAF を防ぐ)。
            const auto kind    = s.info.kind;
            const auto item_id = s.info.item_id;
            const auto value   = s.info.value;
            s.active = false;
            if (m_AliveCount > 0) --m_AliveCount;
            if (m_OnPickup != nullptr) {
                m_OnPickup(m_OnPickupUser, id, kind, item_id, value);
            }
        }
    }
}

/** active pickup の総数を返す。 */
u32 FPickupSystem::AlivePickupCount() const noexcept {
    return m_AliveCount;
}

/**
 * pickup の PickupInfo への生ポインタを返す (handle 検証あり)。
 *
 * @param id 取得する pickup の handle。
 * @return PickupInfo へのポインタ (無効 id / 既消滅は nullptr)。
 */
const PickupInfo* FPickupSystem::GetPickup(PickupId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return &s.info;
}

/**
 * 拾取コールバックと user コンテキストを設定する。
 *
 * @param cb 拾取時に呼ぶコールバック (nullptr で detach)。
 * @param user コールバックへ渡すコンテキスト。
 */
void FPickupSystem::SetOnPickupCallback(PickupCallback cb, void* user) noexcept {
    m_OnPickup      = cb;
    m_OnPickupUser = user;
}

/**
 * 寿命切れコールバックと user コンテキストを設定する。
 *
 * @param cb 失効時に呼ぶコールバック (nullptr で detach)。
 * @param user コールバックへ渡すコンテキスト。
 */
void FPickupSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    m_OnExpire      = cb;
    m_OnExpireUser = user;
}

/**
 * kind 別既定値で円内ランダムスポーンする。
 *
 * @details kind index を範囲 clamp し、spread_radius > 0 なら円板内一様サンプルで位置をずらして
 * Spawn する。item_id は未設定。
 * @param kind スポーンする pickup の種別。
 * @param center スポーン中心の世界座標。
 * @param spread_radius 散布半径。
 */
void FPickupSystem::SpawnRandomAt(EPickupKind kind, FVec2 center, f32 spread_radius) noexcept {
    // kind の index を範囲 clamp (enum 拡張時の保険)。
    u8 ki = static_cast<u8>(kind);
    if (ki >= 8u) ki = 7u;  // Custom にフォールバック
    const KindDefaults& d = kKindDefaults[ki];

    // 円板内一様サンプル。spread_radius <= 0 のときは center 真上。
    FVec2 offset = FVec2::Zero();
    if (spread_radius > 0.0f) {
        offset = FRandom::Global().PointInCircle(spread_radius);
    }

    PickupInfo info{};
    info.kind          = kind;
    info.item_id       = nullptr;  // SpawnRandomAt では item_id は未設定 (呼出側が必要なら Spawn 直呼び)
    info.world_pos     = center + offset;
    info.radius        = d.radius;
    info.magnet_radius = d.magnet_radius;
    info.lifetime_sec  = d.lifetime_sec;
    info.value         = d.value;
    (void)Spawn(info);
}

/**
 * 指定 kind の active pickup を全消滅させる (コールバックなし)。
 *
 * @param kind 消滅させる pickup の種別。
 */
void FPickupSystem::DespawnAllOfKind(EPickupKind kind) noexcept {
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (s.info.kind != kind) continue;
        s.active = false;
        if (m_AliveCount > 0) --m_AliveCount;
    }
}

/**
 * 指定 kind の active pickup 数を線形走査で数える。
 *
 * @param kind 数える pickup の種別。
 * @return 該当する active pickup 数。
 */
u32 FPickupSystem::CountOfKind(EPickupKind kind) const noexcept {
    u32 count = 0;
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (s.info.kind == kind) ++count;
    }
    return count;
}

/** 全 slot を破棄し alive 数を 0 にする (コールバック設定は維持)。 */
void FPickupSystem::ClearAll() noexcept {
    m_Slots.Clear();
    m_AliveCount = 0;
    // コールバック設定は維持 (ヘッダ仕様コメントと対応)。
}

} // namespace acs::game
