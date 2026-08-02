// SPDX-License-Identifier: Apache-2.0
// 2D スプライト描画ヘルパ（バッチ式）
//
// 用途: ピクセル座標で 2D スプライト・矩形を描く。一般的な「ゲーム HUD」や
//       2D ゲームの絵描き用。同じテクスチャの連続スプライトは自動でバッチされる。
//
// 使い方:
//   CSpriteBatch sb;
//   sb.Init(*renderer.Device(), renderer.ColorFormat(), max_sprites=4096);
//
//   // 描画フレーム中
//   auto* cl = renderer.CommandList();
//   sb.Begin(*cl, screen_w, screen_h);
//   sb.Draw(my_tex, 100, 200, 64, 64);                 // 64×64 を (100,200) に
//   sb.DrawRect(0, 0, screen_w, 32, FVec4{0,0,0,0.5f}); // 上部に半透明バー
//   sb.End();
//
// 座標系: 左上原点、ピクセル単位。Y が下方向。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiCommandList.h"
#include "render/RhiTypes.h"

namespace acs {

/**
 * ステンシルマスク描画モード。
 *
 * @details
 * SetStencilMode で切り替える。stencil 付き深度バッファが bind されたパス
 * (AScene2D::SetStencilMaskEnabled(true)) でのみ意味を持つ。
 */
enum class EStencilMode : u8 {
    /** ステンシルテスト無し (ただし DSV bind パスでは DSV 整合 PSO を使う)。 */
    Off,

    /** 描いたピクセルにステンシル参照値を書く (マスク形状を焼く)。 */
    WriteMask,

    /** ステンシル == 参照値 の所だけ描く (マスク内側)。 */
    KeepInside,

    /** ステンシル != 参照値 の所だけ描く (マスク外側)。 */
    KeepOutside,
};

/**
 * スプライトのピクセル効果プリセット (マテリアルアセットが参照する基本シェーダ)。
 *
 * @details
 * SetEffect で切り替える。マテリアルアセット (.acsmat) は「どのプリセットを使うか + パラメータ」
 * を保持し、ノードはそのマテリアルを参照するだけ。シェーダを書けない人でも見た目を変えられる。
 * UV 歪み系 (Wave/Pixelate) はサンプル前、色系 (Grayscale/Tint/...) はサンプル後に適用される。
 */
enum class ESpriteEffect : u8 {
    None       = 0,   ///< 効果なし (通常描画)。
    Grayscale  = 1,   ///< グレースケール (strength=度合い 0..1)。
    Tint       = 2,   ///< 色染め (strength=度合い 0..1, color=染め色)。
    Vignette   = 3,   ///< 周辺減光 (strength=強さ, p0=開始半径, p1=終了半径, color=縁色)。
    Wave       = 4,   ///< 波打ち UV 歪み (strength=振幅, p0=周波数, p1=速度, time)。
    Pixelate   = 5,   ///< モザイク (p0=セル数X, p1=セル数Y)。
    HueShift   = 6,   ///< 色相回転 (strength=角度°, p0=回転速度°/s, time)。
    Brightness = 7,   ///< 明るさ/コントラスト (strength=明るさ, p0=コントラスト)。
    Invert     = 8,   ///< 色反転 (strength=度合い 0..1)。
    Sepia      = 9,   ///< セピア調 (strength=度合い 0..1)。
    Posterize  = 10,  ///< 階調化/ポスタリゼーション (strength=度合い, p0=階調数)。
    Scanline   = 11,  ///< 走査線/CRT風 (strength=濃さ, p0=線の本数)。
    Chromatic  = 12,  ///< 色収差 (strength=度合い, p0=ずれ量)。要テクスチャ。
};

/**
 * 見下ろし水面用の GPU パラメータ。
 *
 * @details AWater2DComponent が SetWaterSurfaceEffect に渡す専用データ。通常の
 * スプライトマテリアルには公開せず、岸距離・プロシージャル波・コースティクスと
 * 最大 48 本の波紋をピクセル単位で評価する。
 */
struct FWaterSurfaceParams {
    static constexpr u32 kMaxRipples = 48;

    f32   time                = 0.0f;
    f32   caustics_intensity = 0.0f;
    f32   caustics_scale     = 1.4f;
    f32   caustics_speed     = 0.7f;
    f32   flow_speed         = 1.0f;
    f32   wave_amplitude     = 0.04f;
    f32   flow_angle         = 0.0f;
    f32   foam_width         = 0.1f;
    f32   foam_speed         = 2.0f;
    f32   foam_alpha         = 0.0f;
    f32   sky_alpha          = 0.0f;
    f32   ripple_wavelength  = 0.8f;
    f32   roughness          = 0.16f;
    f32   normal_strength    = 0.9f;
    f32   normal_tiling      = 0.1f;
    f32   refraction_strength = 0.24f;
    bool  glints_enabled     = false;
    FVec3 caustics_color{0.5f, 0.82f, 1.0f};
    FVec3 foam_color{0.96f, 0.99f, 1.0f};
    FVec3 sky_color{0.40f, 0.60f, 0.90f};
    FVec3 absorption{0.38f, 0.16f, 0.055f};

    /** xy=中心、z=現在の半径、w=減衰済み振幅。 */
    FVec4 ripples[kMaxRipples]{};
    u32   ripple_count = 0;
};

/** 2D ライトの種類。 */
enum class ELightType : u8 {
    Point       = 0,   ///< 点光源 (位置 + 半径減衰、全方向)。
    Spot        = 1,   ///< スポット (位置 + 半径 + 円錐方向)。
    Directional = 2,   ///< 平行光 (方向のみ、減衰なし。太陽光)。
};

/** 2D ライト (lit スプライト用)。座標はスプライトと同じピクセル空間 (左上原点)。 */
struct FSpriteLight {
    FVec2 pos{ 0, 0 };           ///< 光源位置 (px、Point/Spot)。
    f32   radius   = 256.0f;     ///< 届く半径 (px、Point/Spot)。
    FVec3 color{ 1, 1, 1 };      ///< 光の色 (RGB)。
    f32   intensity = 1.0f;      ///< 明るさ倍率。
    i32   type      = 0;         ///< ELightType (0=Point, 1=Spot, 2=Directional)。
    FVec2 dir{ 0, 1 };           ///< スポット軸 / 平行光の進行方向 (正規化)。
    f32   coneInner = 0.92f;     ///< スポット内円錐 (cos、1=細い)。
    f32   coneOuter = 0.70f;     ///< スポット外円錐 (cos、< inner)。
};

/** 影を落とすオクルーダー (lit スプライトのソフト影用)。中心/サイズは screen px。 */
/** 影オクルーダーの多角形頂点の最大数 (三角形=3、クラウン/星等の複雑形状まで。超は切り詰め)。 */
inline constexpr u32 kMaxOccPolyVerts = 32;

struct FSpriteOccluder {
    FVec2 center{ 0, 0 };       ///< 中心 (px)。
    f32   radius = 32.0f;       ///< 円の半径 / 外接半径 (px、penumbra 幅算出にも使用)。
    i32   shape  = 0;           ///< 0=円, 1=箱, 2=多角形。
    FVec2 halfExtents{ 16, 16 };///< 箱の半サイズ (px、shape=1)。
    f32   rotation = 0.0f;      ///< 箱の回転 (ラジアン、shape=1)。
    // --- 多角形 (shape=2: 三角形/ポリゴン) ---
    FVec2 polyVerts[kMaxOccPolyVerts] = {};  ///< 多角形の頂点 (px、world/screen 空間)。
    i32   polyCount = 0;                     ///< 多角形の頂点数 (3..kMaxOccPolyVerts)。
};

/** lit スプライトのマテリアルパラメータ (b1 の cbuffer LitMaterial)。PBR/トゥーン両対応。 */
struct FLitMaterialParams {
    FVec4 baseColor       = FVec4{ 1, 1, 1, 1 };  ///< アルベド tint (rgb) + 不透明度 (a)。
    f32   metallic        = 0.0f;                 ///< 0=誘電体 .. 1=金属 (PBR)。
    f32   roughness       = 0.5f;                 ///< 0=鏡面 .. 1=拡散 (PBR)。
    f32   normalStrength  = 1.0f;                 ///< 法線マップの強さ。
    f32   ao              = 1.0f;                 ///< アンビエントオクルージョン。
    FVec3 emissive        = FVec3{ 0, 0, 0 };     ///< 自己発光色。
    f32   emissiveStrength = 0.0f;                ///< 発光強度。
    // --- シェーディングモード ---
    i32   shadingMode     = 0;                    ///< 0=PBR, 1=Toon。
    // --- トゥーン (shadingMode=1) ---
    FVec3 shadow1Color    = FVec3{ 0.55f, 0.52f, 0.62f }; ///< 1影の色。
    f32   shadow1Threshold = 0.5f;                ///< 1影のしきい値 (lum)。
    FVec3 shadow2Color    = FVec3{ 0.32f, 0.30f, 0.40f }; ///< 2影 (より暗い) の色。
    f32   shadow2Threshold = 0.2f;                ///< 2影のしきい値 (lum)。
    FVec3 rimColor        = FVec3{ 1, 1, 1 };     ///< リムライト色。
    f32   rimPower        = 4.0f;                  ///< リムの鋭さ。
    FVec3 specColor       = FVec3{ 1, 1, 1 };     ///< トゥーンスペキュラ色。
    f32   specThreshold   = 0.85f;                ///< スペキュラのステップしきい値。
    f32   toonSoftness    = 0.05f;                ///< 影境界の柔らかさ。
    // --- Substrate 風 拡張ロブ (shadingMode=0 PBR のみ有効) ---
    f32   clearcoat          = 0.0f;              ///< クリアコート層の強さ (上層 GGX、IOR1.5)。
    f32   clearcoatRoughness = 0.1f;              ///< クリアコート層のラフネス。
    f32   anisotropy         = 0.0f;              ///< 異方性 -1..1 (横/縦に伸びるハイライト、T=画面X)。
    f32   specularLevel      = 0.5f;              ///< 誘電体反射率 (0.5→F0=0.04、glTF KHR_specular)。
    f32   specularTint       = 0.0f;              ///< 鏡面を base 色へ寄せる量 (0=白)。
    f32   sheen              = 0.0f;              ///< シーン (布の逆反射リム) の強さ。
    f32   sheenRoughness     = 0.3f;              ///< シーンのラフネス (Charlie 分布)。
    FVec3 sheenColor         = FVec3{ 1, 1, 1 };  ///< シーン色。
    f32   subsurface         = 0.0f;              ///< サブサーフェス (wrap 拡散 + 影色付け) の量。
    FVec3 subsurfaceColor    = FVec3{ 1.0f, 0.3f, 0.2f }; ///< 透過部の色 (肌/蝋)。
    // --- 描画コンテキスト (マテリアル値ではない。描画側が設定) ---
    i32   selfOccluder       = -1;                ///< 自分自身のオクルーダー番号 (-1=無し)。自己影スキップ。
    i32   selfShadowOccluder = -1;                ///< 自己影 (m_SelfShadow=1) の自分の番号 (-1=無し)。内部=umbra。
    u32   occluderSkipMask   = 0;                 ///< 自分より下 (先描画) のキャスターのスキップマスク (bit j=occluder j)。
};

/** ESpriteEffect のパラメータ (b1 の cbuffer Effect に対応)。 */
struct FEffectParams {
    f32   strength = 1.0f;               ///< 主効果量。
    f32   p0       = 0.0f;               ///< 補助パラメータ 0。
    f32   p1       = 0.0f;               ///< 補助パラメータ 1。
    f32   p2       = 0.0f;               ///< 補助パラメータ 2 (予備)。
    f32   time     = 0.0f;               ///< アニメ用の経過秒。
    FVec4 color    = FVec4{ 1, 1, 1, 1 };///< 染め色 / 縁色。
};

/**
 * ピクセル座標で 2D スプライト・矩形を描くバッチ式ヘルパ。
 *
 * @details
 * HUD や 2D ゲームの絵描き用。同じテクスチャの連続スプライトは自動でバッチされ、
 * テクスチャが切り替わるかバッチが満杯になると Flush して GPU に送る。座標系は
 * 左上原点・ピクセル単位で Y は下方向。Begin/Draw.../End の順に呼ぶ。
 */
class CSpriteBatch {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CSpriteBatch() noexcept = default;

    /** 破棄する (GPU リソースと CPU ステージング領域を解放)。 */
    ~CSpriteBatch() noexcept;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CSpriteBatch(const CSpriteBatch&)            = delete;

    /** コピー代入も禁止。 */
    CSpriteBatch& operator=(const CSpriteBatch&) = delete;

    /**
     * VS+PS・パイプライン・頂点/インデックスバッファ・白テクスチャを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format レンダーターゲットのフォーマット。
     * @param max_sprites 1 フレームで描けるスプライト総数の上限。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format     = EFormat::B8G8R8A8_UNorm,
                      u32 max_sprites      = 4096,
                      u32 msaa_samples     = 1) noexcept;

    /** GPU の完了を待ち、確保した GPU/CPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 描画を開始する。
     *
     * @details screen サイズはピクセル → NDC 変換に使う。view はカメラ無しにリセットされる。
     * @param cl コマンドを積むコマンドリスト。
     * @param screen_w 画面幅 (ピクセル)。
     * @param screen_h 画面高さ (ピクセル)。
     */
    void Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    /**
     * テクスチャ全体を矩形に描く。
     *
     * @param tex 描画するテクスチャ。
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param tint 乗算色 (既定は白)。
     */
    void Draw(IRhiTexture& tex,
              f32 x, f32 y, f32 w, f32 h,
              FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * テクスチャの一部 (UV 0..1) を矩形に描く。
     *
     * @param tex 描画するテクスチャ。
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param u0 サンプル領域の左 U (0..1)。
     * @param v0 サンプル領域の上 V (0..1)。
     * @param u1 サンプル領域の右 U (0..1)。
     * @param v1 サンプル領域の下 V (0..1)。
     * @param tint 乗算色 (既定は白)。
     */
    void DrawSub(IRhiTexture& tex,
                 f32 x, f32 y, f32 w, f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * 単色矩形 (テクスチャ無し) を描く。
     *
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param color 塗りつぶし色。
     */
    void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept;

    /**
     * UTF-8 テキストを描画する。
     *
     * @details \\n で改行し、フォントに未収録のグリフはスキップする。
     * @param font 描画に使うフォント (あらかじめ Init 済みのもの)。
     * @param utf8_text UTF-8 文字列。
     * @param x 行の左上 X (ピクセル)。
     * @param y 行の左上 Y (ピクセル)。
     * @param color 文字色 (既定は白)。
     */
    void DrawString(const class FFont& font, const char* utf8_text,
                  f32 x, f32 y, FVec4 color = FVec4{1,1,1,1}) noexcept;

    /**
     * 回転付きでテクスチャ (の一部) を描く。
     *
     * @details (cx,cy) を中心に radians 回転する。通常スプライトと同じ 4 頂点 / 6 インデックスなので同一バッチに乗る。
     * @param tex 描画するテクスチャ。
     * @param cx 回転中心の X (ピクセル)。
     * @param cy 回転中心の Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param radians 回転角 (ラジアン)。
     * @param u0 サンプル領域の左 U (0..1)。
     * @param v0 サンプル領域の上 V (0..1)。
     * @param u1 サンプル領域の右 U (0..1)。
     * @param v1 サンプル領域の下 V (0..1)。
     * @param tint 乗算色 (既定は白)。
     */
    void DrawRotated(IRhiTexture& tex,
                     f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     f32 u0, f32 v0, f32 u1, f32 v1,
                     FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * 回転付き単色矩形を描く。
     *
     * @param cx 回転中心の X (ピクセル)。
     * @param cy 回転中心の Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param radians 回転角 (ラジアン)。
     * @param color 塗りつぶし色。
     */
    void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                         FVec4 color) noexcept;

    /**
     * 単色の塗りつぶし三角形を描く。
     *
     * @details 4 頂点目に 3 頂点目を重ねた退化三角形を 1 枚挟み、通常スプライトと同じ 4 頂点バッチに乗せる。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param color 塗りつぶし色。
     */
    void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                      FVec4 color) noexcept;

    /**
     * 頂点カラー三角形 (グラデーション) を描く。
     *
     * @details 各頂点に別の色を与えるとシェーダが COLOR を補間する。水面の深さ勾配や泡のフェード等に使う。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param c0 頂点 0 の色。
     * @param c1 頂点 1 の色。
     * @param c2 頂点 2 の色。
     */
    void DrawTriangleVC(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                        FVec4 c0, FVec4 c1, FVec4 c2) noexcept;

    /**
     * 頂点カラーと任意 UV を持つ無地三角形を描く。
     *
     * @details 白テクスチャを使うため外部テクスチャは不要。水面では UV.x に
     * 正規化岸距離 (0=岸、1=深部) を渡す。
     */
    void DrawTriangleVCUV(f32 x0, f32 y0, f32 u0, f32 v0, FVec4 c0,
                          f32 x1, f32 y1, f32 u1, f32 v1, FVec4 c1,
                          f32 x2, f32 y2, f32 u2, f32 v2, FVec4 c2) noexcept;

    /**
     * 任意テクスチャを per-vertex UV で貼る三角形を描く。
     *
     * @details 水の反射 (シーン RT を鏡像 UV でサンプル) 等に使う。tint は全頂点共通の乗算色。
     * @param tex 貼り付けるテクスチャ。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param u0 頂点 0 の U。
     * @param v0 頂点 0 の V。
     * @param u1 頂点 1 の U。
     * @param v1 頂点 1 の V。
     * @param u2 頂点 2 の U。
     * @param v2 頂点 2 の V。
     * @param tint 全頂点共通の乗算色 (既定は白)。
     */
    void DrawTriangleSub(IRhiTexture& tex,
                         f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                         f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2,
                         FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

    /**
     * 2D カメラを設定する。
     *
     * @details (cam_x,cam_y) を画面中心に映し、zoom 倍で拡縮する。Begin() で恒等 (カメラ無し) にリセットされる。
     * @param cam_x カメラ中心の world X。
     * @param cam_y カメラ中心の world Y。
     * @param zoom ズーム倍率。
     */
    void SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept;

    /**
     * クリップ矩形を設定し、以降の描画をこの矩形内 (画面座標) に制限する。
     *
     * @param x クリップ矩形の左上 X (ピクセル)。
     * @param y クリップ矩形の左上 Y (ピクセル)。
     * @param w クリップ矩形の幅 (ピクセル)。
     * @param h クリップ矩形の高さ (ピクセル)。
     */
    void SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept;

    /** クリップ矩形を解除し、画面全体への描画に戻す。 */
    void ClearClipRect() noexcept;

    /**
     * ブレンドモードを切り替える。
     *
     * @details
     * バッチを flush してから PSO を切り替える。Additive PSO は遅延生成で、
     * コースティクス/光のきらめき等「背景を加算で明るくする」表現に使う。
     * Off に戻すときは AlphaBlend を渡す。ステンシルモードとは併用しないこと。
     * @param mode 適用するブレンドモード。
     */
    void SetBlendMode(EBlendMode mode) noexcept;

    /**
     * ステンシルマスクモードを切り替える。
     *
     * @details
     * バッチを flush してから PSO + 参照値を切り替える。任意形状のマスクで描画範囲を
     * 制限する用途で、WriteMask でマスク形状を焼き、KeepInside/KeepOutside でその内/外
     * だけに後続描画を通す。Off で解除。前提として stencil 付き深度バッファが bind された
     * パス (AScene2D::SetStencilMaskEnabled(true) が用意する) でのみ呼ぶこと。それ以外で
     * 呼ぶと DSV 不整合になるため、呼び出し側 (AScene2D / clip component) がガードする。
     * @param mode 適用するステンシルモード。
     * @param ref ステンシル参照値 (既定 1)。
     */
    void SetStencilMode(EStencilMode mode, u8 ref = 1) noexcept;

    /**
     * ピクセル効果 (マテリアルプリセット) を適用する。
     *
     * @details
     * SetBlendMode と同じ要領でバッチを flush してから効果 PSO + b1 パラメータへ切り替える。
     * 以降の Draw 系はすべてこの効果で描かれる。ClearEffect (または Begin) で解除する。
     * effect == None を渡すと ClearEffect と同じ動作。効果 PSO/シェーダ/CB は初回呼び出し時に
     * 遅延生成される (効果を使わないシーンでは作らない)。stencil / additive とは併用しない。
     * @param effect 適用する効果プリセット。
     * @param params 効果のパラメータ。
     */
    void SetEffect(ESpriteEffect effect, const FEffectParams& params) noexcept;

    /**
     * 見下ろし水面の専用 GPU 効果を有効にする。
     *
     * @details 次の ClearEffect まで、DrawTriangleVCUV の岸距離と world 座標から
     * 波・法線・反射・泡・コースティクス・波紋をピクセル単位で再構成する。
     * scene_color / scene_depth が null の既存呼び出しは固定環境色・頂点深度へ fallback する。
     * @param params 水面の光学・アニメーションパラメータ。
     * @param scene_color main pass 前に捕捉した実シーンカラー (null で fallback)。
     * @param scene_depth 水メッシュの正規化深度 RT (null で頂点深度)。
     * @return GPU 水面 effect が利用可能なら true。
     */
    bool SetWaterSurfaceEffect(const FWaterSurfaceParams& params,
                               IRhiTexture* scene_color = nullptr,
                               IRhiTexture* scene_depth = nullptr) noexcept;

    /** ピクセル効果を解除し、通常 (アルファブレンド) パイプラインへ戻す。 */
    void ClearEffect() noexcept;

    /**
     * lit スプライト用の 2D 点光源を設定する (フレーム内で一度、lit 描画の前に呼ぶ)。
     *
     * @details
     * 以降の SetLitMaterial → Draw は、ここで渡したライト群で陰影付けされる。light_height は
     * 光源の «高さ» (法線が効くよう平面より上に置く)。count は kMaxLitLights で頭打ち。
     * @param lights 点光源配列。
     * @param count ライト数。
     * @param ambient 環境光 (RGB)。
     * @param light_height 光源の高さ (px、N·L のため平面より上)。
     * @param occluders 影を落とす円オクルーダー配列 (null=影なし)。
     * @param occ_count オクルーダー数 (kMaxLitLights で頭打ち)。
     */
    void SetLights(const FSpriteLight* lights, u32 count, FVec3 ambient,
                   f32 light_height = 80.0f,
                   const FSpriteOccluder* occluders = nullptr, u32 occ_count = 0) noexcept;

    /** シーンに有効な 2D ライトがあるか (直近の SetLights の count>0)。
     *  マテリアル無しノードを既定 Lit で陰影付けするかの判定に使う。 */
    bool LightsActive() const noexcept { return m_SceneLightCount > 0; }

    /** ライト状態を「無し」に戻す (SetLights を呼ばないパスで前フレームの残留を防ぐ)。 */
    void ClearLights() noexcept { m_SceneLightCount = 0; }

    /**
     * lit (PBR) マテリアルを適用する。以降の Draw は法線マップ + ライトで陰影付けされる。
     *
     * @details
     * SetEffect と同じ要領でバッチを flush してから lit PSO + b1 (マテリアル) + 法線テクスチャ (t1)
     * へ切り替える。アルベドは通常どおり t0 (Draw に渡すテクスチャ)。事前に SetLights が必要。
     * normal_tex が null なら平面法線 (凹凸なし) になる。ClearLit (または Begin) で解除。
     * @param mat PBR マテリアルパラメータ。
     * @param normal_tex 法線マップ (null=平面)。
     */
    void SetLitMaterial(const FLitMaterialParams& mat, IRhiTexture* normal_tex) noexcept;

    /** lit を解除し、通常 (アンリット) パイプラインへ戻す。 */
    void ClearLit() noexcept;

    /**
     * 描画コマンドの蓄積を一時的に抑止する。
     *
     * @details AScene2D の水深捕捉 pass で通常ノードを描かず、TopDown 水だけを一時的に
     * 有効化するための内部向け機能。状態変更時は保留バッチを先に flush する。
     * @param suppress true なら Draw 系を無視、false なら通常描画。
     */
    void SetDrawSuppressed(bool suppress) noexcept;

    /** 描画を終了し、残りのバッチを GPU に送る。 */
    void End() noexcept;

    /**
     * 蓄積済みスプライトを今すぐ描画する (カウントは維持、バッチは継続)。
     *
     * @details カスタムパイプラインで割り込み描画する前に呼び、保留中のスプライトを
     *          先に確定させる。End と違いカウントをリセットしないので、以降のスプライトは
     *          そのまま VB の続きへ追記され破綻しない。割り込み描画後は Rebind() を呼ぶこと。
     */
    void FlushPending() noexcept;

    /**
     * 外部パイプラインで割り込み描画した後、バッチの pipeline/view/VB/IB を貼り直す。
     *
     * @details FlushPending() → 自前パイプライン描画 → Rebind() の順で使う。
     */
    void Rebind() noexcept;

private:
    /** 1 スプライト頂点 (pos2D + uv + color)。 */
    struct FVertex {
        /** スクリーン X 座標。 */
        f32 x;

        /** スクリーン Y 座標。 */
        f32 y;

        /** テクスチャ U 座標。 */
        f32 u;

        /** テクスチャ V 座標。 */
        f32 v;

        /** 頂点カラーの赤成分。 */
        f32 r;

        /** 頂点カラーの緑成分。 */
        f32 g;

        /** 頂点カラーの青成分。 */
        f32 b;

        /** 頂点カラーのアルファ成分。 */
        f32 a;
    };

    /** 蓄積した頂点を GPU へ送り、DrawIndexed を発行する。 */
    void Flush() noexcept;

    /** screen サイズ + view を現在の view 定数バッファに書く。 */
    void WriteScreenCBuffer() noexcept;

    /** 次の view 定数バッファへ進める (フレーム内の view 切替ごと)。 */
    void AdvanceViewBuffer() noexcept;

    /** 現在の view 定数バッファを slot 0 に bind する。 */
    void BindViewBuffer() noexcept;

    /**
     * 4 種のステンシル PSO を遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureStencilPipelines() noexcept;

    /**
     * 加算ブレンド PSO を遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureAdditivePipeline() noexcept;

    /** 不透明ブレンド PSO を遅延生成する。 */
    bool EnsureOpaquePipeline() noexcept;

    /** 乗算ブレンド PSO を遅延生成する。 */
    bool EnsureMultiplyPipeline() noexcept;

    /** alpha を保持する加算ブレンド PSO を遅延生成する。 */
    bool EnsureAdditivePreserveAlphaPipeline() noexcept;

    /**
     * ピクセル効果 PS + PSO + 効果用 CB リングを遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureEffectPipeline() noexcept;

    /**
     * lit (PBR) スプライトの VS/PS + PSO + マテリアル/ライト CB + 平面法線テクスチャを遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureLitPipeline() noexcept;

    /**
     * vs/ps/layout 等のパイプライン共通部を埋める。
     *
     * @param pd 埋める対象のパイプライン記述。
     */
    void FillCommonPipelineDesc(struct FPipelineDesc& pd) const noexcept;

    /** 非所有 device を参照せず、保持している CPU/GPU リソースと状態を破棄する。 */
    void ReleaseResources() noexcept;

    /** Init で受け取った device (ステンシル/加算 PSO の遅延生成用)。 */
    IRhiDevice*              m_Device  = nullptr;

    /** レンダーターゲットのフォーマット (遅延 PSO 生成で再利用)。 */
    EFormat                  m_RtFormat = EFormat::B8G8R8A8_UNorm;

    /** 頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 通常 (アルファブレンド・DSV 無し) 描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /**
     * ステンシルマスク用 PSO (DSVFormat=D24S8)。
     *
     * @details EStencilMode の順に [Off/Write/In/Out]。初回 SetStencilMode で遅延生成 (使わないシーンでは作らない)。
     */
    TUniquePtr<IRhiPipeline> m_StencilPipe[4];

    /** ステンシル PSO 群が生成済みかのフラグ。 */
    bool                     m_StencilReady = false;

    /** 現在のステンシルモード。 */
    EStencilMode             m_StencilMode  = EStencilMode::Off;

    /** 加算ブレンド用 PSO (DSV 無し、Additive)。初回 SetBlendMode(Additive) で遅延生成。 */
    TUniquePtr<IRhiPipeline> m_AdditivePipe;

    /** 加算ブレンド PSO が生成済みかのフラグ。 */
    bool                     m_AdditiveReady = false;

    /** 不透明ブレンド用 PSO。 */
    TUniquePtr<IRhiPipeline> m_OpaquePipe;
    bool                     m_OpaqueReady = false;

    /** 乗算ブレンド用 PSO。 */
    TUniquePtr<IRhiPipeline> m_MultiplyPipe;
    bool                     m_MultiplyReady = false;

    /** alpha 保持加算ブレンド用 PSO。 */
    TUniquePtr<IRhiPipeline> m_AdditivePreserveAlphaPipe;
    bool                     m_AdditivePreserveAlphaReady = false;

    /** 現在のブレンドモード。 */
    EBlendMode               m_BlendMode = EBlendMode::AlphaBlend;

    /** ピクセル効果用ピクセルシェーダ (全プリセットを id 分岐で内包)。初回 SetEffect で遅延生成。 */
    TUniquePtr<IRhiShader>   m_EffectPs;

    /** ピクセル効果用 PSO (b1 に cbuffer Effect、DSV 無し・AlphaBlend)。初回 SetEffect で遅延生成。 */
    TUniquePtr<IRhiPipeline> m_EffectPipe;

    /** 効果 PS/PSO/CB が生成済みかのフラグ。 */
    bool                     m_EffectReady  = false;

    /** 現在ピクセル効果が有効か (ClearEffect/Begin で false)。 */
    bool                     m_EffectActive = false;

    /**
     * 効果パラメータ CB のリング本数。
     *
     * @details
     * view CB と同じく、1 フレーム内で複数ノードが別効果を使うと単一アドレスを上書きし合う
     * (DX12 は即時 memcpy、GPU が読むのは draw 実行時) ため、SetEffect ごとに別スロットへ書く。
     * ただし view 切替がフレーム内 ~6 回なのに対し、効果は «マテリアル付きノード 1 つにつき
     * 1 回» 消費する (DrawScene / DrawTree が per-node で SetEffect) ので桁が大きく異なる。
     * リングがフレーム内で一周すると、まだ GPU 実行されていないスロットを上書きして効果
     * パラメータが化けるため、想定される «同時表示マテリアルノード数» を十分上回る本数にする。
     * 256 を超える効果ノードを 1 フレームで描く極端なシーンではこの値を増やすこと。
     */
    static constexpr u32     kEffectRing = 256;

    /** 効果パラメータ CB のリング (cbuffer Effect)。 */
    TUniquePtr<IRhiBuffer>   m_EffectCb[kEffectRing];

    /** 現在使用中の効果 CB インデックス。 */
    u32                      m_EffectCbCur = 0;

    /** lit スプライトの同時ライト上限。 */
    static constexpr u32     kMaxLitLights = 16;

    /**
     * lit マテリアル CB のリング本数。
     *
     * @details
     * 効果 CB (kEffectRing) と同じく «マテリアル付き lit ノード 1 つにつき 1 回» 消費する
     * (DrawScene / DrawTree が per-node で SetLitMaterial)。フレーム内で一周すると未実行
     * スロットを上書きしてマテリアルパラメータが化けるため、同時表示 lit ノード数を十分
     * 上回る本数にする。これを超える極端なシーンではこの値を増やすこと。
     */
    static constexpr u32     kLitMatRing = 256;

    /** lit スプライト VS (world pos を出力)。初回 SetLitMaterial で遅延生成。 */
    TUniquePtr<IRhiShader>   m_LitVs;

    /** lit スプライト PS (法線マップ + ライトで BRDF)。 */
    TUniquePtr<IRhiShader>   m_LitPs;

    /** lit スプライト PSO (t0=albedo, t1=normal, b1=material, b2=lights)。 */
    TUniquePtr<IRhiPipeline> m_LitPipe;

    /** lit リソースが生成済みか。 */
    bool                     m_LitReady  = false;

    /** 現在 lit が有効か (ClearLit/Begin で false)。 */
    bool                     m_LitActive = false;

    /** マテリアル CB のリング (cbuffer LitMaterial、per-node 消費)。 */
    TUniquePtr<IRhiBuffer>   m_LitMatCb[kLitMatRing];

    /** 現在使用中のマテリアル CB インデックス。 */
    u32                      m_LitMatCbCur = 0;

    /** ライト CB (cbuffer Lights、フレーム内で SetLights が一度書く)。 */
    TUniquePtr<IRhiBuffer>   m_LightsCb;

    /** 直近の SetLights で渡されたライト数 (LightsActive 判定用)。 */
    u32                      m_SceneLightCount = 0;

    /** MSAA サンプル数 (1=非 MSAA)。全 PSO に焼き込まれるため Init 時に決める。
     *  描画先 RT の sample_count と一致させること。 */
    u32                      m_MsaaSamples = 1;

    /** 法線マップ未指定時の平面法線 1x1 テクスチャ ((128,128,255)=+Z)。 */
    TUniquePtr<IRhiTexture>  m_FlatNormal;

    /**
     * 見下ろし水面専用のシームレス法線マップ。
     *
     * @details effect パイプライン初回生成時に決定論的な周期波から CPU 生成し、
     * 水面 PS が異なる縮尺・流速で複数回サンプルする。RGB=接空間法線、A=泡用高さノイズ。
     */
    TUniquePtr<IRhiTexture>  m_WaterNormal;

    /** 頂点バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Vb;

    /** インデックスバッファ。 */
    TUniquePtr<IRhiBuffer>   m_Ib;

    /**
     * view 定数バッファのリング本数。
     *
     * @details
     * 1 フレームで world / HUD / 反射の各パスと複数の SetView があり、定数バッファは
     * 「フレーム内は単一アドレス上書き」なので 1 個だと先に積んだ DrawIndexed が後で
     * 上書きされた view を読んでしまう (world が HUD view で描かれて画面端に潰れる)。
     * view 切替ごとに別スロットへ書き root CBV を貼り直すことで、各 draw が記録時の
     * view を確実に読む。フレーム内の view 切替数 (反射込みで ~6) を十分上回る本数を
     * 確保し、in-flight 2 フレーム分でも巻き戻り衝突しないようにする。
     */
    static constexpr u32     kViewRing = 32;

    /** view 定数バッファのリング (screen size + view)。 */
    TUniquePtr<IRhiBuffer>   m_Cb[kViewRing];

    /** 現在使用中の view 定数バッファのインデックス。 */
    u32                      m_CbCur = 0;

    /** DrawRect 用の 1x1 白テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_White;

    /** CPU 側の頂点バッファステージ (Flush で GPU へコピー)。 */
    FVertex*          m_VertexCpu    = nullptr;

    /** CPU 側の頂点バッファステージを確保したアロケータ。 */
    IAllocator* m_VertexAllocator = nullptr;

    /** 1 フレームで描けるスプライト総数の上限。 */
    u32              m_MaxSprites   = 0;

    /** フレーム内に積んだスプライトの累計数。 */
    u32              m_SpriteCount  = 0;

    /** 既に GPU に投入済みのスプライト数。 */
    u32              m_FlushedCount = 0;

    /** 現在バッチ中のテクスチャ (切り替わると Flush)。 */
    IRhiTexture*     m_CurrentTex   = nullptr;

    /** 水深捕捉 pass で通常ノードの Draw 系を無視するフラグ。 */
    bool             m_DrawSuppressed = false;

    /** Begin で受け取ったコマンドリスト。 */
    IRhiCommandList* m_Cl            = nullptr;

    /** 画面幅 (ピクセル)。 */
    u32              m_ScreenW      = 1;

    /** 画面高さ (ピクセル)。 */
    u32              m_ScreenH      = 1;

    /** カメラ中心の world X。 */
    f32              m_ViewX        = 0.0f;

    /** カメラ中心の world Y。 */
    f32              m_ViewY        = 0.0f;

    /** カメラのズーム倍率。 */
    f32              m_ViewZoom     = 1.0f;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FSpriteBatch = CSpriteBatch;


} // namespace acs
