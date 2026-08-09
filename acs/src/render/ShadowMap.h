// SPDX-License-Identifier: Apache-2.0
#pragma once
// シャドウマップ（有向光源用、orthographic）
//
// 2 つのモード:
//   1. Single cascade (既定、cascade_count=1): 1 つの 2D 深度テクスチャに
//      シーン全体を 1 つの ortho 投影で描く。CStandardShader と IBL 描画で
//      単一のライト空間を共有する方式。
//   2. Cascaded Shadow Map (CSM、cascade_count >= 2): カメラ frustum を
//      距離で 2-4 個に分割し、近景は高解像度・遠景は広範囲を 1 枚の atlas
//      テクスチャ (width = cascade_count * size、height = size) に並べる。
//      ピクセルの view-space z で cascade を選択するため遠景まで鮮鋭な影を
//      保てるため、広い屋外シーンに適する。
//
// 使い方 (single cascade、後方互換):
//   CShadowMap sm;
//   sm.Init(*dev, /*size=*/2048);                    // cascade_count=1 既定
//   sm.SetDirectionalLight(light_dir, scene_center, 15.0f);
//   if (!sm.BeginFrame(/* casters */ 1)) return;
//   cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);
//   cl->SetPipeline(*sm.CasterPipeline());
//   cl->SetConstantBuffer(0, *sm.LightCB());
//   for (each caster) {
//       if (!sm.TrySetCaster(model)) continue;
//       cl->SetConstantBuffer(1, *sm.CasterObjectCB());
//       /* draw */
//   }
//   cl->EndShadowPass(*sm.DepthTexture());
//
// 使い方 (CSM、3 cascade):
//   CShadowMap sm;
//   sm.Init(*dev, 2048, /*cascade_count=*/3);
//   sm.SetDirectionalLightCascades(light_dir, view, proj, 0.1f, 100.0f);
//   if (!sm.BeginFrame(/* casters per cascade */ 1)) return;
//   cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);        // atlas 全体 clear
//   cl->SetPipeline(*sm.CasterPipeline());
//   for (u32 c = 0; c < sm.CascadeCount(); ++c) {
//       cl->SetViewport(sm.CascadeViewport(c));
//       cl->SetScissor(sm.CascadeScissor(c));
//       sm.SetCurrentCascade(c);
//       cl->SetConstantBuffer(0, *sm.LightCB());
//       for (each caster) {
//           if (!sm.TrySetCaster(model)) continue;
//           cl->SetConstantBuffer(1, *sm.CasterObjectCB());
//           /* draw */
//       }
//   }
//   cl->EndShadowPass(*sm.DepthTexture());
//
// 主パスでの使用 (CPbrShader 統合):
//   単一カスケード: pbr.SetShadowMap(sm.DepthTexture(), sm.LightViewProjection(), ...);
//   複数カスケード: FMat4 vps[3] = { sm.LightViewProjection(0), sm.LightViewProjection(1), sm.LightViewProjection(2) };
//                f32  spl[3] = { sm.CascadeSplit(0), sm.CascadeSplit(1), sm.CascadeSplit(2) };
//   複数カスケードを設定: pbr.SetShadowMapCascades(sm.DepthTexture(), vps, spl, sm.CascadeCount(), ...);
#include "foundation/Result.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/RhiTypes.h"        // FViewport / FScissorRect

namespace acs {

/**
 * 有向光源用の直交シャドウマップ (single cascade / CSM atlas)。
 *
 * @details
 * cascade_count=1 では 1 枚の 2D 深度テクスチャにシーン全体を 1 つの ortho 投影で描く
 * (伝統的な single cascade)。cascade_count>=2 ではカメラ frustum を距離で 2-4 個に分割し、
 * 近景は高解像度・遠景は広範囲を 1 枚の atlas (width = cascade_count * size、height = size)
 * に並べる (CSM)。GPU リソースを単独所有する non-copy 型。
 */
class CShadowMap {
public:
    /** サポートする cascade の最大数。 */
    static constexpr u32 kMaxCascades = 4;

    /**
     * Source-compatibility estimates from the former fixed caster ring.
     * The per-cascade pools are growable; neither value is a hard draw limit.
     */
    [[deprecated("growable pool; not a hard limit")]]
    static constexpr u32 kMaxCasterDrawsPerCascade = 256u;
    [[deprecated("growable pool; not a hard limit")]]
    static constexpr u32 kMaxCasterDrawsPerFrame =
        kMaxCascades * 256u;

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CShadowMap() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CShadowMap() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CShadowMap(const CShadowMap&)            = delete;

    /** コピー代入も禁止。 */
    CShadowMap& operator=(const CShadowMap&) = delete;

    /**
     * 深度テクスチャとキャスター用パイプラインを生成する。
     *
     * @details
     * cascade_count=1 (既定) は size × size の single 2D depth texture、cascade_count>=2 は
     * (size * cascade_count) × size の CSM atlas を確保する。cascade_count は kMaxCascades に
     * クランプされる。
     * @param device リソース・パイプライン生成に使う RHI デバイス。
     * @param size 1 cascade あたりの一辺サイズ (0 のとき 2048)。
     * @param cascade_count cascade 数 (0 のとき 1、kMaxCascades 超はクランプ)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 size = 2048,
                      u32 cascade_count = 1) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /** shadow pass の記録前に per-draw caster CB cursor をリセットする。 */
    /**
     * Pre-grow every cascade reserved by Init before recording a complete
     * shadow pass. This also keeps a same-frame single-volume fallback followed
     * by CSM restoration allocation-free while commands are being recorded.
     * UINT32_MAX is rejected because it is the invalid cursor sentinel.
     */
    bool BeginFrame(u32 required_casters_per_cascade = 0u) noexcept;

    /**
     * single cascade 用に有向光源の ortho 投影を計算する (後方互換)。
     *
     * @details cascade_count=1 でのみ意味を持つ。CSM mode では SetDirectionalLightCascades を使う。
     * @param light_dir 光の向き (正規化されていなくてよい)。
     * @param scene_center シーンの中心 (ortho 投影が見る点)。
     * @param scene_radius シーンを覆う半径 (ortho 幅・near/far の基準)。
     */
    void SetDirectionalLight(FVec3 light_dir,
                              FVec3 scene_center,
                              f32 scene_radius) noexcept;

    /**
     * CSM 用にカメラ frustum を分割して各 cascade の light VP を計算する。
     *
     * @details
     * near→far を 2-4 個に Zhang practical split で分割し、各 sub-frustum を bounding sphere で
     * 囲って ortho 投影を求める。
     * @param light_dir 光の向き (正規化されていなくてよい)。
     * @param view カメラの view 行列。
     * @param proj カメラの projection 行列。
     * @param near_z カメラの near 平面距離。
     * @param far_z カメラの far 平面距離。
     * @param lambda 分割の混合比 (0=uniform split で近景密・線形、1=log split で遠景均等、既定 0.5 は両者のブレンド)。
     */
    void SetDirectionalLightCascades(FVec3 light_dir,
                                      const FMat4& view, const FMat4& proj,
                                      f32 near_z, f32 far_z,
                                      f32 lambda = 0.5f) noexcept;

    /**
     * 現在描画する cascade を選択する (CSM mode、BeginShadowPass の後に呼ぶ)。
     *
     * @details LightCB() が返す cascade 専用 CB を選択する。GPU 内容は更新しない。
     * @param cascade 選択する cascade index (範囲外なら 0)。
     */
    void SetCurrentCascade(u32 cascade) noexcept;

    /**
     * キャスター描画ごとのモデル行列を設定する。
     *
     * @param model キャスターの world 変換行列。
     */
    void SetCaster(const FMat4& model) noexcept;

    /**
     * draw 専用 CB を確保してキャスターのモデル行列を設定する。
     *
     * @return 設定できた場合 true。frame 上限または確保失敗時は false で、draw を省略する。
     */
    bool TrySetCaster(const FMat4& model) noexcept;

    /**
     * 深度テクスチャ (single または CSM atlas) を返す。
     *
     * @return シャドウ深度テクスチャ。
     */
    IRhiTexture*  DepthTexture()   const noexcept { return m_Depth.Get(); }

    /**
     * キャスター描画用の depth-only パイプラインを返す。
     *
     * @return キャスター描画パイプライン。
     */
    IRhiPipeline* CasterPipeline() const noexcept { return m_Pipeline.Get(); }

    /**
     * 光源の view-projection を格納する定数バッファ (b0) を返す。
     *
     * @return light_vp を格納する CB。
     */
    IRhiBuffer*   LightCB()        const noexcept {
        return m_LightCbs[m_CurrentCascade].Get();
    }

    /**
     * キャスターの model 行列を格納する定数バッファ (b1) を返す。
     *
     * @return model を格納する CB。
     */
    IRhiBuffer*   CasterObjectCB() const noexcept {
        const u32 slot = m_CurrentCasters[m_CurrentCascade];
        return slot < m_ObjectCbs[m_CurrentCascade].Num()
             ? m_ObjectCbs[m_CurrentCascade][slot].Get()
             : nullptr;
    }

    u32 CasterBufferCapacity(u32 cascade = 0u) const noexcept {
        return cascade < m_CascadeCapacity
             ? static_cast<u32>(m_ObjectCbs[cascade].Num()) : 0u;
    }

    /** BeginFrame() 以降に消費した per-draw caster CB slot 数。 */
    u32 CasterDrawCount() const noexcept { return m_TotalCasterDrawCount; }

    /** 指定 cascade が消費した per-draw caster CB slot 数。 */
    u32 CasterDrawCount(u32 cascade) const noexcept {
        return cascade < m_CascadeCount ? m_CasterDrawCounts[cascade] : 0;
    }

    /** 現 frame で ring 上限または slot 確保失敗が発生したか。 */
    bool CasterOverflowed() const noexcept {
        for (u32 cascade = 0; cascade < m_CascadeCount; ++cascade) {
            if (m_CasterOverflowed[cascade]) return true;
        }
        return false;
    }

    /** 指定 cascade で ring 上限または slot 確保失敗が発生したか。 */
    bool CasterOverflowed(u32 cascade) const noexcept {
        return cascade < m_CascadeCount && m_CasterOverflowed[cascade];
    }

    /**
     * cascade 0 の light view-projection を返す (single cascade 用、後方互換)。
     *
     * @return cascade 0 の light view-projection 行列。
     */
    FMat4 LightViewProjection() const noexcept { return m_LightVp[0]; }

    /**
     * 指定 cascade の light view-projection を返す (CSM 用)。
     *
     * @param cascade cascade index (範囲外なら 0)。
     * @return 当該 cascade の light view-projection 行列。
     */
    FMat4 LightViewProjection(u32 cascade) const noexcept {
        return m_LightVp[cascade < m_CascadeCount ? cascade : 0];
    }

    /**
     * 指定 cascade の分割閾値 (view-space z far) を返す。
     *
     * @param cascade cascade index (範囲外なら 0)。
     * @return 当該 cascade の分割閾値。
     */
    f32 CascadeSplit(u32 cascade) const noexcept {
        return m_CascadeSplits[cascade < m_CascadeCount ? cascade : 0];
    }

    /**
     * cascade 数を返す。
     *
     * @return 確保済みの cascade 数。
     */
    u32 CascadeCount() const noexcept { return m_CascadeCount; }

    /**
     * atlas 内の cascade 領域に対する viewport を返す。
     *
     * @details BeginShadowPass の後・各 caster 群描画の前に SetViewport へ渡す。
     * @param cascade cascade index (範囲外なら 0)。
     * @return 当該 cascade の atlas viewport。
     */
    FViewport    CascadeViewport(u32 cascade) const noexcept;

    /**
     * atlas 内の cascade 領域に対する scissor を返す。
     *
     * @details BeginShadowPass の後・各 caster 群描画の前に SetScissor へ渡す。
     * @param cascade cascade index (範囲外なら 0)。
     * @return 当該 cascade の atlas scissor。
     */
    FScissorRect CascadeScissor (u32 cascade) const noexcept;

    /**
     * 1 cascade あたりの一辺サイズを返す。
     *
     * @return cascade の一辺サイズ。
     */
    u32 Size() const noexcept { return m_Size; }

private:
    bool EnsureCasterCapacity(u32 cascade, u32 required_casters) noexcept;

    /** シャドウ深度テクスチャ (single または CSM atlas)。 */
    TUniquePtr<IRhiTexture>  m_Depth;

    /** キャスター描画の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** キャスター描画の depth-only パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** 光源の view-projection を渡す定数バッファ (b0)。 */
    TUniquePtr<IRhiBuffer>   m_LightCbs[kMaxCascades];

    /** キャスターの model 行列を渡す定数バッファ (b1)。 */
    TArray<TUniquePtr<IRhiBuffer>> m_ObjectCbs[kMaxCascades];

    IRhiDevice*              m_Device = nullptr;

    /** 各 cascade の light view-projection 行列。 */
    FMat4                    m_LightVp      [kMaxCascades] = {};

    /** 各 cascade の分割閾値 (view-space z far)。 */
    f32                     m_CascadeSplits[kMaxCascades] = {};

    /** 1 cascade あたりの一辺サイズ。 */
    u32                     m_Size          = 0;

    /** 現在有効な cascade 数。single-volume fallback 中は 1。 */
    u32                     m_CascadeCount = 1;

    /** Init 時に確保した atlas/CB の cascade 容量。single fallback 後の CSM 復帰に使う。 */
    u32                     m_CascadeCapacity = 1;

    u32                     m_CurrentCascade = 0;
    u32                     m_CurrentCasters[kMaxCascades] = {};
    u32                     m_CasterDrawCounts[kMaxCascades] = {};
    u32                     m_TotalCasterDrawCount = 0;
    static constexpr u32    kInvalidCasterBuffer = ~u32{0};
    bool                    m_FrameCapacityReady = false;
    bool                    m_CasterOverflowed[kMaxCascades] = {};
    bool                    m_CasterWarningIssued[kMaxCascades] = {};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FShadowMap = CShadowMap;


} // namespace acs
