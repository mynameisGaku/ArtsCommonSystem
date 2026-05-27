// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — 3D viewport の RHI リソース管理 + 描画。
//
// 役割:
//   ・viewer panel 背後で動く "default cube" の VS/PS/VB/IB/CB/Pipeline を所有。
//   ・FEditorCamera の view/proj を引数で受け取り、MVP を CB に書き戻す。
//   ・コマンドリストに draw call を発行する。
//
// なぜ独立クラスにしたか:
//   editor UI (workspace / panel / theme / menu) と直交した責務。Scene 側の関心
//   事を「3D の見た目」と「エディタ UI」に二分することで、片方を差し替えても
//   もう一方が壊れないよう疎結合化する。
#pragma once

#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"

namespace acs {
class IRhiDevice;
class IRhiCommandList;
class FRenderer;
} // namespace acs

namespace hellomv {

class ViewerScenePipeline {
public:
    // FRenderer から ColorFormat / DepthFormat を引くので FRenderer 参照を取る。
    // 戻り値 true なら初期化成功。false の caller は Quit を発行する想定。
    bool Init(acs::FRenderer& renderer) noexcept;

    // OnExit から呼ばれる。GPU 側の参照が消えてから Release する責務は caller
    // (Scene 側) が WaitIdle を発行することで保証する。
    void Shutdown() noexcept;

    // MVP をホスト側計算して CB に書き戻す。aspect は swapchain から caller 側で求める。
    void UpdateMvp(const acs::FMat4& view, const acs::FMat4& proj, acs::f32 angle) noexcept;

    // 1 フレーム分の draw call をコマンドリストに発行する。pipeline 未初期化なら no-op。
    void Render(acs::IRhiCommandList& cl) noexcept;

private:
    acs::TUniquePtr<acs::IRhiShader>   _vs;
    acs::TUniquePtr<acs::IRhiShader>   _ps;
    acs::TUniquePtr<acs::IRhiBuffer>   _vb;
    acs::TUniquePtr<acs::IRhiBuffer>   _ib;
    acs::TUniquePtr<acs::IRhiBuffer>   _cb;
    acs::TUniquePtr<acs::IRhiPipeline> _pipeline;
};

} // namespace hellomv
