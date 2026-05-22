// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 3 — TriggerWorld2D (overlap tracking + events)
//
// CollisionWorld2D とは独立に「重なりだけを監視するトリガ専用ワールド」。
// 物理応答や押し戻し (Resolve) は行わず、毎フレーム全 trigger pair の重なり
// 状態を更新し、前フレと比較して以下の 3 種類のイベントを発火する:
//
//   ・OnEnter : 前フレ非重なり → 今フレ重なり (新規 overlap pair)
//   ・OnStay  : 前フレ重なり   → 今フレ重なり (継続中の overlap pair)
//   ・OnExit  : 前フレ重なり   → 今フレ非重なり (離れた overlap pair)
//
// 使い方:
//   TriggerWorld2D world;
//   world.Init();
//
//   TriggerId player = world.AddCircle({ {0,0}, 16.0f }, /*layer=*/0);
//   TriggerId pickup = world.AddAabb  ({ {100,0}, {8,8} }, /*layer=*/1);
//
//   world.SetOnEnter(+[](void* /*user*/, TriggerId self, TriggerId other) noexcept {
//       // self と other が今フレ初めて重なった
//   }, /*user=*/nullptr);
//
//   // 毎フレーム:
//   world.UpdateCircle(player, { player_pos, 16.0f });
//   world.Tick(dt);    // overlap 比較 → OnEnter / OnStay / OnExit 発火
//
// 設計 (Phase 3 = Pillar F Phase 3):
//   ・**broad phase は O(N^2)**: Phase 3 では全 pair を直接比較。Phase 4 で
//     CollisionWorld2D と同じ SpatialGrid に置換し O(N + K) に下げる予定。
//   ・**TriggerId は ShapeId と同じ generational handle** (24bit index + 8bit gen)。
//     remove → re-add で slot 再利用しても旧 handle は無効化される。
//   ・**overlap pair は array で保持**: 前フレの状態は `Array<OverlapPair>` に
//     `was_overlapping` 付きで保存。次フレで再計算し、(was, now) の組合せで
//     どのイベントを発火するか決める。
//   ・**layer はメタデータとして格納のみ**: Phase 3 では event 側で参照する。
//     mask による絞り込みは Phase 4 で導入。
//   ・**コールバックは関数ポインタ + void* user**: STL 不使用のため std::function
//     ではなく C-style コールバックを採用。各イベントは独立に設定可能で、
//     未設定なら発火スキップ。
//   ・**非コピー・非ムーブ**: 内部の overlap state がポインタ的セマンティクスを
//     持つわけではないが、ハンドルの安定性を保証するため複製を禁止。
//   ・**全関数 noexcept**: ACS 全体の規約に従い例外伝播ゼロ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

namespace acs::game {

/// Trigger を識別する packed 32bit handle (generational)。
/// レイアウトは ShapeId / NodeId と同一 (low24=index, high8=generation)。
struct TriggerId {
    u32 _packed = 0;   // 0 = invalid

    constexpr TriggerId() noexcept = default;
    constexpr TriggerId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index() const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(_packed >> 24);
    }
    /// invalid (packed == 0) でなければ true。
    bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(TriggerId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(TriggerId o) const noexcept { return _packed != o._packed; }
};

/// Trigger イベントコールバック: (user, self, other) の順で受け取る。
/// self / other はいずれも生存中の TriggerId (発火直前に world 側で検証済み)。
using TriggerEventCallback = void(*)(void* user, TriggerId self, TriggerId other) noexcept;

class TriggerWorld2D {
public:
    TriggerWorld2D() noexcept = default;
    ~TriggerWorld2D() noexcept = default;

    // 非コピー・非ムーブ (handle 安定性のため)
    TriggerWorld2D(const TriggerWorld2D&)            = delete;
    TriggerWorld2D& operator=(const TriggerWorld2D&) = delete;
    TriggerWorld2D(TriggerWorld2D&&)                 = delete;
    TriggerWorld2D& operator=(TriggerWorld2D&&)      = delete;

    /// 初期化 (Phase 3 では特に状態を持たないが、将来の SpatialGrid 等のため API は予約)。
    void Init() noexcept;

    // ----- Trigger 登録 -----
    TriggerId AddCircle(const Circle& c, u32 layer = 0) noexcept;
    TriggerId AddAabb  (const Aabb2&  a, u32 layer = 0) noexcept;

    /// 形状更新 (移動した時)。kind が一致しない / stale handle の場合は何もしない。
    void UpdateCircle(TriggerId id, const Circle& c) noexcept;
    void UpdateAabb  (TriggerId id, const Aabb2&  a) noexcept;

    /// trigger 削除。slot は再利用、generation が進む。次の Tick で関連 pair も purge。
    void Remove(TriggerId id) noexcept;

    // ----- イベントコールバック (関数ポインタ + user data) -----
    void SetOnEnter(TriggerEventCallback cb, void* user) noexcept;
    void SetOnStay (TriggerEventCallback cb, void* user) noexcept;
    void SetOnExit (TriggerEventCallback cb, void* user) noexcept;

    /// 毎フレ呼ぶ: 全 pair の overlap 状態を更新し、前フレと比較して
    /// OnEnter / OnStay / OnExit を発火する。dt は Phase 3 では未使用だが、
    /// 将来 OnStay の経過時間通知などで利用予定。
    void Tick(f32 dt) noexcept;

    /// active な trigger の総数。
    u32 TriggerCount() const noexcept { return _trigger_count; }

    /// 全 trigger と overlap state を破棄。
    void ClearAll() noexcept;

private:
    enum class Kind : u8 { None = 0, Aabb, Circle };

    struct TriggerSlot {
        Kind   kind   = Kind::None;
        Aabb2  aabb   {};
        Circle circle {};
        u32    layer  = 0;
        bool   active = false;
        u8     gen    = 0;
    };

    /// 前フレと今フレで保持する pair。(a_idx, b_idx) は a_idx < b_idx で正規化済み。
    /// was_overlapping は前フレでの状態、今フレ計算後の新状態と組合せて event を決める。
    struct OverlapPair {
        u32  a_idx           = 0;
        u32  b_idx           = 0;
        bool was_overlapping = false;
    };

    u32  AcquireSlot() noexcept;
    bool ShapesOverlap(const TriggerSlot& a, const TriggerSlot& b) const noexcept;

    Array<TriggerSlot> _slots;
    Array<OverlapPair> _pairs;         // a_idx < b_idx でソート済み
    Array<OverlapPair> _next_pairs;    // Tick 内の作業バッファ (再確保を抑える)
    u32 _trigger_count = 0;

    TriggerEventCallback _on_enter      = nullptr;
    TriggerEventCallback _on_stay       = nullptr;
    TriggerEventCallback _on_exit       = nullptr;
    void*                _on_enter_user = nullptr;
    void*                _on_stay_user  = nullptr;
    void*                _on_exit_user  = nullptr;
};

} // namespace acs::game
