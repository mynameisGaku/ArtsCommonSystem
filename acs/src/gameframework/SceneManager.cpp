// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CSceneManager 実装
#include "gameframework/SceneManager.h"
#include "gameframework/Scene.h"
#include "gameframework/Game.h"
#include "gameframework/RenderContext.h"

#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** Change 遷移を保留にセットする (next==nullptr は警告して無視)。 */
void CSceneManager::ChangeScene(TUniquePtr<AScene> next) noexcept {
    if (!next) {
        ACS_LOG_WARN("FSceneManager::ChangeScene(nullptr) ignored");
        return;
    }
    m_PendingOp  = EOp::Change;
    m_PendingArg = Move(next);
}

/** Push 遷移を保留にセットする (next==nullptr は警告して無視)。 */
void CSceneManager::PushScene(TUniquePtr<AScene> next) noexcept {
    if (!next) {
        ACS_LOG_WARN("FSceneManager::PushScene(nullptr) ignored");
        return;
    }
    m_PendingOp  = EOp::Push;
    m_PendingArg = Move(next);
}

/** Pop 遷移を保留にセットする。 */
void CSceneManager::PopScene() noexcept {
    m_PendingOp = EOp::Pop;
    m_PendingArg.Reset();
}

/** top のシーンを返す (空なら nullptr)。 */
AScene* CSceneManager::Top() const noexcept {
    if (m_Stack.IsEmpty()) return nullptr;
    return m_Stack.Back().Get();
}

/** スタックの深さを返す。 */
u32 CSceneManager::Depth() const noexcept {
    return static_cast<u32>(m_Stack.Size());
}

/** scene を可視化せず、context/services/World サブシステムを全て準備する。 */
bool CSceneManager::PrepareScene(CGame& Game, AScene& Scene) noexcept
{
    if (!Scene._CanPrepare()) return false;
    Scene._SetContext(&Game, this);
    // WantedServices に基づいて CSceneServices を構築・attach。
    // None なら空の CSceneServices だが、Services() を呼ばない限りコスト 0。
    const ESvc Wanted = Scene.WantedServices();
    if (Wanted != ESvc::None) {
        /** scene が要求したサービス束。 */
        TUniquePtr<CSceneServices> Services = MakeUnique<CSceneServices>(Wanted);
        if (!Services || !Services->IsReady()) return false;
        Scene._AttachServices(Move(Services));
    }
    return Scene._InitWorldSubsystems(&Game.GameInstanceSubsystems());
}

/** 準備済み scene だけを stack へ commit し、OnEnter を呼ぶ。 */
bool CSceneManager::CommitPush(TUniquePtr<AScene> Scene, bool PauseCurrent) noexcept
{
    if (!Scene) return false;
    if (PauseCurrent && !m_Stack.IsEmpty()) m_Stack.Back()->OnPause();
    // _ApplyPending が必要容量を事前確保済みなので、ここでは割り当てが発生しない。
    if (!m_Stack.TryPushBack(Move(Scene))) return false;
    m_Stack.Back()->OnEnter();
    return true;
}

/** 内部 pop: top を OnExit して ring buffer へ退避し、任意で新 top を OnResume する。 */
void CSceneManager::DoPopInternal(bool resume_new) noexcept {
    if (m_Stack.IsEmpty()) return;
    m_Stack.Back()->OnExit();
    m_Stack.Back()->_DeinitWorldSubsystems();   // OnExit 後に World サブシステムを解体
    // 退場 Scene を ring buffer の現在ヘッドに格納 (3 フレーム保持)。
    // ヘッドは _ApplyPending の冒頭で前進 + 古いスロット解放済。
    m_Retired[m_RetireHead] = Move(m_Stack.Back());
    m_Stack.PopBack();
    // 新 top を OnResume (Pop 時のみ。Change は次の Push が走るので skip)
    if (resume_new && !m_Stack.IsEmpty()) {
        m_Stack.Back()->OnResume();
    }
}

/** ring buffer を前進させて古い退場 Scene を破棄し、保留中の遷移を適用する。 */
void CSceneManager::_ApplyPending(CGame& game) noexcept {
    // GPU N+1 frame 遅延削除: ring を 1 つ前進し、新ヘッド位置のスロットを
    // 解放する (= 3 フレーム前に退場した Scene を今ここで破棄)。フレーム
    // インフライト 2 + 1 で「直前 2 フレームを GPU が参照中でも安全」を保つ。
    m_RetireHead = (m_RetireHead + 1u) % kRetireRingSize;
    m_Retired[m_RetireHead].Reset();

    EOp op = m_PendingOp;
    m_PendingOp = EOp::None;
    TUniquePtr<AScene> arg = Move(m_PendingArg);

    switch (op) {
    case EOp::None:
        return;
    case EOp::Change:
        // 旧topを残したまま、置換後の要素数を先に収容できることを保証する。
        if (!m_Stack.TryReserve(m_Stack.IsEmpty() ? 1u : m_Stack.Size())) {
            ACS_LOG_ERROR("FSceneManager: change scene stack allocation failed");
            return;
        }
        // 新 scene の準備に失敗した場合は旧 top とその pause 状態を完全に維持する。
        if (!arg || !PrepareScene(game, *arg)) {
            ACS_LOG_ERROR("FSceneManager: change scene preparation failed");
            return;
        }
        DoPopInternal(/*resume_new=*/false);
        if (!CommitPush(Move(arg), /*PauseCurrent=*/false)) {
            ACS_LOG_ERROR("FSceneManager: reserved change scene commit failed");
        }
        break;
    case EOp::Push:
        // pause前に新topを格納する容量を確保し、OOM時は旧topを変更しない。
        if (!m_Stack.TryReserve(m_Stack.Size() + 1u)) {
            ACS_LOG_ERROR("FSceneManager: push scene stack allocation failed");
            return;
        }
        // 準備成功後だけ旧 top を pause する。
        if (!arg || !PrepareScene(game, *arg)) {
            ACS_LOG_ERROR("FSceneManager: push scene preparation failed");
            return;
        }
        if (!CommitPush(Move(arg), /*PauseCurrent=*/true)) {
            ACS_LOG_ERROR("FSceneManager: reserved push scene commit failed");
        }
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

/** top のシーンを services の 2 phase tick (Pre→OnUpdate→Post) で駆動する。 */
void CSceneManager::_Update(f32 ScaledDeltaSeconds, f32 UnscaledDeltaSeconds,
                            u64 FrameNumber) noexcept {
    AScene* top = Top();
    if (!top) return;
    // services 2 phase tick — PreUpdate (Clock 進行) → scene OnUpdate
    // → PostUpdate (Tweens/Sequences tick)。Clock 未要求なら raw dt をそのまま OnUpdate へ。
    CSceneServices* svc = top->_ServicesOrNull();
    f32 WorldDeltaSeconds = ScaledDeltaSeconds;
    if (svc != nullptr) {
        svc->_PreUpdate(ScaledDeltaSeconds);
        WorldDeltaSeconds = svc->_ScaledDt(ScaledDeltaSeconds);
    }
    /** World の更新前コンテキスト。 */
    const FSubsystemFrameContext PreContext{
        WorldDeltaSeconds, UnscaledDeltaSeconds, FrameNumber, ESubsystemTickPhase::PreUpdate};
    top->_TickWorldSubsystems(PreContext);
    top->OnUpdate(WorldDeltaSeconds);
    if (svc != nullptr) svc->_PostUpdate(WorldDeltaSeconds);
    /** World の更新後コンテキスト。 */
    const FSubsystemFrameContext PostContext{
        WorldDeltaSeconds, UnscaledDeltaSeconds, FrameNumber, ESubsystemTickPhase::PostUpdate};
    top->_TickWorldSubsystems(PostContext);
}

/** top のシーンに固定刻み update を流す。 */
void CSceneManager::_FixedUpdate(f32 fixed_dt) noexcept {
    AScene* top = Top();
    if (!top) return;
    top->OnFixedUpdate(fixed_dt);
}

/** top のシーンを描画する。 */
void CSceneManager::_Render(FRenderContext& rc) noexcept {
    AScene* top = Top();
    if (!top) return;
    top->OnRender(rc);
}

/** 受け取った Event を top のシーンへ配送する。 */
void CSceneManager::_DispatchEvent(const FEvent& e) noexcept {
    AScene* top = Top();
    if (!top) return;
    top->OnEvent(e);
}

/** 残った全シーンを top から OnExit して破棄し、保留状態をリセットする。 */
void CSceneManager::_ShutdownAll() noexcept {
    // top から順に OnExit を呼んでから破棄。
    while (!m_Stack.IsEmpty()) {
        m_Stack.Back()->OnExit();
        m_Stack.Back()->_DeinitWorldSubsystems();   // World サブシステムも解体
        m_Stack.PopBack();
    }
    for (u32 i = 0; i < kRetireRingSize; ++i) {
        m_Retired[i].Reset();
    }
    m_PendingArg.Reset();
    m_PendingOp = EOp::None;
}

} // namespace acs::game
