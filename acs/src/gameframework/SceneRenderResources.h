// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS GameFramework — CSceneRenderResources (シーン描画リソースの game 寿命共有)
// -----------------------------------------------------------------------------
// シーンが描画に使う CSpriteBatch と オフスクリーン RT をまとめて保持する。
//
// 所有権の分離:
//   シーン (AScene 系) が持つのは ANode ツリー、つまりオブジェクトだけとする。
//   描画リソースはレンダラ側の責務であり、CGame が game 寿命で 1 組だけ持つ。
//   Unity の Scene / Unreal の UWorld / Godot の SceneTree も同じ分担であり、
//   詳細は docs/SceneUnification.md に記録する。
//
//   シーンごとに持つと、メニューや headless テストのシーンまで CSpriteBatch を
//   3 本抱え、PushScene でモーダルを重ねるたびに倍加していた。共有にすることで
//   シーン数に依存しない一定コストになる。
//
// 遅延生成:
//   すべて「初回に使うまで確保しない」。反射も水深度も stencil も使わないゲームが
//   GPU リソースを 1 つも作らない不変条件を維持する。RT は画面サイズが変わった
//   ときだけ作り直す。
//
// 3 本の CSpriteBatch を分けている理由:
//   オフスクリーン pass と合成 pass で同一バッチを使うと、頂点/定数バッファが
//   フレーム内で上書きし合い、先行 pass の遅延 draw が後続 pass のデータを読む。
//   pass ごとに独立したバッチを与えて GPU 実行まで各データを保持する。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "gameframework/Forward.h"
#include "render/SpriteBatch.h"

namespace acs {
class IRhiTexture;
} // namespace acs

namespace acs::game {

class FRenderContext;

/**
 * シーン描画に使う CSpriteBatch と オフスクリーン RT を game 寿命で保持する。
 *
 * @details
 * CGame が 1 つだけ所有し、現在の top シーンが借りて使う。各 Ensure* は初回呼び出しで
 * 生成し、以後は生成済みを返す。生成に失敗した場合は false を返し、呼び出し側が
 * その機能を無効化したまま描画を続けられるようにする (致命的にしない)。
 */
class CSceneRenderResources {
public:
    /** 空の状態で構築する (リソースは Ensure* まで確保しない)。 */
    CSceneRenderResources() noexcept = default;

    /** 破棄する (保持する RT は TUniquePtr が解放)。 */
    ~CSceneRenderResources() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CSceneRenderResources(const CSceneRenderResources&)            = delete;

    /** コピー代入も禁止。 */
    CSceneRenderResources& operator=(const CSceneRenderResources&) = delete;

    /**
     * world/HUD 描画に使う共有 CSpriteBatch を遅延初期化する。
     *
     * @param Context デバイス・カラーフォーマット取得用のレンダーコンテキスト。
     * @return 利用可能なら true、初期化失敗なら false。
     */
    bool EnsureSpriteBatch(FRenderContext& Context) noexcept;

    /**
     * 反射オフスクリーン pass 専用の別 CSpriteBatch を遅延初期化する。
     *
     * @param Context デバイス・カラーフォーマット取得用のレンダーコンテキスト。
     * @return 利用可能なら true、初期化失敗なら false。
     */
    bool EnsureSceneSprites(FRenderContext& Context) noexcept;

    /**
     * 水面深度捕捉 pass 専用の別 CSpriteBatch を遅延初期化する。
     *
     * @param Context デバイス・カラーフォーマット取得用のレンダーコンテキスト。
     * @return 利用可能なら true、初期化失敗なら false。
     */
    bool EnsureWaterDepthSprites(FRenderContext& Context) noexcept;

    /**
     * 反射用に world を焼くオフスクリーン RT を遅延作成する (サイズ変化時は再作成)。
     *
     * @param Context サイズ・デバイス取得用のレンダーコンテキスト。
     * @return 利用可能なら true、作成失敗なら false。
     */
    bool EnsureSceneRt(FRenderContext& Context) noexcept;

    /**
     * TopDown 水メッシュの正規化岸距離を保持するカラー RT を遅延作成する。
     *
     * @param Context サイズ・デバイス取得用のレンダーコンテキスト。
     * @return 利用可能なら true、作成失敗なら false。
     */
    bool EnsureWaterDepthRt(FRenderContext& Context) noexcept;

    /**
     * マスク用の stencil 付き深度バッファ (D24S8) を遅延作成する (サイズ変化時は再作成)。
     *
     * @param Context サイズ・デバイス取得用のレンダーコンテキスト。
     * @return 利用可能なら true、作成失敗なら false。
     */
    bool EnsureStencilBuffer(FRenderContext& Context) noexcept;

    /**
     * world/HUD 描画に使う共有 CSpriteBatch を返す。
     *
     * @details EnsureSpriteBatch が true を返した後にだけ使う。
     * @return 共有 CSpriteBatch への参照。
     */
    CSpriteBatch& SpriteBatch() noexcept { return m_Sprites; }

    /**
     * 反射オフスクリーン pass 用の CSpriteBatch を返す。
     *
     * @details EnsureSceneSprites が true を返した後にだけ使う。
     * @return 反射用 CSpriteBatch への参照。
     */
    CSpriteBatch& SceneSprites() noexcept { return m_SceneSprites; }

    /**
     * 水面深度捕捉 pass 用の CSpriteBatch を返す。
     *
     * @details EnsureWaterDepthSprites が true を返した後にだけ使う。
     * @return 水面深度用 CSpriteBatch への参照。
     */
    CSpriteBatch& WaterDepthSprites() noexcept { return m_WaterDepthSprites; }

    /**
     * 反射用 scene RT を返す。
     *
     * @return 作成済みなら RT、未作成なら nullptr。
     */
    IRhiTexture* SceneRt() noexcept { return m_SceneRt.Get(); }

    /**
     * 水面深度 RT を返す。
     *
     * @return 作成済みなら RT、未作成なら nullptr。
     */
    IRhiTexture* WaterDepthRt() noexcept { return m_WaterDepthRt.Get(); }

    /**
     * マスク用 stencil バッファを返す。
     *
     * @return 作成済みなら深度バッファ、未作成なら nullptr。
     */
    IRhiTexture* StencilBuffer() noexcept { return m_StencilBuf.Get(); }

private:
    /** world/HUD 描画用の共有スプライトバッチ。 */
    CSpriteBatch m_Sprites;

    /** m_Sprites を初期化済みか。 */
    bool         m_SpritesReady = false;

    /** 反射オフスクリーン pass 専用のスプライトバッチ。 */
    CSpriteBatch m_SceneSprites;

    /** m_SceneSprites を初期化済みか。 */
    bool         m_SceneSpritesReady = false;

    /** 水面深度捕捉 pass 専用のスプライトバッチ。 */
    CSpriteBatch m_WaterDepthSprites;

    /** m_WaterDepthSprites を初期化済みか。 */
    bool         m_WaterDepthSpritesReady = false;

    /** 反射用に world を焼くオフスクリーン RT。 */
    TUniquePtr<IRhiTexture> m_SceneRt;

    /** m_SceneRt を作成したときの幅。 */
    u32          m_RtW = 0;

    /** m_SceneRt を作成したときの高さ。 */
    u32          m_RtH = 0;

    /** 水面の正規化岸距離を保持するカラー RT。 */
    TUniquePtr<IRhiTexture> m_WaterDepthRt;

    /** m_WaterDepthRt を作成したときの幅。 */
    u32          m_WaterDepthW = 0;

    /** m_WaterDepthRt を作成したときの高さ。 */
    u32          m_WaterDepthH = 0;

    /** マスク用の stencil 付き深度バッファ。 */
    TUniquePtr<IRhiTexture> m_StencilBuf;

    /** m_StencilBuf を作成したときの幅。 */
    u32          m_StencilW = 0;

    /** m_StencilBuf を作成したときの高さ。 */
    u32          m_StencilH = 0;
};

} // namespace acs::game
