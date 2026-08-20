// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneManager 実装
#include "gameframework/SceneManager.h"
#include "gameframework/Scene.h"
#include "gameframework/Game.h"
#include "gameframework/RenderContext.h"

#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** Change 遷移を保留にセットする (next==nullptr は警告して無視)。 */
void FSceneManager::ChangeScene(TUniquePtr<FScene> next) noexcept
{
    if (!next) {
        ACS_LOG_WARN("FSceneManager::ChangeScene(nullptr) ignored");
        return;
    }
    m_PendingOp = EOp::Change;
    m_PendingArg = Move(next);
}

/** Push 遷移を保留にセットする (next==nullptr は警告して無視)。 */
void FSceneManager::PushScene(TUniquePtr<FScene> next) noexcept
{
    if (!next) {
        ACS_LOG_WARN("FSceneManager::PushScene(nullptr) ignored");
        return;
    }
    m_PendingOp = EOp::Push;
    m_PendingArg = Move(next);
}

/** Pop 遷移を保留にセットする。 */
void FSceneManager::PopScene() noexcept
{
    m_PendingOp = EOp::Pop;
    m_PendingArg.Reset();
}

/** top のシーンを返す (空なら nullptr)。 */
FScene* FSceneManager::Top() const noexcept
{
    if (m_Stack.IsEmpty()) return nullptr;
    return m_Stack.Back().Get();
}

/** スタックの深さを返す。 */
u32 FSceneManager::Depth() const noexcept
{
    return static_cast<u32>(m_Stack.Size());
}

/** active scene境界の世代を進め、u64を使い切った後は照合不能な0へ固定する。 */
void FSceneManager::AdvanceActiveSceneEpoch_Internal() noexcept
{
    if (m_ActiveSceneEpoch == 0u || m_ActiveSceneEpoch == ~u64{0}) {
        m_ActiveSceneEpoch = 0u;
        return;
    }
    ++m_ActiveSceneEpoch;
}

/** 内部 push: 旧 top を任意で OnPause し、next に context/services を attach して OnEnter する。 */
void FSceneManager::DoPushInternal(FGame& game, TUniquePtr<FScene> next, bool pause_current) noexcept
{
    if (!next) return;
    // 旧 top を OnPause (Push 時のみ。Change は新 top 直入れなので skip)
    if (pause_current && !m_Stack.IsEmpty()) {
        m_Stack.Back()->OnPause();
    }
    next->_SetContext(&game, this);
    // WantedServices に基づいて FSceneServices を構築・attach。
    // None なら空の FSceneServices だが、Services() を呼ばない限りコスト 0。
    const ESvc wanted = next->WantedServices();
    if (wanted != ESvc::None) {
        next->_AttachServices(MakeUnique<FSceneServices>(wanted));
    }
    // World スコープのサブシステムを初期化(parent = GameInstance → Engine)。OnEnter より前。
    next->_InitWorldSubsystems(&game.GameInstanceSubsystems());
    m_Stack.PushBack(Move(next));
    AdvanceActiveSceneEpoch_Internal();
    m_Stack.Back()->OnEnter();
}

/** 内部 pop: top を OnExit して ring buffer へ退避し、任意で新 top を OnResume する。 */
void FSceneManager::DoPopInternal(bool resume_new) noexcept
{
    if (m_Stack.IsEmpty()) return;
    m_Stack.Back()->OnExit();
    m_Stack.Back()->_DeinitWorldSubsystems(); // OnExit 後に World サブシステムを解体
    // 退場 Scene を ring buffer の現在ヘッドに格納 (3 フレーム保持)。
    // ヘッドは _ApplyPending の冒頭で前進 + 古いスロット解放済。
    m_Retired[m_RetireHead] = Move(m_Stack.Back());
    m_Stack.PopBack();
    AdvanceActiveSceneEpoch_Internal();
    // 新 top を OnResume (Pop 時のみ。Change は次の Push が走るので skip)
    if (resume_new && !m_Stack.IsEmpty()) {
        ResetFixedInput_Internal();
        m_Stack.Back()->OnResume();
    }
}

/** ring buffer を前進させて古い退場 Scene を破棄し、保留中の遷移を適用する。 */
void FSceneManager::ApplyPending_Internal(FGame& game) noexcept
{
    // GPU N+1 frame 遅延削除: ring を 1 つ前進し、新ヘッド位置のスロットを
    // 解放する (= 3 フレーム前に退場した Scene を今ここで破棄)。フレーム
    // インフライト 2 + 1 で「直前 2 フレームを GPU が参照中でも安全」を保つ。
    m_RetireHead = (m_RetireHead + 1u) % kRetireRingSize;
    m_Retired[m_RetireHead].Reset();

    EOp op = m_PendingOp;
    m_PendingOp = EOp::None;
    TUniquePtr<FScene> arg = Move(m_PendingArg);

    switch (op) {
    case EOp::None:
        return;
    case EOp::Change:
        // Pop + Push の合成 = 旧 top を OnExit、新 top を OnEnter。途中で
        // OnResume/OnPause が一瞬走らないように両方 false を渡す。
        DoPopInternal(/*resume_new=*/false);
        DoPushInternal(game, Move(arg), /*pause_current=*/false);
        break;
    case EOp::Push:
        // 新 top が乗るので、旧 top を OnPause する。
        DoPushInternal(game, Move(arg), /*pause_current=*/true);
        break;
    case EOp::Pop:
        if (m_Stack.Size() <= 1) {
            ACS_LOG_WARN("FSceneManager::PopScene on a stack of size %u (need >=2) — ignored",
                         static_cast<u32>(m_Stack.Size()));
            return;
        }
        // 新しく top に戻るシーンを OnResume する。
        DoPopInternal(/*resume_new=*/true);
        break;
    }
}

/** top シーンの Input サービスへ一フレーム分の入力を提出する。 */
bool FSceneManager::SubmitFrameInput_Internal(const IInputStateView& input) noexcept
{
    FScene* top = Top();
    if (top == nullptr) return true;
    FSceneServices* services = top->_ServicesOrNull();
    return services == nullptr || services->UpdateAccess().SubmitFrameInput(input);
}

/** top シーンの未消費固定入力を初期化する。 */
void FSceneManager::ResetFixedInput_Internal() noexcept
{
    FScene* top = Top();
    if (top == nullptr) return;
    FSceneServices* services = top->_ServicesOrNull();
    if (services != nullptr) services->UpdateAccess().ResetFixedInput();
}

/** active scene の未消費固定入力を保存値へ複製する。 */
bool FSceneManager::TryCaptureActiveFixedInputSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept
{
    FFixedStepInputBufferSnapshot candidate{};
    FScene* top = Top();
    if (top == nullptr || !top->HasServices()) {
        snapshot = candidate;
        return true;
    }

    FSceneServices& services = top->Services();
    if (!services.Has(ESvc::Input)) {
        snapshot = candidate;
        return true;
    }
    if (!services.UpdateAccess().TryCaptureFixedInputSnapshot(candidate)) return false;
    snapshot = candidate;
    return true;
}

/** active scene の未消費固定入力を保存値から復元する。 */
bool FSceneManager::TryRestoreActiveFixedInputSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept
{
    FScene* top = Top();
    if (top == nullptr || !top->HasServices()) return !snapshot.has_input_state;
    FSceneServices& services = top->Services();
    if (!services.Has(ESvc::Input)) return !snapshot.has_input_state;
    return services.UpdateAccess().TryRestoreFixedInputSnapshot(snapshot);
}

/** top のシーンを services の 2 phase tick (Pre→OnUpdate→Post) で駆動する。 */
void FSceneManager::Update_Internal(f32 dt) noexcept
{
    FScene* top = Top();
    if (!top) return;
    // services 2 phase tick — PreUpdate (Clock 進行) → scene OnUpdate
    // → PostUpdate (Tweens/Sequences tick)。Clock 未要求なら raw dt をそのまま OnUpdate へ。
    FSceneServices* svc = top->_ServicesOrNull();
    if (svc != nullptr) {
        FSceneServices::FUpdateAdapter service_update = svc->UpdateAccess();
        service_update.PreUpdate(dt);
        const f32 scaled = service_update.ScaledDt(dt);
        top->_TickWorldSubsystems(scaled); // World サブシステムを OnUpdate より先に tick
        top->OnUpdate(scaled);
        service_update.PostUpdate(scaled);
    } else {
        top->_TickWorldSubsystems(dt);
        top->OnUpdate(dt);
    }
}

/** top のシーンに固定刻み update を流す。 */
void FSceneManager::FixedUpdate_Internal(f32 fixed_dt) noexcept
{
    FScene* top = Top();
    if (!top) return;
    FSceneServices* services = top->_ServicesOrNull();
    if (services != nullptr) services->UpdateAccess().BeginFixedStepInput();
    top->OnFixedUpdate(fixed_dt);
}

/** top のシーンを描画する。 */
void FSceneManager::Render_Internal(FRenderContext& rc) noexcept
{
    FScene* top = Top();
    if (!top) return;
    top->OnRender(rc);
}

/** 受け取った Event を top のシーンへ配送する。 */
void FSceneManager::DispatchEvent_Internal(const FEvent& e) noexcept
{
    FScene* top = Top();
    if (!top) return;
    top->OnEvent(e);
}

/** 残った全シーンを top から OnExit して破棄し、保留状態をリセットする。 */
void FSceneManager::ShutdownAll_Internal() noexcept
{
    // top から順に OnExit を呼んでから破棄。
    while (!m_Stack.IsEmpty()) {
        m_Stack.Back()->OnExit();
        m_Stack.Back()->_DeinitWorldSubsystems(); // World サブシステムも解体
        m_Stack.PopBack();
        AdvanceActiveSceneEpoch_Internal();
    }
    for (u32 i = 0; i < kRetireRingSize; ++i) {
        m_Retired[i].Reset();
    }
    m_PendingArg.Reset();
    m_PendingOp = EOp::None;
}

} // namespace acs::game
