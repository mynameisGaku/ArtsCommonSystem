// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "math/Vec.h"

namespace acs {

class IRhiTexture;
class CSpriteBatch;
class IAllocator;

/**
 * エミッタのパラメータ (粒子の初期分布を決める)。
 *
 * @details
 * 各粒子は spawn 時にこの記述から初期位置・速度・寿命・サイズ・色を引く。
 * size と color は start..end を寿命比で線形補間して描画される。プリセット
 * (Fire/Sparks/Fountain/Smoke) で典型的なエフェクトを即座に構成できる。
 */
struct FEmitterDesc {
    /** エミッタの基準位置 (粒子の出現中心)。 */
    FVec2  position           = FVec2{0, 0};

    /** 出現オフセット範囲の最小 (position に min..max の乱数を加算)。 */
    FVec2  spawn_offset_min   = FVec2{0, 0};

    /** 出現オフセット範囲の最大。 */
    FVec2  spawn_offset_max   = FVec2{0, 0};

    /** 初速の基準ベクトル。 */
    FVec2  velocity           = FVec2{0, -100};

    /** 初速のばらつき (各軸 ±この値の乱数を velocity に加算)。 */
    FVec2  velocity_variance  = FVec2{30, 30};

    /** 重力加速度 (毎フレーム velocity に dt 乗算で加算。画面 Y は下が +)。 */
    FVec2  gravity            = FVec2{0, 0};

    /** 誕生時のサイズ (pixel)。 */
    f32   size_start         = 16.0f;

    /** 死亡時のサイズ (pixel。size_start から線形補間)。 */
    f32   size_end           = 0.0f;

    /** 誕生時の色 (RGBA)。 */
    FVec4  color_start        = FVec4{1, 1, 1, 1};

    /** 死亡時の色 (RGBA。color_start から線形補間)。 */
    FVec4  color_end          = FVec4{1, 1, 1, 0};

    /** 寿命の基準秒。 */
    f32   life_seconds       = 1.0f;

    /** 寿命のばらつき (±この秒の乱数を life_seconds に加算)。 */
    f32   life_variance      = 0.2f;

    /** 連続生成レート (1 秒あたりの粒子数)。 */
    f32   rate_per_sec       = 60.0f;

    /** 回転速度 (描画には未使用、v2 拡張用)。 */
    f32   rotation_speed     = 0.0f;

    /** 連続生成フラグ (false で新規生成を停止)。 */
    bool  active             = true;

    /**
     * 火 (上昇する黄〜赤の炎) のプリセットを生成する。
     *
     * @param pos エミッタの基準位置。
     * @return 火エフェクト用に設定済みの FEmitterDesc。
     */
    static FEmitterDesc Fire(FVec2 pos)     noexcept;

    /**
     * 火花 (全方位に飛び散り重力で落下) のプリセットを生成する。
     *
     * @param pos エミッタの基準位置。
     * @return 火花エフェクト用に設定済みの FEmitterDesc。
     */
    static FEmitterDesc Sparks(FVec2 pos)   noexcept;

    /**
     * 噴水 (上方へ噴き出し重力で放物落下する青い水滴) のプリセットを生成する。
     *
     * @param pos エミッタの基準位置。
     * @return 噴水エフェクト用に設定済みの FEmitterDesc。
     */
    static FEmitterDesc Fountain(FVec2 pos) noexcept;

    /**
     * 煙 (ゆっくり上昇しながら拡大する灰色) のプリセットを生成する。
     *
     * @param pos エミッタの基準位置。
     * @return 煙エフェクト用に設定済みの FEmitterDesc。
     */
    static FEmitterDesc Smoke(FVec2 pos)    noexcept;
};

/**
 * 2D パーティクルシステム (CPU プール + CSpriteBatch 描画)。
 *
 * @details
 * 火花・煙・爆発・魔法効果などの視覚エフェクト全般に使う。最大 max_particles の
 * 固定プールを確保し、FEmitterDesc に従って連続生成・物理積分・寿命管理を行う
 * (死亡粒子は swap-pop で除去)。描画は事前に Begin 済みの CSpriteBatch にバッチ追加する。
 */
class CParticleSystem {
public:
    /** 空状態で構築する (プールは Init で確保)。 */
    CParticleSystem() noexcept = default;

    /** 破棄する (Shutdown でプールを解放)。 */
    ~CParticleSystem() noexcept;

    /** コピー禁止 (粒子プールを単独所有するため)。 */
    CParticleSystem(const CParticleSystem&)            = delete;

    /** コピー代入も禁止。 */
    CParticleSystem& operator=(const CParticleSystem&) = delete;

    /**
     * max_particles ぶんの粒子プールを確保する。
     *
     * @details テクスチャは後で SetTexture で設定する。0 を渡すと 1024 に丸める。
     * @param max_particles プールの最大粒子数。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(u32 max_particles = 4096) noexcept;

    /** 粒子プールを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * 描画に使うテクスチャを設定する。
     *
     * @param tex 粒子に貼るテクスチャ。null なら DrawRect 相当の白矩形 (CSpriteBatch の内部白テクスチャ)。
     */
    void SetTexture(IRhiTexture* tex) noexcept { m_Tex = tex; }

    /**
     * エミッタ記述を設定する。
     *
     * @param d 適用する FEmitterDesc。active=true で連続生成、false で停止のみ。
     */
    void SetEmitter(const FEmitterDesc& d) noexcept { m_Emitter = d; }

    /**
     * エミッタ記述への可変参照を返す (パラメータを直接書き換える)。
     *
     * @return 内部 FEmitterDesc への参照。
     */
    FEmitterDesc& Emitter() noexcept { return m_Emitter; }

    /**
     * エミッタ記述への const 参照を返す。
     *
     * @return 内部 FEmitterDesc への const 参照。
     */
    const FEmitterDesc& Emitter() const noexcept { return m_Emitter; }

    /**
     * 1 フレーム分シミュレーションを進める (連続生成 + 物理積分 + 寿命管理)。
     *
     * @param dt 前フレームからの経過秒。
     */
    void Update(f32 dt) noexcept;

    /**
     * count 個の粒子を即座に生成する (爆発等)。
     *
     * @param count 生成する粒子数 (プール容量で頭打ち)。
     */
    void EmitBurst(u32 count) noexcept;

    /** 全粒子を即座に消滅させ、生成アキュムレータをリセットする。 */
    void Reset() noexcept { m_Active = 0; m_SpawnAccum = 0; }

    /**
     * アクティブな粒子を CSpriteBatch に積む (事前に Begin 済みであること)。
     *
     * @param sb 粒子を描画する先の CSpriteBatch。
     */
    void Render(CSpriteBatch& sb) noexcept;

    /**
     * 現在生存中の粒子数を返す。
     *
     * @return アクティブ粒子数。
     */
    u32  ActiveCount() const noexcept { return m_Active; }

    /**
     * プールの最大容量を返す。
     *
     * @return 確保済みの最大粒子数。
     */
    u32  Capacity()    const noexcept { return m_Capacity; }

private:
    /**
     * 1 粒子分のシミュレーション状態。
     *
     * @details プール上に連続配置され、size/color は start..end を age/life で補間する。
     */
    struct FParticle {
        /** 現在位置 (pixel)。 */
        FVec2 pos;

        /** 現在速度。 */
        FVec2 vel;

        /** 経過時間 (秒)。 */
        f32  age;

        /** 寿命 (秒。age >= life で死亡)。 */
        f32  life;

        /** 誕生時サイズ。 */
        f32  size_start;

        /** 死亡時サイズ。 */
        f32  size_end;

        /** 誕生時の色 (RGBA)。 */
        FVec4 color_start;

        /** 死亡時の色 (RGBA)。 */
        FVec4 color_end;
    };

    /** エミッタ記述に従って粒子を 1 つ生成する (容量上限なら何もしない)。 */
    void SpawnOne() noexcept;

    /**
     * xorshift で 0..1 の擬似乱数を返す。
     *
     * @return [0, 1) の乱数。
     */
    f32  RandF() noexcept;

    /**
     * a..b の一様乱数を返す。
     *
     * @param a 下限。
     * @param b 上限。
     * @return [a, b) の乱数。
     */
    f32  RandRange(f32 a, f32 b) noexcept;

    /** 粒子プールの先頭 (容量ぶん確保、所有権を持つ)。 */
    FParticle*    m_Pool         = nullptr;

    /** 粒子プールを確保したアロケータ。既定アロケータ切替後も同じ元へ返す。 */
    IAllocator* m_Allocator = nullptr;

    /** プールの最大容量。 */
    u32          m_Capacity     = 0;

    /** 現在生存中の粒子数 (プール先頭から詰めて配置)。 */
    u32          m_Active       = 0;

    /** 連続生成の小数アキュムレータ (rate_per_sec * dt の端数を蓄積)。 */
    f32          m_SpawnAccum  = 0.0f;

    /** 現在のエミッタ記述。 */
    FEmitterDesc  m_Emitter;

    /** 描画テクスチャ (非所有。null なら白矩形)。 */
    IRhiTexture* m_Tex          = nullptr;

    /** xorshift 乱数の状態シード。 */
    u32          m_Seed         = 0xC0FFEEu;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FParticleSystem = CParticleSystem;


} // namespace acs
