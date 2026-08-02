// SPDX-License-Identifier: Apache-2.0
// 2D 動的ライティング + ソフト影 (Core Keeper 風) と簡易ブロブ影。
//
// CLighting2D:
//   トップダウン/横スクロール 2D 向けの動的ライト。複数のカラー点光源を
//   持ち、occluder (影を落とすもの) のシルエットからソフト影を落とす。
//   ambient を低く (暗い洞窟) しておき、光源の周りだけが照らされる Core Keeper
//   のような表現を狙う。
//
//   描画フロー (CApplication::OnCustomFrame 内):
//     li.BeginScene(cl);                       // 内部 scene RT を bind + clear
//       sb.Begin(cl,w,h); ...world を描画...; sb.End();
//     li.EndScene(cl);
//     li.BeginOccluders(cl);                   // occluder RT を黒 clear
//       sb.Begin(cl,w,h); ...影を落とすスプライトを白 tint で描画...; sb.End();
//     li.EndOccluders(cl);                     // → シルエットが occluder mask に
//     li.SetAmbient({0.06,0.06,0.09});
//     li.ClearLights();
//     li.AddLight({ {px,py}, radius, {r,g,b}, intensity, softness });
//     cl.BeginRenderToSwapchain(sc, idx, clear);
//       li.Composite(cl, w, h);                // scene × (ambient + Σ light·影) を backbuffer へ
//       sb.Begin(...); ...HUD...; sb.End();
//     cl.EndRenderToSwapchain(sc, idx);
//
//   occluder は「スプライトのシルエット」をそのまま使う (白 tint + alpha blend で
//   黒地に焼くと、不透明部分が occluder になる) ので、矩形でなくスプライト形状に
//   沿った影が落ちる。影は occluder mask を linear sample + 複数レイ (面光源近似)
//   で柔らかくする。
//
// CBlobShadow:
//   光源計算を伴わない激軽の「足元の影」。柔らかい楕円テクスチャを 1 枚持ち、
//   CSpriteBatch で暗く落とすだけ。動的ライトを使わない/負荷を抑えたい時の fallback。
//
// ACS 規約: STL/<string> 不使用、全 noexcept、TResult、非コピー。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"
#include "render/RhiTypes.h"

namespace acs {

class CSpriteBatch;


/**
 * 1 個の 2D 点光源。座標はスプライトと同じピクセル空間 (左上原点)。
 */
struct FLight2D {
    /** 光源位置 (px)。 */
    FVec2 pos{0.0f, 0.0f};

    /** 光が届く半径 (px)。 */
    f32   radius   = 256.0f;

    /** 光の色 (RGB)。 */
    FVec3 color{1.0f, 1.0f, 1.0f};

    /** 明るさ倍率。 */
    f32   intensity = 1.0f;

    /** 影の柔らかさ (0=くっきり影、1=とても柔らかい penumbra)。 */
    f32   softness  = 0.5f;
};

/**
 * 2D 動的ライティング + ソフト影 (Core Keeper 風)。
 *
 * @details
 * トップダウン/横スクロール 2D 向けの動的ライト。複数のカラー点光源を持ち、occluder
 * のシルエットからソフト影を落とす。BeginScene/EndScene で世界の albedo を内部 scene RT
 * に描き、BeginOccluders/EndOccluders で影を落とすスプライトを白 tint で occluder mask
 * に焼き、Composite で scene × (ambient + Σ light·影) を現在の RT へ合成する。影は
 * occluder mask の linear sample と複数レイ (面光源近似) で柔らかくする。
 */
class CLighting2D {
public:
    /** 同時に扱える点光源の上限。 */
    static constexpr u32 kMaxLights = 16;

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CLighting2D() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CLighting2D() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CLighting2D(const CLighting2D&)            = delete;

    /** コピー代入も禁止。 */
    CLighting2D& operator=(const CLighting2D&) = delete;

    /**
     * GPU リソース (scene/occluder RT・パイプライン・定数バッファ) を確保する。
     *
     * @details width/height が 0 のときは RT 作成を遅延し、最初の有効サイズで Resize() が作る。
     * @param device リソース生成に使う RHI デバイス。
     * @param color_format backbuffer の色フォーマット (scene/occluder RT と composite 出力をこれに合わせる)。
     * @param width 初期幅 (px)。
     * @param height 初期高さ (px)。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat color_format,
                       u32 width, u32 height) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 解像度変更時に内部 RT を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @details 0 サイズや現在と同サイズ (RT 確保済み) のときは no-op。
     * @param width 新しい幅 (px)。
     * @param height 新しい高さ (px)。
     * @return 成功なら空の TResult、再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * 環境光を設定する。
     *
     * @param ambient 環境光の色 (低くすると暗い洞窟風)。
     */
    void SetAmbient(FVec3 ambient) noexcept { m_Ambient = ambient; }

    /** 蓄積済みの光源を全て破棄する (毎フレーム先頭で呼ぶ)。 */
    void ClearLights() noexcept { m_LightCount = 0; }

    /**
     * 点光源を 1 個追加する。
     *
     * @param light 追加する点光源。
     * @return 追加成功で true、上限 (kMaxLights) 到達で false。
     */
    bool AddLight(const FLight2D& light) noexcept;

    /**
     * 現在の光源数を返す。
     *
     * @return 蓄積済みの光源数。
     */
    u32  LightCount() const noexcept { return m_LightCount; }

    /**
     * 影の品質を設定する。
     *
     * @details 値は内部で clamp される (march_steps は 2..64、ray_count は 1..16)。
     * @param march_steps 1 レイあたりの行進数。
     * @param ray_count 面光源近似のレイ本数 (多いほど penumbra が滑らかだが重い)。
     */
    void SetShadowQuality(u32 march_steps, u32 ray_count) noexcept;

    /**
     * scene (世界の albedo) 描画 bracket を開始し、内部 scene RT を bind + clear する。
     *
     * @param cl コマンドを積むコマンドリスト。
     * @param clear scene RT のクリア色 (既定は不透明黒)。
     */
    void BeginScene(IRhiCommandList& cl, FVec4 clear = FVec4{0, 0, 0, 1}) noexcept;

    /**
     * scene 描画 bracket を終了する。
     *
     * @param cl コマンドを積むコマンドリスト。
     */
    void EndScene(IRhiCommandList& cl) noexcept;

    /**
     * occluder (影を落とすスプライト) 描画 bracket を開始し、occluder RT を黒 clear する。
     *
     * @details 白 tint で描くと不透明部分が occluder mask になる。
     * @param cl コマンドを積むコマンドリスト。
     */
    void BeginOccluders(IRhiCommandList& cl) noexcept;

    /**
     * occluder 描画 bracket を終了する。
     *
     * @param cl コマンドを積むコマンドリスト。
     */
    void EndOccluders(IRhiCommandList& cl) noexcept;

    /**
     * 現在 bind 中のターゲット (通常 backbuffer) に scene × light を合成する。
     *
     * @details 呼ぶ前に BeginRenderToSwapchain 等でターゲットを bind しておくこと。
     * @param cl コマンドを積むコマンドリスト。
     * @param screen_w 画面幅 (px、ピクセル↔UV 変換に使う)。
     * @param screen_h 画面高さ (px)。
     */
    void Composite(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    /**
     * scene RT を返す。
     *
     * @return 世界の albedo を保持する scene RT。
     */
    IRhiTexture* SceneTexture() const noexcept { return m_SceneRt.Get(); }

    /**
     * occluder RT を返す。
     *
     * @return 遮蔽シルエットを保持する occluder RT。
     */
    IRhiTexture* OccluderTexture() const noexcept { return m_OccluderRt.Get(); }

private:
    /**
     * scene RT と occluder RT を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param w RT の幅 (px)。
     * @param h RT の高さ (px)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept;

    /**
     * 合成シェーダ・パイプラインを生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice* m_Device      = nullptr;

    /** scene/occluder RT と composite 出力の色フォーマット。 */
    EFormat     m_ColorFormat = EFormat::B8G8R8A8_UNorm;

    /** 現在の RT 幅 (px)。 */
    u32         m_Width       = 0;

    /** 現在の RT 高さ (px)。 */
    u32         m_Height      = 0;

    /** 世界の albedo を描く scene RT。 */
    TUniquePtr<IRhiTexture>  m_SceneRt;

    /** 影を落とすシルエットを焼く occluder RT (白=遮蔽)。 */
    TUniquePtr<IRhiTexture>  m_OccluderRt;

    /** 合成パスの頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** 合成パスのピクセルシェーダ (ライト・影計算)。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 合成パスのパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** ライト・画面パラメータを渡す定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** 環境光の色。 */
    FVec3     m_Ambient{0.08f, 0.08f, 0.10f};

    /** 蓄積中の点光源配列。 */
    FLight2D  m_Lights[kMaxLights]{};

    /** 蓄積済みの光源数。 */
    u32       m_LightCount  = 0;

    /** 影レイの行進数。 */
    u32       m_MarchSteps  = 24;

    /** 面光源近似のレイ本数。 */
    u32       m_RayCount    = 6;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FLighting2D = CLighting2D;

/**
 * 光源計算なしの簡易ブロブ影 (足元の楕円)。
 *
 * @details
 * 柔らかい楕円テクスチャを 1 枚持ち、CSpriteBatch で暗く落とすだけの激軽な「足元の影」。
 * 動的ライトを使わない/負荷を抑えたいときの fallback / 補助に使う。
 */
class CBlobShadow {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CBlobShadow() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CBlobShadow() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CBlobShadow(const CBlobShadow&)            = delete;

    /** コピー代入も禁止。 */
    CBlobShadow& operator=(const CBlobShadow&) = delete;

    /**
     * 柔らかい放射状グラデーションのテクスチャを 1 枚生成する。
     *
     * @details resolution は内部で 16..64 に clamp される。
     * @param device テクスチャ生成に使う RHI デバイス。
     * @param resolution 影テクスチャの一辺の解像度 (px)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 resolution = 64) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * (cx,cy) 中心に w×h の柔らかい影を sb 経由で描く (要 sb.Begin 済み)。
     *
     * @param sb 描画に使う CSpriteBatch。
     * @param cx 影の中心 X (px)。
     * @param cy 影の中心 Y (px)。
     * @param w 影の幅 (px)。
     * @param h 影の高さ (px)。
     * @param alpha 影の濃さ。
     * @param color 影の色 (既定は黒)。
     */
    void Draw(CSpriteBatch& sb, f32 cx, f32 cy, f32 w, f32 h,
              f32 alpha = 0.5f, FVec3 color = FVec3{0, 0, 0}) noexcept;

    /**
     * 影テクスチャを返す。
     *
     * @return 放射状グラデーションの影テクスチャ。
     */
    IRhiTexture* Texture() const noexcept { return m_Tex.Get(); }

private:
    /** 放射状グラデーションの影テクスチャ。 */
    TUniquePtr<IRhiTexture> m_Tex;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FBlobShadow = CBlobShadow;


} // namespace acs
