// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Game.h"
#include "gameframework/LegacyScene3DAdapter.h"
#include "gameframework/RenderContext.h"
#include "container/StringView.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "platform/Time.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "threading/Thread.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 通常のGame/World所有関係を準備し、本物のLegacy描画を呼ぶ試験用ゲーム。 */
class ALegacyAtmosphereTestGame final : public CGame {
public:
    /** Engineの親範囲とシーンを開始する。描画器は先に初期化しておく。 */
    bool StartForTest() noexcept
    {
        if (!EngineSubsystems().TryInitialize(ESubsystemScope::Engine, nullptr, FSubsystemOwner{this, ESubsystemOwnerKind::Application})) return false;
        OnStart();
        return m_Scene != nullptr && !Scenes().IsEmpty();
    }
    /** GPUの完了確認後に、シーンから親範囲の順で終了する。 */
    void StopForTest() noexcept { OnShutdown(); EngineSubsystems().Deinitialize(); }
    /** Gameが所有するシーンを借用する。終了後は使わない。 */
    ALegacyScene3DAdapter* SceneForTest() const noexcept { return m_Scene; }

protected:
    /** 水面やアセット読み込みを含めない、空の通常3Dシーンを作る。 */
    TUniquePtr<AScene> InitialScene() noexcept override
    {
        // 所有権は標準のSceneManagerへ移す。
        TUniquePtr<ALegacyScene3DAdapter> scene = MakeUnique<ALegacyScene3DAdapter>();
        m_Scene = scene.Get();
        return TUniquePtr<AScene>(Move(scene));
    }

private:
    /** 所有者はSceneManager。観測のためだけに借用する。 */
    ALegacyScene3DAdapter* m_Scene = nullptr;
};

/** 完了が確認できなければ、GPUに参照され得る所有者全体を保持する。 */
struct FLegacyAtmosphereTestResources {
    /** 描画器より長く生きる非表示ウィンドウ。 */
    TUniquePtr<FWindow> Window;
    /** 描画器とシーンを所有する通常C++ゲーム。 */
    TUniquePtr<ALegacyAtmosphereTestGame> Game;
    /** シーンと親範囲の終了処理が必要か。 */
    bool Started = false;
    /** 提出・読み戻しの途中失敗で、GPUの完了が不明になっていないか。 */
    bool CompletionUnconfirmed = false;

    /** 正常系は順に終了し、完了不明時だけプロセス終了まで資源を保持する。 */
    ~FLegacyAtmosphereTestResources() noexcept
    {
        if (CompletionUnconfirmed) {
            test::RecordInfo(FSourceLoc::Current(), "通常C++大気試験: GPU完了が不明なため所有資源を保持する。");
            (void)Game.Release();
            (void)Window.Release();
            return;
        }
        if (Game && Started) Game->StopForTest();
    }
};

/** 検証する最小画面幅。深度の全画素を読み戻して提出完了も確認する。 */
constexpr u32 kRuntimeWidth = 64u;
/** 画面の高さ。 */
constexpr u32 kRuntimeHeight = 48u;

/** 一フレームを実際に描画・提出し、同じキューの深度コピー完了まで確認する。 */
bool RenderAtmosphereFrame_Internal(FLegacyAtmosphereTestResources& resources) noexcept
{
    if (resources.CompletionUnconfirmed) return false;
    // 描画器・命令・深度の所有権はGameに残す。
    CRenderer& renderer = resources.Game->GetRenderer();
    if (!renderer.IsOperational()) return false;
    resources.Window->PollEvents();
    resources.CompletionUnconfirmed = true;
    renderer.BeginFrame(resources.Game->GetClearColor());
    if (!renderer.IsFrameOpen() || renderer.CommandList() == nullptr) return false;
    // 通常C++と同じ公開接続窓口で、そのフレームの借用参照を配線する。
    FRenderContext context;
    context.WiringAccess().BeginFrame(renderer, *renderer.CommandList(), kRuntimeWidth, kRuntimeHeight);
    resources.Game->SceneForTest()->OnRender(context);
    context.WiringAccess().EndFrame();
    // 提出結果を標準のGame/World通知にも届ける。
    const FRendererFrameEndResult result = renderer.EndFrameDetailed();
    renderer.Device()->WaitIdle();
    if (!result.submitted || !result.presented || !renderer.IsOperational()) return false;
    // ウィンドウの大きさが変わっていても、固定長の読戻し先を超えて書かせない。
    IRhiTexture* depthBuffer = renderer.DepthBuffer();
    if (depthBuffer == nullptr || depthBuffer->Width() != kRuntimeWidth || depthBuffer->Height() != kRuntimeHeight || depthBuffer->PixelFormat() != EFormat::D32_Float || depthBuffer->SampleCount() != 1u || depthBuffer->ArraySize() != 1u) return false;
    // ReadTextureの同期コピー成功も必要とし、WaitIdleの復帰だけでは採用しない。
    f32 depth[kRuntimeWidth * kRuntimeHeight]{};
    if (!renderer.Device()->ReadTexture(*depthBuffer, depth, static_cast<u32>(sizeof(depth))) || !renderer.IsOperational()) return false;
    resources.CompletionUnconfirmed = false;
    // 空シーンの消去済み深度が全域で保持されることを確認する。大気の画質の期待値ではない。
    for (const f32 value : depth) if (value != 1.0f) return false;
    return true;
}

} // 無名名前空間

ACS_TEST(LegacyAtmosphereRuntime, RawDx12AerialPerspectiveDoesNotDependOnIblSupport)
{
    // 提出失敗時も所有者をまとめて保持する。
    FLegacyAtmosphereTestResources resources;
    // ユーザーの作業画面を覆わない実ウィンドウ。
    FWindowConfig windowConfiguration{};
    windowConfiguration.title = L"ACS 大気初期化試験";
    windowConfiguration.width = kRuntimeWidth;
    windowConfiguration.height = kRuntimeHeight;
    windowConfiguration.visible = false;
    // 実ウィンドウの作成結果。
    auto window = FWindow::Create(windowConfiguration);
    EXPECT_TRUE(window.IsOk());
    if (window.IsErr()) return;
    resources.Window = MakeUnique<FWindow>(Move(window.Value()));
    resources.Game = MakeUnique<ALegacyAtmosphereTestGame>();
    EXPECT_TRUE(resources.Window && resources.Game);
    if (!resources.Window || !resources.Game) return;
    // 深度つきの通常描画器を作る。実GPU不可を合格扱いにしない。
    const auto initialized = resources.Game->GetRenderer().Init(*resources.Window, false, true);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    EXPECT_TRUE(FStringView(resources.Game->GetRenderer().Device()->BackendName()) == FStringView("DX12"));
    resources.Started = true;
    // 製品のGameInstance/Worldを通してシーンを接続する。
    const bool started = resources.Game->StartForTest();
    EXPECT_TRUE(started);
    if (!started) return;
    // Game所有の空シーンを借用する。
    ALegacyScene3DAdapter& scene = *resources.Game->SceneForTest();
    EXPECT_FALSE(scene.AerialPerspectiveEnabled());
    EXPECT_FALSE(scene.AtmosphereResourcesReady());
    EXPECT_EQ(scene.AerialPerspectiveDispatchCount(), u64{0});
    // 無効時に高価な大気初期化を増やさない。
    const bool disabledFrame = RenderAtmosphereFrame_Internal(resources);
    EXPECT_TRUE(disabledFrame);
    if (!disabledFrame) return;
    EXPECT_FALSE(scene.AtmosphereResourcesReady());
    // 正射影では有効設定でも空気遠近法を生成しない。
    scene.SetAerialPerspectiveEnabled(true);
    // 自動カメラ選択が投影方式を戻さないよう、試験が操作するカメラを明示する。
    scene.SetOrbitCameraActive(true);
    EXPECT_TRUE(scene.OrbitCameraOverrideActive());
    scene.SetProjectionMode(ESceneProjectionMode::Orthographic);
    const bool orthographicFrame = RenderAtmosphereFrame_Internal(resources);
    EXPECT_TRUE(orthographicFrame);
    if (!orthographicFrame) return;
    EXPECT_FALSE(scene.AtmosphereResourcesReady());
    EXPECT_EQ(scene.AerialPerspectiveDispatchCount(), u64{0});
    // 後から透視投影へ変えても、IBL未対応で大気初期化を飛ばしてはならない。
    scene.SetProjectionMode(ESceneProjectionMode::Perspective);
    const bool perspectiveFrame = RenderAtmosphereFrame_Internal(resources);
    EXPECT_TRUE(perspectiveFrame);
    if (!perspectiveFrame) return;
    EXPECT_TRUE(scene.AtmosphereResourcesReady());
    if (!scene.AtmosphereResourcesReady()) return;
    // 初期化成功だけでは受け入れず、背景側の準備後に実際の体積生成命令へ到達させる。
    const f64 deadline = CClock::SecondsSinceStartup() + 90.0;
    while (scene.AerialPerspectiveDispatchCount() == 0u && CClock::SecondsSinceStartup() < deadline) {
        const bool rendered = RenderAtmosphereFrame_Internal(resources);
        EXPECT_TRUE(rendered);
        if (!rendered) return;
        SleepMs(1u);
    }
    EXPECT_TRUE(scene.AerialPerspectiveDispatchCount() > 0u);
    // 設定を切ったフレームでは記録回数が増えず、既存資源は無用に再生成しない。
    const u64 recorded = scene.AerialPerspectiveDispatchCount();
    scene.SetAerialPerspectiveEnabled(false);
    // 高度を変えて保存済み体積と入力を不一致にし、単なる再利用で合格するのを防ぐ。
    scene.SetOrbit(FVec3{0.0f, 1500.0f, 0.0f}, 0.0f, 0.0f, 10.0f);
    EXPECT_TRUE(RenderAtmosphereFrame_Internal(resources));
    EXPECT_TRUE(scene.AtmosphereResourcesReady());
    EXPECT_EQ(scene.AerialPerspectiveDispatchCount(), recorded);
    // 同じ入力のまま再有効化すると一度だけ更新されることを対照として確認する。
    scene.SetAerialPerspectiveEnabled(true);
    EXPECT_TRUE(RenderAtmosphereFrame_Internal(resources));
    EXPECT_EQ(scene.AerialPerspectiveDispatchCount(), recorded + 1u);
    // 変化がなければ生成結果を再利用し、毎フレームの高価な積分を増やさない。
    EXPECT_TRUE(RenderAtmosphereFrame_Internal(resources));
    EXPECT_EQ(scene.AerialPerspectiveDispatchCount(), recorded + 1u);
#if defined(_WIN64)
    EXPECT_EQ(sizeof(ALegacyScene3DAdapter), static_cast<usize>(377408u));
#endif
    test::RecordInfo(FSourceLoc::Current(), "通常C++大気試験: 初期化=%u 体積記録数=%llu。空シーンなので画質・物体への合成・故障回復の受入ではない。", scene.AtmosphereResourcesReady() ? 1u : 0u, static_cast<unsigned long long>(scene.AerialPerspectiveDispatchCount()));
}
