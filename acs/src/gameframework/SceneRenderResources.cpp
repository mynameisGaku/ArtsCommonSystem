// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SceneRenderResources.h"

#include "gameframework/RenderContext.h"
#include "render/Renderer.h"
#include "render/IRhiTexture.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** world/HUD 描画用の共有 CSpriteBatch を遅延初期化する (成功で true)。 */
bool CSceneRenderResources::EnsureSpriteBatch(FRenderContext& Context) noexcept {
    if (m_SpritesReady) return true;
    CRenderer& renderer = Context.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_Sprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("CSceneRenderResources: CSpriteBatch init failed");
        return false;
    }
    m_SpritesReady = true;
    return true;
}

/** 反射オフスクリーン pass 専用の別 CSpriteBatch を遅延初期化する (成功で true)。 */
bool CSceneRenderResources::EnsureSceneSprites(FRenderContext& Context) noexcept {
    // 反射のオフスクリーン pass 専用の SpriteBatch。オフスクリーン pass と
    // 合成 pass で同一バッチを使うと、頂点/定数バッファがフレーム内で上書きし
    // 合い、オフスクリーン pass の遅延 draw が合成 pass のデータを読んでしまう
    // (RT = 反射元が壊れる)。別バッチに分離して両パスが GPU バッファを共有
    // しないようにする。
    if (m_SceneSpritesReady) return true;
    CRenderer& renderer = Context.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_SceneSprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("CSceneRenderResources: 反射用 CSpriteBatch init failed");
        return false;
    }
    m_SceneSpritesReady = true;
    return true;
}

/** 水面深度捕捉 pass 専用の別 CSpriteBatch を遅延初期化する (成功で true)。 */
bool CSceneRenderResources::EnsureWaterDepthSprites(FRenderContext& Context) noexcept {
    // scene-color pass と同じバッチを使うと、同一フレーム内の VB/CB 更新が先行 draw の
    // 内容を上書きする。depth pass も独立バッチにして GPU 実行まで各データを保持する。
    if (m_WaterDepthSpritesReady) return true;
    CRenderer& renderer = Context.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_WaterDepthSprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("CSceneRenderResources: 水面深度用 CSpriteBatch init failed");
        return false;
    }
    m_WaterDepthSpritesReady = true;
    return true;
}

/** 反射用に world を焼くオフスクリーン RT を遅延作成する (成功で true)。 */
bool CSceneRenderResources::EnsureSceneRt(FRenderContext& Context) noexcept {
    const u32 w = Context.Width(), h = Context.Height();
    if (w == 0 || h == 0) return false;
    if (m_SceneRt && m_RtW == w && m_RtH == h) return true;
    IRhiDevice* dev = Context.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = Context.GetRenderer().ColorFormat();
    td.is_render_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) {
        ACS_LOG_WARN("CSceneRenderResources: 反射用 scene RT の作成に失敗 (反射無効)");
        return false;
    }
    m_SceneRt = Move(r.Value());
    m_RtW = w; m_RtH = h;
    return true;
}

/** TopDown 水メッシュの正規化水深を保持するカラー RT を遅延作成する (成功で true)。 */
bool CSceneRenderResources::EnsureWaterDepthRt(FRenderContext& Context) noexcept {
    const u32 w = Context.Width(), h = Context.Height();
    if (w == 0 || h == 0) return false;
    if (m_WaterDepthRt && m_WaterDepthW == w && m_WaterDepthH == h) return true;
    IRhiDevice* dev = Context.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w;
    td.height = h;
    // SpriteBatch と同じ PSO を使えるよう scene color と同じ RT format にする。
    // R に 0..1 の正規化岸距離を格納し、main water PS から SRV sample する。
    td.format = Context.GetRenderer().ColorFormat();
    td.is_render_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) {
        ACS_LOG_WARN("CSceneRenderResources: 水面深度 RT の作成に失敗 (頂点深度へ fallback)");
        return false;
    }
    m_WaterDepthRt = Move(r.Value());
    m_WaterDepthW = w;
    m_WaterDepthH = h;
    return true;
}

/** マスク用の stencil 付き深度バッファ (D24S8) を遅延作成・再作成する (成功で true)。 */
bool CSceneRenderResources::EnsureStencilBuffer(FRenderContext& Context) noexcept {
    const u32 w = Context.Width(), h = Context.Height();
    if (w == 0 || h == 0) return false;
    if (m_StencilBuf && m_StencilW == w && m_StencilH == h) return true;
    IRhiDevice* dev = Context.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = EFormat::D24_UNorm_S8_UInt;   // 深度 8bit ステンシル付き
    td.is_depth_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) {
        ACS_LOG_WARN("CSceneRenderResources: マスク用 stencil バッファの作成に失敗 (マスク無効)");
        return false;
    }
    m_StencilBuf = Move(r.Value());
    m_StencilW = w; m_StencilH = h;
    return true;
}

} // namespace acs::game
