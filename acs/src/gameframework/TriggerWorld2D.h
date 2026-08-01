// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — CTriggerWorld2D (overlap tracking + events)
//
// CCollisionWorld2D とは独立に「重なりだけを監視するトリガ専用ワールド」。
// 物理応答や押し戻し (Resolve) は行わず、毎フレーム全 trigger pair の重なり
// 状態を更新し、前フレと比較して以下の 3 種類のイベントを発火する:
//
//   ・OnEnter : 前フレ非重なり → 今フレ重なり (新規 overlap pair)
//   ・OnStay  : 前フレ重なり   → 今フレ重なり (継続中の overlap pair)
//   ・OnExit  : 前フレ重なり   → 今フレ非重なり (離れた overlap pair)
//
// 使い方:
//   CTriggerWorld2D world;
//   world.Init();
//
//   FTriggerId player = world.AddCircle({ {0,0}, 16.0f }, /*layer=*/0);
//   FTriggerId pickup = world.AddAabb  ({ {100,0}, {8,8} }, /*layer=*/1);
//
//   world.SetOnEnter(+[](void* /*user*/, FTriggerId self, FTriggerId other) noexcept {
//       // self と other が今フレ初めて重なった
//   }, /*user=*/nullptr);
//
//   // 毎フレーム:
//   world.UpdateCircle(player, { player_pos, 16.0f });
//   world.Tick(dt);    // overlap 比較 → OnEnter / OnStay / OnExit 発火
//
// 設計:
//   ・**broad phase は O(N^2)**: 全 pair を直接比較。将来
//     CCollisionWorld2D と同じ SpatialGrid に置換し O(N + K) に下げる予定。
//   ・**FTriggerId は FShapeId と同じ generational handle** (24bit index + 8bit gen)。
//     remove → re-add で slot 再利用しても旧 handle は無効化される。
//   ・**overlap pair は array で保持**: 前フレの状態は `TArray<FOverlapPair>` に
//     `was_overlapping` 付きで保存。次フレで再計算し、(was, now) の組合せで
//     どのイベントを発火するか決める。
//   ・**layer はメタデータとして格納のみ**: event 側で参照する。
//     mask による絞り込みは将来導入。
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

/**
 * Trigger を識別する packed 32bit handle (generational)。
 *
 * @details
 * レイアウトは FShapeId / FNodeId と同一 (low24=index, high8=generation)。
 * remove → re-add で slot を再利用しても generation が進むため旧 handle は無効化される。
 */
struct FTriggerId {
    /** packed 表現 (low24=index, high8=generation、0 = invalid)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed = 0) な handle を構築する。 */
    constexpr FTriggerId() noexcept = default;

    /**
     * index と generation を pack して handle を構築する。
     *
     * @param index slot index (low 24bit)。
     * @param gen generation (high 8bit)。
     */
    constexpr FTriggerId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot index を取り出す。
     *
     * @return low 24bit の index。
     */
    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を取り出す。
     *
     * @return high 8bit の generation。
     */
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * 有効な handle かを返す。
     *
     * @return invalid (m_Packed == 0) でなければ true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 2 つの handle が等しいかを返す。
     *
     * @param o 比較対象の handle。
     * @return packed 表現が一致すれば true。
     */
    constexpr bool operator==(FTriggerId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 2 つの handle が異なるかを返す。
     *
     * @param o 比較対象の handle。
     * @return packed 表現が異なれば true。
     */
    constexpr bool operator!=(FTriggerId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * Trigger イベントのコールバック型: (user, self, other) の順で受け取る。
 *
 * @details self / other はいずれも生存中の FTriggerId (発火直前に world 側で検証済み)。
 * @param user コールバック登録時に渡した任意ポインタ。
 * @param self イベント対象となった一方の trigger id。
 * @param other overlap した相手の trigger id。
 */
using TriggerEventCallback = void(*)(void* user, FTriggerId self, FTriggerId other) noexcept;

/**
 * 重なりだけを監視するトリガ専用ワールド (overlap tracking + events)。
 *
 * @details
 * CCollisionWorld2D とは独立に、物理応答や押し戻しは行わず、毎フレーム全 trigger
 * pair の重なり状態を更新して前フレと比較し OnEnter / OnStay / OnExit を発火する。
 * broad phase は O(N^2) 全 pair 直接比較。overlap pair は前フレ状態付きで配列保持し、
 * (was, now) の組合せでイベントを決める。コールバックは関数ポインタ + void* user で、
 * handle 安定性のため non-copy / non-move、全関数 noexcept。
 */
class CTriggerWorld2D {
public:
    /** 空のトリガワールドを構築する。 */
    CTriggerWorld2D() noexcept = default;

    /** 破棄する。 */
    ~CTriggerWorld2D() noexcept = default;

    /** コピー禁止 (handle 安定性のため)。 */
    CTriggerWorld2D(const CTriggerWorld2D&)            = delete;

    /** コピー代入も禁止。 */
    CTriggerWorld2D& operator=(const CTriggerWorld2D&) = delete;

    /** ムーブ禁止。 */
    CTriggerWorld2D(CTriggerWorld2D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CTriggerWorld2D& operator=(CTriggerWorld2D&&)      = delete;

    /** 初期化する (現状は状態を持たないが、将来の SpatialGrid 等のため API を予約)。 */
    void Init() noexcept;

    /**
     * 円形 trigger を登録する。
     *
     * @param c 登録する円。
     * @param layer メタデータとして格納するレイヤ (既定 0)。
     * @return 新しい trigger の handle。
     */
    FTriggerId AddCircle(const FCircle& c, u32 layer = 0) noexcept;

    /**
     * AABB trigger を登録する。
     *
     * @param a 登録する AABB。
     * @param layer メタデータとして格納するレイヤ (既定 0)。
     * @return 新しい trigger の handle。
     */
    FTriggerId AddAabb  (const FAabb2&  a, u32 layer = 0) noexcept;

    /**
     * 円形 trigger の形状を更新する (移動時)。
     *
     * @details kind が一致しない / stale handle の場合は何もしない。
     * @param id 対象 trigger の handle。
     * @param c 新しい円。
     */
    void UpdateCircle(FTriggerId id, const FCircle& c) noexcept;

    /**
     * AABB trigger の形状を更新する (移動時)。
     *
     * @details kind が一致しない / stale handle の場合は何もしない。
     * @param id 対象 trigger の handle。
     * @param a 新しい AABB。
     */
    void UpdateAabb  (FTriggerId id, const FAabb2&  a) noexcept;

    /**
     * trigger を削除する。
     *
     * @details slot は再利用され generation が進む。関連 pair は次の Tick で
     * OnExit 発火後に自然消滅する。
     * @param id 削除する trigger の handle。
     */
    void Remove(FTriggerId id) noexcept;

    /**
     * trigger に任意の user ポインタを紐付ける。
     *
     * @details ATriggerComponent がイベント発火時に id → component を逆引きするのに
     * 使う (overlap 判定には不使用)。stale handle の場合は何もしない。
     * @param id 対象 trigger の handle。
     * @param user 紐付ける任意ポインタ。
     */
    void  SetUserData(FTriggerId id, void* user) noexcept;

    /**
     * trigger に紐付けた user ポインタを取得する。
     *
     * @param id 対象 trigger の handle。
     * @return 紐付けられた user ポインタ (stale handle なら nullptr)。
     */
    void* UserData(FTriggerId id) const noexcept;

    /**
     * overlap 開始イベントのコールバックを設定する。
     *
     * @param cb 発火時に呼ぶコールバック (nullptr で無効化)。
     * @param user cb に渡す任意ポインタ。
     */
    void SetOnEnter(TriggerEventCallback cb, void* user) noexcept;

    /**
     * overlap 継続イベントのコールバックを設定する。
     *
     * @param cb 発火時に呼ぶコールバック (nullptr で無効化)。
     * @param user cb に渡す任意ポインタ。
     */
    void SetOnStay (TriggerEventCallback cb, void* user) noexcept;

    /**
     * overlap 終了イベントのコールバックを設定する。
     *
     * @param cb 発火時に呼ぶコールバック (nullptr で無効化)。
     * @param user cb に渡す任意ポインタ。
     */
    void SetOnExit (TriggerEventCallback cb, void* user) noexcept;

    /**
     * 毎フレ呼び、全 pair の overlap 状態を更新してイベントを発火する。
     *
     * @details
     * 前フレと比較して OnEnter / OnStay / OnExit を発火する。dt は現状未使用だが、
     * 将来 OnStay の経過時間通知などで利用予定。
     * @param dt 前フレームからの経過秒 (現状未使用)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * active な trigger の総数を返す。
     *
     * @return 登録中の trigger 数。
     */
    u32 TriggerCount() const noexcept { return m_TriggerCount; }

    /** 全 trigger と overlap state を破棄する。 */
    void ClearAll() noexcept;

private:
    /** trigger 形状の種別。 */
    enum class EKind : u8 {
        /** 未使用 slot。 */
        None = 0,

        /** AABB 形状。 */
        Aabb,

        /** 円形状。 */
        Circle
    };

    /** 1 つの trigger を保持する slot。 */
    struct FTriggerSlot {
        /** 形状種別。 */
        EKind   kind   = EKind::None;

        /** AABB 形状時のジオメトリ。 */
        FAabb2  aabb   {};

        /** 円形状時のジオメトリ。 */
        FCircle circle {};

        /** メタデータとして格納するレイヤ。 */
        u32    layer  = 0;

        /** id→object 逆引き用の任意ポインタ (ATriggerComponent* 等)。 */
        void*  user   = nullptr;

        /** slot が使用中かどうか。 */
        bool   active = false;

        /** generation (stale handle 検出用)。 */
        u8     gen    = 0;
    };

    /**
     * 前フレと今フレで保持する overlap pair。
     *
     * @details (a_idx, b_idx) は a_idx < b_idx で正規化済み。was_overlapping は
     * 前フレでの状態で、今フレ計算後の新状態と組合せて発火イベントを決める。
     */
    struct FOverlapPair {
        /** 正規化された小さい方の slot index。 */
        u32  a_idx           = 0;

        /** 正規化された大きい方の slot index。 */
        u32  b_idx           = 0;

        /** 前フレで overlap していたか。 */
        bool was_overlapping = false;
    };

    /**
     * 空き slot を確保する (index 0 は invalid 用に予約)。
     *
     * @return 確保した slot index。
     */
    u32  AcquireSlot() noexcept;

    /**
     * 2 つの trigger slot が幾何的に重なるかを返す。
     *
     * @param a 一方の trigger slot。
     * @param b もう一方の trigger slot。
     * @return 形状が重なっていれば true。
     */
    bool ShapesOverlap(const FTriggerSlot& a, const FTriggerSlot& b) const noexcept;

    /** trigger slot 配列 (index 0 は invalid 予約)。 */
    TArray<FTriggerSlot> m_Slots;

    /** 前フレの overlap pair (a_idx < b_idx でソート済み)。 */
    TArray<FOverlapPair> m_Pairs;

    /** Tick 内で今フレ pair を構築する作業バッファ (再確保を抑える)。 */
    TArray<FOverlapPair> m_NextPairs;

    /** active な trigger の総数。 */
    u32 m_TriggerCount = 0;

    /** overlap 開始コールバック。 */
    TriggerEventCallback m_OnEnter      = nullptr;

    /** overlap 継続コールバック。 */
    TriggerEventCallback m_OnStay       = nullptr;

    /** overlap 終了コールバック。 */
    TriggerEventCallback m_OnExit       = nullptr;

    /** m_OnEnter に渡す任意ポインタ。 */
    void*                m_OnEnterUser = nullptr;

    /** m_OnStay に渡す任意ポインタ。 */
    void*                m_OnStayUser  = nullptr;

    /** m_OnExit に渡す任意ポインタ。 */
    void*                m_OnExitUser  = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTriggerWorld2D = CTriggerWorld2D;

} // namespace acs::game
