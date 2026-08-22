// SPDX-License-Identifier: Apache-2.0
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
#include "render/IRhiTexture.h"
#include "render/SkySunProfile.h"
#include "render/VolumetricCloudWorldShadow.h"

namespace acs {

class CCamera;

/**
 * 手続き生成スカイ (グラデーション + 太陽)。
 *
 * @details
 * テクスチャ (キューブマップ) 不要で、ピクセルシェーダが視線方向から天頂・地平線・
 * 地面の色を補間して背景を描く。シーン描画より先にフルスクリーン三角形で塗り、
 * 深度の書き込み・テストは行わない (背景塗りなので既存深度を維持)。太陽は視線と太陽
 * 方向の角度で半径・ハローを付ける。低コスト雲は本格的な CVolumetricClouds が使えない
 * 場合だけ明示的に有効化する fallback で、既定では描かない。VS/PS/PSO/定数バッファを
 * 単独所有する。
 */
class CSky {
public:
    /** CPU-compiled shader bytecode handed to the render-owner thread. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> pixel;

        /** Aggregate a backend-managed asynchronous compile without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CSky() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CSky() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CSky(const CSky&)            = delete;

    /** コピー代入も禁止。 */
    CSky& operator=(const CSky&) = delete;

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

    /**
     * Compile the raw-DX12 HLSL bytecode without touching an RHI device.
     * Other backends retain the regular owner-thread Init path.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /**
     * Submit shader compilation to a supporting RHI backend. Submission and
     * later PSO/resource creation stay on the render-owner thread.
     */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Install CPU-compiled shaders and create the buffer and PSO.
     * Must be called by the render-owner thread.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat rt_format = EFormat::B8G8R8A8_UNorm,
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

    /** 太陽円盤の角半径を1-cos形式で設定する。
     * @param one_minus_cosine 太陽中心から円盤外端までの1-cos値。
     */
    void SetSunRadius(f32 one_minus_cosine) noexcept;

    /** 太陽の光彩外端を1-cos形式で設定する。
     * @param one_minus_cosine 太陽中心から光彩外端までの1-cos値。
     */
    void SetSunGlow(f32 one_minus_cosine) noexcept;

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
     * 低コスト fallback 雲の量と濃さを設定する。
     *
     * @details カメラ中心の仮想層を固定刻みで積分する近似であり、ワールド高度、物理大気、
     * 時間再構成を使わない。本番品質の雲には CVolumetricClouds を使う。
     * @param coverage 雲量。有限値を 0〜1 に収め、不正値は 0 にする。
     * @param density 雲の濃さ。有限値を 0.1〜8 に収め、不正値は 1.6 にする。
     */
    void SetFallbackClouds(f32 coverage, f32 density = 1.6f) noexcept;

    /**
     * 低コスト fallback 雲を描くか切り替える。
     *
     * @param on true で描画する。既定は false。
     */
    void SetFallbackCloudsEnabled(bool on) noexcept { m_FallbackCloudsEnabled = on; }

    /**
     * 低コスト fallback 雲の基本色を設定する。
     *
     * @param color 雲の RGB 色。有限でない成分は直前の値を保ち、有限値は 0〜16384 に収める。
     */
    void SetFallbackCloudColor(FVec3 color) noexcept;

    /**
     * 低コスト fallback 雲の移動速度を設定する。
     *
     * @param speed 風速。有限値を -20〜20 に収め、不正値は 0 にする。
     */
    void SetFallbackCloudWind(f32 speed) noexcept;

    /**
     * 低コスト fallback 雲が参照する時刻を設定する。
     *
     * @details Render は時間を進めない。同じ入力は同じ雲配置になり、FPS や描画回数に依存しない。
     * @param seconds 起動からの経過秒など。有限値を -10000000〜10000000 に収める。
     */
    void SetFallbackCloudTime(f32 seconds) noexcept;

    /** 旧 API から明示的な fallback 設定へ渡す互換アダプター。 */
    void SetClouds(f32 coverage, f32 density = 1.6f) noexcept { SetFallbackClouds(coverage, density); }

    /** 旧 API から明示的な fallback 有効状態へ渡す互換アダプター。 */
    void SetCloudsEnabled(bool on) noexcept { SetFallbackCloudsEnabled(on); }

    /** 旧 API から明示的な fallback 色へ渡す互換アダプター。 */
    void SetCloudColor(FVec3 color) noexcept { SetFallbackCloudColor(color); }

    /** 旧 API から明示的な fallback 風速へ渡す互換アダプター。 */
    void SetCloudWind(f32 speed) noexcept { SetFallbackCloudWind(speed); }

    /** 旧 API から明示的な fallback 時刻へ渡す互換アダプター。 */
    void SetTime(f32 seconds) noexcept { SetFallbackCloudTime(seconds); }

    /** 低コスト fallback 雲が有効なら true を返す。 */
    bool FallbackCloudsEnabled() const noexcept { return m_FallbackCloudsEnabled; }

    /** 検証済みの低コスト fallback 雲量を返す。 */
    f32 FallbackCloudCoverage() const noexcept { return m_FallbackCloudCoverage; }

    /** 検証済みの低コスト fallback 雲密度を返す。 */
    f32 FallbackCloudDensity() const noexcept { return m_FallbackCloudDensity; }

    /** 検証済みの低コスト fallback 雲風速を返す。 */
    f32 FallbackCloudWind() const noexcept { return m_FallbackCloudWind; }

    /** 検証済みの低コスト fallback 雲色を返す。 */
    FVec3 FallbackCloudColor() const noexcept { return m_FallbackCloudColor; }

    /** 現在の低コスト fallback 雲時刻を返す。 */
    f32 FallbackCloudTime() const noexcept { return m_FallbackCloudTime; }

    /** 昼空プリセットを適用する (青空 + 白い太陽)。 */
    void PresetDay()    noexcept;

    /** 夕焼けプリセットを適用する (茜色 + 暖色太陽)。 */
    void PresetSunset() noexcept;

    /** 夜空プリセットを適用する (紺青 + 弱い月光)。 */
    void PresetNight()  noexcept;

    /**
     * 現在の太陽方向を返す (CStandardShader / IBL と整合させたいときに)。
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
    void Render(IRhiCommandList& cl, const CCamera& camera) noexcept;

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

    /** 太陽円盤の角半径を表す1-cos値。 */
    f32 m_SunRadius = kSkySolarDiscRadiusOneMinusCosine;

    /** 太陽の光彩外端を表す1-cos値。 */
    f32 m_SunGlow = kSkyDaySunHaloRadiusOneMinusCosine;

    /** 天頂方向の RGB 色。 */
    FVec3 m_Zenith     = FVec3{0.18f, 0.40f, 0.78f};

    /** 地平線方向の RGB 色。 */
    FVec3 m_Horizon    = FVec3{0.70f, 0.80f, 0.95f};

    /** 地面方向の RGB 色。 */
    FVec3 m_Ground     = FVec3{0.20f, 0.18f, 0.16f};

    /** 低コスト fallback 雲を描画するか。 */
    bool m_FallbackCloudsEnabled = false;

    /** 雲量 (0=快晴、1=全天曇り)。 */
    f32  m_FallbackCloudCoverage = 0.50f;

    /** 雲の濃さ/輪郭の鋭さ。 */
    f32  m_FallbackCloudDensity  = 1.6f;

    /** 雲が流れる速さ。 */
    f32  m_FallbackCloudWind     = 1.0f;

    /** 低コスト fallback 雲の決定論的な時刻。 */
    f32  m_FallbackCloudTime = 0.0f;

    /** 雲の基本 RGB 色。 */
    FVec3 m_FallbackCloudColor = FVec3{1.0f, 1.0f, 1.0f};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FSky = CSky;

/**
 * どこまで雲を描くか。
 *
 * @details
 * 雲の値段は **画面のうち雲に当たる画素の数 × 1 本のレイの刻み数** で決まる。上を向くと
 * 画面全部が雲になり、地平線を見ると 1 本が数十 km を貫く。どちらもここで抑える。
 *
 * 見え方の理由もある。**遠くの雲は 1 画素に何 km も詰め込まれるので、まともに積分できない。**
 * 描くほど «ちらつく細かいゴミ» になる。消した方が綺麗に見える。
 */
struct FVolumetricCloudRange {
    /**
     * ここより遠いレイは追わない (メートル)。
     *
     * @details
     * 既定の 250 km は «地平線の果てまで» を意味する。**ゲームには広すぎる。**
     * 地上を歩くなら 40〜80 km で足りる (雲底 2.6 km なら地平線は約 180 km 先だが、
     * そこまでの雲は 1 画素未満にしかならない)。飛ぶなら広げる。
     */
    f32 MaxDistance = 250000.0f;

    /**
     * 打ち切りの手前、どれだけの割合を使って消していくか。
     *
     * @details
     * 0.28 なら `MaxDistance` の 72 % 地点から薄くなり始める。**0 にすると境界で
     * ぱっつり切れて «壁» が見える。** 遠いほど大気の霞に紛れるので、広めに取ってよい。
     */
    f32 FadeFraction = 0.28f;

    /**
     * 遠くから始まるレイの刻みを、どれだけ広げるか。
     *
     * @details
     * 0 で一定 (これまでと同じ)。1.0 なら `MaxDistance` から入るレイの刻みが 2 倍になる。
     * 地平線へ向かうレイほど長く、かつ 1 画素の担当範囲が広いので、細かく刻んでも
     * 結果に出ない。**上を向いたときのレイは近くから始まるので、ここを上げても粗くならない。**
     */
    f32 StepGrowth = 0.0f;

    /**
     * 1 本のレイに使う刻みの上限。
     *
     * @details
     * 0 なら既定 (通常 192 / 参照 512)。**「重い」ときにいちばん効く摘み。**
     * 下げると厚い雲の内部が粗くなり、縞が出る。
     */
    u32 ViewSteps = 0u;
};

/**
 * 雲を «光を散らす媒質» として照らすための係数。
 *
 * @details
 * 消散 (extinction) を見た目調整の摘みとして使いすぎないこと。同じ雲なのに見る方向と
 * 光の方向で消散が違うと、カメラからはすぐ不透明なのに太陽光だけ内部へ届く、という
 * «気体でない» 見え方になる。明るさは `SunScatter` や散乱側で調整する。
 */
struct FVolumetricCloudLighting {
    /**
     * 見る方向の消散。
     *
     * @details
     * 光の方向と**同じ値**にしてある。以前は 7.0 対 4.2 で、同じ雲なのに向きで消散が違い、
     * カメラからはすぐ不透明なのに太陽光だけ内部へ届く «半透明の白い物体» に見えていた。
     */
    f32 ViewExtinction = 5.0f;

    /** 光の方向の消散。見る方向と揃える。 */
    f32 LightExtinction = 5.0f;

    /**
     * 太陽光のうち散乱に回る割合（単散乱アルベド）。
     *
     * @details
     * 視線側の区間不透明度は消散係数を含むため、ここへ見た目調整用の小さな倍率を置くと
     * 雲だけが光を吸収する灰色の媒質になる。可視光の水滴雲として1に近い値を既定にし、
     * 太陽付近の明るさは位相上限と露出で制御する。
     */
    f32 SunScatter = 0.92f;

    /**
     * 周囲に高次散乱の光源がある確率を混ぜる割合。
     *
     * @details
     * 公開名は互換性のため `PowderStrength` を維持する。0 なら補正なし、1 なら低 LOD 密度と
     * 層内高さから求めた周囲散乱源の確率をそのまま使う。一次散乱は現在の密度標本で既に
     * 制限されるため、この値は二次以降の散乱だけへ適用する。
     */
    f32 PowderStrength = 0.30f;

    /** 前方散乱の鋭さ (Henyey-Greenstein の g)。 */
    f32 PhaseForward = 0.60f;

    /** 後方散乱の鋭さ。負で後ろ向き。 */
    f32 PhaseBackward = -0.20f;

    /** 前方の混ぜ率 (残りが後方)。 */
    f32 PhaseBlend = 0.85f;

    /**
     * 位相の下限。
     *
     * @details
     * **0 が本来。** 下限を上げると、本来暗くなる方向まで明るくなり、光の向きによる
     * 明暗差が消えて «綿菓子» に見える。以前は 0.25 で潰していた。
     */
    f32 PhaseMin = 0.0f;

    /**
     * 位相の上限。
     *
     * @details
     * 太陽方向の強い前方散乱をどこで止めるか。以前は 2.4 で、縁の光 (silver lining) まで
     * 潰れていた。
     */
    f32 PhaseMax = 8.0f;

    /**
     * 二次以降の散乱係数へ次数ごとに掛ける縮小率 (0 で単散乱のみ)。
     *
     * @details
     * 二次ではこの値、三次ではこの値の二乗を使う。散乱が消散を越えないように
     * `MultiScatterOcclusion` 以下へ正規化する。
     */
    f32 MultiScatterContribution = 0.28f;

    /** 二次以降の消散係数へ次数ごとに掛ける縮小率 (小さいほど内部まで光が回る)。 */
    f32 MultiScatterOcclusion = 0.28f;

    /**
     * 天頂の空の色 (放射輝度)。
     *
     * @details
     * **雲頂は天頂の空を、雲底は地平の空を受ける。** これを分けないと、上面と側面と底で
     * 空から受ける光が同じになり、立体感が出ない。
     *
     * 既定の (0,0,0) は «分けない» 意味で、`RenderCompute` に渡した空の色を上下とも使う
     * (これまでと同じ)。地平の色はそちらの引数がそのまま担う。
     */
    FVec3 SkyZenithColor{0.0f, 0.0f, 0.0f};

    /**
     * 多重散乱に使う位相の鋭さ。
     *
     * @details
     * **何度も散乱した光は向きを失う**ので、単散乱より等方に近い。既定の 0 は完全な等方。
     * ここに単散乱と同じ鋭さを入れると、内部で回った光まで太陽方向へ偏り、雲が薄く見える。
     */
    f32 MultiScatterEccentricity = 0.0f;

    /** 雲底が空から受ける割合。 */
    f32 AmbientAtBase = 0.26f;

    /** 雲頂が空から受ける割合。 */
    f32 AmbientAtTop = 0.52f;

    /**
     * 地面からの照り返しの強さ。
     *
     * @details
     * 地面や海が太陽光と空の光を跳ね返して雲底を照らす。無いと雲底が真っ黒で平坦になる。
     * 海や雪原の上ではもっと上げてよい。
     */
    f32 GroundContribution = 0.15f;

    /**
     * 太陽光が雲へ届くまでの大気透過率。
     *
     * @details
     * 低い太陽ほど青が削られて赤くなる。これを掛けないと**夕方でも雲が昼の白さのまま**になる。
     * `SunTransmittanceAtAltitude` (Atmosphere.h) で雲の高さぶんを求めて渡すとよい。
     * 既定の (1,1,1) は «大気を通らない» = これまでと同じ。
     */
    FVec3 SunTransmittance{1.0f, 1.0f, 1.0f};

    /** 照り返しの色。地面や海の色を入れる。 */
    FVec3 GroundColor{0.20f, 0.19f, 0.21f};
};

/**
 * 雲密度を固定するワールド空間の高度帯。
 *
 * @details カメラ移動で密度場を動かさず、描画と時間再構成が同じ雲を参照する。
 */
struct FVolumetricCloudLayer {
    /** 下層の雲底高度 (メートル)。 */
    f32 base_height = 1500.0f;

    /** 下層の雲頂高度 (メートル)。 */
    f32 top_height = 4000.0f;

    /** ワールド XZ を形状ノイズ座標へ変換する倍率。 */
    f32 horizontal_noise_scale = 0.035f;
};

/**
 * 下層の上に重ねる、もう 1 枚の高い雲。
 *
 * @details
 * 空が 1 枚しか無いと «高さ» が読めない。**遠近感は «上に何かある» ことで出る。**
 * 積雲の上に薄い巻雲を敷くと、同じ雲でも高度差が見えるようになる。
 *
 * トレースは 1 本のままで、殻の外側を上層の天井まで伸ばす。あいだの隙間は密度 0 なので、
 * レイは粗い刻みで素通りする。**2 回描くわけではない。**
 *
 * `top_height <= base_height` なら無効 (既定)。無効のときの見え方はこの層を足す前と
 * 完全に同じ。
 */
struct FVolumetricCloudUpperLayer {
    /** 上層の底 (メートル)。下層の天井より上に置くこと。 */
    f32 base_height = 0.0f;

    /** 上層の天井 (メートル)。`base_height` 以下なら無効。 */
    f32 top_height = 0.0f;

    /**
     * 下層の被覆に対する割合。
     *
     * @details 1.0 だと «積乱雲を 2 枚» になって空が閉じる。薄く敷くもの。
     */
    f32 coverage_scale = 0.55f;

    /**
     * 下層の濃さに対する割合。
     *
     * @details 巻雲は光を通す。低くするほど «透ける薄い雲» になる。
     */
    f32 density_scale = 0.30f;
};

/** Ray interval through a horizontal world-space cloud layer. */
struct FVolumetricCloudRayInterval {
    f32 enter = 0.0f;
    f32 exit = 0.0f;
    bool hit = false;
};

/** Local planet radius used by the curved world-space cloud shell. */
inline constexpr f32 kVolumetricCloudPlanetRadius = 6360000.0f;

/**
 * World-origin grid used by the local curved-shell patch.
 *
 * The shell origin stays fixed through the centre of each cell and is eased
 * across a transition band near cell boundaries. Density/weather coordinates
 * remain absolute world coordinates; only the numerically local planet
 * tangent patch is rebased.
 */
inline constexpr f32 kVolumetricCloudOriginRebaseGrid = 64.0f;
inline constexpr f32 kVolumetricCloudMaxDistance = 250000.0f;

/** 設定可能な最短描画距離。これ未満では距離フェードとレイ刻みが不安定になる。 */
inline constexpr f32 kVolumetricCloudMinDistance = 1000.0f;

/** 雲層として保持する最小厚さ。逆数を GPU へ渡しても有限になる値。 */
inline constexpr f32 kVolumetricCloudMinLayerThickness = 0.25f;

/** 通常描画で利用者が指定できる最小レイ刻み数。 */
inline constexpr u32 kVolumetricCloudMinViewSteps = 32u;

/** 遠距離レイの刻み拡大率の上限。最大でも既定刻みの 5 倍に抑える。 */
inline constexpr f32 kVolumetricCloudMaxStepGrowth = 4.0f;

/** 描画距離の終端を滑らかに消す割合の上限。 */
inline constexpr f32 kVolumetricCloudMaxFadeFraction = 0.95f;

/** 視線・光レイへ適用する消散係数の上限。 */
inline constexpr f32 kVolumetricCloudMaxExtinction = 64.0f;

/** 周囲散乱源の確率を高次散乱へ混ぜる割合の上限。 */
inline constexpr f32 kVolumetricCloudMaxPowderStrength = 1.0f;

/** 位相関数を描画用に切り詰める上限値。 */
inline constexpr f32 kVolumetricCloudMaxPhaseValue = 64.0f;

/** Henyey-Greenstein 位相関数を特異点から離す異方性の絶対上限。 */
inline constexpr f32 kVolumetricCloudMaxPhaseEccentricity = 0.99f;

/**
 * Internal trace divisor used by the Ultra output-quality policy.
 *
 * Ultra still resolves and composites at the complete viewport resolution.
 * Every reduced texel is freshly marched each frame, and a sixteen-phase 4x4
 * subpixel schedule maps those rays onto exact full-resolution coordinates.
 * World/depth reprojection retains the other fifteen phases, so the steady
 * image recovers native detail without increasing the quarter-size workload.
 */
inline constexpr u32 kVolumetricCloudUltraTraceDivisor = 4u;

/** 通常描画で 1 本のレイに使う刻みの上限。 */
inline constexpr u32 kVolumetricCloudViewSteps = 192u;

/**
 * 参照描画で 1 本のレイに使う刻みの上限。
 *
 * @details 正解画像を作るためのもので、速度は捨てている。
 */
inline constexpr u32 kVolumetricCloudReferenceViewSteps = 512u;
inline constexpr u32 kVolumetricCloudMaxViewMarchSamples = 192u;
inline constexpr u32 kVolumetricCloudMaxLightMarchSamples = 8u;

/** 下層設定を有限で順序付けされた GPU 安全値へ直す。 */
FVolumetricCloudLayer SanitizeVolumetricCloudLayer(const FVolumetricCloudLayer& requested) noexcept;

/** 照明設定を有限で物理的に意味のある範囲へ直す。 */
FVolumetricCloudLighting SanitizeVolumetricCloudLighting(const FVolumetricCloudLighting& requested) noexcept;

/**
 * 雲の一次散乱と、近似二次・三次散乱の方向別係数を CPU で評価する。
 *
 * @param light_optical_depth 太陽までの光学的な厚さ。負値は 0、非有限値は寄与なしとして扱う。
 * @param single_phase 一次散乱の位相値。照明設定の位相範囲へ収める。
 * @param multiple_phase 二次・三次散乱の位相値。照明設定の位相範囲へ収める。
 * @param lighting 正規化前でもよい照明設定。
 * @return x が一次散乱、y が二次と三次の合計。全散乱は x+y。
 */
FVec2 EvaluateVolumetricCloudDirectionalScattering(f32 light_optical_depth, f32 single_phase, f32 multiple_phase, const FVolumetricCloudLighting& lighting) noexcept;

/**
 * 低 LOD 密度と層内高さから、高次散乱へ掛ける周囲散乱源の係数を CPU で評価する。
 *
 * @param low_lod_density 周囲を表す低 LOD 密度。有限な 0..1 へ収める。
 * @param normalized_height 雲層の底を 0、上端を 1 とした高さ。有限な 0..1 へ収める。
 * @param strength 周囲散乱源の確率を混ぜる割合。有限な 0..1 へ収める。
 * @return 補正なしを 1 とする有限な係数。0..1 の範囲を越えない。
 */
f32 EvaluateVolumetricCloudInScatterFactor(f32 low_lod_density, f32 normalized_height, f32 strength) noexcept;

/** 描画距離と刻み数を実装上の上限内へ直す。 */
FVolumetricCloudRange SanitizeVolumetricCloudRange(const FVolumetricCloudRange& requested) noexcept;

/**
 * 雲の標本距離から、打ち切り区間で密度へ掛ける係数を求める。
 *
 * @param sample_distance カメラから標本までの距離。
 * @param max_distance 雲描画を打ち切る距離。
 * @param fade_fraction 打ち切り手前で薄める区間の割合。0 は終端での即時打ち切り。
 * @return 有限な 0～1 の密度係数。不正な標本距離は見えない値へ戻す。
 */
f32 EvaluateVolumetricCloudDistanceFade(f32 sample_distance, f32 max_distance, f32 fade_fraction) noexcept;

/** 上層設定を下層と交差しない有限値へ直す。成立しない層は無効化する。 */
FVolumetricCloudUpperLayer SanitizeVolumetricCloudUpperLayer(const FVolumetricCloudUpperLayer& requested,
                                                             const FVolumetricCloudLayer& lower_layer) noexcept;

/** Sanitized current-trace dimensions selected for a full-resolution output. */
struct FVolumetricCloudTraceResolution {
    u32 width = 1u;
    u32 height = 1u;
    f32 quality_multiplier = 1.0f;
    f32 effective_dimension_scale = 0.25f;
};

/**
 * 雲の内部描画品質倍率を有限な 0.5〜4.0 へ収める。
 *
 * @param requested_render_scale 利用側が指定した品質倍率。非有限値は 1.0 として扱う。
 * @return 1.0 は画面寸法の 1/4、4.0 は画面と同じ寸法になる品質倍率。
 */
f32 SanitizeVolumetricCloudQualityMultiplier(
    f32 requested_render_scale) noexcept;

/**
 * Resolve the authored CloudRenderScale and the internal Ultra trace policy.
 *
 * CloudRenderScale is a monotonic quality multiplier over the policy's base
 * quarter-dimension trace: authored 1.0 uses quarter dimensions, 0.75 uses
 * 0.1875 dimensions, and 0.5 or below uses the 0.125 lower bound. The resolved
 * output and temporal history remain full resolution.
 *
 * Values above 1.0 trace finer than the policy: 2.0 halves the full dimensions and
 * 4.0 matches them. The cap used to be 1.0, which left the visible dot pattern of a
 * quarter-resolution trace unfixable outside reference mode.
 *
 * `reference_mode` bypasses the policy entirely and traces at full dimensions.
 * It exists to tell «the lighting is wrong» apart from «the reconstruction is wrong»,
 * and is far too slow for gameplay.
 */
FVolumetricCloudTraceResolution ResolveVolumetricCloudTraceResolution(
    u32 full_width, u32 full_height, f32 requested_render_scale,
    bool reference_mode = false) noexcept;

/** 1フレームの雲描画で投入する計算量を数えるための入力。 */
struct FVolumetricCloudFrameWorkloadPlan {
    u32 trace_width = 0u;
    u32 trace_height = 0u;
    u32 output_width = 0u;
    u32 output_height = 0u;
    /** 1 本の視線レイで実行できる密度採取回数。 */
    u32 maximum_view_steps = kVolumetricCloudViewSteps;
    /** 自己影を各軸で何画素おきに更新するか。1 は全更新。 */
    u32 shadow_update_divisor = 1u;
    bool bake_shape_noise = false;
    bool bake_weather = false;
    bool bake_detail_noise = false;
    bool bake_curl_noise = false;
    bool rebuild_shadow_cache = false;
    bool rebuild_world_shadow = false;
};

enum class EVolumetricCloudFrameSkipReason : u32 {
    None = 0u,
    ResourcesNotReady = 1u,
    InvalidCamera = 2u,
    InvalidProjection = 3u,
};

/**
 * Allocation-free diagnostic for the most recent RenderCompute attempt.
 *
 * Logical invocations exclude workgroup padding; launched threads include it.
 * maximum_*_samples are conservative shader-loop ceilings rather than measured
 * samples because empty-space skipping and transmittance exits are data
 * dependent. GPU timestamps remain authoritative for elapsed cost.
 */
struct FVolumetricCloudFrameWorkload {
    u64 submission_index = 0u;
    u32 trace_width = 0u;
    u32 trace_height = 0u;
    u32 output_width = 0u;
    u32 output_height = 0u;

    u32 steady_dispatches = 0u;
    u32 one_time_bake_dispatches = 0u;
    u32 shadow_cache_dispatches = 0u;
    u32 world_shadow_dispatches = 0u;
    u32 total_compute_dispatches = 0u;
    u32 composite_draws = 0u;

    u64 trace_logical_invocations = 0u;
    u64 trace_launched_threads = 0u;
    u64 resolve_logical_invocations = 0u;
    u64 resolve_launched_threads = 0u;
    u64 one_time_bake_logical_invocations = 0u;
    u64 one_time_bake_launched_threads = 0u;
    u64 shadow_cache_logical_invocations = 0u;
    u64 shadow_cache_launched_threads = 0u;
    u64 world_shadow_logical_invocations = 0u;
    u64 world_shadow_launched_threads = 0u;
    u64 total_logical_invocations = 0u;
    u64 total_launched_threads = 0u;
    u64 maximum_view_samples = 0u;
    u64 maximum_light_samples = 0u;
    u64 maximum_world_shadow_samples = 0u;

    EVolumetricCloudFrameSkipReason skip_reason =
        EVolumetricCloudFrameSkipReason::None;
    bool attempted = false;
    bool submitted = false;
    bool history_was_available = false;
    bool history_reused = false;
    bool history_invalidated = false;
    bool temporal_super_resolution = false;
};

/**
 * Deterministically account for a cloud frame without recording GPU work.
 *
 * All additions and products saturate at u64 max so malformed diagnostic input
 * cannot wrap into a deceptively small workload.
 */
FVolumetricCloudFrameWorkload PlanVolumetricCloudFrameWorkload(
    const FVolumetricCloudFrameWorkloadPlan& plan) noexcept;

/**
 * Resolve centered analytic coverage for the planet/cloud horizon.
 *
 * elevation_delta_x/y are the signed elevation differences to exact adjacent
 * screen-space rays.  The footprint is centered on the physical tangent so a
 * sloped horizon crosses pixels continuously instead of snapping to a
 * one-sided row.  Non-finite inputs fail closed to zero coverage.
 */
f32 ResolveVolumetricCloudHorizonCoverage(
    f32 signed_elevation, f32 cutoff,
    f32 elevation_delta_x, f32 elevation_delta_y) noexcept;

/**
 * Per-frame camera/planet terms shared by every cloud trace/resolve pixel.
 *
 * local_up is the normalized camera vector from the rebased planet centre.
 * ground_cutoff is the signed ray-elevation tangent of the physical ground
 * sphere. Values below -1 disable the cutoff when the camera is in/above the
 * authored cloud layer or when hostile input cannot be represented safely.
 */
struct FVolumetricCloudGroundHorizon {
    FVec3 local_up{0.0f, 1.0f, 0.0f};
    f32 ground_cutoff = -2.0f;
};

/**
 * Hoist camera-invariant ground-horizon geometry out of per-pixel shaders.
 *
 * The equations deliberately mirror cloudAltitude/groundCutoff in kCloudCS.
 * Ordinary finite inputs therefore preserve the analytic two-axis horizon
 * coverage while avoiding identical divisions, normalization and square root
 * work in every trace and full-resolution resolve invocation.
 */
FVolumetricCloudGroundHorizon ResolveVolumetricCloudGroundHorizon(
    FVec3 camera_position, const FVolumetricCloudLayer& layer,
    FVec3 world_origin) noexcept;

/**
 * 雲の 1 フレーム中に変わらない密度座標の計算結果。
 *
 * @details 風による移流、基本形状の周波数、層高の逆数を CPU で一度だけ求める。
 * 密度座標は絶対ワールド座標のまま維持する。
 */
struct FVolumetricCloudDensityFrameTerms {
    /** 風で移動した雲を参照するためにワールド座標から引く XZ 距離。 */
    FVec2 wind_world{};

    /** 基本形状をワールド距離からテクスチャ座標へ変換する倍率。 */
    f32 shape_scale = 0.00012f;

    /** 雲層内の高さを 0 から 1 へ正規化する層高の逆数。 */
    f32 inverse_layer_height = 1.0f;
};

/** 層設定と風移流距離から 1 フレーム共通の密度座標項を求める。 */
FVolumetricCloudDensityFrameTerms ResolveVolumetricCloudDensityFrameTerms(
    const FVolumetricCloudLayer& layer, f32 wind_offset) noexcept;

/**
 * 追加のテクスチャ採取なしで雲形状を時間変化させる位相ずれ。
 *
 * @details 基準となる基本形状と天候領域は固定し、独立した形状・渦・侵食領域だけを
 * 相対移動させる。非有限の時刻または風速は 0 として扱う。
 */
struct FVolumetricCloudEvolutionFrameTerms {
    /** 基本形状の独立領域へ加える低周波の位相ずれ。 */
    FVec2 shape_phase{};

    /** 渦と侵食の独立領域へ加える高周波の位相ずれ。 */
    FVec2 fine_phase{};
};

/** 時刻と風速から 1 フレーム共通の雲形状の位相ずれを求める。 */
FVolumetricCloudEvolutionFrameTerms ResolveVolumetricCloudEvolutionFrameTerms(
    f32 time, f32 wind_speed) noexcept;

/**
 * Normalized sun direction and its continuous Duff/Frisvad tangent basis.
 *
 * The basis is frame-invariant and is shared by every cloud view/light probe.
 */
struct FVolumetricCloudLightBasis {
    FVec3 direction{0.0f, 1.0f, 0.0f};
    FVec3 tangent{1.0f, 0.0f, 0.0f};
    FVec3 bitangent{0.0f, 0.0f, -1.0f};
};

FVolumetricCloudLightBasis ResolveVolumetricCloudLightBasis(
    FVec3 sun_direction) noexcept;

/**
 * 遠方の太陽方向積分を置き換える、浅い光学的深さのキャッシュ。
 *
 * 現在の密度場から一つの3次元テクスチャへ生成する。近距離3点は正確な採取を残し、
 * キャッシュの信頼度が不足する場所では遠距離5点も正確な積分へ戻す。
 */
inline constexpr bool kVolumetricCloudShadowCacheEnabled = true;

/** 品質を保つ太陽方向光学的深さキャッシュの寸法。 */
inline constexpr u32 kVolumetricCloudShadowCacheWidth = 96u;
inline constexpr u32 kVolumetricCloudShadowCacheHeight = 32u;
inline constexpr u32 kVolumetricCloudShadowCacheDepth = 96u;
inline constexpr f32 kVolumetricCloudShadowCacheExtent = 48000.0f;
inline constexpr f32 kVolumetricCloudShadowCacheCellSize =
    kVolumetricCloudShadowCacheExtent /
    static_cast<f32>(kVolumetricCloudShadowCacheWidth);
inline constexpr f32 kVolumetricCloudShadowCacheSafeRadius = 8000.0f;

/** 安定フレームの自己影を各軸で2画素おきに更新し、4フレームで全体を巡回する。 */
inline constexpr u32 kVolumetricCloudShadowTemporalDivisor = 2u;
static_assert(kVolumetricCloudShadowTemporalDivisor == 2u);
static_assert(kVolumetricCloudShadowCacheWidth % kVolumetricCloudShadowTemporalDivisor == 0u);
static_assert(kVolumetricCloudShadowCacheDepth % kVolumetricCloudShadowTemporalDivisor == 0u);

/** 立体物用の雲影透過率地図を生成するか。 */
inline constexpr bool kVolumetricCloudWorldShadowEnabled = true;

/** 立体物用の雲影透過率地図の一辺の画素数。 */
inline constexpr u32 kVolumetricCloudWorldShadowMapResolution = 256u;
static_assert(kVolumetricCloudWorldShadowMapResolution % kVolumetricCloudShadowTemporalDivisor == 0u);

/** 立体物用の雲影透過率地図が覆うワールド距離。 */
inline constexpr f32 kVolumetricCloudWorldShadowMapExtent = 32768.0f;

/** 立体物用の雲影透過率地図の一画素が覆うワールド距離。 */
inline constexpr f32 kVolumetricCloudWorldShadowMapTexelSize = kVolumetricCloudWorldShadowMapExtent / static_cast<f32>(kVolumetricCloudWorldShadowMapResolution);

/** 一画素の透過率を求める太陽方向の密度採取数。 */
inline constexpr u32 kVolumetricCloudWorldShadowSamples = 32u;

/** 地平線付近の極端に長い光路を無効化する太陽方向 Y の下限。 */
inline constexpr f32 kVolumetricCloudWorldShadowMinimumSunY = 0.03f;

/** 雲の太陽方向深さキャッシュが使う、移流を除いた安定座標の範囲。 */
struct FVolumetricCloudShadowCacheMapping {
    FVec2 min_material_xz{};
    FVec2 center_material_xz{};
};

/** Convert the scalar cloud advection distance to its shared XZ direction. */
FVec2 VolumetricCloudWindOffsetXZ(f32 wind_offset) noexcept;

/** Absolute world XZ with the shared cloud advection removed. */
FVec2 VolumetricCloudMaterialXZ(FVec3 world_position,
                                f32 wind_offset) noexcept;

/** Snap a cache footprint to the material-space voxel lattice. */
FVolumetricCloudShadowCacheMapping CenterVolumetricCloudShadowCache(
    FVec2 material_position) noexcept;

/**
 * 受光点を太陽方向に沿って基準高さへ投影し、雲影地図で使う XZ を返す。
 *
 * 太陽方向が不正または地平線に近すぎる場合は、受光点の XZ をそのまま返す。
 */
FVec2 ProjectVolumetricCloudWorldShadowReferenceXZ(FVec3 world_position, FVec3 sun_direction, f32 reference_height) noexcept;

/** 地図の中心を画素格子へ固定し、左下の基準面 XZ を返す。 */
FVec2 VolumetricCloudWorldShadowMapMinimum(FVec2 center_reference_xz) noexcept;

/**
 * Compute the soft-snapped XZ world origin used by the curved cloud shell.
 *
 * The returned Y is zero so authored cloud heights retain their world-Y
 * meaning. The cell centre is stationary during ordinary editor orbits, while
 * a C1-continuous transition near cell boundaries avoids a full-cell shell
 * jump. The density field itself is never translated with the camera.
 */
FVec3 RebaseVolumetricCloudWorldOrigin(
    FVec3 camera_position,
    f32 grid_size = kVolumetricCloudOriginRebaseGrid) noexcept;

/**
 * Stable world-distance march parameters for a cloud ray.
 *
 * Uniformly splitting the layer by height makes every pixel sample the same
 * horizontal planes.  In perspective that correlation appears as rays
 * converging on the view zenith.  A world-distance step keeps the sampling
 * frequency independent of view angle; empty space may use coarse_step while
 * occupied density uses fine_step.
 */
struct FVolumetricCloudMarchPlan {
    /** 雲層へ入る視線距離。 */
    f32 enter = 0.0f;

    /** 雲層を出るか、描画を打ち切る視線距離。 */
    f32 exit = 0.0f;

    /** 密度がある区間の採取間隔。 */
    f32 fine_step = 1.0f;

    /** 空領域を探す採取間隔。 */
    f32 coarse_step = 4.0f;

    /** 区間入口の距離減衰。実積分では各標本の距離から個別に求める。 */
    f32 visibility = 0.0f;

    /** 1 本の視線で許可する最大採取数。 */
    u32 max_samples = kVolumetricCloudMaxViewMarchSamples;

    /** 打ち切り距離内に積分可能な区間があるか。 */
    bool hit = false;
};

/**
 * Intersect a world-space ray with a horizontal cloud altitude band.
 * This CPU companion mirrors the shader interval calculation and is useful for
 * camera/world-anchoring validation.
 */
FVolumetricCloudRayInterval IntersectVolumetricCloudLayer(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer) noexcept;

/**
 * Intersect the nearest continuous segment of a curved cloud shell.
 *
 * The planet centre is
 * (world_origin.x, world_origin.y-planet_radius, world_origin.z).  A discrete
 * rebased origin keeps the local tangent patch numerically stable during long
 * XZ travel without making the density field camera-relative.
 */
FVolumetricCloudRayInterval IntersectVolumetricCloudShell(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer,
    f32 planet_radius = kVolumetricCloudPlanetRadius,
    FVec3 world_origin = FVec3{}) noexcept;

/**
 * 雲シェーダーと同じ、角度によらず有界な光線積分計画を作る。
 *
 * @param ray_origin 光線の始点。
 * @param ray_direction 光線の方向。関数内で正規化する。
 * @param layer 対象の雲層。
 * @param max_distance 描画を打ち切る距離。
 * @param world_origin 曲面雲層の基準原点。
 * @param maximum_samples 密度採取回数の上限。0 は通常描画の既定値として扱う。
 * @return 雲層との交差区間、刻み幅、採取上限をまとめた計画。
 */
FVolumetricCloudMarchPlan PlanVolumetricCloudRayMarch(FVec3 ray_origin, FVec3 ray_direction, const FVolumetricCloudLayer& layer, f32 max_distance = kVolumetricCloudMaxDistance, FVec3 world_origin = FVec3{}, u32 maximum_samples = kVolumetricCloudMaxViewMarchSamples) noexcept;

/**
 * Return true when lighting changes make accumulated cloud color stale.
 *
 * Direction is expected to be normalized, matching RenderCompute's stored
 * frame signature.
 */
bool VolumetricCloudLightingChanged(
    FVec3 previous_sun_direction, FVec3 previous_sun_color,
    FVec3 previous_sky_color, FVec3 sun_direction,
    FVec3 sun_color, FVec3 sky_color) noexcept;

/**
 * 通常移動と、履歴を破棄すべき不連続なカメラ切替を見分ける。
 *
 * 行列要素ではなく画面中央と四辺の視線角を比較し、位置は実距離で判定する。
 * 遠方座標で視線精度を失わないよう、逆行列には平行移動を含まないものを渡す。
 * 非数、復元不能な行列、256メートルを超える移動は切替として扱う。
 *
 * @param previous_camera_relative_inv_view_proj 前フレームのカメラ相対逆行列。
 * @param previous_camera_position 前フレームのカメラ位置。
 * @param current_camera_relative_inv_view_proj 現在フレームのカメラ相対逆行列。
 * @param current_camera_position 現在フレームのカメラ位置。
 * @return 履歴を破棄すべき場合は true。
 */
bool VolumetricCloudViewCutDetected(const FMat4& previous_camera_relative_inv_view_proj, FVec3 previous_camera_position, const FMat4& current_camera_relative_inv_view_proj, FVec3 current_camera_position) noexcept;

/**
 * GPU レイマーチ、影キャッシュ、時間再構成を所有するボリューメトリック雲描画。
 *
 * @details
 * 入力設定は CPU で正規化してから保持する。描画時は同じ密度場を視線方向と太陽方向へ積分し、
 * 低解像度結果を雲距離と前フレームのカメラ相対位置から全解像度へ再構成する。
 */
class CVolumetricClouds {
public:
    /** CPU-compiled shader bytecode handed to the render-owner thread. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> cloud;
        TUniquePtr<IRhiShader> noise;
        TUniquePtr<IRhiShader> weather;
        TUniquePtr<IRhiShader> detail;
        TUniquePtr<IRhiShader> curl;
        TUniquePtr<IRhiShader> composite_vertex;
        TUniquePtr<IRhiShader> composite_pixel;
        TUniquePtr<IRhiShader> composite_atmosphere_pixel;
        TUniquePtr<IRhiShader> resolve;
        TUniquePtr<IRhiShader> shadow;
        TUniquePtr<IRhiShader> world_shadow;
        /** 旧二段構成とのソース互換用。現在は常に空であり、初期化には使わない。 */
        TUniquePtr<IRhiShader> shadow_finalize;

        /** Aggregate all submitted shader jobs without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** compute (雲レイマーチ) + composite (全画面 alpha blend) パイプラインを生成。 */
    TResult<void> Init(IRhiDevice& device, EFormat hdr_format) noexcept;

    /** Compile raw-DX12 HLSL without touching an RHI device. */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit all cloud shaders to a backend-managed compiler pool. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Create owner-thread resources from CPU-compiled bytecode and publish the
     * complete mandatory cloud resource set atomically.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat hdr_format) noexcept;

    /** 初期化済みか。 */
    bool Ready() const noexcept { return m_Ready; }

    /**
     * 縮小した光線積分先と、画面解像度へ戻す履歴を確保または再確保する。
     *
     * @param device 描画資源を作る装置。
     * @param scW 出力画面の幅。
     * @param scH 出力画面の高さ。
     * @param render_scale 内部描画の品質倍率。1.0 は画面寸法の1/4、4.0は等倍。
     * @param reference_mode trueなら等倍描画に固定し、時間再構成を使わない。
     * @return 必要な描画資源を利用できるならtrue。
     */
    bool EnsureSize(IRhiDevice& device, u32 scW, u32 scH,
                    f32 render_scale = 0.5f, bool reference_mode = false) noexcept;

    /** Set the fixed world-space cloud altitude band and invalidate history. */
    void SetLayer(const FVolumetricCloudLayer& layer) noexcept;

    /**
     * 正解画像を作るための «参照» 描画に切り替える。
     *
     * @details
     * 通常は 1/4 の寸法でレイマーチし、時間方向の再構成で埋めている。そのため «汚い» ときに
     * 原因がライティングなのか再構成なのか分からない。参照描画では**等倍でレイマーチし、
     * 時間方向の再構成を切り、刻みを細かくする**。
     *
     * - 参照でも汚い → 密度かライティングか大気の側
     * - 参照だけ綺麗 → 低解像度か再構成か履歴の側
     *
     * **遊ぶには重すぎる。** 見比べるためだけのもの。
     * @param enabled 参照描画にするなら true。
     */
    void SetReferenceMode(bool enabled) noexcept;

    /**
     * 参照描画かどうかを返す。
     *
     * @return 参照描画なら true。
     */
    bool ReferenceMode() const noexcept { return m_ReferenceMode; }

    /**
     * 照らし方の係数を設定する。
     *
     * @param lighting 新しい係数。次のフレームから効く。
     */
    void SetLighting(const FVolumetricCloudLighting& lighting) noexcept;

    /**
     * 照らし方の係数を返す。
     *
     * @return 現在の係数。
     */
    const FVolumetricCloudLighting& Lighting() const noexcept { return m_Lighting; }

    /**
     * どこまで雲を描くかを設定する。
     *
     * @param range 新しい設定。次のフレームから効く。
     */
    void SetRange(const FVolumetricCloudRange& range) noexcept;

    /**
     * どこまで雲を描くかを返す。
     *
     * @return 現在の設定。
     */
    const FVolumetricCloudRange& Range() const noexcept { return m_Range; }

    /**
     * 上に重ねる高い雲を設定する。
     *
     * @param layer 新しい設定。`top_height <= base_height` で無効。
     */
    void SetUpperLayer(const FVolumetricCloudUpperLayer& layer) noexcept;

    /**
     * 上に重ねる高い雲の設定を返す。
     *
     * @return 現在の設定。
     */
    const FVolumetricCloudUpperLayer& UpperLayer() const noexcept {
        return m_UpperLayer;
    }

    /** Current sanitized world-space cloud altitude band. */
    const FVolumetricCloudLayer& Layer() const noexcept { return m_Layer; }

    /**
     * Keep allocated cloud targets but reject the previous view's temporal
     * reconstruction history on the next render.
     */
    void InvalidateHistory() noexcept
    {
        InvalidateCloudHistory_Internal(false);
    }

    /** 太陽方向深さキャッシュを生成したフレーム数。 */
    u64 ShadowCacheDispatchCount() const noexcept {
        return m_ShadowCacheDispatchCount;
    }

    /** 任意機能である影キャッシュの描画資源を利用できるか。 */
    bool ShadowCacheAvailable() const noexcept {
        return m_ShadowCacheAvailable;
    }

    /** 現在または直近3フレーム以内の密度場から生成した影キャッシュを利用できるか。 */
    bool ShadowCacheValid() const noexcept { return m_ShadowCacheValid; }

    /** 現在フレームの立体物用雲影透過率地図と座標情報を返す。 */
    FVolumetricCloudWorldShadowMap WorldShadowMap() const noexcept;

    /** 立体物用雲影透過率地図を生成したフレーム数。 */
    u64 WorldShadowDispatchCount() const noexcept {
        return m_WorldShadowDispatchCount;
    }

    /** 立体物用雲影の描画資源を利用できるか。 */
    bool WorldShadowAvailable() const noexcept {
        return m_WorldShadowAvailable;
    }

    /** 現在または直近3フレーム以内の立体物用雲影を利用できるか。 */
    bool WorldShadowValid() const noexcept {
        return m_WorldShadowValid;
    }

    /** Exact submitted-work accounting for the latest compute/composite frame. */
    const FVolumetricCloudFrameWorkload& LastFrameWorkload() const noexcept {
        return m_LastFrameWorkload;
    }

    /** Full-resolution resolved cloud distance/confidence for later fog passes. */
    IRhiTexture* ResolvedDepth() const noexcept {
        return m_HistoryValid ? m_HistoryDepth[m_ResolvedIndex].Get() : nullptr;
    }

    /**
     * 雲を計算シェーダーで追跡し、内部テクスチャへ書く既存互換入口。
     * 描画処理の外側で呼ぶ。遠方座標では、より高精度な
     * RenderComputeCameraRelative() を使う。
     */
    void RenderCompute(IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos,
                       FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color,
                       f32 coverage, f32 density, f32 wind, f32 time) noexcept;

    /**
     * カメラ相対逆行列で視線精度を保ちながら雲を描く。
     *
     * @param camera_relative_inv_view_proj 平行移動を含めず BuildCameraRelativeInverseViewProjection() で作った逆行列。
     * 行列またはカメラ位置が非数なら雲を描かず、時間履歴を破棄する。
     */
    void RenderComputeCameraRelative(IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color, f32 coverage, f32 density, f32 wind, f32 time) noexcept;

    /**
     * 雲 (straight 散乱色+alpha) を現在の RT へ alpha blend する。
     * 解決済み cloud ray distance と scene_depth から復元した scene ray distance を比較し、
     * 手前にある方だけを表示する。カメラが雲層内/上空にいる場合も前景雲を失わない。
     * atmosphere_volume と atmosphere_transmittance がある場合は、可視 cloud の
     * 代表距離までの RGB transmittance と premultiplied atmospheric in-scatter を
     * 適用してから背景へ alpha blend する。
     * local fog は ResolvedDepth() を使う後段 pass で同じ cloud 距離へ終端する。
     * depth は SRV として読むので、この render pass の DSV には同時 bind しないこと。
     */
    void Composite(IRhiCommandList& cl, IRhiTexture& scene_depth,
                   u32 scW, u32 scH,
                   IRhiTexture* atmosphere_volume = nullptr,
                   IRhiTexture* atmosphere_transmittance = nullptr,
                   f32 atmosphere_max_distance = 1.0f) noexcept;

    /** 全 GPU リソースを解放 (acs_editor_destroy から呼ぶ。UAF 防止)。 */
    void Shutdown() noexcept;

private:
    /** 時間履歴を破棄し、密度場も変わる場合は影キャッシュも破棄する。 */
    void InvalidateCloudHistory_Internal(bool density_field_changed) noexcept;

    TResult<void> InitCandidateWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat hdr_format) noexcept;

    /** 参照描画 (等倍・再構成なし) か。 */
    bool m_ReferenceMode = false;

    /** 照らし方の係数。 */
    FVolumetricCloudLighting m_Lighting{};

    /** どこまで描くか。 */
    FVolumetricCloudRange    m_Range{};

    /** 上に重ねる高い雲。 */
    FVolumetricCloudUpperLayer m_UpperLayer{};

    bool                     m_Ready = false;
    /** 形状ノイズを生成済みか。 */
    bool                     m_NoiseBaked = false;
    EFormat                  m_HdrFormat = EFormat::R16G16B16A16_Float;
    /** Perlin-Worley ノイズを生成する計算シェーダー。 */
    TUniquePtr<IRhiShader>   m_NoiseCs;
    TUniquePtr<IRhiPipeline> m_NoisePipe;                // compute (noise gen)
    TUniquePtr<IRhiTexture>  m_ShapeTex;                 // 128^3 RG16F Perlin-Worley/low-frequency Worley
    bool                     m_WeatherBaked = false;
    TUniquePtr<IRhiShader>   m_WeatherCs;
    TUniquePtr<IRhiPipeline> m_WeatherPipe;
    TUniquePtr<IRhiTexture>  m_WeatherTex;               // 512^2 coverage/type/precipitation/warp
    bool                     m_DetailBaked = false;
    TUniquePtr<IRhiShader>   m_DetailCs;
    TUniquePtr<IRhiPipeline> m_DetailPipe;
    TUniquePtr<IRhiTexture>  m_DetailTex;                // 64^3 RG16F independent Worley edge erosion
    bool                     m_CurlBaked = false;
    TUniquePtr<IRhiShader>   m_CurlCs;
    TUniquePtr<IRhiPipeline> m_CurlPipe;
    TUniquePtr<IRhiTexture>  m_CurlTex;                  // 128^2 independent world-space curl warp
    TUniquePtr<IRhiShader>   m_CloudCs;
    TUniquePtr<IRhiPipeline> m_CloudPipe;     // compute
    /** 浅い太陽方向光学的深さを生成するシェーダー。 */
    TUniquePtr<IRhiShader>   m_ShadowCs;
    TUniquePtr<IRhiPipeline> m_ShadowPipe;
    /** 96x32x96の平均深さと二標本差。 */
    TUniquePtr<IRhiTexture>  m_ShadowTex;
    /** 立体物の直接光へ掛ける256角の雲透過率地図。 */
    TUniquePtr<IRhiShader>   m_WorldShadowCs;
    TUniquePtr<IRhiPipeline> m_WorldShadowPipe;
    TUniquePtr<IRhiTexture>  m_WorldShadowTex;
    TUniquePtr<IRhiShader>   m_CompVs, m_CompPs;
    TUniquePtr<IRhiPipeline> m_CompPipe;      // graphics (alpha blend)
    TUniquePtr<IRhiShader>   m_CompAtmosPs;
    TUniquePtr<IRhiPipeline> m_CompAtmosPipe; // cloud-distance terminated physical AP (RGB L/T)
    TUniquePtr<IRhiBuffer>   m_CompAtmosCb;
    TUniquePtr<IRhiShader>   m_ResolveCs;      // 深度対応の color/depth 空間・時間再構成
    TUniquePtr<IRhiPipeline> m_ResolvePipe;    // 全解像度 color/depth compute UAV 解決
    TUniquePtr<IRhiBuffer>   m_Cb;
    TUniquePtr<IRhiTexture>  m_CloudTex;       // scaled ray-march RGBA16F の非乗算カラー + アルファ
    TUniquePtr<IRhiTexture>  m_CloudDepth;     // scaled ray-march RG32F (代表距離、信頼度)
    TUniquePtr<IRhiTexture>  m_HistoryColor[2];// 全解像度 RGBA16F の時間履歴 ping-pong
    TUniquePtr<IRhiTexture>  m_HistoryDepth[2];// 全解像度 RG32F の雲距離・信頼度 ping-pong
    FMat4                    m_PrevCameraRelativeViewProj = FMat4::Identity();
    FMat4                    m_PrevCameraRelativeInvViewProj = FMat4::Identity();
    FVec3                    m_PrevCamPos{};
    FVec3                    m_WorldOrigin{};
    FVec3                    m_PrevSunDir{};
    FVec3                    m_PrevSunColor{};
    FVec3                    m_PrevSkyColor{};
    f32                      m_PrevWindOffset = 0.0f;
    f32                      m_PrevWindSpeed = 0.0f;
    f32                      m_PrevCoverage = -1.0f;
    f32                      m_PrevDensity = -1.0f;
    f32                      m_PrevTime = -1.0f;
    FVec2                    m_ShadowGridMinQ{};
    FVec2                    m_ShadowGridCenterQ{};
    FVec2                    m_WorldShadowMapMinReferenceXz{};
    f32                      m_WorldShadowReferenceHeight = 0.0f;
    FVec3                    m_WorldShadowSunDirection{0.0f, 1.0f, 0.0f};
    FVec3                    m_WorldShadowWorldOrigin{};
    f32                      m_WorldShadowCloudBaseAltitude = 0.0f;
    u64                      m_ShadowCacheDispatchCount = 0;
    u64                      m_WorldShadowDispatchCount = 0;
    bool                     m_ShadowGridInitialized = false;
    bool                     m_ShadowCacheAvailable = false;
    bool                     m_ShadowCacheValid = false;
    bool                     m_WorldShadowAvailable = false;
    bool                     m_WorldShadowValid = false;
    FVolumetricCloudLayer    m_Layer{};
    u32                      m_FrameIndex = 0;
    u32                      m_TemporalPhase = 0;
    u32                      m_ResolvedIndex = 0;
    bool                     m_HistoryValid = false;
    u32                      m_W = 0, m_H = 0;         // scaled ray-march の寸法
    u32                      m_FullW = 0, m_FullH = 0; // 再構成先の寸法
    u64                      m_WorkloadSubmissionIndex = 0u;
    FVolumetricCloudFrameWorkload m_LastFrameWorkload{};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FVolumetricClouds = CVolumetricClouds;


} // namespace acs
