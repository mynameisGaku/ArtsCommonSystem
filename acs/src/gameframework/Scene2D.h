// SPDX-License-Identifier: Apache-2.0
// FScene2D - practical base scene for 2D games.
//
// It wires the common 2D stack:
//   SceneServices(Default2D | Camera2D | Physics2D)
//   root FNode2D tree
//   one shared FSpriteBatch for world + HUD drawing
//
// Users override OnReady/OnTick/OnFixedTick/OnDrawWorld/OnDrawHud instead of
// re-implementing the same root/update/render plumbing in every scene.
#pragma once

#include "gameframework/Scene.h"
#include "gameframework/Node2D.h"
#include "render/SpriteBatch.h"

namespace acs::game {

/**
 * 2D ゲーム向けの実用的なシーン基底クラス。
 *
 * @details
 * 共通の 2D スタック (SceneServices(Default2D | Camera2D | Physics2D)、root FNode2D ツリー、
 * world/HUD 描画用の共有 FSpriteBatch) を配線する。利用者は root/update/render の定型
 * 処理を毎シーン書き直す代わりに OnReady/OnTick/OnFixedTick/OnDrawWorld/OnDrawHud を
 * override する。平面反射とステンシルマスクをオプションで有効化できる。
 */
class FScene2D : public Scene {
public:
    /** 空の 2D シーンを構築する (リソースは OnEnter/OnRender で遅延確保)。 */
    FScene2D() noexcept = default;

    /** シーンを破棄する (GPU リソースは各メンバが解放)。 */
    ~FScene2D() noexcept override = default;

    /** コピー禁止 (FNode2D ツリーと GPU リソースを単独所有するため)。 */
    FScene2D(const FScene2D&)            = delete;

    /** コピー代入も禁止。 */
    FScene2D& operator=(const FScene2D&) = delete;

    /**
     * このシーンが要求するサービスを返す。
     *
     * @return Default2D | Camera2D | Physics2D の合成フラグ。
     */
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D;
    }

    /**
     * シーンの root ノードへの可変参照を返す。
     *
     * @return root FNode2D への参照 (ここに子を AddChild してツリーを組む)。
     */
    FNode2D& Root() noexcept { return m_Root; }

    /**
     * シーンの root ノードへの const 参照を返す。
     *
     * @return root FNode2D への const 参照。
     */
    const FNode2D& Root() const noexcept { return m_Root; }

    /**
     * world/HUD 描画に使う共有 FSpriteBatch を返す。
     *
     * @return 共有 FSpriteBatch への参照。
     */
    FSpriteBatch& SpriteBatch() noexcept { return m_Sprites; }

    /**
     * 1 ワールド単位あたりのピクセル数を設定する。
     *
     * @param ppu ピクセル/ユニット (0.001 以下なら 1.0 にクランプ)。
     */
    void SetPixelsPerUnit(f32 ppu) noexcept { m_PixelsPerUnit = ppu > 0.001f ? ppu : 1.0f; }

    /**
     * 1 ワールド単位あたりのピクセル数を返す。
     *
     * @return 設定済みのピクセル/ユニット (既定 64.0)。
     */
    f32  PixelsPerUnit() const noexcept { return m_PixelsPerUnit; }

    /**
     * 画面ピクセル座標をワールド座標へ変換する (マウスピッキング用)。
     *
     * @details
     * 入力は左上原点の画面ピクセル (Input::MousePos() の値)。FScene2D のレンダリング
     * (ppu * camera zoom、camera 中心) と厳密に逆対応するので、Camera2D::ScreenToWorld
     * (ppu 非考慮) ではなくこちらを使う。画面サイズは直近の OnRender でキャッシュした値を用いる。
     * @param screen_px 変換する画面ピクセル座標 (左上原点)。
     * @return 対応するワールド座標。
     */
    FVec2 ScreenToWorld(FVec2 screen_px) noexcept;

    /**
     * 直近 OnRender でキャッシュした画面幅を返す。
     *
     * @return 画面幅 (ピクセル)。
     */
    u32   ScreenWidth()  const noexcept { return m_ScreenW; }

    /**
     * 直近 OnRender でキャッシュした画面高さを返す。
     *
     * @return 画面高さ (ピクセル)。
     */
    u32   ScreenHeight() const noexcept { return m_ScreenH; }

    /**
     * 平面反射を有効/無効にする。
     *
     * @details
     * ON にすると OnRender が「world をオフスクリーン RT に焼く → スワップチェーンに
     * world+水(反射)+HUD」の 3 パスになる。反射する水が無くても world を 2 度描く
     * コストがかかるので、反射を使うシーンだけ ON にする。既定 OFF = 従来の単一パス。
     * @param on true で反射を有効化。
     */
    void SetReflectionEnabled(bool on) noexcept { m_ReflectionEnabled = on; }

    /**
     * 平面反射が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool ReflectionEnabled() const noexcept { return m_ReflectionEnabled; }

    /**
     * ステンシルマスクを有効/無効にする。
     *
     * @details
     * ON にすると world パスが stencil 付き深度バッファ (D24S8) を bind した状態で描かれ、
     * FStencilClip2DComponent 等が任意形状で描画範囲をマスクできるようになる。
     * 既定 OFF = 従来どおり (DSV 無し)。反射と併用可。
     * @param on true でステンシルマスクを有効化。
     */
    void SetStencilMaskEnabled(bool on) noexcept { m_StencilMaskEnabled = on; }

    /**
     * ステンシルマスクが有効かを返す。
     *
     * @return 有効なら true。
     */
    bool StencilMaskEnabled() const noexcept { return m_StencilMaskEnabled; }

    /** シーン入場時に OnReady を呼ぶ。 */
    void OnEnter() noexcept override;

    /** シーン退場時に構造変更を解決し、物理・トゥイーン・スプライトバッチを後始末する。 */
    void OnExit() noexcept override;

    /**
     * 毎フレームの update。
     *
     * @details OnTick → root の UpdateTree → 構造変更解決の順で実行する。
     * @param dt 経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /**
     * 固定刻みの update。
     *
     * @details OnFixedTick → root の FixedUpdateTree → 構造変更解決の順で実行する。
     * @param fixed_dt 固定刻みの秒。
     */
    void OnFixedUpdate(f32 fixed_dt) noexcept override;

    /**
     * シーンを描画する (反射/ステンシル設定に応じて単一〜複数パス)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnRender(RenderContext& rc) noexcept override;

protected:
    /** シーンが top に来たとき 1 度だけ呼ばれる初期化フック (派生で override)。 */
    virtual void OnReady() noexcept {}

    /**
     * 毎フレームのゲームロジックフック (root の更新前に呼ばれる)。
     *
     * @param dt 経過秒。
     */
    virtual void OnTick(f32 /*dt*/) noexcept {}

    /**
     * 固定刻みのゲームロジックフック。
     *
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedTick(f32 /*fixed_dt*/) noexcept {}

    /**
     * world view でのカスタム描画フック (root ツリー描画の後に呼ばれる)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     * @param sb 現パスに配線された FSpriteBatch。
     */
    virtual void OnDrawWorld(RenderContext& /*rc*/, FSpriteBatch& /*sb*/) noexcept {}

    /**
     * HUD view でのカスタム描画フック (画面座標、カメラ非依存)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     * @param sb 現パスに配線された FSpriteBatch。
     */
    virtual void OnDrawHud(RenderContext& /*rc*/, FSpriteBatch& /*sb*/) noexcept {}

private:
    /**
     * 共有 FSpriteBatch を遅延初期化する。
     *
     * @param rc デバイス・カラーフォーマット取得用のレンダーコンテキスト。
     * @return 利用可能なら true、初期化失敗なら false。
     */
    bool EnsureSpriteBatch(RenderContext& rc) noexcept;

    /**
     * 反射オフスクリーン pass 専用の別 FSpriteBatch を遅延初期化する。
     *
     * @details 合成 pass と GPU バッファを共有しないよう分離している。
     * @param rc デバイス・カラーフォーマット取得用のレンダーコンテキスト。
     * @return 利用可能なら true、初期化失敗なら false。
     */
    bool EnsureSceneSprites(RenderContext& rc) noexcept;

    /**
     * 反射用に world を焼くオフスクリーン RT を遅延作成する (サイズ変化時は再作成)。
     *
     * @param rc サイズ・デバイス取得用のレンダーコンテキスト。
     * @return 利用可能なら true、作成失敗なら false。
     */
    bool EnsureSceneRt(RenderContext& rc) noexcept;

    /**
     * マスク用の stencil 付き深度バッファ (D24S8) を遅延作成する (サイズ変化時は再作成)。
     *
     * @param rc サイズ・デバイス取得用のレンダーコンテキスト。
     * @return 利用可能なら true、作成失敗なら false。
     */
    bool EnsureStencilBuffer(RenderContext& rc) noexcept;

    /**
     * world パスを描画する (camera view を設定し root を DrawTree → OnDrawWorld)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawWorldPass(RenderContext& rc) noexcept;

    /**
     * HUD パスを描画する (画面中心の view を設定し OnDrawHud)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawHudPass(RenderContext& rc) noexcept;

    /** シーンの root ノード (ツリーの起点)。 */
    FNode2D      m_Root;

    /** world/HUD 描画用の共有スプライトバッチ。 */
    FSpriteBatch m_Sprites;

    /** m_Sprites が初期化済みかのフラグ。 */
    bool         m_SpritesReady = false;

    /** 反射オフスクリーン pass 専用のスプライトバッチ。 */
    FSpriteBatch m_SceneSprites;

    /** m_SceneSprites が初期化済みかのフラグ。 */
    bool         m_SceneSpritesReady = false;

    /** 1 ワールド単位あたりのピクセル数 (既定 64)。 */
    f32          m_PixelsPerUnit = 64.0f;

    /** 直近 OnRender でキャッシュした画面幅 (picking 用)。 */
    u32          m_ScreenW = 1280;

    /** 直近 OnRender でキャッシュした画面高さ (picking 用)。 */
    u32          m_ScreenH = 720;

    /** 反射用に world を焼くオフスクリーン RT (所有権を持つ)。 */
    TUniquePtr<IRhiTexture> m_SceneRt;

    /** m_SceneRt の現在の幅。 */
    u32          m_RtW = 0;

    /** m_SceneRt の現在の高さ。 */
    u32          m_RtH = 0;

    /** 平面反射が有効かのフラグ。 */
    bool         m_ReflectionEnabled = false;

    /** マスク用の stencil 付き深度バッファ (D24S8、所有権を持つ)。 */
    TUniquePtr<IRhiTexture> m_StencilBuf;

    /** m_StencilBuf の現在の幅。 */
    u32          m_StencilW = 0;

    /** m_StencilBuf の現在の高さ。 */
    u32          m_StencilH = 0;

    /** ステンシルマスクが有効かのフラグ。 */
    bool         m_StencilMaskEnabled = false;
};

} // namespace acs::game
