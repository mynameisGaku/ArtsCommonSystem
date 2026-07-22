// SPDX-License-Identifier: Apache-2.0
// Effects2D — シェーダー無し・アセット無しの drop-in エフェクトコンポーネント集。
//
// すべて FSpriteBatch のプリミティブ (三角形/矩形) を時間で手続き生成して描く。
// HLSL を書けなくても AddComponent + パラメータ設定だけで動く。インタラクティブな
// API (Disturb / SetIntensity / owner 追従) を持ち、入力や物理と連動できる。
//
//   auto& w = node->AddComponent<AWater2DComponent>();
//   w.SetArea({0,3}, {16,2}); w.SetColor({0.1f,0.35f,0.6f}); w.SetWaves(0.15f,1.5f);  // +Y=画面下: 水面は下に置く
//   w.Disturb(mouse_world_x, 0.4f);     // ← 触れると波紋
//
//   auto& f = node->AddComponent<AFire2DComponent>();
//   f.SetSize(0.8f,1.8f); f.SetIntensity(1.0f);   // ← 近づくと勢い UP 等
//
//   auto& t = node->AddComponent<ATrail2DComponent>();  // owner を追従して残像
//   t.SetColor({0.3f,0.8f,1.0f}); t.SetWidth(0.3f);
#pragma once

#include "gameframework/AComponent.h"
#include "math/Vec.h"

namespace acs { class FSpriteBatch; }   // 加算パス描画ヘルパで使う (前方宣言)

namespace acs::game {

/**
 * 水の見せ方 (視点スタイル)。
 *
 * @details
 * SideView は横視点 (海・プール: 上端が水面で波打ち、平面反射あり)。TopDown は見下ろし
 * (縁が浅く中心が深い radial 深度 + コースティクス + 全周の岸泡、平面反射は無し)。
 */
enum class EWaterStyle : u8 {
    /** 横視点 (上端が水面で波打ち、平面反射あり)。 */
    SideView,

    /** 見下ろし (radial 深度 + コースティクス + 全周岸泡、平面反射なし)。 */
    TopDown
};

/**
 * 波打つ水面 + 干渉 (ripple / splash) + コースティクスを手続き生成する水コンポーネント。
 *
 * @details
 * shape 指定 (矩形 / 楕円 / 川 / 多角形 / スプライン) で実三角形メッシュを焼く。TopDown では
 * 専用 GPU 水面効果が深さ・波・泡・コースティクス・きらめき・リップルをピクセル単位で評価し、
 * SideView では上端を波で揺らし平面反射も行う。
 */
class AWater2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AWater2DComponent)

    /** 内部メッシュの頂点上限 (実三角形メッシュで深さ・反射 UV を滑らかに補間するため内部頂点を持つ)。 */
    static constexpr u32 kMaxVerts = 320;

    /** 内部メッシュの三角形上限。 */
    static constexpr u32 kMaxTris  = 560;

    /**
     * 矩形 (海・プール) の水域を設定する (owner 相対)。
     *
     * @details 上辺を top_segments 分割して滑らかに波打たせる。
     * @param center 矩形中心 (owner 相対)。
     * @param half_size 矩形の半サイズ。
     * @param top_segments 上辺の分割数。
     */
    void SetRect(FVec2 center, FVec2 half_size, u32 top_segments = 32) noexcept;

    /**
     * 矩形水域を設定する旧 API 互換 (= SetRect)。
     *
     * @param center 矩形中心 (owner 相対)。
     * @param half_size 矩形の半サイズ。
     */
    void SetArea(FVec2 center, FVec2 half_size) noexcept { SetRect(center, half_size); }

    /**
     * 楕円 (水溜まり・池) の水域を設定する (owner 相対)。
     *
     * @param center 楕円中心 (owner 相対)。
     * @param rx X 半径。
     * @param ry Y 半径。
     * @param segments 外周の分割数。
     */
    void SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments = 28) noexcept;

    /**
     * 川の水域を設定する (polyline を太さ width の帯に膨らませる)。
     *
     * @param path 川の経路点 (owner 相対)。
     * @param count path の点数。
     * @param width 川幅。
     */
    void SetRiver(const FVec2* path, u32 count, f32 width) noexcept;

    /**
     * 任意の単純多角形の水域を設定する (owner 相対)。
     *
     * @details centroid から扇状に塗るため凸〜緩い凹向け (強い凹形状は複数の水域に分けるのが安全)。
     * @param pts 多角形の頂点列 (owner 相対)。
     * @param count 頂点数。
     */
    void SetPolygon(const FVec2* pts, u32 count) noexcept;

    /**
     * Catmull-Rom スプライン (制御点を通る滑らかな曲線) で川を設定する。
     *
     * @details 少ない制御点で曲がりくねった川を作れる。内部で密にサンプルして SetRiver に渡す。
     * @param control 制御点列 (owner 相対)。
     * @param count 制御点数。
     * @param width 川幅。
     * @param samples_per_seg 1 区間あたりのサンプル数 (曲線の細かさ)。
     */
    void SetSplineRiver(const FVec2* control, u32 count, f32 width, u32 samples_per_seg = 8) noexcept;

    /**
     * Catmull-Rom スプラインの閉ループで滑らかな輪郭の水域を設定する。
     *
     * @param control 制御点列 (owner 相対、閉ループ)。
     * @param count 制御点数。
     * @param samples_per_seg 1 区間あたりのサンプル数 (曲線の細かさ)。
     */
    void SetSplineRegion(const FVec2* control, u32 count, u32 samples_per_seg = 8) noexcept;

    /**
     * 基本色を設定する (後方互換: 深さ勾配の shallow/deep を自動設定する)。
     *
     * @details deep は rgb * 0.45 に設定される。
     * @param rgb 水の基本色 RGB。
     */
    void SetColor(FVec3 rgb) noexcept {
        m_Color = rgb; m_ShallowColor = rgb;
        m_DeepColor = FVec3{ rgb.x * 0.45f, rgb.y * 0.45f, rgb.z * 0.45f };
    }

    /**
     * 不透明度を設定する (shallow/deep のアルファも同値に揃える)。
     *
     * @param a 不透明度 (0..1)。
     */
    void SetAlpha(f32 a) noexcept { m_Alpha = a; m_ShallowAlpha = a; m_DeepAlpha = a; }

    /**
     * 波の振幅と速度を設定する (流速も同じ speed に揃える)。
     *
     * @param amplitude 波の振幅。
     * @param speed 波 (と流れ) の速度。
     */
    void SetWaves(f32 amplitude, f32 speed) noexcept { m_Amp = amplitude; m_Speed = speed; m_FlowSpeed = speed; }

    /**
     * 波と泡が流れる方向と速さを設定する。
     *
     * @details dir は内部で正規化する (既定は +X 方向)。
     * @param dir 流れる向き (正規化前で可)。
     * @param speed 流れの速さ。
     */
    void SetFlow(FVec2 dir, f32 speed) noexcept;

    /**
     * 深さ勾配の浅部・深部の色を設定する。
     *
     * @details 水面 (shallow) → 深部 (deep) を頂点ごとに補間して DrawTriangleVC で描く。
     * @param shallow 浅部 (水面側) の色。
     * @param deep 深部の色。
     */
    void SetDepthColors(FVec3 shallow, FVec3 deep) noexcept { m_ShallowColor = shallow; m_DeepColor = deep; }

    /**
     * 深さ勾配の浅部・深部のアルファを設定する。
     *
     * @param shallow_alpha 浅部の不透明度。
     * @param deep_alpha 深部の不透明度。
     */
    void SetDepthAlpha(f32 shallow_alpha, f32 deep_alpha) noexcept { m_ShallowAlpha = shallow_alpha; m_DeepAlpha = deep_alpha; }

    /**
     * 陸際の白泡を設定する。
     *
     * @param color 泡の色。
     * @param width 泡の幅 (0 で bbox から自動)。
     * @param speed 泡アニメの速度。
     * @param alpha 泡の濃さ。
     */
    void SetFoam(FVec3 color, f32 width = 0.0f, f32 speed = 3.0f, f32 alpha = 0.9f) noexcept {
        m_FoamColor = color; m_FoamWidth = width; m_FoamSpeed = speed; m_FoamAlpha = alpha;
    }

    /**
     * 陸際の白泡の描画を有効/無効にする。
     *
     * @param on true で泡を描く。
     */
    void EnableFoam(bool on) noexcept { m_FoamEnabled = on; }

    /**
     * 水際を引き締める縁取り (内側へフェードする細い帯) を設定する。
     *
     * @param color 縁取りの色。
     * @param width 縁取りの幅 (0 で自動)。
     * @param alpha 縁取りの濃さ。
     */
    void SetRim(FVec3 color, f32 width = 0.0f, f32 alpha = 0.5f) noexcept {
        m_RimColor = color; m_RimWidth = width; m_RimAlpha = alpha;
    }

    /**
     * 縁取りの描画を有効/無効にする。
     *
     * @param on true で縁取りを描く。
     */
    void EnableRim(bool on) noexcept { m_RimEnabled = on; }

    /**
     * 水面より上のシーンを映す平面反射を設定する。
     *
     * @details FScene2D::SetReflectionEnabled(true) が前提で、RT が rc に配線されていなければ無視される。
     * @param enable true で反射を有効化。
     * @param tint 反射の色味。
     * @param alpha 反射の強さ。
     */
    void SetReflection(bool enable, FVec3 tint = FVec3{1.0f, 1.0f, 1.0f}, f32 alpha = 0.55f) noexcept {
        m_ReflectEnabled = enable; m_ReflectTint = tint; m_ReflectAlpha = alpha;
    }

    /**
     * 反射 UV を波で歪ませる量の倍率を設定する。
     *
     * @param amplitude_mult 波振幅に対する歪み倍率。
     */
    void SetReflectionDistortion(f32 amplitude_mult) noexcept { m_ReflectDistort = amplitude_mult; }

    /**
     * 反射を上部 (水面側) のみに効かせ深部をフェード/カリングする度合いを設定する。
     *
     * @details fade=1 で上 ~45% に集中、0 で全面 (従来)。負値は 0 に clamp。残スメア隠し + 軽量化に使う。
     * @param depth_fade 上部集中の度合い (0..1)。
     */
    void SetReflectionFade(f32 depth_fade) noexcept { m_ReflectFade = depth_fade < 0.0f ? 0.0f : depth_fade; }

    /**
     * 視点スタイル (横視点 / 見下ろし) を設定する。
     *
     * @param s 適用する EWaterStyle。
     */
    void SetStyle(EWaterStyle s) noexcept { m_Style = s; }

    /**
     * 現在の視点スタイルを返す。
     *
     * @return 設定済みの EWaterStyle。
     */
    EWaterStyle Style() const noexcept { return m_Style; }

    /**
     * コースティクス (光の網目) の描画を有効/無効にする。
     *
     * @param on true でコースティクスを描く。
     */
    void EnableCaustics(bool on) noexcept { m_CausticsOn = on; }

    /**
     * コースティクスのパラメータを設定する (同時に有効化)。
     *
     * @param tint 網目の色味。
     * @param intensity 強度。
     * @param scale 網目のスケール。
     * @param speed 蠢く速度。
     */
    void SetCaustics(FVec3 tint, f32 intensity = 0.55f, f32 scale = 1.4f, f32 speed = 0.7f) noexcept {
        m_CausticsOn = true; m_CausticsTint = tint;
        m_CausticsIntensity = intensity; m_CausticsScale = scale; m_CausticsSpeed = speed;
    }

    /**
     * 波頭・リップル front の散発的なきらめき (加算、両スタイル) を有効/無効にする。
     *
     * @param on true できらめきを描く。
     */
    void EnableGlints(bool on) noexcept { m_GlintsOn = on; }

    /**
     * 見下ろしで平面反射の代わりに薄く乗せる空色を設定する。
     *
     * @param rgb 空色 RGB。
     * @param alpha 空色の濃さ (0 で無効)。
     */
    void SetSkyTint(FVec3 rgb, f32 alpha) noexcept { m_SkyTint = rgb; m_SkyAlpha = alpha; }

    /**
     * 見下ろし水面のマイクロ法線と光学特性を設定する。
     *
     * @param roughness GGX 鏡面反射の粗さ (0.04..1)。
     * @param normal_strength マルチスケール法線マップの強さ (0 で無効)。
     * @param normal_tiling 法線マップの world 座標タイリング密度。
     * @param refraction_strength 屈折による水底コースティクスの変位量。
     */
    void SetSurfaceOptics(f32 roughness, f32 normal_strength = 0.9f,
                          f32 normal_tiling = 0.1f,
                          f32 refraction_strength = 0.24f) noexcept {
        m_SurfaceRoughness = roughness < 0.04f ? 0.04f : (roughness > 1.0f ? 1.0f : roughness);
        m_NormalStrength = normal_strength < 0.0f ? 0.0f : normal_strength;
        m_NormalTiling = normal_tiling > 0.001f ? normal_tiling : 0.001f;
        m_RefractionStrength = refraction_strength < 0.0f ? 0.0f : refraction_strength;
    }

    /**
     * Beer-Lambert 吸収係数を RGB ごとに設定する (通常は R > G > B)。
     */
    void SetAbsorption(FVec3 coefficient) noexcept {
        m_Absorption = FVec3{
            coefficient.x < 0.0f ? 0.0f : coefficient.x,
            coefficient.y < 0.0f ? 0.0f : coefficient.y,
            coefficient.z < 0.0f ? 0.0f : coefficient.z,
        };
    }

    /**
     * メッシュ密度を手動指定する (0 で自動)。
     *
     * @details 意味は shape 依存 (rect: cols,rows / ellipse・polygon: segments,rings / river: spans,_)。
     * @param a 第 1 軸の密度 (0 で自動)。
     * @param b 第 2 軸の密度 (0 で自動)。
     */
    void SetMeshResolution(u32 a, u32 b) noexcept { m_ResA = a; m_ResB = b; }

    /**
     * 互換 API: 触れた点から 2D の放射状リップル (輪) を立てる。
     *
     * @param world_point リップルの中心 (world 座標)。
     * @param strength リップルの初期振幅。
     */
    void AddRipple(FVec2 world_point, f32 strength) noexcept {
        AddDisturbance(world_point, 0.0f, strength);
    }

    /** 互換 API: 水面上の world x にリップルを立てる。 */
    void AddRipple(f32 world_x, f32 strength) noexcept {
        AddDisturbance(FVec2{world_x, SurfaceY()}, 0.0f, strength);
    }

    /**
     * 接触半径を持つ衝撃を水面へ追加する。
     *
     * @details radius は最初の波面半径として使われ、法線・接触 foam・GGX highlight の
     * 全てに同じ波面が応答する。ゲーム内オブジェクトの接触判定から直接呼べる。
     */
    void AddDisturbance(FVec2 world_point, f32 radius, f32 strength) noexcept;

    /**
     * 移動物体の位置・速度から方向性のある wake を追加する。
     *
     * @details 速度方向の後方と左右へ振幅を変えた複数波面を配置し、円形 ripple の
     * 重ね合わせで V 字 wake を作る。velocity は world units/sec。
     */
    void AddWake(FVec2 world_point, FVec2 velocity, f32 radius, f32 strength) noexcept;

    /** 従来名。AddRipple と同じ動作を保つ。 */
    void Disturb(FVec2 world_point, f32 strength) noexcept;

    /**
     * 水面 (SurfaceY) 上の点にリップルを立てる後方互換版。
     *
     * @param world_x リップル中心の world x (y は水面に固定)。
     * @param strength リップルの初期振幅。
     */
    void Disturb(f32 world_x, f32 strength) noexcept;

    /** 直近の描画で高品質 GPU 水面 shader が有効だったかを返す。 */
    bool IsGpuSurfaceReady() const noexcept { return m_LastGpuSurfaceReady; }

    /** 寿命内にあり、現在重ね合わせへ参加している波紋数を返す。 */
    u32 ActiveRippleCount() const noexcept;

    /**
     * リップルの伝播パラメータを設定する。
     *
     * @details wavelength は下限 1e-3 を下回ると 1.0 にフォールバックする。
     * @param speed 輪が広がる速さ。
     * @param wavelength リップルの波長。
     */
    void SetRipplePropagation(f32 speed, f32 wavelength) noexcept {
        m_RippleSpeed = speed; m_RippleWavelength = wavelength > 1e-3f ? wavelength : 1.0f;
    }

    /**
     * 点が実際の水面三角形内にあるかを判定する。
     *
     * @param world 判定する点 (world 座標)。
     * @return 水面メッシュ内 (境界上を含む) なら true。
     */
    bool ContainsPoint(FVec2 world) const noexcept;

    /**
     * world x が水域 bbox の x 範囲内かを判定する。
     *
     * @param world_x 判定する world x。
     * @return bbox の x 範囲内なら true。
     */
    bool ContainsX(f32 world_x) const noexcept;

    /**
     * 水域 bbox 上端の world y (水面の高さ) を返す。
     *
     * @return 水面の world y。
     */
    f32  SurfaceY() const noexcept;

    /**
     * 毎フレーム時刻とリップルの寿命を進める。
     *
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /**
     * 水面メッシュ・反射・泡・縁取り・加算パスを描画する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(FRenderContext& rc) noexcept override;

private:
    /**
     * 頂点を 1 つ追加する (上限まで)。
     *
     * @param v 追加する頂点 (owner 相対)。
     * @param edge_dist 縁距離 (-1 で builder 未設定、Finish で境界距離 fallback)。
     */
    void PushVert(FVec2 v, f32 edge_dist = -1.0f) noexcept;

    /**
     * 三角形を 1 つ追加する (上限・index 範囲チェックあり)。
     *
     * @param a 頂点 index その 1。
     * @param b 頂点 index その 2。
     * @param c 頂点 index その 3。
     */
    void PushTri(u32 a, u32 b, u32 c) noexcept;

    /** メッシュ構築後に weight / edge-dist / bbox / 境界を再計算する。 */
    void Finish() noexcept;

    /** edge-incidence で外周ループを抽出し、各境界頂点の外向き法線を求める。 */
    void BuildBoundary() noexcept;

    /** builder が edge-dist を埋めていない頂点を境界距離で補完する。 */
    void ComputeEdgeDistFallback() noexcept;

    /**
     * ambient 波 + 2D 放射状リップルによる垂直変位を返す。
     *
     * @param world 評価する点 (world 座標)。
     * @return その点の波変位量。
     */
    f32  WaveRadialAt(FVec2 world) const noexcept;

    /**
     * リップルのみ (ambient 波を除く) の変位を返す (輪 glow 用)。
     *
     * @param world 評価する点 (world 座標)。
     * @return リップルによる変位量。
     */
    f32  RippleAt(FVec2 world) const noexcept;

    /**
     * 有効なリップルが 1 つでもあるかを返す。
     *
     * @return active なリップルがあれば true。
     */
    bool AnyRippleActive() const noexcept;

    /**
     * 光の網目 (コースティクス) のセル値を返す。
     *
     * @param world 評価する点 (world 座標)。
     * @return 網目の明るさ [0,1]。
     */
    f32  CausticAt(FVec2 world) const noexcept;

    /**
     * 深さ勾配に応じた頂点カラーを返す (style 依存)。
     *
     * @param vert_index 対象頂点の index。
     * @return その頂点の RGBA カラー。
     */
    FVec4 DepthColorAt(u32 vert_index) const noexcept;

    /**
     * 空色・コースティクス・リップル輪を加算ブレンドで描く。
     *
     * @param sb 描画先のスプライトバッチ。
     * @param disp 波で変位済みの頂点 world 位置配列。
     */
    void DrawCaustics(FSpriteBatch& sb, const FVec2* disp) noexcept;

    /**
     * 散発的なきらめきを加算ブレンドで描く。
     *
     * @param sb 描画先のスプライトバッチ。
     * @param disp 波で変位済みの頂点 world 位置配列。
     */
    void DrawGlints(FSpriteBatch& sb, const FVec2* disp) noexcept;

    /** 放射状リップル 1 つの状態 (中心・振幅・経過時間・伝播速度・有効フラグ)。 */
    struct FRipple {
        FVec2 center{0,0};
        f32 amp0 = 0;
        f32 amp = 0;
        f32 time = 0;
        f32 speed = 0;
        f32 initial_radius = 0;
        bool active = false;
    };

    /**
     * 衝撃波紋用の予約枠。
     *
     * @details click / splash / rain は wake と別枠にし、カーソル移動が既存の
     * 衝撃波紋を寿命前に追い出さないようにする。
     */
    static constexpr u32 kMaxImpactRipples = 16;

    /** 連続する移動 wake 用の予約枠。 */
    static constexpr u32 kMaxWakeRipples = 32;

    /** GPU へ同時転送する全波紋数。 */
    static constexpr u32 kMaxRipples = kMaxImpactRipples + kMaxWakeRipples;

    /**
     * 指定した予約範囲の空き枠へ波紋を追加する。
     *
     * @details active な枠は上書きしない。満杯時は新規波紋を捨て、既存波紋を
     * 必ず寿命まで残す。
     */
    bool TryAddDisturbance(FVec2 world_point, f32 radius, f32 strength,
                           u32 first_slot, u32 slot_count) noexcept;

    /** アクティブなリップル群。先頭が衝撃用、後半が wake 用の予約領域。 */
    FRipple m_Ripples[kMaxRipples];

    /** 直近の TopDown 描画で専用 shader を bind できたか。HUD/診断用。 */
    bool m_LastGpuSurfaceReady = false;

    /** owner 相対の頂点座標。 */
    FVec2 m_Vert[kMaxVerts];

    /** 各頂点の水面らしさ [0,1] (bbox 上端ほど 1、SideView の揺れに使う)。 */
    f32   m_Weight[kMaxVerts];

    /** 各頂点の縁距離 (縁 0 .. 最深 1。TopDown 深度と泡/コースティクスの falloff に使う)。 */
    f32   m_EdgeDist[kMaxVerts];

    /** 有効な頂点数。 */
    u32   m_VCount = 0;

    /** 三角形の頂点 index (m_Vert への index、3 個 1 組)。 */
    u16   m_Tri[kMaxTris * 3];

    /** 有効な三角形数。 */
    u32   m_TCount = 0;

    /** 外周ループ (m_Vert への順序付き index)。 */
    u16   m_Boundary[kMaxVerts + 1];

    /** 外周ループの頂点数。 */
    u32   m_BoundaryCount = 0;

    /** 各境界頂点の外向き単位法線 (owner 相対)。 */
    FVec2 m_BoundaryNormal[kMaxVerts];

    /** メッシュ bbox の最小 X。 */
    f32   m_MinX = 0;

    /** メッシュ bbox の最大 X。 */
    f32   m_MaxX = 0;

    /** メッシュ bbox の最小 Y。 */
    f32   m_MinY = 0;

    /** メッシュ bbox の最大 Y。 */
    f32   m_MaxY = 0;

    /** 深さ勾配の浅部 (水面側) の色。 */
    FVec3 m_ShallowColor{0.20f, 0.55f, 0.78f};

    /** 深さ勾配の深部の色。 */
    FVec3 m_DeepColor{0.04f, 0.16f, 0.32f};

    /** 深さ勾配の浅部の不透明度。 */
    f32   m_ShallowAlpha = 0.62f;

    /** 深さ勾配の深部の不透明度。 */
    f32   m_DeepAlpha    = 0.92f;

    /** 陸際の白泡の色。 */
    FVec3 m_FoamColor{0.95f, 0.98f, 1.0f};

    /** 白泡の幅 (0 で bbox から自動)。 */
    f32   m_FoamWidth = 0.0f;

    /** 白泡アニメの速度。 */
    f32   m_FoamSpeed = 3.0f;

    /** 白泡の濃さ。 */
    f32   m_FoamAlpha = 0.9f;

    /** 白泡を描くか。 */
    bool  m_FoamEnabled = true;

    /** 縁取りの色。 */
    FVec3 m_RimColor{0.55f, 0.80f, 0.95f};

    /** 縁取りの幅 (0 で自動)。 */
    f32   m_RimWidth = 0.0f;

    /** 縁取りの濃さ。 */
    f32   m_RimAlpha = 0.5f;

    /** 縁取りを描くか。 */
    bool  m_RimEnabled = true;

    /** 平面反射を行うか。 */
    bool  m_ReflectEnabled = false;

    /** 反射の色味。 */
    FVec3 m_ReflectTint{1.0f, 1.0f, 1.0f};

    /** 反射の強さ。 */
    f32   m_ReflectAlpha = 0.55f;

    /** 反射 UV を波で歪ませる倍率。 */
    f32   m_ReflectDistort = 1.0f;

    /** 反射の上部集中度合い (1=上部のみ、0=全面)。 */
    f32   m_ReflectFade = 1.0f;

    /** 視点スタイル。 */
    EWaterStyle m_Style = EWaterStyle::SideView;

    /** コースティクスを描くか。 */
    bool  m_CausticsOn = false;

    /** コースティクスの色味。 */
    FVec3 m_CausticsTint{0.85f, 0.95f, 1.0f};

    /** コースティクスの強度。 */
    f32   m_CausticsIntensity = 0.55f;

    /** コースティクスのスケール。 */
    f32   m_CausticsScale = 1.4f;

    /** コースティクスの蠢く速度。 */
    f32   m_CausticsSpeed = 0.7f;

    /** きらめきを描くか。 */
    bool  m_GlintsOn = false;

    /** 見下ろし用の空色。 */
    FVec3 m_SkyTint{0.55f, 0.72f, 0.95f};

    /** 空色の濃さ (0 で無効)。 */
    f32   m_SkyAlpha = 0.0f;

    /** GGX 鏡面反射の粗さ。 */
    f32   m_SurfaceRoughness = 0.16f;

    /** マルチスケール法線マップの強さ。 */
    f32   m_NormalStrength = 0.9f;

    /** 法線マップの world 座標タイリング密度。 */
    f32   m_NormalTiling = 0.1f;

    /** 屈折による水底サンプル変位量。 */
    f32   m_RefractionStrength = 0.24f;

    /** Beer-Lambert 吸収係数 (R, G, B)。 */
    FVec3 m_Absorption{0.38f, 0.16f, 0.055f};

    /** メッシュ密度の第 1 軸指定 (0 で自動)。 */
    u32   m_ResA = 0;

    /** メッシュ密度の第 2 軸指定 (0 で自動)。 */
    u32   m_ResB = 0;

    /** 基本色 (SetColor で設定)。 */
    FVec3 m_Color{0.12f, 0.35f, 0.6f};

    /** 基本不透明度 (SetAlpha で設定)。 */
    f32   m_Alpha = 0.72f;

    /** 波の振幅。 */
    f32   m_Amp   = 0.12f;

    /** 波の速度。 */
    f32   m_Speed = 1.6f;

    /** 経過時刻 (秒)。 */
    f32   m_Time  = 0.0f;

    /** 波が流れる向き (正規化済)。 */
    FVec2 m_FlowDir{1.0f, 0.0f};

    /** 流れの速さ。 */
    f32   m_FlowSpeed = 1.6f;

    /** リップルの輪が広がる速さ。 */
    f32   m_RippleSpeed = 3.2f;

    /** リップルの波長。 */
    f32   m_RippleWavelength = 0.9f;
};

/**
 * 手続き的な炎 (ちらつき + 揺らぎ) を描くコンポーネント。強さを動的に制御できる。
 *
 * @details
 * owner の world 位置を炎の根元とし、M 個の縦積み矩形を時間で揺らして黄→橙→赤の
 * グラデーションで描く。intensity で高さ・ちらつきを変調する。HLSL もアセットも不要。
 */
class AFire2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AFire2DComponent)

    /**
     * 炎のサイズ (幅・高さ) を設定する。
     *
     * @param width 炎の幅。
     * @param height 炎の高さ。
     */
    void SetSize(f32 width, f32 height) noexcept { m_W = width; m_H = height; }

    /**
     * 炎の強さを設定する (負値は 0 に clamp)。
     *
     * @param i 炎の強さ (高さ・ちらつきを変調)。
     */
    void SetIntensity(f32 i) noexcept { m_Intensity = i < 0.0f ? 0.0f : i; }

    /**
     * 現在の炎の強さを返す。
     *
     * @return 設定済みの強さ。
     */
    f32  Intensity() const noexcept { return m_Intensity; }

    /**
     * 経過時刻を進める (ちらつき・揺らぎのアニメに使う)。
     *
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override { m_Time += dt; }

    /**
     * 炎を描画する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(FRenderContext& rc) noexcept override;

private:
    /** 炎の幅。 */
    f32 m_W = 0.8f;

    /** 炎の高さ。 */
    f32 m_H = 1.6f;

    /** 炎の強さ (高さ・ちらつきを変調)。 */
    f32 m_Intensity = 1.0f;

    /** 経過時刻 (秒)。 */
    f32 m_Time = 0.0f;
};

/**
 * owner ノードを追従するモーショントレイル (残像) コンポーネント。
 *
 * @details
 * owner の world 位置を一定時間間隔で FIFO サンプルし、隣接点を先細りの帯 (三角形 2 枚)
 * で繋いで残像を描く。head ほど太く不透明、tail ほど細く透明になる。HLSL もアセットも不要。
 */
class ATrail2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ATrail2DComponent)

    /**
     * トレイルの色を設定する。
     *
     * @param rgb トレイルの色 RGB。
     */
    void SetColor(FVec3 rgb) noexcept { m_Color = rgb; }

    /**
     * トレイルの幅 (head の太さ) を設定する。
     *
     * @param w トレイルの幅。
     */
    void SetWidth(f32 w) noexcept { m_Width = w; }

    /**
     * トレイルのサンプル点数を設定する (2..kCap にクランプ)。
     *
     * @param k 残像に保持する点数。
     */
    void SetPoints(u32 k) noexcept { m_Max = k < 2u ? 2u : (k > kCap ? kCap : k); }

    /**
     * owner 位置を一定間隔でサンプルし、FIFO を更新する。
     *
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /**
     * トレイル (先細りの帯) を描画する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(FRenderContext& rc) noexcept override;

private:
    /** サンプル点の物理的な上限。 */
    static constexpr u32 kCap = 48;

    /** サンプル点の FIFO (m_Pts[0] = 最新 head)。 */
    FVec2 m_Pts[kCap];

    /** 現在保持しているサンプル点数。 */
    u32   m_Count = 0;

    /** トレイルの色。 */
    FVec3 m_Color{0.3f, 0.8f, 1.0f};

    /** トレイルの幅 (head の太さ)。 */
    f32   m_Width = 0.25f;

    /** サンプル間隔を測る経過時間アキュムレータ。 */
    f32   m_SampleAccum = 0.0f;

    /** 保持するサンプル点数の上限 (SetPoints で設定)。 */
    u32   m_Max = 24;
};

/**
 * 任意形状のステンシルマスクで子ツリーの描画をクリップするコンポーネント。
 *
 * @details
 * attach したノードの子ツリーを、指定形状の内側 (既定) か外側だけに通して描く。窓越しに
 * 他のエフェクトを覗かせる / 穴あきマスク等に使う。シェーダ不要で FSpriteBatch の stencil
 * モードを使う。シーンで SetStencilMaskEnabled(true) を呼んでおくのが前提で、stencil バッファ
 * が無いパス (例: 反射の RT パス) では自動的に素通しする。
 */
class AStencilClip2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AStencilClip2DComponent)

    /** マスク形状の三角形上限。 */
    static constexpr u32 kMaxTris = 96;

    /**
     * 矩形マスクを設定する (owner 相対)。
     *
     * @param center 矩形中心 (owner 相対)。
     * @param half_size 矩形の半サイズ。
     */
    void SetRect(FVec2 center, FVec2 half_size) noexcept;

    /**
     * 円マスクを設定する (owner 相対)。
     *
     * @param center 円中心 (owner 相対)。
     * @param radius 半径。
     * @param segments 外周の分割数。
     */
    void SetCircle(FVec2 center, f32 radius, u32 segments = 40) noexcept;

    /**
     * 楕円マスクを設定する (owner 相対)。
     *
     * @param center 楕円中心 (owner 相対)。
     * @param rx X 半径。
     * @param ry Y 半径。
     * @param segments 外周の分割数。
     */
    void SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments = 40) noexcept;

    /**
     * 単純多角形マスクを設定する (centroid からの扇状三角形化、owner 相対)。
     *
     * @param pts 多角形の頂点列 (owner 相対)。
     * @param count 頂点数。
     */
    void SetPolygon(const FVec2* pts, u32 count) noexcept;

    /**
     * マスクの内側ではなく外側だけを残すよう設定する (穴あき)。
     *
     * @param clip_outside true で外側を残す (既定 false = 内側を残す)。
     */
    void SetInvert(bool clip_outside) noexcept { m_Outside = clip_outside; }

    /**
     * ネスト用のステンシル参照値を設定する (入れ子マスクは別値にすること)。
     *
     * @param ref ステンシル参照値 (1..255、0 は 1 に clamp)。
     */
    void SetRef(u8 ref) noexcept { m_Ref = ref < 1 ? 1u : ref; }

    /**
     * マスク形状自体を可視化する (デバッグ用)。
     *
     * @param on true でマスク形状を描く (既定 false = 不可視)。
     * @param color 可視化時の色。
     */
    void SetDebugVisualize(bool on, FVec4 color = FVec4{1.0f, 1.0f, 0.0f, 0.25f}) noexcept {
        m_DebugVis = on; m_DebugColor = color;
    }

    /**
     * マスク形状をステンシルへ書き込み、以降の子描画を内側/外側へ通す。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(FRenderContext& rc) noexcept override;

    /**
     * 子ツリー描画後にステンシルマスクを解除する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDrawPostChildren(FRenderContext& rc) noexcept override;

private:
    /**
     * マスク用の三角形を 1 つ追加する (上限まで)。
     *
     * @param a 頂点 a (owner 相対)。
     * @param b 頂点 b (owner 相対)。
     * @param c 頂点 c (owner 相対)。
     */
    void PushTri(FVec2 a, FVec2 b, FVec2 c) noexcept;

    /** owner 相対のマスク三角形頂点 (3 個 1 組、扇状)。 */
    FVec2 m_Tri[kMaxTris * 3];

    /** 有効なマスク三角形数。 */
    u32   m_TriCount = 0;

    /** 外側を残すか (true=穴あき、false=内側を残す)。 */
    bool  m_Outside  = false;

    /** ステンシル参照値 (ネスト用)。 */
    u8    m_Ref      = 1;

    /** OnDraw でマスクを張ったか (PostChildren での解除判定に使う)。 */
    bool  m_Active   = false;

    /** マスク形状を可視化するか (デバッグ)。 */
    bool  m_DebugVis = false;

    /** 可視化時の色。 */
    FVec4 m_DebugColor{1.0f, 1.0f, 0.0f, 0.25f};
};

} // namespace acs::game
