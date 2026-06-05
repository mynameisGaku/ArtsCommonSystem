// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — FParticleEffectSystem (軽量 sprite particle)
//
// 2D ゲーム向けの最小コスト sprite パーティクル。`FEffectSystem`
// が「Flash / HitStop / Shake のような単発演出」を担うのに対し、本クラスは
// 「emitter で粒子を放出し続ける」連続演出 (炎・煙・スパーク・吹き出しなど) を担う。
//
// 設計選択:
//   ・**emitter は generational handle**: 24bit index + 8bit gen を packed した
//      `FEmitterHandle`。`FSceneTimer::FTimerHandle` / `FTriggerWorld2D::FTriggerId` と
//      同じ規約。slot 再利用後の stale 参照は IsValid + gen 一致で検出可能。
//   ・**particle pool は固定容量**: `Init(max_particles)` で確保した後はリサイズ
//      しない。リアルタイムループのフレーム落ちを避けるため、最悪ケースを上限と
//      する保守的なポリシー。空き探索は `next_free` カーソル + 線形走査の
//      ハイブリッド (大半のフレームで O(1)、最悪 O(N))。
//   ・**描画は user 側**: 本クラスは描画 API を一切呼ばず、`AllParticles()` で
//      `const Particle*` を渡すのみ。FSpriteBatch / FDebugDraw / カスタム pipeline
//      など、ユーザー側の描画戦略に依存しない (= テスト容易・headless 動作可)。
//   ・**FRandom は内部 LCG**: `acs::game::FRandom` (xoshiro128**) には依存せず、
//      FParticleEffectSystem は単一の `u32` state を持つ簡素な Linear Congruential
//      Generator で十分。理由は (1) particle 用途は統計的品質要求が弱い (見た目で
//      バラけていれば十分), (2) 外部 PRNG 状態と独立にしておくと determinism を
//      議論しやすい (replay 再現で外部 PRNG が動いても particle 配列は影響しない),
//      (3) ヘッダ依存を最小化できる。Numerical Recipes の LCG (a=1664525,
//      c=1013904223) を採用。
//   ・**lifetime 切れは pool に返却**: particle.age >= particle.lifetime で
//      `m_Active[i] = 0`、`--m_ActiveParticles` で実質的にプールへ戻す。slot の
//      物理 index は変えない (= 描画側がフレームをまたぐ参照を握っている可能性は
//      無いが、内部での swap-and-pop も不要にして単純化)。
//   ・**emit_rate は accumulator 方式**: `m_EmitAccum += emit_rate * dt` を加算し、
//      整数部分だけ放出する。dt が小さくても累積で放出できるし、大 dt のときも
//      欠落しない。ただし上限は「pool 空き数」でクランプ。
//   ・**Burst は emit_rate と独立**: `Burst()` は burst_count 個を一気に放出。
//      連射やヒット時のフラッシュ用。
//   ・**非コピー・非ムーブ**: TPool 同様、ポインタを外部に渡す API があるため
//      コピーで実体が分裂すると未定義動作になりやすい。明示的に削除。
//   ・**全 noexcept**: ACS 規約。失敗は invalid handle / no-op で表現。
//
// 使い方:
//   ParticleEmitterDef def{};
//   def.color_start      = {1.0f, 0.6f, 0.1f};  // 橙
//   def.color_end        = {0.5f, 0.0f, 0.0f};  // 暗赤
//   def.lifetime_sec     = 0.8f;
//   def.emit_rate_per_sec= 30.0f;
//   def.burst_count      = 12.0f;
//   def.speed_min        = 40.0f;
//   def.speed_max        = 80.0f;
//   def.scale_start      = 8.0f;
//   def.scale_end        = 0.0f;
//   def.gravity          = {0.0f, 60.0f};       // y 下方向加速度
//
//   FParticleEffectSystem fx;
//   fx.Init(2048);
//   auto h = fx.CreateEmitter(def, {200.0f, 300.0f});
//
//   // 毎フレーム:
//   fx.Tick(dt);
//
//   // 描画 (ユーザー側):
//   u32 n = 0;
//   const Particle* p = fx.AllParticles(n);
//   for (u32 i = 0; i < n; ++i) {
//       if (!p[i].IsAlive()) continue;
//       // FSpriteBatch.Draw(p[i].position, current_scale, current_color);
//   }
//
//   // 爆発:
//   fx.Burst(h);
//
//   // emitter 破棄 (Scene 退場時など):
//   fx.DestroyEmitter(h);
//
// 範囲外:
//   ・テクスチャ参照 / アニメーション (sprite sheet)
//   ・回転 / 角速度 / 角減衰
//   ・3D 位置 (現状は 2D FVec2)
//   ・curve-based size/color (現状は線形補間のみ)
//   ・GPU particle (現状は CPU)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * emitter の挙動パラメータ。
 *
 * @details
 * particle を放出する emitter の見た目と物理を決める設定値の束。CreateEmitter に
 * 渡してコピー保持され、放出時に各 particle へ複製される。
 */
struct ParticleEmitterDef {
    /** 出生時の RGB 色 (0..1)。color_end へ線形補間される。 */
    FVec3 color_start         {1.0f, 1.0f, 1.0f};

    /** 寿命末の RGB 色 (0..1)。color_start から線形補間される。alpha は描画側が決める。 */
    FVec3 color_end           {1.0f, 1.0f, 1.0f};

    /** 各 particle の寿命 (秒)。<= 0 の def は CreateEmitter で弾かれ放出されない。 */
    f32  lifetime_sec        = 1.0f;

    /** 毎秒の連続放出個数。0 なら自動放出は無し (Burst のみ)。 */
    f32  emit_rate_per_sec   = 0.0f;

    /** Burst() 1 回で放出する個数 (端数は切り捨て)。 */
    f32  burst_count         = 0.0f;

    /** 初速度の大きさの下限。方向は円周一様。 */
    f32  speed_min           = 0.0f;

    /** 初速度の大きさの上限。 */
    f32  speed_max           = 0.0f;

    /** 出生時の見た目サイズ (px、描画側が解釈)。scale_end へ補間される。 */
    f32  scale_start         = 1.0f;

    /** 寿命末の見た目サイズ (px)。scale_start から補間される。 */
    f32  scale_end           = 1.0f;

    /** particle ごとの定数加速度 (px/sec^2 想定)。 */
    FVec2 gravity             {0.0f, 0.0f};
};

/**
 * 個別粒子の生データ。
 *
 * @details
 * 描画側はこの構造体を直接読み取り、age/lifetime から補間係数を計算する。
 * position / velocity 以外は emitter から複製してきた瞬時パラメータで、emitter の
 * def が後から変わっても放出済 particle は出生時の値で進む (self-contained particle)。
 */
struct Particle {
    /** world 座標の現在位置。 */
    FVec2 position           {0.0f, 0.0f};

    /** 現在の速度 (px/sec)。 */
    FVec2 velocity           {0.0f, 0.0f};

    /** 出生からの経過秒 (0 で出生)。 */
    f32  age                = 0.0f;

    /** 寿命 (秒)。age がこれを超えると死亡しプールへ返却される。 */
    f32  lifetime           = 0.0f;

    /** 出生時の RGB 色 (emitter からコピー)。 */
    FVec3 color_start        {1.0f, 1.0f, 1.0f};

    /** 寿命末の RGB 色 (emitter からコピー)。 */
    FVec3 color_end          {1.0f, 1.0f, 1.0f};

    /** 出生時の見た目サイズ (emitter からコピー)。 */
    f32  scale_start        = 1.0f;

    /** 寿命末の見た目サイズ (emitter からコピー)。 */
    f32  scale_end          = 1.0f;

    /** 出生時に emitter からコピーする定数加速度 (emitter 破棄後も粒子が持ち回る)。 */
    FVec2 gravity            {0.0f, 0.0f};

    /**
     * 経過寿命比 (0..1) を返す。
     *
     * @details age / lifetime を [0,1] にクランプする。lifetime <= 0 のときは 0 (defensive)。
     * @return 0..1 の経過比。
     */
    f32 NormalizedAge() const noexcept {
        if (lifetime <= 0.0f) return 0.0f;
        const f32 t = age / lifetime;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    /**
     * まだ生きているか (age < lifetime) を返す。
     *
     * @return 生存中なら true。
     */
    bool IsAlive() const noexcept { return age < lifetime; }
};

/**
 * emitter を指す 24bit index + 8bit gen を packed した opaque handle。
 *
 * @details
 * m_Packed == 0 を invalid と定義 (gen は常に 1 以上で配る)。FSceneTimer 等と同一規約。
 * slot 再利用後の stale 参照は IsValid + 内部の gen 一致で検出する。
 */
struct FEmitterHandle {
    /** 上位 8bit に gen、下位 24bit に index を詰めた packed 値 (0 = invalid)。 */
    u32 m_Packed = 0u;

    /**
     * 有効な handle か (m_Packed != 0) を返す。
     *
     * @return 有効なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index 部のビット幅 (下位 24bit)。 */
    static constexpr u32 kIndexBits = 24u;

    /** index 部を取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** index の最大値 (16777215)。emitter slot 数の上限。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * index と gen を packed して handle を作る。
     *
     * @param index emitter slot の index (下位 24bit に格納)。
     * @param gen 世代カウンタ (上位 8bit に格納)。
     * @return 構築した FEmitterHandle。
     */
    static FEmitterHandle Pack(u32 index, u8 gen) noexcept {
        FEmitterHandle h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * index 部 (下位 24bit) を取り出す。
     *
     * @return emitter slot の index。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * gen 部 (上位 8bit) を取り出す。
     *
     * @return 世代カウンタ。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }
};

/**
 * 複数 emitter と共有 particle pool を束ねる軽量 sprite particle システム。
 *
 * @details
 * 各 emitter は generational handle で参照され、固定容量の particle pool を共有する。
 * 描画 API は一切呼ばず AllParticles() で生バッファを渡すのみ (描画戦略は user 側)。
 * 乱数は内部 LCG、寿命切れは active flag を落として pool へ返却する。
 */
class FParticleEffectSystem {
public:
    /** 空状態で構築する (pool は Init で確保)。 */
    FParticleEffectSystem() noexcept = default;

    /** 破棄する (pool は TArray が解放)。 */
    ~FParticleEffectSystem() noexcept = default;

    /** コピー禁止 (AllParticles() が内部 buffer の生ポインタを返すため)。 */
    FParticleEffectSystem(const FParticleEffectSystem&)            = delete;

    /** コピー代入も禁止。 */
    FParticleEffectSystem& operator=(const FParticleEffectSystem&) = delete;

    /** ムーブ禁止 (実体アドレスが変わると外部参照が破綻するため)。 */
    FParticleEffectSystem(FParticleEffectSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FParticleEffectSystem& operator=(FParticleEffectSystem&&)      = delete;

    /**
     * pool 上限を確定して初期化する。
     *
     * @details 固定容量ポリシーのため再 Init は no-op。max_particles == 0 は誤呼出しとみなし 1024 を採用する。
     * @param max_particles particle pool の容量 (既定 1024)。
     */
    void Init(u32 max_particles = 1024) noexcept;

    /**
     * emitter を 1 個作成して handle を返す。
     *
     * @details emitter slot 上限 (24bit) 到達や lifetime_sec <= 0 の def は invalid handle を返す。
     * @param def emitter の挙動パラメータ (コピー保持される)。
     * @param pos emitter の world 座標 (描画側解釈)。
     * @return 作成した emitter の handle (失敗時は invalid)。
     */
    FEmitterHandle CreateEmitter(const ParticleEmitterDef& def, FVec2 pos) noexcept;

    /**
     * 既存 emitter の位置を変更する。
     *
     * @details invalid / stale handle は no-op。
     * @param h 対象 emitter の handle。
     * @param pos 新しい world 座標。
     */
    void SetEmitterPosition(FEmitterHandle h, FVec2 pos) noexcept;

    /**
     * 既存 emitter の active 状態を変更する。
     *
     * @details false にすると自動放出を止める (累積もリセット)。invalid / stale handle は no-op。Burst は active と無関係に発火する。
     * @param h 対象 emitter の handle。
     * @param active 自動放出するなら true。
     */
    void SetEmitterActive(FEmitterHandle h, bool active) noexcept;

    /**
     * burst_count 個を一気に放出する。
     *
     * @details pool 空き数で上限クランプ。emitter が in_use なら active に関わらず発火。invalid / stale handle は no-op。
     * @param h 対象 emitter の handle。
     */
    void Burst(FEmitterHandle h) noexcept;

    /**
     * emitter を破棄する (slot 解放 + gen 進める)。
     *
     * @details 既存の particle は emitter とは独立に管理されるため、寿命をまっとうするまで生き続ける。
     * @param h 破棄する emitter の handle。
     */
    void DestroyEmitter(FEmitterHandle h) noexcept;

    /**
     * dt 秒進める (連続放出 → 物理更新 → 寿命切れ返却)。
     *
     * @details 各 emitter の累積放出 → 全 particle の semi-implicit Euler 積分 → 寿命切れを pool に返却する。dt <= 0 は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在 active な particle 数を返す。
     *
     * @return 生存中の particle 数 (描画予定数)。
     */
    u32 ActiveParticleCount() const noexcept { return m_ActiveParticles; }

    /**
     * 現在予約中の emitter slot 数を返す。
     *
     * @return 生きている emitter slot 数。
     */
    u32 EmitterCount() const noexcept { return m_EmitterCount; }

    /**
     * 描画用の particle 生バッファを返す。
     *
     * @details out_count には pool 全体サイズが入る (各要素の生存判定は Particle::IsAlive())。返却ポインタは Init で確定し破棄まで安定。Init 前は nullptr / 0。
     * @param out_count pool 全体サイズの出力先。
     * @return particle 配列の先頭ポインタ (Init 前は nullptr)。
     */
    const Particle* AllParticles(u32& out_count) const noexcept;

    /** 全 emitter と全 particle を即座にクリアする (pool 容量は維持)。 */
    void ClearAll() noexcept;

private:
    /**
     * 1 個の emitter slot。generational handle + 動作パラメータ + 連続放出累積器。
     */
    struct Emitter {
        /** emitter の挙動パラメータ (CreateEmitter でコピー)。 */
        ParticleEmitterDef def        {};

        /** emitter の world 座標。 */
        FVec2               pos        {0.0f, 0.0f};

        /** 連続放出の累積 (>=1 で 1 個放出して 1 減らす)。 */
        f32                emit_accum = 0.0f;

        /** 世代カウンタ (0 = 未使用、配るときは 1 以上)。 */
        u8                 gen        = 0u;

        /** 自動放出フラグ (false で停止)。 */
        bool               active     = false;

        /** slot 自体が予約されているか。 */
        bool               in_use     = false;
    };

    /**
     * emitter から particle を 1 個出生させる。
     *
     * @details pool が満杯なら何もしない (見た目の劣化は許容し、フレームレートを保つ)。
     * @param e 出生元の emitter。
     */
    void EmitOne(const Emitter& e) noexcept;

    /** 空き探索・slot 確保の失敗を表す番兵 index (u32 全 1)。 */
    static constexpr u32 kInvalidIdx = 0xFFFFFFFFu;

    /**
     * 空き particle slot を 1 個確保し index を返す。
     *
     * @details m_NextFree を起点に巡回探索する (大半のフレームで O(1))。
     * @return 確保した slot の index (満杯なら kInvalidIdx)。
     */
    u32 AcquireParticleSlot() noexcept;

    /**
     * emitter slot を 1 個確保する (既存の inactive を再利用、無ければ末尾追加)。
     *
     * @return 確保した slot の index (24bit index 上限到達時は kInvalidIdx)。
     */
    u32 AcquireEmitterSlot() noexcept;

    /**
     * 内部 LCG で [0,1) の一様乱数を返す。
     *
     * @return [0,1) の f32。
     */
    f32 NextRandUnit() noexcept;

    /**
     * [min, max] の一様乱数を返す。
     *
     * @details min > max のときは swap する。
     * @param min 下限。
     * @param max 上限。
     * @return [min, max] の f32。
     */
    f32 NextRandRange(f32 min, f32 max) noexcept;

    /** emitter slot 配列 (generational)。 */
    TArray<Emitter>  m_Emitters       {};

    /** particle pool (固定容量)。 */
    TArray<Particle> m_Particles      {};

    /** particle の使用中フラグ (1 で使用中)。 */
    TArray<u8>       m_ParticleActive{};

    /** particle 上限 (Init で確定)。 */
    u32             m_Capacity         = 0u;

    /** 現在使用中の particle 数。 */
    u32             m_ActiveParticles = 0u;

    /** 現在予約中の emitter slot 数。 */
    u32             m_EmitterCount    = 0u;

    /** particle 空き探索のヒントカーソル。 */
    u32             m_NextFree        = 0u;

    /** LCG の state (golden ratio で初期化)。 */
    u32             m_RngState        = 0x9E3779B9u;
};

} // namespace acs::game
