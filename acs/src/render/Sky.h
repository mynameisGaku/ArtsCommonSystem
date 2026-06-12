// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ（グラデーション + 太陽）
//
// 用途: 3D シーンの背景に「空」を描く。シーン描画より先に呼ぶ。
//       テクスチャ（キューブマップ）不要、ピクセルシェーダで天頂・地平線・
//       地面の色を補間して描画する。
//
// 使い方:
//   FSky sky;
//   sky.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
//   sky.PresetDay();
//
//   // 描画フレーム中、シーンの最初に
//   sky.Render(*cl, camera);
//   // ... FStandardShader でメッシュを描く ...
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"

namespace acs {

class FCamera;

/**
 * 手続き生成スカイ (グラデーション + 太陽)。
 *
 * @details
 * テクスチャ (キューブマップ) 不要で、ピクセルシェーダが視線方向から天頂・地平線・
 * 地面の色を補間して背景を描く。シーン描画より先にフルスクリーン三角形で塗り、
 * 深度の書き込み・テストは行わない (背景塗りなので既存深度を維持)。太陽は視線と太陽
 * 方向の角度で半径・ハローを付ける。VS/PS/PSO/定数バッファを単独所有する。
 */
class FSky {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSky() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSky() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSky(const FSky&)            = delete;

    /** コピー代入も禁止。 */
    FSky& operator=(const FSky&) = delete;

    /**
     * GPU リソース (VS/PS/PSO/定数バッファ) を確保する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format 描画先カラーターゲットのフォーマット。
     * @param depth_format 深度ターゲットのフォーマット (パイプライン作成用)。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 太陽方向を設定する (内部で正規化する)。
     *
     * @details カメラの右手/左手系の前提なし。シェーダ側でも normalize する。
     * @param dir 原点から太陽へ向く方向ベクトル。
     */
    void SetSunDirection(FVec3 dir) noexcept;

    /**
     * 太陽の色を設定する。
     *
     * @param c 太陽の RGB 色。
     */
    void SetSunColor(FVec3 c)       noexcept { m_SunColor = c; }

    /**
     * 太陽の見かけ半径を設定する。
     *
     * @param angular 視線角の cos 値からの差 (0.001 = 鋭い、0.05 = 大きい)。
     */
    void SetSunRadius(f32 angular) noexcept { m_SunRadius  = angular; }

    /**
     * 太陽の周りのハロー (グロー) の広がりを設定する。
     *
     * @param angular ハローの角度的な広がり。
     */
    void SetSunGlow(f32 angular)   noexcept { m_SunGlow    = angular; }

    /**
     * 天頂の色を設定する。
     *
     * @param c 天頂 (真上) 方向の RGB 色。
     */
    void SetZenithColor(FVec3 c)    noexcept { m_Zenith  = c; }

    /**
     * 地平線の色を設定する。
     *
     * @param c 地平線 (dir.y=0) 方向の RGB 色。
     */
    void SetHorizonColor(FVec3 c)   noexcept { m_Horizon = c; }

    /**
     * 地面方向の色を設定する。
     *
     * @param c 地面 (真下) 方向の RGB 色。
     */
    void SetGroundColor(FVec3 c)    noexcept { m_Ground  = c; }

    /**
     * 手続き的な雲の量と濃さを設定する。
     *
     * @param coverage 雲量 (0=快晴、1=全天曇り)。
     * @param density 雲の濃さ/輪郭の鋭さ (1=やわらか、2〜3=もくもく)。
     */
    void SetClouds(f32 coverage, f32 density = 1.6f) noexcept {
        m_CloudCoverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        m_CloudDensity  = density  < 0.1f ? 0.1f : density;
    }

    /**
     * 雲を描くかどうかを切り替える。
     *
     * @param on true で雲を描画 (既定 ON)。
     */
    void SetCloudsEnabled(bool on)  noexcept { m_bCloudsEnabled = on; }

    /**
     * 雲の基本色を設定する。
     *
     * @param c 雲の RGB 色 (太陽方向で明色に、濃い所は暗色に補間される)。
     */
    void SetCloudColor(FVec3 c)     noexcept { m_CloudColor = c; }

    /**
     * 雲が流れる速さを設定する。
     *
     * @param speed 風速 (0=静止、1=標準)。SetTime と併用してアニメする。
     */
    void SetCloudWind(f32 speed)    noexcept { m_CloudWind = speed; }

    /**
     * 雲アニメ用の時間を設定する (任意。決定論的に制御したいときだけ呼ぶ)。
     *
     * @details 呼ばなくても Render() が内部で時間を進めるので雲は流れる。毎フレーム
     *          経過秒を渡すと、その値が当該フレームで優先される (リプレイ/スクショ向け)。
     * @param seconds 起動からの経過秒など、単調増加する時間値。
     */
    void SetTime(f32 seconds)       noexcept { m_Time = seconds; }

    /** 昼空プリセットを適用する (青空 + 白い太陽)。 */
    void PresetDay()    noexcept;

    /** 夕焼けプリセットを適用する (茜色 + 暖色太陽)。 */
    void PresetSunset() noexcept;

    /** 夜空プリセットを適用する (紺青 + 弱い月光)。 */
    void PresetNight()  noexcept;

    /**
     * 現在の太陽方向を返す (FStandardShader / IBL と整合させたいときに)。
     *
     * @return 正規化済みの太陽方向ベクトル。
     */
    FVec3 SunDirection() const noexcept { return m_SunDir; }

    /**
     * 現在の太陽色を返す。
     *
     * @return 太陽の RGB 色。
     */
    FVec3 SunColor()     const noexcept { return m_SunColor; }

    /**
     * 現在の太陽半径を返す。
     *
     * @return 太陽の見かけ半径 (視線角 cos 値からの差)。
     */
    f32  SunRadius()    const noexcept { return m_SunRadius; }

    /**
     * 現在の太陽ハロー幅を返す。
     *
     * @return 太陽ハローの角度的な広がり。
     */
    f32  SunGlow()      const noexcept { return m_SunGlow; }

    /**
     * 現在の天頂色を返す。
     *
     * @return 天頂方向の RGB 色。
     */
    FVec3 ZenithColor()  const noexcept { return m_Zenith; }

    /**
     * 現在の地平線色を返す。
     *
     * @return 地平線方向の RGB 色。
     */
    FVec3 HorizonColor() const noexcept { return m_Horizon; }

    /**
     * 現在の地面色を返す。
     *
     * @return 地面方向の RGB 色。
     */
    FVec3 GroundColor()  const noexcept { return m_Ground; }

    /**
     * スカイを描画する (フレーム先頭で呼ぶ)。
     *
     * @details 深度バッファは「背景に塗る」想定で書き込み無し・テスト無し。
     * @param cl 描画コマンドを積むコマンドリスト。
     * @param camera 逆 view-projection と視点を取り出すカメラ。
     */
    void Render(IRhiCommandList& cl, const FCamera& camera) noexcept;

private:
    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** 空の色を計算するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** スカイ描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** スカイパラメータを渡す定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** 太陽方向 (正規化済み)。 */
    FVec3 m_SunDir    = FVec3{0.5f, 0.8f, 0.3f};

    /** 太陽の RGB 色。 */
    FVec3 m_SunColor  = FVec3{1.0f, 0.95f, 0.85f};

    /** 太陽の見かけ半径 (視線角 cos 値からの差)。 */
    f32  m_SunRadius = 0.0006f;

    /** 太陽ハローの角度的な広がり。 */
    f32  m_SunGlow   = 0.04f;

    /** 天頂方向の RGB 色。 */
    FVec3 m_Zenith     = FVec3{0.18f, 0.40f, 0.78f};

    /** 地平線方向の RGB 色。 */
    FVec3 m_Horizon    = FVec3{0.70f, 0.80f, 0.95f};

    /** 地面方向の RGB 色。 */
    FVec3 m_Ground     = FVec3{0.20f, 0.18f, 0.16f};

    /** 雲を描画するか (既定 ON)。 */
    bool m_bCloudsEnabled = true;

    /** 雲量 (0=快晴、1=全天曇り)。 */
    f32  m_CloudCoverage = 0.50f;

    /** 雲の濃さ/輪郭の鋭さ。 */
    f32  m_CloudDensity  = 1.6f;

    /** 雲が流れる速さ。 */
    f32  m_CloudWind     = 1.0f;

    /** 雲アニメ用の時間 (SetTime で更新)。 */
    f32  m_Time          = 0.0f;

    /** 雲の基本 RGB 色。 */
    FVec3 m_CloudColor   = FVec3{1.0f, 1.0f, 1.0f};
};

} // namespace acs
