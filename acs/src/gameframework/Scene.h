// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — AScene 基底
//
// 1 つの画面/状態を 1 つの AScene サブクラスで書く。CGame がスタック上で
// 切り替え・更新・描画する。AScene の override は全て `noexcept`。
//
// 使い方:
//   class FTitleScene : public acs::AScene {
//   public:
//       void OnEnter()      noexcept override { /* 起動時の初期化 */ }
//       void OnUpdate(f32)  noexcept override { /* ロジック */ }
//       void OnRender(acs::FRenderContext&) noexcept override { /* 描画 */ }
//       void OnExit()       noexcept override { /* 後片付け */ }
//   };
//
// 遷移: OnUpdate 内で `Scenes().ChangeScene(MakeUnique<FNextScene>())` を呼ぶと
// **次フレーム頭**で適用される (走査中の構造変更を避ける、1 フレーム 1 遷移)。
//
// 本ヘッダは lifecycle hook + CGame/CSceneManager 参照を提供する。
#pragma once

#include "foundation/Types.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "gameframework/Forward.h"
#include "gameframework/SceneServices.h"
#include "gameframework/SubsystemCollection.h"
#include "gameframework/ANode.h"

namespace acs {

// 描画リソースは CGame が game 寿命で所有するため、シーンヘッダは実体を必要としない
// (docs/SceneUnification.md)。参照と引数にしか使わないので前方宣言で足りる。
class CSpriteBatch;

struct FEvent;

namespace game {

class FRenderContext;

/**
 * 1 つの画面/状態を表すシーンの基底クラス。
 *
 * @details
 * 派生クラスが OnEnter/OnUpdate/OnRender/OnExit 等の lifecycle hook を override して
 * ロジックと描画を書く。CGame がスタック上で切り替え・更新・描画する。コピー禁止で、
 * CGame/CSceneManager が _SetContext / _AttachServices で実行コンテキストを配線する。
 * シーン遷移は OnUpdate 内で Scenes().ChangeScene() を呼ぶと次フレーム頭で適用される。
 */
class AScene {
public:
    /** 空のシーンを構築する (コンテキスト・サービスは未配線)。 */
    AScene() noexcept : m_Root(NewObject<ANode>(FStringView("Root"))) {}

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~AScene() noexcept = default;

    /** コピー禁止 (シーンは単独所有・参照が前提のため)。 */
    AScene(const AScene&)            = delete;

    /** コピー代入も禁止。 */
    AScene& operator=(const AScene&) = delete;

    /**
     * 別シーンが上に Push されて自分が更新も描画も止まるときに呼ばれるフック。
     *
     * @details BGM ダッキングなどに使う。
     */
    virtual void OnPause() noexcept {}

    /** 上のシーンが Pop されて自分が top に戻った直後に呼ばれる復帰フック。 */
    virtual void OnResume() noexcept {}

    /**
     * ウィンドウ/入力イベントの配送フック (最上段シーンにのみ届く)。
     *
     * @param e 配送されたイベント。
     */
    virtual void OnEvent(const FEvent& /*e*/) noexcept {}

    /**
     * このシーンが使いたいサービスを bit flag で宣言する。
     *
     * @details 派生クラスで override する。既定は None (= サービス無し、scenes が
     * 直接メンバーで service を持つ古いパターン互換)。
     * @return 使用するサービスのビットフラグ (既定 ESvc::None)。
     */
    virtual ESvc WantedServices() const noexcept { return ESvc::None; }

    /**
     * CSceneManager が attach 済みの CSceneServices を返す。
     *
     * @details WantedServices が None で attach されていない場合は ACS_ASSERT で停止する
     * (= 使う気がないなら呼ばない)。
     * @return attach 済みの CSceneServices への参照。
     */
    CSceneServices& Services() const noexcept {
        ACS_ASSERTF(m_Services.Get() != nullptr,
                    "Scene::Services() called but WantedServices() returned None (or attach failed)");
        return *m_Services;
    }

    /**
     * CSceneServices が attach されているかを返す。
     *
     * @return attach 済みなら true。
     */
    bool HasServices() const noexcept { return m_Services.Get() != nullptr; }

    /**
     * 所属する CGame を返す。
     *
     * @return CGame への参照。
     */
    CGame&         GetGame() const noexcept { return *m_Game; }

    /**
     * シーンスタックを管理する CSceneManager を返す。
     *
     * @return CSceneManager への参照 (ChangeScene 等の遷移要求に使う)。
     */
    CSceneManager& Scenes()  const noexcept { return *m_Scenes; }

    /**
     * CGame/CSceneManager の参照を配線する (内部用。利用者は触らない)。
     *
     * @param g 所属する CGame。
     * @param sm シーンスタックを管理する CSceneManager。
     */
    void _SetContext(CGame* g, CSceneManager* sm) noexcept {
        m_Game   = g;
        m_Scenes = sm;
    }

    /**
     * 生成済みの CSceneServices を attach する (内部用)。
     *
     * @param svc attach する CSceneServices (所有権が移る)。
     */
    void _AttachServices(TUniquePtr<CSceneServices> svc) noexcept {
        m_Services = Move(svc);
    }

    /**
     * attach 済みの CSceneServices をポインタで返す (内部用)。
     *
     * @return CSceneServices へのポインタ (未 attach なら nullptr)。
     */
    CSceneServices* _ServicesOrNull() const noexcept { return m_Services.Get(); }

    // ===== サブシステム (World スコープ) =====

    /**
     * このシーン(World スコープ)のサブシステム束を返す。
     *
     * @details World に無いサブシステムは GameInstance → Engine へフォールバックする
     * (CSceneManager が parent を配線する)。
     * @return World スコープのコレクション。
     */
    CSubsystemCollection& Subsystems() noexcept { return m_WorldSubsystems; }

    /**
     * 型でサブシステムを取得する (World → GameInstance → Engine の順に検索)。
     *
     * @tparam T ASubsystem 派生型。
     * @return T*(未登録なら nullptr)。
     */
    template<typename T>
    T* GetSubsystem() noexcept { return m_WorldSubsystems.Get<T>(); }

    /**
     * World サブシステムを初期化する (内部用。CSceneManager が push 時に呼ぶ)。
     *
     * @param parent GameInstance スコープのコレクション(フォールバック先)。
     */
    bool _InitWorldSubsystems(CSubsystemCollection* parent) noexcept {
        if (!m_WorldSubsystems.TryInitialize(
                ESubsystemScope::World, parent,
                FSubsystemOwner{this, ESubsystemOwnerKind::Scene})) {
            return false;
        }
        /** hook前のWorld lifecycle世代。 */
        const u64 Generation = m_WorldSubsystems.LifecycleGeneration();
        _OnWorldSubsystemsReady();
        if (!m_WorldSubsystems.IsInitialized() ||
            m_WorldSubsystems.LifecycleGeneration() != Generation) {
            m_WorldSubsystems.Deinitialize();
            return false;
        }
        return true;
    }

    /** 指定 phase の World サブシステムを 1 フレーム進める (内部用)。 */
    void _TickWorldSubsystems(const FSubsystemFrameContext& Context) noexcept
    {
        m_WorldSubsystems.TickFrame(Context);
    }

    /** World サブシステムを解体する (内部用。CSceneManager が pop 時に呼ぶ)。 */
    void _DeinitWorldSubsystems() noexcept { m_WorldSubsystems.Deinitialize(); }

    /** World サブシステムへのポインタ (内部用。派生がノードへ配線するのに使う)。 */
    CSubsystemCollection* _WorldSubsystemsPtr() noexcept { return &m_WorldSubsystems; }

    /** constructor後のscene固有状態が遷移準備可能かを返す。 */
    bool _CanPrepare() const noexcept { return _IsPreparationReady(); }

    /**
     * シーンの root ノードへの可変参照を返す。
     *
     * @return root ANode への参照 (ここに子を AddChild してツリーを組む)。
     */
    ANode& Root() noexcept { return *m_Root; }

    /**
     * シーンの root ノードへの const 参照を返す。
     *
     * @return root ANode への const 参照。
     */
    const ANode& Root() const noexcept { return *m_Root; }

    /**
     * world/HUD 描画に使う共有 CSpriteBatch を返す。
     *
     * @details 実体は CGame が game 寿命で持つ共有束 (docs/SceneUnification.md)。
     * シーンは所有せず借りて使うため、初回描画より前に呼ぶと未初期化のバッチを返す。
     * @return 共有 CSpriteBatch への参照。
     */
    CSpriteBatch& SpriteBatch() noexcept;

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
     * 入力は左上原点の画面ピクセル (CInput::MousePos() の値)。AScene2D のレンダリング
     * (ppu * camera zoom、camera 中心) と厳密に逆対応するので、CCamera2D::ScreenToWorld
     * (ppu 非考慮) ではなくこちらを使う。画面サイズは直近の OnRender でキャッシュした値を用いる。
     * @param screen_px 変換する画面ピクセル座標 (左上原点)。
     * @return 対応するワールド座標。
     */
    FVec2 ScreenToWorld(FVec2 screen_px) noexcept;

    /**
     * 描画に使う view 中心を返す。
     *
     * @details Camera2D service を要求しないシーン (AScene の既定 ESvc::None) では原点を返す。
     * これにより素の AScene 派生でも 2D 描画が成立する (docs/SceneUnification.md)。
     * @return world 座標での view 中心。
     */
    FVec2 ViewCenter() noexcept;

    /**
     * 描画に使う zoom 倍率を返す。
     *
     * @details Camera2D service が無ければ等倍 (1.0) を返す。
     * @return camera zoom 倍率。
     */
    f32   ViewZoom() noexcept;

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
     * TopDown 水面向けの実シーンカラー/水深サンプリングを有効・無効にする。
     *
     * @details
     * SetReflectionEnabled(true) と併用した場合だけ、反射 RT から TopDown 水を除外して
     * 屈折元の実シーンカラーにし、専用の正規化水深 RT も生成する。既定 OFF のため、
     * 従来シーンや SideView 反射には追加 pass のコストがかからない。
     * @param on true で実シーン水面サンプリングを有効化。
     */
    void SetWaterSceneSamplingEnabled(bool on) noexcept { m_WaterSceneSamplingEnabled = on; }

    /** TopDown 水面向け実シーンサンプリングが有効かを返す。 */
    bool WaterSceneSamplingEnabled() const noexcept { return m_WaterSceneSamplingEnabled; }

    /**
     * ステンシルマスクを有効/無効にする。
     *
     * @details
     * ON にすると world パスが stencil 付き深度バッファ (D24S8) を bind した状態で描かれ、
     * AStencilClip2DComponent 等が任意形状で描画範囲をマスクできるようになる。
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
    virtual void OnEnter() noexcept;

    /** シーン退場時に構造変更を解決し、物理・トゥイーン・スプライトバッチを後始末する。 */
    virtual void OnExit() noexcept;

    /**
     * 毎フレームの update。
     *
     * @details OnTick → root の UpdateTree → 構造変更解決の順で実行する。
     * @param dt 経過秒。
     */
    virtual void OnUpdate(f32 dt) noexcept;

    /**
     * 固定刻みの update。
     *
     * @details OnFixedTick → root の FixedUpdateTree → 構造変更解決の順で実行する。
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 fixed_dt) noexcept;

    /**
     * シーンを描画する (反射/ステンシル設定に応じて単一〜複数パス)。
     *
     * @details FRenderContext は CGame が game 寿命で共有する CSpriteBatch / FFont /
     * 現フレームの IRhiCommandList を持つ (docs/SceneUnification.md)。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnRender(FRenderContext& rc) noexcept;

protected:
    /** scene固有の必須所有物が生成済みならtrueを返す。 */
    virtual bool _IsPreparationReady() const noexcept { return m_Root.Get() != nullptr; }

    /**
     * World サブシステムの初期化直後に呼ばれる内部フック(OnEnter より前)。
     *
     * @details AScene2D が override してルートノードへサブシステム束を配線し、
     * 配下のノード/コンポーネントから GetSubsystem<T>() を使えるようにする。
     */
    virtual void _OnWorldSubsystemsReady() noexcept;

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
     * @param sb 現パスに配線された CSpriteBatch。
     */
    virtual void OnDrawWorld(FRenderContext& /*rc*/, CSpriteBatch& /*sb*/) noexcept {}

    /**
     * HUD view でのカスタム描画フック (画面座標、カメラ非依存)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     * @param sb 現パスに配線された CSpriteBatch。
     */
    virtual void OnDrawHud(FRenderContext& /*rc*/, CSpriteBatch& /*sb*/) noexcept {}

private:
    /**
     * world パスを描画する (camera view を設定し root を DrawTree → OnDrawWorld)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawWorldPass(FRenderContext& rc) noexcept;

    /**
     * HUD パスを描画する (画面中心の view を設定し OnDrawHud)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawHudPass(FRenderContext& rc) noexcept;

    /** シーンの root ノード (ツリーの起点)。 */
    TObjectPtr<ANode> m_Root;

    /** 1 ワールド単位あたりのピクセル数 (既定 64)。 */
    f32          m_PixelsPerUnit = 64.0f;

    /** 直近 OnRender でキャッシュした画面幅 (picking 用)。 */
    u32          m_ScreenW = 1280;

    /** 直近 OnRender でキャッシュした画面高さ (picking 用)。 */
    u32          m_ScreenH = 720;

    /** 平面反射が有効かのフラグ。 */
    bool         m_ReflectionEnabled = false;

    /** TopDown 水の実シーンカラー/水深サンプリングを生成するか。 */
    bool         m_WaterSceneSamplingEnabled = false;

    /** ステンシルマスクが有効かのフラグ。 */
    bool         m_StencilMaskEnabled = false;
    /** 所属する CGame (default = nullptr、_SetContext で配線)。 */
    CGame*                    m_Game     = nullptr;

    /** シーンスタックを管理する CSceneManager (default = nullptr、_SetContext で配線)。 */
    CSceneManager*            m_Scenes   = nullptr;

    /** attach されたサービス束 (WantedServices に応じて CSceneManager が確保、所有権を持つ)。 */
    TUniquePtr<CSceneServices> m_Services;

    /** World スコープのサブシステム束 (push 時に CSceneManager が Initialize)。 */
    CSubsystemCollection m_WorldSubsystems;
};

} // namespace game
} // namespace acs

namespace acs {

/** scene描画コンテキストをトップレベルから参照する正規入口。 */
using game::FRenderContext;

} // namespace acs
