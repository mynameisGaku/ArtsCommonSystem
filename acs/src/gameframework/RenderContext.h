// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {
class IRhiCommandList;
class CRenderer;
class CSpriteBatch;
class FFont;
class IRhiTexture;
}

namespace acs::game {

/**
 * 描画先への非所有参照とフレーム・描画パス状態を受け渡す描画コンテキスト。
 *
 * @details
 * CGame 経路は WiringAccess().BeginFrame() で CRenderer、描画命令一覧、画面サイズを配線する。
 * UI フォントと表示変換は必要時、2D 一括描画器、テクスチャ、描画状態は各描画パスで同じ接続窓口
 * から配線する。エディタなどの独自描画経路は必要な配線だけを選べる。参照を返す取得関数は、
 * 対応する参照の配線中だけ使う。描画資源や描画処理の所有・生成は行わない。
 * WiringAccess().EndFrame() は描画命令一覧、2D 一括描画器、UI フォント、テクスチャの参照と
 * 描画パス状態を解除し、CRenderer、画面サイズ、表示変換値は次の配線まで保持する。
 */
class FRenderContext {
public:
    /** 参照未配線の既定状態で構築する (利用経路に必要な WiringAccess() から配線)。 */
    FRenderContext() noexcept = default;

    /** 破棄する (参照のみ保持するため何もしない)。 */
    ~FRenderContext() noexcept = default;

    /** コピー禁止 (フレーム配線を単独で保持するため)。 */
    FRenderContext(const FRenderContext&)            = delete;

    /** コピー代入も禁止。 */
    FRenderContext& operator=(const FRenderContext&) = delete;

    /**
     * フレームと描画パスの非所有参照を明示的に配線する接続窓口。
     *
     * @details エンジン、エディタ、外部の描画側だけが短時間保持する。ゲーム側の描画処理は
     * FRenderContext の読み取り用公開関数を使い、この接続窓口から状態を変更しない。
     */
    class FWiringAdapter final {
    public:
        /** 配線対象の描画コンテキストを保持する。 */
        explicit FWiringAdapter(FRenderContext& context) noexcept : m_Context(context) {}

        /** CRenderer、描画命令一覧、画面サイズを配線してフレームを開始する。 */
        void BeginFrame(CRenderer& renderer, IRhiCommandList& command_list, u32 width, u32 height) noexcept
        {
            m_Context.BeginFrame_Internal(renderer, command_list, width, height);
        }

        /** フレーム固有の非所有参照と描画パス状態を解除する。 */
        void EndFrame() noexcept { m_Context.EndFrame_Internal(); }

        /** 現在の描画パスで使う 2D 一括描画器を配線する (nullptr で解除)。 */
        void SetSpriteBatch(CSpriteBatch* sprite_batch) noexcept
        {
            m_Context.SetSpriteBatch_Internal(sprite_batch);
        }

        /** 全シーン共有の UI フォントを配線する (nullptr で解除)。 */
        void SetFont(FFont* font) noexcept { m_Context.SetFont_Internal(font); }

        /** 反射用シーンテクスチャを配線する (nullptr で解除)。 */
        void SetReflection(IRhiTexture* texture) noexcept { m_Context.SetReflection_Internal(texture); }

        /** 屈折・画面反射用シーンカラーを配線する (nullptr で解除)。 */
        void SetSceneColor(IRhiTexture* texture) noexcept { m_Context.SetSceneColor_Internal(texture); }

        /** 水面用の正規化深度テクスチャを配線する (nullptr で解除)。 */
        void SetSceneDepth(IRhiTexture* texture) noexcept { m_Context.SetSceneDepth_Internal(texture); }

        /** 主描画パス前のシーンカラーを捕捉中かを配線する。 */
        void SetSceneColorCapturePass(bool active) noexcept
        {
            m_Context.SetSceneColorCapturePass_Internal(active);
        }

        /** 水面深度捕捉中かを配線する。 */
        void SetWaterDepthCapturePass(bool active) noexcept
        {
            m_Context.SetWaterDepthCapturePass_Internal(active);
        }

        /** 2D ワールド座標から画面座標への変換値を配線する。 */
        void SetView2D(FVec2 center, f32 scale) noexcept { m_Context.SetView2D_Internal(center, scale); }

        /** 現在の描画パスでステンシルマスクが利用可能かを配線する。 */
        void SetStencilMaskActive(bool active) noexcept
        {
            m_Context.SetStencilMaskActive_Internal(active);
        }

    private:
        /** 配線対象の描画コンテキスト。 */
        FRenderContext& m_Context;
    };

    /** フレームと描画パスの接続窓口を返す。 */
    FWiringAdapter WiringAccess() noexcept { return FWiringAdapter(*this); }

    /**
     * 現フレームの IRhiCommandList を返す。
     *
     * @details nullptr の可能性は OnRender 外でのみ起きる。
     * @return 現フレームのコマンドリスト参照。
     */
    IRhiCommandList& Cmd() const noexcept { return *m_Cmd; }

    /**
     * 描画に使う CRenderer を返す。
     *
     * @return 配線済みの CRenderer 参照。
     */
    CRenderer&        GetRenderer() const noexcept { return *m_Renderer; }

    /**
     * 画面の幅を返す。
     *
     * @return 画面の幅 (px)。
     */
    u32              Width()  const noexcept { return m_Width; }

    /**
     * 画面の高さを返す。
     *
     * @return 画面の高さ (px)。
     */
    u32              Height() const noexcept { return m_Height; }

    /**
     * フレーム描画が進行中か (コマンドリストが配線済みか) を返す。
     *
     * @return 描画中なら true。
     */
    bool             IsFrameActive() const noexcept { return m_Cmd != nullptr; }

    /**
     * 2D 描画バッチが配線済みかを返す。
     *
     * @return 配線済みなら true。
     */
    bool HasSprites() const noexcept { return m_Sprites != nullptr; }

    /**
     * 配線済みの 2D 描画バッチを返す。
     *
     * @return CSpriteBatch 参照。
     */
    CSpriteBatch& Sprites() const noexcept { return *m_Sprites; }

    /**
     * UI フォントが配線済みかを返す。
     *
     * @return 配線済みなら true (false ならテキストは単に描かれない)。
     */
    bool HasFont() const noexcept { return m_Font != nullptr; }

    /**
     * 配線済みの UI フォントを返す。
     *
     * @return フォント参照。
     */
    FFont& GetFont() const noexcept { return *m_Font; }

    /**
     * 反射テクスチャが配線済みかを返す。
     *
     * @return 配線済みなら true。
     */
    bool HasReflection() const noexcept { return m_Reflection != nullptr; }

    /**
     * 配線済みの反射テクスチャを返す。
     *
     * @return 反射テクスチャ参照。
     */
    IRhiTexture& Reflection() const noexcept { return *m_Reflection; }

    /** 実シーンカラーが配線済みかを返す。 */
    bool HasSceneColor() const noexcept { return m_SceneColor != nullptr; }

    /** 配線済みの実シーンカラーを返す。 */
    IRhiTexture& SceneColor() const noexcept { return *m_SceneColor; }

    /** 水面用深度が配線済みかを返す。 */
    bool HasSceneDepth() const noexcept { return m_SceneDepth != nullptr; }

    /** 配線済みの水面用深度を返す。 */
    IRhiTexture& SceneDepth() const noexcept { return *m_SceneDepth; }

    /** シーンカラー捕捉中なら true。 */
    bool IsSceneColorCapturePass() const noexcept { return m_SceneColorCapturePass; }

    /** 水面深度捕捉中なら true。 */
    bool IsWaterDepthCapturePass() const noexcept { return m_WaterDepthCapturePass; }

    /**
     * world 空間のビュー中心を返す。
     *
     * @return 配線済みのビュー中心。
     */
    FVec2 ViewCenter() const noexcept { return m_ViewCenter; }

    /**
     * world→screen のスケールを返す。
     *
     * @return 配線済みのスケール。
     */
    f32   ViewScale()  const noexcept { return m_ViewScale; }

    /**
     * ステンシルマスクが有効なパスかを返す。
     *
     * @details clip コンポーネントはこれを見て、stencil が使えないパス (反射の RT パス等)
     * ではマスクせず素通しする。
     * @return stencil マスクが有効なら true。
     */
    bool StencilMaskActive() const noexcept { return m_StencilMaskActive; }

private:
    /** CRenderer、描画命令一覧、画面サイズを配線し、描画パス状態を初期化する内部処理。 */
    void BeginFrame_Internal(CRenderer& renderer, IRhiCommandList& command_list, u32 width, u32 height) noexcept
    {
        m_Renderer = &renderer;
        m_Cmd = &command_list;
        m_Width = width;
        m_Height = height;
        m_Sprites = nullptr;
        m_Reflection = nullptr;
        m_SceneColor = nullptr;
        m_SceneDepth = nullptr;
        m_SceneColorCapturePass = false;
        m_WaterDepthCapturePass = false;
        m_StencilMaskActive = false;
        // m_Font は BeginFrame_Internal 後に SetFont_Internal でフレーム中だけ配線する。
    }

    /** フレーム固有の非所有参照と描画パス状態を解除する内部処理。 */
    void EndFrame_Internal() noexcept
    {
        m_Cmd = nullptr;
        m_Sprites = nullptr;
        m_Font = nullptr;
        m_Reflection = nullptr;
        m_SceneColor = nullptr;
        m_SceneDepth = nullptr;
        m_SceneColorCapturePass = false;
        m_WaterDepthCapturePass = false;
        m_StencilMaskActive = false;
    }

    /** 現在の描画パスで使う 2D 一括描画器を配線する内部処理。 */
    void SetSpriteBatch_Internal(CSpriteBatch* sprite_batch) noexcept { m_Sprites = sprite_batch; }

    /** 全シーン共有の UI フォントを配線する内部処理。 */
    void SetFont_Internal(FFont* font) noexcept { m_Font = font; }

    /** 反射用シーンテクスチャを配線する内部処理。 */
    void SetReflection_Internal(IRhiTexture* texture) noexcept { m_Reflection = texture; }

    /** 屈折・画面反射用シーンカラーを配線する内部処理。 */
    void SetSceneColor_Internal(IRhiTexture* texture) noexcept { m_SceneColor = texture; }

    /** 水面用の正規化深度テクスチャを配線する内部処理。 */
    void SetSceneDepth_Internal(IRhiTexture* texture) noexcept { m_SceneDepth = texture; }

    /** 主描画パス前のシーンカラー捕捉状態を配線する内部処理。 */
    void SetSceneColorCapturePass_Internal(bool active) noexcept { m_SceneColorCapturePass = active; }

    /** 水面深度捕捉状態を配線する内部処理。 */
    void SetWaterDepthCapturePass_Internal(bool active) noexcept { m_WaterDepthCapturePass = active; }

    /** 2D ワールド座標から画面座標への変換値を配線する内部処理。 */
    void SetView2D_Internal(FVec2 center, f32 scale) noexcept { m_ViewCenter = center; m_ViewScale = scale; }

    /** 現在の描画パスのステンシルマスク利用状態を配線する内部処理。 */
    void SetStencilMaskActive_Internal(bool active) noexcept { m_StencilMaskActive = active; }

    /** 描画に使う CRenderer (フレーム冒頭に配線)。 */
    CRenderer*        m_Renderer = nullptr;

    /** 現フレームのコマンドリスト (フレーム外では nullptr)。 */
    IRhiCommandList* m_Cmd      = nullptr;

    /** 現パスの 2D 描画バッチ (未配線なら nullptr)。 */
    CSpriteBatch*    m_Sprites  = nullptr;

    /** 全シーン共有の UI フォント (未配線なら nullptr)。 */
    FFont*            m_Font     = nullptr;

    /** 反射用シーンテクスチャ (未配線なら nullptr)。 */
    IRhiTexture*     m_Reflection = nullptr;

    /** 水面の屈折・画面反射用に main pass 前へ捕捉した実シーンカラー。 */
    IRhiTexture*     m_SceneColor = nullptr;

    /** 水面メッシュから main pass 前へ捕捉した正規化水深。 */
    IRhiTexture*     m_SceneDepth = nullptr;

    /** TopDown 水を除外して実シーンカラーを捕捉中か。 */
    bool             m_SceneColorCapturePass = false;

    /** TopDown 水だけを正規化水深として捕捉中か。 */
    bool             m_WaterDepthCapturePass = false;

    /** world 空間のビュー中心 (world→screen 変換用)。 */
    FVec2            m_ViewCenter{0.0f, 0.0f};

    /** world→screen のスケール。 */
    f32              m_ViewScale = 1.0f;

    /** ステンシルマスクが有効なパスかのフラグ。 */
    bool             m_StencilMaskActive = false;

    /** 画面の幅 (px)。 */
    u32              m_Width    = 0;

    /** 画面の高さ (px)。 */
    u32              m_Height   = 0;
};

} // namespace acs::game

namespace acs {

/** GameFramework 内の実装型をトップレベルから参照する正規入口。 */
using game::FRenderContext;

} // namespace acs
