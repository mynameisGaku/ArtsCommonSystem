// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — PickupSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・slot+gen pattern: CollisionWorld2D と統一。index 0 は予約。
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

// ---- kind 別の既定値 (SpawnRandomAt 用) ------------------------------------
// 数値は AAA でも indie でも違和感のない一般的なバランスを狙う。
// 単位: world unit (= サンプル既定で 1px 想定)。
struct KindDefaults {
    f32 radius;
    f32 magnet_radius;
    f32 lifetime_sec;
    u32 value;
};

// 列挙順 = EPickupKind enum 順。インデックスは static_cast<u8>(kind)。
// 配列サイズ = enum 値の数 (Custom 含む 8 個)。
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

// =============================================================================
// 初期化 / Slot 取得
// =============================================================================

void PickupSystem::Init() noexcept {
    // 再 Init は ClearAll と等価 (ヘッダの仕様コメントと対応)。
    ClearAll();
}

u32 PickupSystem::AcquireSlot() noexcept {
    // index 0 は予約 (= invalid)。i >= 1 から線形検索で inactive slot を探す。
    for (u32 i = 1; i < _slots.Size(); ++i) {
        if (!_slots[i].active) return i;
    }
    // 初回は dummy slot を 0 番に置く (CollisionWorld2D と同パターン)。
    if (_slots.IsEmpty()) {
        _slots.PushBack({});
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

// =============================================================================
// Spawn / Despawn
// =============================================================================

PickupId PickupSystem::Spawn(const PickupInfo& info) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.info   = info;
    // gen をインクリメント。0 はスキップ (= invalid id 生成を回避)。
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++_alive_count;
    return PickupId{idx, s.gen};
}

void PickupSystem::Despawn(PickupId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return;
    Slot& s = _slots[idx];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    if (_alive_count > 0) --_alive_count;
}

// =============================================================================
// Tick (lifetime + 磁石 + 拾取)
// =============================================================================

void PickupSystem::Tick(f32 dt, Vec2 player_pos, f32 magnet_strength) noexcept {
    const usize n = _slots.Size();
    // index 0 は予約なのでスキップ。
    for (usize i = 1; i < n; ++i) {
        Slot& s = _slots[i];
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
                if (_on_expire != nullptr) {
                    _on_expire(_on_expire_user, id);
                }
                s.active = false;
                if (_alive_count > 0) --_alive_count;
                continue;
            }
        }

        // ---- 2) 磁石効果 ---------------------------------------------------
        // |player - pickup| < magnet_radius のとき player 方向へ引き寄せる。
        // LengthSq で比較して sqrt を 1 回減らす。
        const Vec2 to_player = player_pos - s.info.world_pos;
        const f32  dist_sq   = LengthSq(to_player);
        const f32  mr        = s.info.magnet_radius;
        if (mr > 0.0f && dist_sq < mr * mr) {
            // Normalize は 0 ベクトルで {0,0} を返す safe 実装 (math/Vec.h 参照)。
            const Vec2 dir = Normalize(to_player);
            s.info.world_pos += dir * (magnet_strength * dt);
        }

        // ---- 3) 拾取判定 ---------------------------------------------------
        // 磁石移動後の最新位置で再評価する (1 フレームで吸い込まれて拾える)。
        const Vec2 to_player2 = player_pos - s.info.world_pos;
        const f32  dist_sq2   = LengthSq(to_player2);
        const f32  r          = s.info.radius;
        if (r > 0.0f && dist_sq2 < r * r) {
            // pickup: コールバック発火 → Despawn。
            const PickupId id{static_cast<u32>(i), s.gen};
            if (_on_pickup != nullptr) {
                _on_pickup(_on_pickup_user, id, s.info.kind,
                            s.info.item_id, s.info.value);
            }
            s.active = false;
            if (_alive_count > 0) --_alive_count;
        }
    }
}

// =============================================================================
// 照会
// =============================================================================

u32 PickupSystem::AlivePickupCount() const noexcept {
    return _alive_count;
}

const PickupInfo* PickupSystem::GetPickup(PickupId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    const Slot& s = _slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return &s.info;
}

// =============================================================================
// コールバック設定
// =============================================================================

void PickupSystem::SetOnPickupCallback(PickupCallback cb, void* user) noexcept {
    _on_pickup      = cb;
    _on_pickup_user = user;
}

void PickupSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    _on_expire      = cb;
    _on_expire_user = user;
}

// =============================================================================
// ランダムスポーン / kind 別操作
// =============================================================================

void PickupSystem::SpawnRandomAt(EPickupKind kind, Vec2 center, f32 spread_radius) noexcept {
    // kind の index を範囲 clamp (enum 拡張時の保険)。
    u8 ki = static_cast<u8>(kind);
    if (ki >= 8u) ki = 7u;  // Custom にフォールバック
    const KindDefaults& d = kKindDefaults[ki];

    // 円板内一様サンプル。spread_radius <= 0 のときは center 真上。
    Vec2 offset = Vec2::Zero();
    if (spread_radius > 0.0f) {
        offset = Random::Global().PointInCircle(spread_radius);
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

void PickupSystem::DespawnAllOfKind(EPickupKind kind) noexcept {
    const usize n = _slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;
        if (s.info.kind != kind) continue;
        s.active = false;
        if (_alive_count > 0) --_alive_count;
    }
}

u32 PickupSystem::CountOfKind(EPickupKind kind) const noexcept {
    u32 count = 0;
    const usize n = _slots.Size();
    for (usize i = 1; i < n; ++i) {
        const Slot& s = _slots[i];
        if (!s.active) continue;
        if (s.info.kind == kind) ++count;
    }
    return count;
}

void PickupSystem::ClearAll() noexcept {
    _slots.Clear();
    _alive_count = 0;
    // コールバック設定は維持 (ヘッダ仕様コメントと対応)。
}

} // namespace acs::game
