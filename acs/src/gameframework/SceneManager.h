// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneManager (Phase 1 着手)
//
// TUniquePtr<Scene> のスタック。top のシーンを毎フレーム update/render する。
// 遷移要求 (Change/Push/Pop) はフレーム境界まで遅延 → 走査中 (Update/Render
// 内) からの構造変更が安全。1 フレーム 1 遷移、複数要求が来た場合は後勝ち。
//
// Phase 1 範囲:
//   ・3 種の遷移 (Change/Push/Pop) と pending state machine
//   ・OnEvent は top のみへ配送
// Phase 2 拡張:
//   ・退場 Scene を **3 フレーム保持** (= フレームインフライト 2 + 1) する ring
//     buffer。GPU が直前フレームで参照中のリソースの use-after-free を防ぐ
//   ・Push 時に旧 top の OnPause、Pop 時に新 top の OnResume を呼ぶ
//   ・OnFixedUpdate を FGame の accumulator から呼び込めるよう _FixedUpdate 追加
// Phase 3+ で:
//   ・フェード遷移 / FSceneServices 配線
#pragma once

#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "gameframework/Scene.h"

namespace acs::game {

class FGame;
class RenderContext;

class FSceneManager {
public:
    FSceneManager() noexcept = default;
    ~FSceneManager() noexcept = default;

    FSceneManager(const FSceneManager&)            = delete;
    FSceneManager& operator=(const FSceneManager&) = delete;

    // ----- 遷移要求 (即時適用しない、次フレーム頭で _ApplyPending) -----

    // 現 top を pop して `next` を push (= 単純な画面切替)。pending が既に
    // 立っていれば上書きする (= 後勝ち)。
    void ChangeScene(TUniquePtr<Scene> next) noexcept;

    // 現 top をスタック上に残したまま `next` を push (= モーダル/ダイアログ)。
    void PushScene(TUniquePtr<Scene> next) noexcept;

    // top を pop (スタックが 1 枚以下なら何もせず警告)。
    void PopScene() noexcept;

    // ----- 状態取得 -----
    Scene*  Top()   const noexcept;
    u32     Depth() const noexcept;
    bool    IsEmpty() const noexcept { return _stack.IsEmpty(); }

    // ----- FGame から呼ぶ駆動 API -----

    // フレーム頭で pending 遷移を適用する。退場 Scene は GPU が直前フレームで
    // 参照中のリソースを破棄しないよう ring buffer で 3 フレーム保持される
    // (= フレームインフライト 2 + 1)。
    void _ApplyPending(FGame& game) noexcept;

    // top のシーンに variable-rate dt を流す。
    void _Update(f32 dt) noexcept;

    // top のシーンに fixed_dt を流す (FGame の accumulator から複数回 / フレーム
    // 呼ばれる可能性あり)。
    void _FixedUpdate(f32 fixed_dt) noexcept;

    // top のシーンを描画する。RenderContext は呼び出し側が用意。
    void _Render(RenderContext& rc) noexcept;

    // 受け取った Event を top のシーンへ配送する。
    void _DispatchEvent(const Event& e) noexcept;

    // 終了処理。残った全シーンに OnExit を呼んでから破棄。
    void _ShutdownAll() noexcept;

    // 退場 Scene retain frames (= フレームインフライト 2 + 1)。
    static constexpr u32 kRetireRingSize = 3;

private:
    enum class Op : u8 { None, Change, Push, Pop };

    // pause_current: Push 時に旧 top に OnPause を呼ぶか (Change は false、Push は true)。
    void DoPushInternal(FGame& game, TUniquePtr<Scene> next, bool pause_current) noexcept;
    // resume_new: Pop 後に新 top に OnResume を呼ぶか (Change は false、Pop は true)。
    void DoPopInternal(bool resume_new) noexcept;

    TArray<TUniquePtr<Scene>> _stack;          // top = Back()
    Op                      _pending_op   = Op::None;
    TUniquePtr<Scene>        _pending_arg;    // Change/Push の next、Pop では未使用
    TUniquePtr<Scene>        _retired[kRetireRingSize];  // GPU 遅延削除 ring buffer
    u32                     _retire_head  = 0;           // 次に release するスロット
};

} // namespace acs::game
