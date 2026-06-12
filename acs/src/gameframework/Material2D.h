// SPDX-License-Identifier: Apache-2.0
// マテリアルアセット (.acsmat) — シェーダプリセット + パラメータの束ね。
//
// 用途: シェーダを書けない人でも見た目を変えられるよう、ピクセル効果プリセット
//       (ESpriteEffect) とそのパラメータを「マテリアルアセット」として外部ファイルに
//       保存する。ノードは「どのマテリアルを使うか」を参照するだけ (material_path)。
//       効果の切替やパラメータ編集はマテリアルエディタ側で行う。
//
// .acsmat テキスト形式 (v1、行指向):
//   ACSMAT 1
//   name MyMaterial
//   effect Vignette          ; ESpriteEffect の名前
//   strength 0.8
//   p0 0.30
//   p1 1.00
//   p2 0.00
//   color 0 0 0 1            ; 染め色 / 縁色 (RGBA)
//   animated 0               ; 1 なら描画時に経過秒を time に流す (Wave/HueShift 用)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "render/SpriteBatch.h"   // ESpriteEffect / FEffectParams / FSpriteBatch

namespace acs::game {

/**
/**
 * マテリアルの種別。
 *
 * @details Lit (PBR/BRDF) が既定。Effect は従来の 2D ポストプロセス効果プリセット。
 */
enum class EMaterialKind : u8 {
    Lit    = 0,   ///< PBR (BRDF) サーフェスマテリアル。ベースカラー/法線/メタリック/ラフネス/エミッシブ。
    Effect = 1,   ///< 2D ポストプロセス効果プリセット (ESpriteEffect)。
};

/**
 * PBR (BRDF) サーフェスマテリアルのプロパティ。metallic-roughness ワークフロー。
 *
 * @details
 * ベースカラー (アルベド) はテクスチャ + tint、法線マップ、メタリック/ラフネス、エミッシブ、
 * AO を持つ。テクスチャはパス参照 (sprite_path と同様にランタイムが遅延ロード)。2D の
 * 動的ライト (FLighting2D) で陰影付けされる lit スプライト用。
 */
struct FPbrParams2D {
    FVec4 baseColor       = FVec4{ 1, 1, 1, 1 };  ///< アルベド tint (rgb) + 不透明度 (a)。
    f32   metallic        = 0.0f;                 ///< 0=誘電体 .. 1=金属。
    f32   roughness       = 0.5f;                 ///< 0=鏡面 .. 1=完全拡散。
    FVec3 emissive        = FVec3{ 0, 0, 0 };     ///< 自己発光色 (rgb)。
    f32   emissiveStrength = 0.0f;                ///< 発光強度倍率。
    f32   normalStrength   = 1.0f;                ///< 法線マップの強さ (0=平面)。
    f32   ao               = 1.0f;                ///< アンビエントオクルージョン (1=遮蔽なし)。
    char  albedoPath[256]  = {};                  ///< ベースカラー画像パス (UTF-8、空=tint のみ)。
    char  normalPath[256]  = {};                  ///< 法線マップ画像パス (UTF-8、空=平面)。
    // --- シェーディングモード + トゥーン (UTS 風 cel) ---
    i32   shadingMode      = 0;                    ///< 0=PBR, 1=Toon。
    FVec3 shadow1Color     = FVec3{ 0.55f, 0.52f, 0.62f }; ///< 1影の色。
    f32   shadow1Threshold = 0.5f;                ///< 1影しきい値 (lum)。
    FVec3 shadow2Color     = FVec3{ 0.32f, 0.30f, 0.40f }; ///< 2影の色。
    f32   shadow2Threshold = 0.2f;                ///< 2影しきい値 (lum)。
    FVec3 rimColor         = FVec3{ 1, 1, 1 };     ///< リムライト色。
    f32   rimPower         = 4.0f;                  ///< リムの鋭さ。
    FVec3 specColor        = FVec3{ 1, 1, 1 };     ///< トゥーンスペキュラ色。
    f32   specThreshold    = 0.85f;                ///< スペキュラのステップしきい値。
    f32   toonSoftness     = 0.05f;                ///< 影境界の柔らかさ。
    // --- Substrate 風 拡張ロブ (shadingMode=0 PBR のみ) ---
    f32   clearcoat          = 0.0f;               ///< クリアコート層の強さ。
    f32   clearcoatRoughness = 0.1f;               ///< クリアコート層のラフネス。
    f32   anisotropy         = 0.0f;               ///< 異方性 -1..1。
    f32   specularLevel      = 0.5f;               ///< 誘電体反射率 (0.5→F0=0.04)。
    f32   specularTint       = 0.0f;               ///< 鏡面を base 色へ寄せる量。
    f32   sheen              = 0.0f;               ///< シーン強さ。
    f32   sheenRoughness     = 0.3f;               ///< シーンのラフネス。
    FVec3 sheenColor         = FVec3{ 1, 1, 1 };   ///< シーン色。
    f32   subsurface         = 0.0f;               ///< サブサーフェス量。
    FVec3 subsurfaceColor    = FVec3{ 1.0f, 0.3f, 0.2f }; ///< 透過部の色。
};

/**
 * マテリアルアセット (.acsmat) のランタイム表現。
 *
 * @details
 * kind で «PBR サーフェス (Lit)» か «効果プリセット (Effect)» を切り替える。既定は Lit (BRDF)。
 * ノードはこれを参照するだけで、編集はマテリアル側 (マテリアルエディタ) で行う。Effect の
 * animated が true なら描画時に経過秒を params.time へ流す (Wave/HueShift 等)。
 */
struct FMaterial2D {
    char          name[64] = "Material";   ///< 表示名。
    EMaterialKind kind     = EMaterialKind::Lit;  ///< マテリアル種別 (既定 = PBR)。
    // --- Effect 種別 ---
    ESpriteEffect effect   = ESpriteEffect::None;  ///< 使用する効果プリセット。
    FEffectParams params   = {};           ///< 効果パラメータ (color は染め色/縁色)。
    bool          animated = false;        ///< true で描画時に経過秒を time に流す。
    // --- Lit (PBR) 種別 ---
    FPbrParams2D  pbr      = {};            ///< PBR サーフェスプロパティ。
};

/** PBR/トゥーンの永続パラメータ (FPbrParams2D) を描画パラメータ (FLitMaterialParams) へ変換する。
 *  editor の DrawScene・Node2D・GPU プレビューが共通で使う (DRY)。 */
FLitMaterialParams ToLitParams(const FPbrParams2D& pbr) noexcept;

/** 効果プリセットの総数 (None を含む)。エディタのドロップダウン用。 */
u32 SpriteEffectCount() noexcept;

/** 効果 enum → 名前 ("None"/"Grayscale"/...)。未知は "None"。 */
const char* SpriteEffectName(ESpriteEffect e) noexcept;

/** index 番目 (0..SpriteEffectCount-1) の効果名。範囲外は "None"。 */
const char* SpriteEffectNameAt(u32 index) noexcept;

/** index 番目の効果 enum。範囲外は None。 */
ESpriteEffect SpriteEffectAt(u32 index) noexcept;

/** 名前 → 効果 enum (大文字小文字無視)。未知は None。 */
ESpriteEffect SpriteEffectFromName(const char* name) noexcept;

/**
 * 効果プリセットの「見栄えのする既定パラメータ」を返す。
 *
 * @details
 * 効果ごとに意味のあるパラメータ範囲が違う (Vignette の半径は 0..1、Pixelate のセル数は
 * 数十、HueShift の速度は度/秒…)。マテリアル新規作成時やエディタで効果を切り替えたときに
 * これを入れておくと、いきなり破綻した値にならず「選んだ瞬間にそれっぽく見える」。
 * @param e 効果プリセット。
 * @return その効果に適した既定パラメータ。
 */
FEffectParams DefaultEffectParams(ESpriteEffect e) noexcept;

/** その効果が既定でアニメーション (時間駆動) すべきか (Wave/HueShift)。 */
bool EffectAnimatedByDefault(ESpriteEffect e) noexcept;

/**
 * .acsmat テキストを解析する。
 *
 * @param text .acsmat の本文 (NUL 終端)。
 * @return 解析結果。ヘッダ不正や text==nullptr のときは既定 (None) を返す。
 */
FMaterial2D ParseAcsmatText(const char* text) noexcept;

/**
 * マテリアルを .acsmat テキストへ書き出す。
 *
 * @param mat 書き出すマテリアル。
 * @param buf 出力バッファ。
 * @param buf_size buf のバイト数。
 * @return 書き込んだ文字数 (NUL を除く)。
 */
u32 WriteAcsmatText(const FMaterial2D& mat, char* buf, u32 buf_size) noexcept;

/**
 * .acsmat ファイルを読み込む。
 *
 * @param path ファイルパス (UTF-8)。
 * @param out 解析結果の格納先。
 * @return 読み込み + 解析に成功したら true。
 */
bool LoadAcsmatFile(const char* path, FMaterial2D& out) noexcept;

/**
 * マテリアルを .acsmat ファイルへ書き出す。
 *
 * @param path ファイルパス (UTF-8)。
 * @param mat 書き出すマテリアル。
 * @return 書き込みに成功したら true。
 */
bool SaveAcsmatFile(const char* path, const FMaterial2D& mat) noexcept;

/**
 * アニメーション付きマテリアルの「経過秒」を設定する (プロセス全体で共有)。
 *
 * @details
 * FNode2D::DrawTree がマテリアルを適用するとき、RenderContext には時刻が無いため、
 * この共有クロックを参照する。スタンドアロンのシーンが毎フレーム OnTick で
 * SetMaterialClock(累積秒) を呼ぶ。エディタは host->time を直接渡すのでこれは使わない。
 * @param seconds 起動からの経過秒。
 */
void SetMaterialClock(f32 seconds) noexcept;

/** 共有マテリアルクロック (秒) を返す。 */
f32 MaterialClock() noexcept;

/**
 * マテリアルの効果を SpriteBatch へ適用する。
 *
 * @details effect == None なら何もしない。animated が true なら time_sec を params.time
 *          に差し込んで適用する。適用したら戻り値 true → 描画後に sb.ClearEffect() を呼ぶこと。
 * @param sb 適用先のスプライトバッチ。
 * @param mat 適用するマテリアル。
 * @param time_sec animated 効果のアニメ時間 (秒)。
 * @return 効果を適用したら true (要 ClearEffect)、None で未適用なら false。
 */
bool ApplyMaterial(FSpriteBatch& sb, const FMaterial2D& mat, f32 time_sec) noexcept;

} // namespace acs::game
