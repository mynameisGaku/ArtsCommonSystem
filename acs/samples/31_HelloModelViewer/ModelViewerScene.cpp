// SPDX-License-Identifier: Apache-2.0
#include "ModelViewerScene.h"

#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Mat.h"

using namespace acs;
using namespace acs::game;

namespace hellomv {

void ModelViewerScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー。背景は ImGui に隠れるが Quad 描画の余白
    // (= viewport の外側) のクリア色を編集向けに揃える。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    _panels.Init(kAssetRoot);

    if (!_pipeline.Init(GetGame().GetRenderer())) {
        GetGame().Quit();
        return;
    }

    // 実 model loader (glTF) は未配線。cube を「常駐 default model」として扱い、
    // 各 panel に "(default cube)" を伝える設計。
    ACS_LOG_INFO("[ModelViewer] default model loaded (stub): %s", kDefaultModelPath);

    ACS_LOG_INFO("[ModelViewer] entered (workspace + 4 panel + asset browser + theme + cube)");
}

void ModelViewerScene::OnExit() noexcept {
    // GPU リソースは FApplication::Quit() → WaitIdle が走った後、FScene デストラクタ
    // 前のここで明示解放。CmdList が次フレームを参照し続けないよう、まず WaitIdle
    // で in-flight を落としきってから pipeline / buffer / shader を Release。
    if (GetGame().GetRenderer().Device()) {
        GetGame().GetRenderer().Device()->WaitIdle();
    }
    _pipeline.Shutdown();
    _panels.Shutdown();

    ACS_LOG_INFO("[ModelViewer] exited");
}

void ModelViewerScene::OnUpdate(f32 dt) noexcept {
    // ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
    // DrawUI) はすべて OnRender 側へ。`ImGui::Begin` 等は NewFrame() と Render()
    // の間でしか呼べないため、ここで TickAllPanels は呼ばない。
    if (FInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt スパイク防御 (FApplication 側の大 dt で animation/回転が暴れない)。
    if (dt > 0.1f) dt = 0.1f;

    // animation 時間を進める (Tick は ImGui を触らない契約のため OnUpdate で OK)。
    // frame callback が pose を適用する実 loader は未配線。
    _panels.Animation().Tick(dt);

    // 3D cube の自転 (sample 17 と同形)。
    _angle += dt * 0.8f;

    // MVP 更新: FModelViewerPanel::FCamera() の view/proj を使う。editor 上の
    // マウスドラッグで orbit / dolly した姿勢がそのまま 3D 像に反映される。
    // FEditorCamera::Tick (smooth target) は OnRender 側の TickAllPanels →
    // OnFrameBegin から呼ばれる想定。
    editor_core::FEditorCamera& cam = _panels.Viewer().Camera();
    const FMat4 view = cam.ViewMatrix();
    const f32  aspect = static_cast<f32>(GetGame().GetRenderer().Swapchain()->Width()) /
                        static_cast<f32>(GetGame().GetRenderer().Swapchain()->Height());
    const FMat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 100.0f);
    _pipeline.UpdateMvp(view, proj, _angle);
}

void ModelViewerScene::OnRender(FRenderContext& rc) noexcept {
    // (1) 3D scene 描画 (cube only)。同じコマンドリスト上で ImGui より前に
    //     出すことで、ImGui ウィンドウが viewport をオーバーレイで覆える
    //     (= フレームバッファ直書きの背景レイヤ)。
    _pipeline.Render(rc.Cmd());

    // (2) File メニュー (Workspace::DrawMenuBar の前に push する)。
    _menu_bar.Draw(GetGame(), _panels.Workspace(), _panels.Theme(),
                   kDefaultModelPath, kLayoutPath, kThemePath);

    // (3) workspace 全描画 + asset browser。dt は FRenderContext からは取れない
    //     ので FGame の DeltaTime() を使う。
    _panels.Draw(GetGame().DeltaTime());
}

} // namespace hellomv
