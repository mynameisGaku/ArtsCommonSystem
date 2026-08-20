// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: サブシステム基盤の検証 (GPU 非依存)。
//   ・CSubsystemCollection が登録簿から «該当スコープ» のサブシステムを全生成し
//     OnInitialize する
//   ・Get<T>() が自スコープ → 上位スコープ (World→GameInstance→Engine) へフォール
//     バック検索する。上位から下位は «見えない»
//   ・OnTick がフレーム毎に呼ばれ、OnDeinitialize が解体時に呼ばれる
//   ・ANode / AComponent の GetSubsystem<T>() が root 配線を walk-to-root で
//     解決する (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemRegistry.h"
#include "gameframework/SubsystemCollection.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/Game.h"
#include "gameframework/Scene.h"
#include "gameframework/SubsystemCatalog.h"
#include "gameframework/EventBus.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/WorldClockSubsystem.h"
#include "subsystem/SubsystemFrameContext.h"
#include "memory/UniquePtr.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cstring>

using namespace acs;
using namespace acs::game;

namespace {

static_assert(sizeof(CSubsystemCollection) == 80u);
static_assert(alignof(CSubsystemCollection) == 8u);

/** collectionが保持する全確保の解放を数えるtest allocator。 */
class CSubsystemCountingAllocator final : public IAllocator {
public:
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        ++allocation_calls;
        if (fail_on_allocation != 0u && allocation_calls == fail_on_allocation) return nullptr;
        void* const Pointer = m_Backing.Alloc(Size, Alignment, Location);
        if (Pointer != nullptr) ++outstanding_allocations;
        return Pointer;
    }

    void Free(void* Pointer) noexcept override
    {
        if (Pointer == nullptr) return;
        --outstanding_allocations;
        m_Backing.Free(Pointer);
    }

    const char* Name() const noexcept override { return "SubsystemCounting"; }

    /** 現在解放されていない確保数。 */
    u32 outstanding_allocations = 0u;
    /** Allocを呼んだ回数。 */
    u32 allocation_calls = 0u;
    /** 1始まりで失敗させるAlloc番号。0は失敗なし。 */
    u32 fail_on_allocation = 0u;

private:
    CSystemAllocator m_Backing;
};

/** test中だけ既定allocatorを差し替える。 */
class CSubsystemDefaultAllocatorScope {
public:
    explicit CSubsystemDefaultAllocatorScope(IAllocator& Allocator) noexcept
        : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&Allocator);
    }

    ~CSubsystemDefaultAllocatorScope() noexcept { SetDefaultAllocator(m_Previous); }

private:
    IAllocator* m_Previous = nullptr;
};

/** Free中のcollection再入を一度だけ注入するallocator。 */
class CSubsystemReentrantFreeAllocator final : public IAllocator {
public:
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        void* const Pointer = m_Backing.Alloc(Size, Alignment, Location);
        if (Pointer != nullptr) ++outstanding_allocations;
        return Pointer;
    }

    void Free(void* Pointer) noexcept override
    {
        if (Pointer == nullptr) return;
        if (armed && !triggered && collection != nullptr && collection->Count() > 0u) {
            triggered = true;
            nested_initialize_result = collection->TryInitialize(
                ESubsystemScope::Engine, nullptr, owner);
            collection->Deinitialize();
        }
        --outstanding_allocations;
        m_Backing.Free(Pointer);
    }

    const char* Name() const noexcept override { return "SubsystemReentrantFree"; }

    /** 再入先collection。 */
    CSubsystemCollection* collection = nullptr;
    /** nested初期化へ渡すowner。 */
    FSubsystemOwner owner{};
    /** Free再入を有効にする。 */
    bool armed = false;
    /** 再入を実行済みならtrue。 */
    bool triggered = false;
    /** nested初期化の結果。 */
    bool nested_initialize_result = true;
    /** 現在解放されていない確保数。 */
    u32 outstanding_allocations = 0u;

private:
    CSystemAllocator m_Backing;
};

// World スコープ: スコア管理 (オブジェクト間のやり取りのハブを想定)。
class CScoreSub : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CScoreSub)
    int score  = -999;
    int ticks  = 0;
    int deinit = 0;
    void OnInitialize()   noexcept override { score = 0; }
    void OnDeinitialize() noexcept override { ++deinit; }
    void OnTick(f32)      noexcept override { ++ticks; }
    void Add(int n)       noexcept { score += n; }
};

// GameInstance スコープ: シーン跨ぎの設定 (シングルトン)。
class CConfigSub : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CConfigSub)
    int volume = 0;
    void OnInitialize() noexcept override { volume = 50; }
};

bool g_DestructorParentVisible = false;
u32 g_DestructorProbeDeinitializeCount = 0u;

class CDestructorParentProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CDestructorParentProbe)
    void OnDeinitialize() noexcept override
    {
        AScene* const Scene = OwnerAs<AScene>();
        g_DestructorParentVisible = Scene != nullptr && Scene->GetSubsystem<CConfigSub>() != nullptr;
        ++g_DestructorProbeDeinitializeCount;
    }
};

TUniquePtr<ASubsystem> CreateDestructorParentProbe() noexcept
{
    return MakeUnique<CDestructorParentProbe>();
}

// Engine スコープ: アプリ全体寿命。
class CEngineSub : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CEngineSub)
    bool inited = false;
    void OnInitialize() noexcept override { inited = true; }
};

// コンポーネントからサブシステムを引いて使うテストコンポーネント。
struct AProbeComponent : public AComponent {
    ACS_GAME_COMPONENT_KIND(AProbeComponent)
};

// OnInitialize 時点で Owner() を読み取って記録するサブシステム (World)。
class COwnerSub : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(COwnerSub)
    void* seenOwner = reinterpret_cast<void*>(0x1);   // 初期値は «未設定» と区別できる値
    void OnInitialize() noexcept override { seenOwner = Owner(); }
};

class CDynamicSubsystemOne : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CDynamicSubsystemOne)
};

class CDynamicSubsystemTwo : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CDynamicSubsystemTwo)
};

TUniquePtr<ASubsystem> CreateDynamicSubsystemOne() noexcept
{
    return MakeUnique<CDynamicSubsystemOne>();
}

TUniquePtr<ASubsystem> CreateDynamicSubsystemTwo() noexcept
{
    return MakeUnique<CDynamicSubsystemTwo>();
}

const FSubsystemFactory* FindFactoryByKind(const CSubsystemRegistry& registry, const void* kind) noexcept
{
    for (u32 index = 0; index < registry.Count(); ++index) {
        if (registry.At(index).kind == kind) return &registry.At(index);
    }
    return nullptr;
}

/** lifecycle/phase の呼出順を記録する固定長トレース。 */
struct FSubsystemTrace {
    char values[32]{};
    u32 count = 0;

    void Add(char value) noexcept
    {
        if (count < 32u) values[count++] = value;
    }
};

/** 再入テストから対象 collection を参照する owner。 */
struct FSubsystemReentryOwner {
    CSubsystemCollection* collection = nullptr;
    FSubsystemTrace* trace = nullptr;
};

FSubsystemTrace* g_SubsystemTrace = nullptr;

/** SceneServices→World→GameInstance→Engineの更新順とcontextを記録する。 */
struct FPhaseOrderData {
    FSubsystemTrace trace{};
    f32 scaled_delta_seconds = 0.0f;
    f32 unscaled_delta_seconds = 0.0f;
    u64 frame_number = 0u;
    bool world_context_matches = true;
    bool game_context_matches = true;
    bool engine_context_matches = true;
};

FPhaseOrderData* g_PhaseOrderData = nullptr;

/** contextの時刻とframeが期待値に一致するかを判定する。 */
bool PhaseContextMatches(
    const FSubsystemFrameContext& Context, f32 Scaled, f32 Unscaled, u64 Frame) noexcept
{
    return Context.scaled_delta_seconds == Scaled &&
           Context.unscaled_delta_seconds == Unscaled && Context.frame_number == Frame;
}

class CWorldParticleTraceSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CWorldParticleTraceSubsystem)
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        FPhaseOrderData& Data = *g_PhaseOrderData;
        Data.world_context_matches = Data.world_context_matches && PhaseContextMatches(
            Context, Data.scaled_delta_seconds, Data.unscaled_delta_seconds, Data.frame_number);
        AScene* const Scene = OwnerAs<AScene>();
        const bool CameraUpdated = Scene != nullptr && Scene->Services().Camera().Position().x == 10.0f;
        Data.trace.Add(CameraUpdated ? 'C' : '!');
        Data.trace.Add('P');
    }
};

class CWorldEffectTraceSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CWorldEffectTraceSubsystem)
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        FPhaseOrderData& Data = *g_PhaseOrderData;
        Data.world_context_matches = Data.world_context_matches && PhaseContextMatches(
            Context, Data.scaled_delta_seconds, Data.unscaled_delta_seconds, Data.frame_number);
        Data.trace.Add('E');
    }
};

class CGamePostTraceSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CGamePostTraceSubsystem)
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        FPhaseOrderData& Data = *g_PhaseOrderData;
        Data.game_context_matches = Data.game_context_matches && PhaseContextMatches(
            Context, Data.scaled_delta_seconds, Data.unscaled_delta_seconds, Data.frame_number);
        Data.trace.Add('G');
    }
};

class CEnginePostTraceSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CEnginePostTraceSubsystem)
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        FPhaseOrderData& Data = *g_PhaseOrderData;
        Data.engine_context_matches = Data.engine_context_matches && PhaseContextMatches(
            Context, Data.unscaled_delta_seconds, Data.unscaled_delta_seconds, Data.frame_number);
        Data.trace.Add('N');
    }
};

TUniquePtr<ASubsystem> CreateWorldParticleTraceSubsystem() noexcept
{
    return MakeUnique<CWorldParticleTraceSubsystem>();
}

TUniquePtr<ASubsystem> CreateWorldEffectTraceSubsystem() noexcept
{
    return MakeUnique<CWorldEffectTraceSubsystem>();
}

TUniquePtr<ASubsystem> CreateGamePostTraceSubsystem() noexcept
{
    return MakeUnique<CGamePostTraceSubsystem>();
}

TUniquePtr<ASubsystem> CreateEnginePostTraceSubsystem() noexcept
{
    return MakeUnique<CEnginePostTraceSubsystem>();
}

class COrderedSubsystemA final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(COrderedSubsystemA)
    void OnInitialize() noexcept override { g_SubsystemTrace->Add('A'); }
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        g_SubsystemTrace->Add(Context.phase == ESubsystemTickPhase::PreUpdate ? 'a' : 'x');
    }
    void OnDeinitialize() noexcept override { g_SubsystemTrace->Add('D'); }
};

class COrderedSubsystemB final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(COrderedSubsystemB)
    void OnInitialize() noexcept override { g_SubsystemTrace->Add('B'); }
    void OnTickFrame(const FSubsystemFrameContext& Context) noexcept override
    {
        g_SubsystemTrace->Add(Context.phase == ESubsystemTickPhase::PostUpdate ? 'b' : 'x');
    }
    void OnDeinitialize() noexcept override { g_SubsystemTrace->Add('E'); }
};

class CCancelTickSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CCancelTickSubsystem)
    void OnTick(f32) noexcept override
    {
        FSubsystemReentryOwner* const Reentry = OwnerAs<FSubsystemReentryOwner>();
        Reentry->trace->Add('T');
        Reentry->collection->Deinitialize();
    }
    void OnDeinitialize() noexcept override
    {
        OwnerAs<FSubsystemReentryOwner>()->trace->Add('C');
    }
};

class CSkippedTickSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CSkippedTickSubsystem)
    void OnTick(f32) noexcept override { OwnerAs<FSubsystemReentryOwner>()->trace->Add('S'); }
    void OnDeinitialize() noexcept override
    {
        OwnerAs<FSubsystemReentryOwner>()->trace->Add('K');
    }
};

TUniquePtr<ASubsystem> CreateOrderedSubsystemA() noexcept { return MakeUnique<COrderedSubsystemA>(); }
TUniquePtr<ASubsystem> CreateOrderedSubsystemB() noexcept { return MakeUnique<COrderedSubsystemB>(); }
TUniquePtr<ASubsystem> CreateCancelTickSubsystem() noexcept { return MakeUnique<CCancelTickSubsystem>(); }
TUniquePtr<ASubsystem> CreateSkippedTickSubsystem() noexcept { return MakeUnique<CSkippedTickSubsystem>(); }

u32 g_DuplicateFactoryCreateCount = 0u;

TUniquePtr<ASubsystem> CreateDuplicateOrderedSubsystemA() noexcept
{
    ++g_DuplicateFactoryCreateCount;
    return MakeUnique<CDynamicSubsystemOne>();
}

TUniquePtr<ASubsystem> CreateDuplicateOrderedSubsystemB() noexcept
{
    ++g_DuplicateFactoryCreateCount;
    return MakeUnique<CDynamicSubsystemTwo>();
}

u32 g_OwnerProbeInitializeCount = 0u;

class COwnerValidationProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(COwnerValidationProbe)
    void OnInitialize() noexcept override { ++g_OwnerProbeInitializeCount; }
};

TUniquePtr<ASubsystem> CreateOwnerValidationProbe() noexcept
{
    return MakeUnique<COwnerValidationProbe>();
}

TUniquePtr<ASubsystem> CreateEmptySubsystem() noexcept
{
    return {};
}

u32 g_ParentFactoryCreateCount = 0u;

class CParentValidationProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CParentValidationProbe)
};

TUniquePtr<ASubsystem> CreateParentValidationProbe() noexcept
{
    ++g_ParentFactoryCreateCount;
    return MakeUnique<CParentValidationProbe>();
}

/** parent tick中のchild初期化結果を記録する。 */
struct FTickParentContext {
    CSubsystemCollection* parent = nullptr;
    CSubsystemCollection* child = nullptr;
    bool attempted = false;
    bool initialized = true;
};

class CTickParentInitializer final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CTickParentInitializer)
    void OnTick(f32) noexcept override
    {
        FTickParentContext* const Context = OwnerAs<FTickParentContext>();
        Context->attempted = true;
        Context->initialized = Context->child->TryInitialize(
            ESubsystemScope::GameInstance, Context->parent,
            FSubsystemOwner{Context, ESubsystemOwnerKind::Game});
    }
};

TUniquePtr<ASubsystem> CreateTickParentInitializer() noexcept
{
    return MakeUnique<CTickParentInitializer>();
}

/** Scene遷移の準備/commit境界を記録する。 */
struct FSceneTransitionTrace {
    u32 entered = 0u;
    u32 paused = 0u;
    u32 exited = 0u;
};

/** cameraを要求しないsceneの共通lifecycle配線を記録する。 */
struct FSceneRootLifecycleTrace {
    /** 利用者OnEnterを呼んだ。 */
    bool entered = false;
    /** OnEnterより前にrootへserviceを配線した。 */
    bool root_services_wired = false;
    /** 既存componentへserviceを通知した。 */
    bool component_services_attached = false;
    /** 利用者OnExitを呼んだ。 */
    bool exited = false;
    /** 利用者OnUpdateの呼出回数。 */
    u32 scene_updates = 0u;
    /** 利用者OnFixedUpdateの呼出回数。 */
    u32 scene_fixed_updates = 0u;
    /** root componentのOnUpdate呼出回数。 */
    u32 component_updates = 0u;
    /** root componentのOnFixedUpdate呼出回数。 */
    u32 component_fixed_updates = 0u;
};

/** root treeの自動更新とservice通知を観測するcomponent。 */
class ARootLifecycleProbeComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ARootLifecycleProbeComponent)

    explicit ARootLifecycleProbeComponent(FSceneRootLifecycleTrace* Trace) noexcept : m_Trace(Trace) {}

    void OnAttachServices(CSceneServices&) noexcept override
    {
        m_Trace->component_services_attached = true;
    }

    void OnUpdate(f32) noexcept override { ++m_Trace->component_updates; }
    void OnFixedUpdate(f32) noexcept override { ++m_Trace->component_fixed_updates; }

private:
    /** 呼出側が所有する観測先。 */
    FSceneRootLifecycleTrace* m_Trace = nullptr;
};

/** Cameraを要求せず、Clock・Physics2D・Tweensを使うlifecycle検査scene。 */
class ANonCameraLifecycleScene final : public AScene {
public:
    explicit ANonCameraLifecycleScene(FSceneRootLifecycleTrace* Trace) noexcept : m_Trace(Trace)
    {
        Root().AddComponent<ARootLifecycleProbeComponent>(Trace);
        /** 退場時の構造変更解決を検査する子node。 */
        ANode& Child = Root().AddChild(NewObject<ANode>());
        m_Child = &Child;
    }

    ESvc WantedServices() const noexcept override
    {
        return ESvc::Clock | ESvc::Physics2D | ESvc::Tweens;
    }

    void OnEnter() noexcept override
    {
        m_Trace->entered = true;
        m_Trace->root_services_wired = Root().SceneServices() == &Services();
        Services().Physics().AddAabb(
            FAabb2{FVec2{0.0f, 0.0f}, FVec2{1.0f, 1.0f}});
        Services().Tweens().Tween(&m_TweenValue, 0.0f, 1.0f, 10.0f);
    }

    void OnUpdate(f32) noexcept override { ++m_Trace->scene_updates; }
    void OnFixedUpdate(f32) noexcept override { ++m_Trace->scene_fixed_updates; }
    void OnExit() noexcept override
    {
        m_Trace->exited = true;
        m_Child->Destroy();
    }

private:
    /** 呼出側が所有する観測先。 */
    FSceneRootLifecycleTrace* m_Trace = nullptr;
    /** 退場時に破棄予約する子node。 */
    ANode* m_Child = nullptr;
    /** 退場時のtween一括解除を検査する値。 */
    f32 m_TweenValue = 0.0f;
};

class ATransitionProbeScene final : public AScene {
public:
    explicit ATransitionProbeScene(FSceneTransitionTrace* Trace) noexcept : m_Trace(Trace) {}
    void OnEnter() noexcept override { ++m_Trace->entered; }
    void OnPause() noexcept override { ++m_Trace->paused; }
    void OnExit() noexcept override { ++m_Trace->exited; }

private:
    FSceneTransitionTrace* m_Trace = nullptr;
};

/** 全service生成失敗時のscene可視化状態を記録する。 */
struct FServiceAllocationTrace {
    bool destroyed = false;
    bool had_services = false;
    u32 entered = 0u;
};

/** 全serviceを要求し、失敗候補がattachされなかったことを記録する。 */
class AAllServicesProbeScene final : public AScene {
public:
    explicit AAllServicesProbeScene(FServiceAllocationTrace* Trace) noexcept : m_Trace(Trace) {}
    ~AAllServicesProbeScene() noexcept override
    {
        m_Trace->destroyed = true;
        m_Trace->had_services = HasServices();
    }

    ESvc WantedServices() const noexcept override
    {
        return ESvc::Clock | ESvc::Tweens | ESvc::Sequences | ESvc::Input |
               ESvc::Camera2D | ESvc::Physics2D | ESvc::Triggers;
    }

    void OnEnter() noexcept override { ++m_Trace->entered; }

private:
    /** 呼出側が所有する観測先。 */
    FServiceAllocationTrace* m_Trace = nullptr;
};

/** root OOM候補の可視化状態を記録する。 */
struct FRootAllocationTrace {
    /** candidateを破棄した。 */
    bool destroyed = false;
    /** OnEnter呼出回数。 */
    u32 entered = 0u;
};

/** scene root生成失敗がcommitされないことを記録する。 */
class ARootAllocationProbeScene final : public AScene {
public:
    explicit ARootAllocationProbeScene(FRootAllocationTrace* Trace) noexcept : m_Trace(Trace) {}
    ~ARootAllocationProbeScene() noexcept override { m_Trace->destroyed = true; }
    void OnEnter() noexcept override { ++m_Trace->entered; }

private:
    /** 呼出側が所有する観測先。 */
    FRootAllocationTrace* m_Trace = nullptr;
};

/** World hook再入とrollback順序を記録する。 */
struct FWorldHookReentryTrace {
    /** Worldのparent。 */
    CSubsystemCollection* parent = nullptr;
    /** Worldを同一構成で再初期化する。 */
    bool reinitialize_world = false;
    /** World再初期化の結果。 */
    bool world_reinitialize_result = false;
    /** candidate OnEnter回数。 */
    u32 entered = 0u;
    /** lifecycle callback順序。 */
    char callbacks[16]{};
    /** callback記録数。 */
    u32 callback_count = 0u;

    void Record(char Callback) noexcept { callbacks[callback_count++] = Callback; }
};

/** 現在のWorld hook観測先。 */
FWorldHookReentryTrace* g_WorldHookReentryTrace = nullptr;

class CWorldHookProbeA final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CWorldHookProbeA)
    void OnInitialize() noexcept override { g_WorldHookReentryTrace->Record('A'); }
    void OnDeinitialize() noexcept override { g_WorldHookReentryTrace->Record('a'); }
};

class CWorldHookProbeB final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CWorldHookProbeB)
    void OnInitialize() noexcept override { g_WorldHookReentryTrace->Record('B'); }
    void OnDeinitialize() noexcept override { g_WorldHookReentryTrace->Record('b'); }
};

TUniquePtr<ASubsystem> CreateWorldHookProbeA() noexcept
{
    return MakeUnique<CWorldHookProbeA>();
}

TUniquePtr<ASubsystem> CreateWorldHookProbeB() noexcept
{
    return MakeUnique<CWorldHookProbeB>();
}

/** World準備hook内で終了または同一構成再初期化を行うscene。 */
class AWorldHookReentryScene final : public AScene {
public:
    explicit AWorldHookReentryScene(FWorldHookReentryTrace* Trace) noexcept : m_Trace(Trace) {}
    void OnEnter() noexcept override { ++m_Trace->entered; }

protected:
    void OnWorldSubsystemsReady_Internal() noexcept override
    {
        CSubsystemCollection* const World = WorldSubsystemsPtr_Internal();
        World->Deinitialize();
        if (m_Trace->reinitialize_world) {
            m_Trace->world_reinitialize_result = World->TryInitialize(
                ESubsystemScope::World, m_Trace->parent,
                FSubsystemOwner{this, ESubsystemOwnerKind::Scene});
        }
    }

private:
    /** 呼出側が所有する観測先。 */
    FWorldHookReentryTrace* m_Trace = nullptr;
};

/** factory再入の観測状態。 */
struct FFactoryReentryTrace {
    /** nested初期化の対象。 */
    CSubsystemCollection* collection = nullptr;
    /** nested初期化へ渡すowner。 */
    FSubsystemOwner owner{};
    /** factory create回数。 */
    u32 create_count = 0u;
    /** lifecycle callback回数。 */
    u32 callback_count = 0u;
    /** destructor再入回数。 */
    u32 destructor_count = 0u;
    /** nested初期化を試みたか。 */
    bool nested_attempted = false;
    /** nested初期化の結果。 */
    bool nested_result = true;
};

/** 現在のfactory再入観測先。 */
FFactoryReentryTrace* g_FactoryReentryTrace = nullptr;

class CFactoryReentryProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CFactoryReentryProbe)
    bool OnOwnerAssigned() noexcept override
    {
        ++g_FactoryReentryTrace->callback_count;
        return true;
    }
};

TUniquePtr<ASubsystem> CreateFactoryReentryProbe() noexcept
{
    FFactoryReentryTrace& Trace = *g_FactoryReentryTrace;
    ++Trace.create_count;
    Trace.nested_attempted = true;
    Trace.nested_result = Trace.collection->TryInitialize(
        ESubsystemScope::Engine, nullptr, Trace.owner);
    return {};
}

/** Kind virtual内からnested初期化を試みるprobe。 */
class CKindFactoryReentryProbe final : public ASubsystem {
public:
    const void* Kind() const noexcept override
    {
        FFactoryReentryTrace& Trace = *g_FactoryReentryTrace;
        Trace.nested_attempted = true;
        Trace.nested_result = Trace.collection->TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace.owner);
        return SubsystemKindOf<CFactoryReentryProbe>();
    }
};

TUniquePtr<ASubsystem> CreateKindFactoryReentryProbe() noexcept
{
    ++g_FactoryReentryTrace->create_count;
    return MakeUnique<CKindFactoryReentryProbe>();
}

/** staged破棄中からnested初期化を試みるprobe。 */
class CDestructorFactoryReentryProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CDestructorFactoryReentryProbe)
    ~CDestructorFactoryReentryProbe() noexcept override
    {
        FFactoryReentryTrace& Trace = *g_FactoryReentryTrace;
        ++Trace.destructor_count;
        Trace.nested_attempted = true;
        Trace.nested_result = Trace.collection->TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace.owner);
    }
};

TUniquePtr<ASubsystem> CreateDestructorFactoryReentryProbe() noexcept
{
    ++g_FactoryReentryTrace->create_count;
    return MakeUnique<CDestructorFactoryReentryProbe>();
}

TUniquePtr<ASubsystem> CreateFailureAfterStagedProbe() noexcept
{
    ++g_FactoryReentryTrace->create_count;
    return {};
}

/** lookupがKind virtualを再呼出ししないことを観測する。 */
struct FStoredKindTrace {
    /** lookup対象collection。 */
    CSubsystemCollection* collection = nullptr;
    /** Kind呼出回数。 */
    u32 kind_calls = 0u;
    /** Kindから終了要求を注入する。 */
    bool deinitialize_on_kind = false;
};

/** 現在のKind lookup観測先。 */
FStoredKindTrace* g_StoredKindTrace = nullptr;

class CStoredKindProbe final : public ASubsystem {
public:
    const void* Kind() const noexcept override
    {
        ++g_StoredKindTrace->kind_calls;
        if (g_StoredKindTrace->deinitialize_on_kind) {
            g_StoredKindTrace->collection->Deinitialize();
        }
        return SubsystemKindOf<CStoredKindProbe>();
    }
};

TUniquePtr<ASubsystem> CreateStoredKindProbe() noexcept
{
    return MakeUnique<CStoredKindProbe>();
}

enum class EParentTeardownStage : u8 {
    OwnerAssigned = 0,
    Initialize = 1,
};

/** child callbackからparentを終了する順序を記録する。 */
struct FParentTeardownTrace {
    /** 終了して再初期化するparent。 */
    CSubsystemCollection* parent = nullptr;
    /** parent再初期化へ渡すowner。 */
    FSubsystemOwner parent_owner{};
    /** parentを終了するcallback段階。 */
    EParentTeardownStage stage = EParentTeardownStage::OwnerAssigned;
    /** callback順序。 */
    char callbacks[4]{};
    /** callback記録数。 */
    u32 callback_count = 0u;
    /** parentを同一構成で再初期化する。 */
    bool reinitialize_parent = false;
    /** parent再初期化の結果。 */
    bool parent_reinitialize_result = false;
    /** instance破棄前にownerが空へ戻った。 */
    bool owner_cleared_before_destroy = false;

    void Record(char Callback) noexcept { callbacks[callback_count++] = Callback; }

    void RestartParent() noexcept
    {
        parent->Deinitialize();
        if (reinitialize_parent) {
            parent_reinitialize_result = parent->TryInitialize(
                ESubsystemScope::Engine, nullptr, parent_owner);
        }
    }
};

class CParentTeardownProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CParentTeardownProbe)
    ~CParentTeardownProbe() noexcept override
    {
        if (m_Trace != nullptr) {
            m_Trace->owner_cleared_before_destroy = Owner() == nullptr;
            m_Trace->Record('X');
        }
    }

    bool OnOwnerAssigned() noexcept override
    {
        m_Trace = OwnerAs<FParentTeardownTrace>();
        m_Trace->Record('A');
        if (m_Trace->stage == EParentTeardownStage::OwnerAssigned) {
            m_Trace->RestartParent();
        }
        return true;
    }

    void OnInitialize() noexcept override
    {
        m_Trace->Record('I');
        if (m_Trace->stage == EParentTeardownStage::Initialize) {
            m_Trace->RestartParent();
        }
    }

    void OnDeinitialize() noexcept override { m_Trace->Record('D'); }

private:
    /** owner callbackで得た観測先。 */
    FParentTeardownTrace* m_Trace = nullptr;
};

TUniquePtr<ASubsystem> CreateParentTeardownProbe() noexcept
{
    return MakeUnique<CParentTeardownProbe>();
}

/** child tick中のparent lifecycle変更を記録する。 */
struct FTickParentTeardownTrace {
    /** 変更するparent。 */
    CSubsystemCollection* parent = nullptr;
    /** parent再初期化へ渡すowner。 */
    FSubsystemOwner parent_owner{};
    /** parentを同一構成で再初期化する。 */
    bool reinitialize_parent = false;
    /** parent再初期化の結果。 */
    bool parent_reinitialize_result = false;
    /** 先頭child tick回数。 */
    u32 first_ticks = 0u;
    /** 後続child tick回数。 */
    u32 second_ticks = 0u;
    /** child終了callback順序。 */
    char deinitialize_order[2]{};
    /** child終了callback数。 */
    u32 deinitialize_count = 0u;
};

class CTickParentTeardownFirst final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CTickParentTeardownFirst)
    void OnTick(f32) noexcept override
    {
        FTickParentTeardownTrace* const Trace = OwnerAs<FTickParentTeardownTrace>();
        ++Trace->first_ticks;
        Trace->parent->Deinitialize();
        if (Trace->reinitialize_parent) {
            Trace->parent_reinitialize_result = Trace->parent->TryInitialize(
                ESubsystemScope::Engine, nullptr, Trace->parent_owner);
        }
    }
    void OnDeinitialize() noexcept override
    {
        FTickParentTeardownTrace* const Trace = OwnerAs<FTickParentTeardownTrace>();
        Trace->deinitialize_order[Trace->deinitialize_count++] = 'A';
    }
};

class CTickParentTeardownSecond final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CTickParentTeardownSecond)
    void OnTick(f32) noexcept override
    {
        ++OwnerAs<FTickParentTeardownTrace>()->second_ticks;
    }
    void OnDeinitialize() noexcept override
    {
        FTickParentTeardownTrace* const Trace = OwnerAs<FTickParentTeardownTrace>();
        Trace->deinitialize_order[Trace->deinitialize_count++] = 'B';
    }
};

TUniquePtr<ASubsystem> CreateTickParentTeardownFirst() noexcept
{
    return MakeUnique<CTickParentTeardownFirst>();
}

TUniquePtr<ASubsystem> CreateTickParentTeardownSecond() noexcept
{
    return MakeUnique<CTickParentTeardownSecond>();
}

/** parent更新中の論理可視性と終了要求後の遮断を記録する。 */
struct FTickVisibilityTrace {
    /** 更新中のEngine collection。 */
    CSubsystemCollection* parent = nullptr;
    /** Engineをparentに持つGameInstance collection。 */
    CSubsystemCollection* child = nullptr;
    /** Engine再照合へ渡すowner。 */
    FSubsystemOwner parent_owner{};
    /** 終了要求前にparentが利用可能だった。 */
    bool parent_visible_before_request = false;
    /** 終了要求前にchildが利用可能だった。 */
    bool child_visible_before_request = false;
    /** 終了要求前の同一構成再初期化結果。 */
    bool exact_initialize_before_request = false;
    /** 終了要求後にparentが遮断された。 */
    bool parent_hidden_after_request = false;
    /** 終了要求後にchildが遮断された。 */
    bool child_hidden_after_request = false;
    /** 終了要求後の同一構成再初期化結果。 */
    bool exact_initialize_after_request = true;
    /** parent終了後に試みたchild更新回数。 */
    u32 child_ticks = 0u;
    /** child終了callback回数。 */
    u32 child_deinitializes = 0u;
};

class CTickVisibilityChild final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CTickVisibilityChild)
    void OnTick(f32) noexcept override
    {
        ++OwnerAs<FTickVisibilityTrace>()->child_ticks;
    }
    void OnDeinitialize() noexcept override
    {
        ++OwnerAs<FTickVisibilityTrace>()->child_deinitializes;
    }
};

class CTickVisibilityParent final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CTickVisibilityParent)
    void OnTick(f32) noexcept override
    {
        /** collection可視性を共有する観測先。 */
        FTickVisibilityTrace* const Trace = OwnerAs<FTickVisibilityTrace>();
        Trace->parent_visible_before_request =
            Trace->parent->IsInitialized() && Trace->parent->Count() > 0u &&
            Trace->parent->Get<CTickVisibilityParent>() != nullptr;
        Trace->child_visible_before_request =
            Trace->child->IsInitialized() && Trace->child->Count() > 0u &&
            Trace->child->Get<CTickVisibilityChild>() != nullptr;
        Trace->exact_initialize_before_request = Trace->parent->TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace->parent_owner);

        Trace->parent->Deinitialize();
        Trace->parent_hidden_after_request =
            !Trace->parent->IsInitialized() && Trace->parent->Count() == 0u &&
            Trace->parent->Get<CTickVisibilityParent>() == nullptr;
        Trace->child_hidden_after_request =
            !Trace->child->IsInitialized() && Trace->child->Count() == 0u &&
            Trace->child->Get<CTickVisibilityChild>() == nullptr;
        Trace->exact_initialize_after_request = Trace->parent->TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace->parent_owner);
    }
};

TUniquePtr<ASubsystem> CreateTickVisibilityParent() noexcept
{
    return MakeUnique<CTickVisibilityParent>();
}

TUniquePtr<ASubsystem> CreateTickVisibilityChild() noexcept
{
    return MakeUnique<CTickVisibilityChild>();
}

/** 深いparent世代切替後にWorld callbackが実行されないことを数える。 */
u32 g_AncestorWorldTicks = 0u;

class CAncestorWorldProbe final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CAncestorWorldProbe)
    void OnTick(f32) noexcept override { ++g_AncestorWorldTicks; }
};

TUniquePtr<ASubsystem> CreateAncestorWorldProbe() noexcept
{
    return MakeUnique<CAncestorWorldProbe>();
}

class CTransitionProbeGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return {}; }
};

/** OnStart時のcatalog再照合を直接検証する最小game。 */
class CStartCatalogProbeGame final : public CGame {
public:
    void InvokeStart() noexcept { OnStart(); }
    void InvokeShutdown() noexcept { OnShutdown(); }
    u32 initial_scene_calls = 0u;

protected:
    TUniquePtr<AScene> InitialScene() noexcept override
    {
        ++initial_scene_calls;
        return MakeUnique<AScene>();
    }
};

class APhaseOrderScene final : public AScene {
public:
    ESvc WantedServices() const noexcept override { return ESvc::Camera2D; }
    void OnEnter() noexcept override
    {
        Services().Camera().SetPosition(FVec2{});
        Services().Camera().SetTargetPos(FVec2{10.0f, 0.0f}, 0.0f);
    }
    void OnUpdate(f32 DeltaSeconds) noexcept override
    {
        g_PhaseOrderData->world_context_matches =
            g_PhaseOrderData->world_context_matches &&
            DeltaSeconds == g_PhaseOrderData->scaled_delta_seconds;
        g_PhaseOrderData->trace.Add('S');
    }
};

class CPhaseOrderGame final : public CGame {
public:
    void InvokeStart() noexcept { OnStart(); }
    void InvokeUpdate(f32 DeltaSeconds) noexcept { OnUpdate(DeltaSeconds); }
    void InvokeShutdown() noexcept { OnShutdown(); }

protected:
    TUniquePtr<AScene> InitialScene() noexcept override
    {
        return MakeUnique<APhaseOrderScene>();
    }
};

} // namespace

ACS_REGISTER_SUBSYSTEM(CScoreSub,  ESubsystemScope::World)
ACS_REGISTER_SUBSYSTEM(CConfigSub, ESubsystemScope::GameInstance)
ACS_REGISTER_SUBSYSTEM(CEngineSub, ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(COwnerSub,  ESubsystemScope::World)

namespace {

// Engine→GameInstance→World の親チェーンで 3 コレクションを初期化するヘルパ。
struct FStack {
    CSubsystemCollection engine, gameInst, world;
    FStack() noexcept {
        engine.Initialize(ESubsystemScope::Engine);
        gameInst.Initialize(ESubsystemScope::GameInstance, &engine);
        world.Initialize(ESubsystemScope::World, &gameInst);
    }
};

} // namespace

// 各スコープのサブシステムが «該当スコープのコレクションにのみ» 生成される。
ACS_TEST(Subsystem, InstantiatedPerScope) {
    FStack s;
    EXPECT_TRUE(s.world.Get<CScoreSub>()     != nullptr);
    EXPECT_TRUE(s.gameInst.Get<CConfigSub>() != nullptr);
    EXPECT_TRUE(s.engine.Get<CEngineSub>()   != nullptr);
    // OnInitialize が走っている。
    EXPECT_EQ(s.world.Get<CScoreSub>()->score, 0);
    EXPECT_EQ(s.gameInst.Get<CConfigSub>()->volume, 50);
    EXPECT_TRUE(s.engine.Get<CEngineSub>()->inited);
}

// 下位スコープから上位スコープを取得できる (フォールバック)。上位→下位は不可。
ACS_TEST(Subsystem, ScopeFallback) {
    FStack s;
    // World から GameInstance / Engine が見える。
    EXPECT_TRUE(s.world.Get<CConfigSub>() != nullptr);
    EXPECT_TRUE(s.world.Get<CEngineSub>() != nullptr);
    // GameInstance から Engine は見えるが World は見えない。
    EXPECT_TRUE(s.gameInst.Get<CEngineSub>() != nullptr);
    EXPECT_TRUE(s.gameInst.Get<CScoreSub>()  == nullptr);
    // Engine から下位は見えない。
    EXPECT_TRUE(s.engine.Get<CConfigSub>() == nullptr);
    EXPECT_TRUE(s.engine.Get<CScoreSub>()  == nullptr);
}

// Tick が毎フレーム呼ばれる。
ACS_TEST(Subsystem, TickForwards) {
    FStack s;
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    EXPECT_EQ(s.world.Get<CScoreSub>()->ticks, 3);
}

// Deinitialize で OnDeinitialize が呼ばれ、以後取得不可になる。冪等。
ACS_TEST(Subsystem, DeinitializeTearsDown) {
    CSubsystemCollection world;
    world.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(world.Get<CScoreSub>() != nullptr);
    world.Deinitialize();
    EXPECT_TRUE(world.Get<CScoreSub>() == nullptr);
    world.Deinitialize();   // 冪等 (二重解体しても安全)
}

// ノード/コンポーネントが root 配線のサブシステムを walk-to-root で取得できる
// (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)。
ACS_TEST(Subsystem, NodeAndComponentAccess) {
    FStack s;
    ANode root;
    root.ManagementAccess().SetSubsystems(&s.world);        // root にのみ配線
    ANode& child = root.AddChild(NewObject<ANode>());
    AProbeComponent& comp = child.AddComponent<AProbeComponent>();

    // 子ノードから (walk-to-root)。
    EXPECT_TRUE(child.GetSubsystem<CScoreSub>()  != nullptr);
    EXPECT_TRUE(child.GetSubsystem<CConfigSub>() != nullptr);   // 上位へフォールバック
    // コンポーネントから (owner → root)。
    EXPECT_TRUE(comp.GetSubsystem<CScoreSub>() != nullptr);

    // コンポーネント経由でスコアを加算 → 同じ World サブシステムへ反映される。
    comp.GetSubsystem<CScoreSub>()->Add(7);
    comp.GetSubsystem<CScoreSub>()->Add(3);
    EXPECT_EQ(s.world.Get<CScoreSub>()->score, 10);

    // 未配線ノードは nullptr。
    ANode lonely;
    EXPECT_TRUE(lonely.GetSubsystem<CScoreSub>() == nullptr);
}

// Owner() が Initialize で渡したコンテキストになる(OnInitialize 時点で有効)。
ACS_TEST(Subsystem, OwnerContextIsSet) {
    int dummyOwner = 0;
    void* owner = &dummyOwner;
    CSubsystemCollection world;
    world.Initialize(ESubsystemScope::World, nullptr, owner);
    COwnerSub* s = world.Get<COwnerSub>();
    EXPECT_TRUE(s != nullptr);
    EXPECT_TRUE(s->Owner() == owner);            // 取得 API
    EXPECT_TRUE(s->seenOwner == owner);          // OnInitialize 時点で既に配線済み
    EXPECT_TRUE(s->OwnerAs<int>() == &dummyOwner);
    // owner 未指定なら nullptr。
    CSubsystemCollection w2;
    w2.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(w2.Get<COwnerSub>()->Owner() == nullptr);
}

ACS_TEST(Subsystem, DuplicateFactorySourcesPromoteOnUnregister)
{
    static const int source_kind = 0;
    const FSubsystemFactory first{&source_kind, ESubsystemScope::World, "DynamicSubsystem", &CreateDynamicSubsystemOne};
    const FSubsystemFactory second{&source_kind, ESubsystemScope::World, "DynamicSubsystem",
                                   &CreateDynamicSubsystemTwo};

    CSubsystemRegistry& registry = CSubsystemRegistry::Get();
    const u32 count_before = registry.Count();
    registry.Register(first);
    registry.Register(second);
    EXPECT_EQ(registry.Count(), count_before + 1u);
    EXPECT_TRUE(FindFactoryByKind(registry, &source_kind)->create == &CreateDynamicSubsystemOne);

    // 先に有効だった module/source を外すと、残る factory へ昇格する。
    EXPECT_TRUE(registry.Unregister(first));
    const FSubsystemFactory* promoted = FindFactoryByKind(registry, &source_kind);
    EXPECT_TRUE(promoted != nullptr);
    EXPECT_TRUE(promoted->create == &CreateDynamicSubsystemTwo);

    EXPECT_TRUE(registry.Unregister(second));
    EXPECT_EQ(registry.Count(), count_before);
    EXPECT_TRUE(FindFactoryByKind(registry, &source_kind) == nullptr);
    EXPECT_TRUE(!registry.Unregister(second));
}

// エンジン提供の AWorldClockSubsystem が各 World に自動生成され、OnTick で経過時間と
// フレーム数を積む。Deinitialize→再 Initialize で 0 から始まる。
ACS_TEST(Subsystem, WorldClockAccumulatesElapsedAndFrames)
{
    // lifecycle 専用targetはstatic-link検証と分離し、必要な製品catalogを明示登録する。
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    AScene OwnerScene;
    CSubsystemCollection world;
    EXPECT_TRUE(world.TryInitialize(
        ESubsystemScope::World, nullptr,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    AWorldClockSubsystem* clock = world.Get<AWorldClockSubsystem>();
    EXPECT_TRUE(clock != nullptr);
    if (clock == nullptr) return;
    EXPECT_TRUE(std::strcmp(clock->Name(), "FWorldClockSubsystem") == 0);
    AEventBus* const event_bus = world.Get<AEventBus>();
    ASpawn2DSubsystem* const spawn = world.Get<ASpawn2DSubsystem>();
    EXPECT_TRUE(event_bus != nullptr);
    EXPECT_TRUE(spawn != nullptr);
    if (event_bus != nullptr) EXPECT_TRUE(std::strcmp(event_bus->Name(), "FEventBus") == 0);
    if (spawn != nullptr) EXPECT_TRUE(std::strcmp(spawn->Name(), "FSpawn2DSubsystem") == 0);

    // OnInitialize 直後は 0。
    EXPECT_NEAR(clock->ElapsedSeconds(), 0.0, 1e-9);
    EXPECT_EQ(clock->FrameCount(), 0ull);

    world.Tick(0.5f);
    world.Tick(0.25f);
    world.Tick(0.25f);
    EXPECT_EQ(clock->FrameCount(), 3ull);
    EXPECT_NEAR(clock->ElapsedSeconds(), 1.0, 1e-6);        // 0.5 + 0.25 + 0.25
    EXPECT_NEAR(clock->LastDeltaSeconds(), 0.25f, 1e-6f);

    // Deinitialize で消え、再 Initialize で 0 から。
    world.Deinitialize();
    EXPECT_TRUE(world.Get<AWorldClockSubsystem>() == nullptr);
    EXPECT_TRUE(world.TryInitialize(
        ESubsystemScope::World, nullptr,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    AWorldClockSubsystem* reinit = world.Get<AWorldClockSubsystem>();
    EXPECT_TRUE(reinit != nullptr);
    if (reinit != nullptr) {
        EXPECT_NEAR(reinit->ElapsedSeconds(), 0.0, 1e-9);
        EXPECT_EQ(reinit->FrameCount(), 0ull);
    }
}

ACS_TEST(Subsystem, PhaseAndOrderAreDeterministic)
{
    const FSubsystemFactory Later{SubsystemKindOf<COrderedSubsystemB>(), ESubsystemScope::Engine,
                                  "COrderedSubsystemB", &CreateOrderedSubsystemB,
                                  ESubsystemTickPhase::PostUpdate, 20};
    const FSubsystemFactory Earlier{SubsystemKindOf<COrderedSubsystemA>(), ESubsystemScope::Engine,
                                    "COrderedSubsystemA", &CreateOrderedSubsystemA,
                                    ESubsystemTickPhase::PreUpdate, -20};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    Registry.Register(Later);
    Registry.Register(Earlier);

    FSubsystemTrace Trace{};
    g_SubsystemTrace = &Trace;
    CSubsystemCollection Collection;
    EXPECT_TRUE(Collection.TryInitialize(ESubsystemScope::Engine));
    Collection.TickFrame(FSubsystemFrameContext{0.25f, 0.5f, 7u, ESubsystemTickPhase::PreUpdate});
    Collection.TickFrame(FSubsystemFrameContext{0.25f, 0.5f, 7u, ESubsystemTickPhase::PostUpdate});
    Collection.Deinitialize();

    EXPECT_EQ(Trace.count, 6u);
    EXPECT_EQ(Trace.values[0], 'A');
    EXPECT_EQ(Trace.values[1], 'B');
    EXPECT_EQ(Trace.values[2], 'a');
    EXPECT_EQ(Trace.values[3], 'b');
    EXPECT_EQ(Trace.values[4], 'E');
    EXPECT_EQ(Trace.values[5], 'D');
    g_SubsystemTrace = nullptr;
    EXPECT_TRUE(Registry.Unregister(Earlier));
    EXPECT_TRUE(Registry.Unregister(Later));
}

ACS_TEST(Subsystem, FramePhasesRunServicesWorldGameAndEngineInOrder)
{
    const FSubsystemFactory WorldParticle{
        SubsystemKindOf<CWorldParticleTraceSubsystem>(), ESubsystemScope::World,
        "CWorldParticleTraceSubsystem", &CreateWorldParticleTraceSubsystem,
        ESubsystemTickPhase::PostUpdate, 800};
    const FSubsystemFactory WorldEffect{
        SubsystemKindOf<CWorldEffectTraceSubsystem>(), ESubsystemScope::World,
        "CWorldEffectTraceSubsystem", &CreateWorldEffectTraceSubsystem,
        ESubsystemTickPhase::PostUpdate, 900};
    const FSubsystemFactory GamePost{
        SubsystemKindOf<CGamePostTraceSubsystem>(), ESubsystemScope::GameInstance,
        "CGamePostTraceSubsystem", &CreateGamePostTraceSubsystem,
        ESubsystemTickPhase::PostUpdate, 800};
    const FSubsystemFactory EnginePost{
        SubsystemKindOf<CEnginePostTraceSubsystem>(), ESubsystemScope::Engine,
        "CEnginePostTraceSubsystem", &CreateEnginePostTraceSubsystem,
        ESubsystemTickPhase::PostUpdate, 800};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(WorldParticle));
    EXPECT_TRUE(Registry.TryRegister(WorldEffect));
    EXPECT_TRUE(Registry.TryRegister(GamePost));
    EXPECT_TRUE(Registry.TryRegister(EnginePost));

    FPhaseOrderData Data{};
    Data.scaled_delta_seconds = 0.2f;
    Data.unscaled_delta_seconds = 0.4f;
    g_PhaseOrderData = &Data;
    CPhaseOrderGame Game;
    Game.SetTimeScale(0.5f);
    Game.SetFixedTimestep(0.0f);
    Data.frame_number = Game.FrameCount();
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    Game.InvokeStart();

    Game.EngineSubsystems().TickFrame(FSubsystemFrameContext{
        Data.unscaled_delta_seconds, Data.unscaled_delta_seconds,
        Data.frame_number, ESubsystemTickPhase::PreUpdate});
    Game.InvokeUpdate(Data.unscaled_delta_seconds);
    Game.EngineSubsystems().TickFrame(FSubsystemFrameContext{
        Data.unscaled_delta_seconds, Data.unscaled_delta_seconds,
        Data.frame_number, ESubsystemTickPhase::PostUpdate});

    EXPECT_EQ(Data.trace.count, 6u);
    EXPECT_EQ(Data.trace.values[0], 'S');
    EXPECT_EQ(Data.trace.values[1], 'C');
    EXPECT_EQ(Data.trace.values[2], 'P');
    EXPECT_EQ(Data.trace.values[3], 'E');
    EXPECT_EQ(Data.trace.values[4], 'G');
    EXPECT_EQ(Data.trace.values[5], 'N');
    EXPECT_TRUE(Data.world_context_matches);
    EXPECT_TRUE(Data.game_context_matches);
    EXPECT_TRUE(Data.engine_context_matches);

    Game.InvokeShutdown();
    Game.EngineSubsystems().Deinitialize();
    g_PhaseOrderData = nullptr;
    EXPECT_TRUE(Registry.Unregister(EnginePost));
    EXPECT_TRUE(Registry.Unregister(GamePost));
    EXPECT_TRUE(Registry.Unregister(WorldEffect));
    EXPECT_TRUE(Registry.Unregister(WorldParticle));
}

ACS_TEST(Subsystem, TickDeinitializeIsDeferredToCallbackBoundary)
{
    const FSubsystemFactory Cancel{SubsystemKindOf<CCancelTickSubsystem>(), ESubsystemScope::Engine,
                                   "CCancelTickSubsystem", &CreateCancelTickSubsystem,
                                   ESubsystemTickPhase::PreUpdate, -10};
    const FSubsystemFactory Skipped{SubsystemKindOf<CSkippedTickSubsystem>(), ESubsystemScope::Engine,
                                    "CSkippedTickSubsystem", &CreateSkippedTickSubsystem,
                                    ESubsystemTickPhase::PreUpdate, 10};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    Registry.Register(Skipped);
    Registry.Register(Cancel);

    FSubsystemTrace Trace{};
    CSubsystemCollection Collection;
    FSubsystemReentryOwner Owner{&Collection, &Trace};
    EXPECT_TRUE(Collection.TryInitialize(ESubsystemScope::Engine, nullptr, &Owner));
    Collection.Tick(1.0f);
    EXPECT_TRUE(!Collection.IsInitialized());
    EXPECT_EQ(Trace.count, 3u);
    EXPECT_EQ(Trace.values[0], 'T');
    EXPECT_EQ(Trace.values[1], 'K');
    EXPECT_EQ(Trace.values[2], 'C');
    EXPECT_TRUE(Registry.Unregister(Cancel));
    EXPECT_TRUE(Registry.Unregister(Skipped));
}

ACS_TEST(Subsystem, InvalidFactoryAndParentCycleFailBeforeCallbacks)
{
    static const int InvalidKind = 0;
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    const u32 CountBefore = Registry.Count();
    const FSubsystemFactory Valid{&InvalidKind, ESubsystemScope::Engine, "InvalidProbe",
                                  &CreateParentValidationProbe,
                                  ESubsystemTickPhase::PreUpdate, 0};
    g_ParentFactoryCreateCount = 0u;

    FSubsystemFactory Invalid = Valid;
    Invalid.kind = nullptr;
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Invalid = Valid;
    Invalid.name = nullptr;
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Invalid = Valid;
    Invalid.name = "";
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Invalid = Valid;
    Invalid.create = nullptr;
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Invalid = Valid;
    Invalid.scope = static_cast<ESubsystemScope>(255u);
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Invalid = Valid;
    Invalid.phase = static_cast<ESubsystemTickPhase>(255u);
    EXPECT_TRUE(!Registry.TryRegister(Invalid));
    Registry.Register(Invalid);
    EXPECT_EQ(Registry.Count(), CountBefore);
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);

    CSubsystemCollection SelfParent;
    EXPECT_TRUE(!SelfParent.TryInitialize(ESubsystemScope::Engine, &SelfParent));
    EXPECT_EQ(SelfParent.Count(), 0u);
}

ACS_TEST(Subsystem, DuplicateOrderAndNameFailBeforeFactoryCreate)
{
    const FSubsystemFactory First{
        SubsystemKindOf<CDynamicSubsystemOne>(), ESubsystemScope::Engine, "DuplicateOrderName",
        &CreateDuplicateOrderedSubsystemA, ESubsystemTickPhase::PreUpdate, 300};
    const FSubsystemFactory Second{
        SubsystemKindOf<CDynamicSubsystemTwo>(), ESubsystemScope::Engine, "DuplicateOrderName",
        &CreateDuplicateOrderedSubsystemB, ESubsystemTickPhase::PostUpdate, 300};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(First));
    EXPECT_TRUE(Registry.TryRegister(Second));

    g_DuplicateFactoryCreateCount = 0u;
    CSubsystemCollection Collection;
    EXPECT_TRUE(!Collection.TryInitialize(ESubsystemScope::Engine));
    EXPECT_EQ(Collection.Count(), 0u);
    EXPECT_EQ(g_DuplicateFactoryCreateCount, 0u);

    EXPECT_TRUE(Registry.Unregister(Second));
    EXPECT_TRUE(Registry.Unregister(First));
}

ACS_TEST(Subsystem, DeinitializeReleasesCollectionAllocatorStorage)
{
    const FSubsystemFactory Probe{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::Engine,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 600};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Probe));

    CSubsystemCountingAllocator Allocator;
    {
        CSubsystemDefaultAllocatorScope AllocatorScope(Allocator);
        CSubsystemCollection Collection;
        EXPECT_TRUE(Collection.TryInitialize(ESubsystemScope::Engine));
        EXPECT_TRUE(Allocator.outstanding_allocations > 0u);
        Collection.Deinitialize();
        EXPECT_EQ(Allocator.outstanding_allocations, 0u);
    }

    EXPECT_TRUE(Registry.Unregister(Probe));
}

ACS_TEST(Subsystem, InvalidParentScopeFailsBeforeFactoryCreate)
{
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    CTransitionProbeGame Game;
    AScene OwnerScene;

    CSubsystemCollection UninitializedParent;
    const FSubsystemFactory WorldProbe{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::World,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 500};
    EXPECT_TRUE(Registry.TryRegister(WorldProbe));
    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection UninitializedChild;
    EXPECT_TRUE(!UninitializedChild.TryInitialize(
        ESubsystemScope::World, &UninitializedParent,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_TRUE(Registry.Unregister(WorldProbe));

    CSubsystemCollection ActiveWorld;
    EXPECT_TRUE(ActiveWorld.TryInitialize(
        ESubsystemScope::World, nullptr,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_TRUE(Registry.TryRegister(WorldProbe));
    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection SameScopeChild;
    EXPECT_TRUE(!SameScopeChild.TryInitialize(
        ESubsystemScope::World, &ActiveWorld,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_TRUE(Registry.Unregister(WorldProbe));

    const FSubsystemFactory GameProbe{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::GameInstance,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 500};
    EXPECT_TRUE(Registry.TryRegister(GameProbe));
    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection ReverseChild;
    EXPECT_TRUE(!ReverseChild.TryInitialize(
        ESubsystemScope::GameInstance, &ActiveWorld,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_TRUE(Registry.Unregister(GameProbe));

    const FSubsystemFactory EngineProbe{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::Engine,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 500};
    EXPECT_TRUE(Registry.TryRegister(EngineProbe));
    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection EngineWithParent;
    EXPECT_TRUE(!EngineWithParent.TryInitialize(
        ESubsystemScope::Engine, &ActiveWorld,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_TRUE(Registry.Unregister(EngineProbe));

    CSubsystemCollection ActiveEngine;
    EXPECT_TRUE(ActiveEngine.TryInitialize(ESubsystemScope::Engine));
    EXPECT_TRUE(Registry.TryRegister(WorldProbe));
    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection SkippedParentChild;
    EXPECT_TRUE(!SkippedParentChild.TryInitialize(
        ESubsystemScope::World, &ActiveEngine,
        FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_TRUE(Registry.Unregister(WorldProbe));
}

ACS_TEST(Subsystem, TypedOwnerScopeMismatchFailsBeforeFactoryCreate)
{
    const FSubsystemFactory Probe{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::GameInstance,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 650};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Probe));
    int Owner = 0;

    g_ParentFactoryCreateCount = 0u;
    CSubsystemCollection WrongKind;
    EXPECT_TRUE(!WrongKind.TryInitialize(
        ESubsystemScope::GameInstance, nullptr,
        FSubsystemOwner{&Owner, ESubsystemOwnerKind::Scene}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_EQ(WrongKind.Count(), 0u);

    CSubsystemCollection NullKnownOwner;
    EXPECT_TRUE(!NullKnownOwner.TryInitialize(
        ESubsystemScope::GameInstance, nullptr,
        FSubsystemOwner{nullptr, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_EQ(NullKnownOwner.Count(), 0u);
    EXPECT_TRUE(Registry.Unregister(Probe));
}

ACS_TEST(Subsystem, ParentTickRejectsChildInitializationBeforeFactoryCreate)
{
    const FSubsystemFactory ParentFactory{
        SubsystemKindOf<CTickParentInitializer>(), ESubsystemScope::Engine,
        "CTickParentInitializer", &CreateTickParentInitializer,
        ESubsystemTickPhase::PreUpdate, 700};
    const FSubsystemFactory ChildFactory{
        SubsystemKindOf<CParentValidationProbe>(), ESubsystemScope::GameInstance,
        "CParentValidationProbe", &CreateParentValidationProbe,
        ESubsystemTickPhase::None, 700};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(ParentFactory));
    EXPECT_TRUE(Registry.TryRegister(ChildFactory));

    CSubsystemCollection Parent;
    CSubsystemCollection Child;
    FTickParentContext Context{&Parent, &Child, false, true};
    EXPECT_TRUE(Parent.TryInitialize(ESubsystemScope::Engine, nullptr, &Context));
    g_ParentFactoryCreateCount = 0u;
    Parent.Tick(0.1f);
    EXPECT_TRUE(Context.attempted);
    EXPECT_TRUE(!Context.initialized);
    EXPECT_EQ(g_ParentFactoryCreateCount, 0u);
    EXPECT_EQ(Child.Count(), 0u);

    EXPECT_TRUE(Registry.Unregister(ChildFactory));
    EXPECT_TRUE(Registry.Unregister(ParentFactory));
}

ACS_TEST(Subsystem, GameFrameworkCatalogRepairsMissingAndRejectsShadow)
{
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();

    const FSubsystemFactory* const ClockActive =
        FindFactoryByKind(Registry, SubsystemKindOf<AWorldClockSubsystem>());
    EXPECT_TRUE(ClockActive != nullptr);
    if (ClockActive == nullptr) return;
    EXPECT_TRUE(std::strcmp(ClockActive->name, "FWorldClockSubsystem") == 0);
    const FSubsystemFactory* const EventActive =
        FindFactoryByKind(Registry, SubsystemKindOf<AEventBus>());
    EXPECT_TRUE(EventActive != nullptr);
    if (EventActive == nullptr) return;
    EXPECT_TRUE(std::strcmp(EventActive->name, "FEventBus") == 0);
    const FSubsystemFactory ClockExpected = *ClockActive;
    EXPECT_TRUE(Registry.Unregister(ClockExpected));
    EXPECT_TRUE(FindFactoryByKind(Registry, ClockExpected.kind) == nullptr);
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());

    const FSubsystemFactory* const SpawnActive =
        FindFactoryByKind(Registry, SubsystemKindOf<ASpawn2DSubsystem>());
    EXPECT_TRUE(SpawnActive != nullptr);
    if (SpawnActive == nullptr) return;
    EXPECT_TRUE(std::strcmp(SpawnActive->name, "FSpawn2DSubsystem") == 0);
    const FSubsystemFactory SpawnExpected = *SpawnActive;
    EXPECT_TRUE(Registry.Unregister(SpawnExpected));
    FSubsystemFactory Shadow = SpawnExpected;
    Shadow.phase = ESubsystemTickPhase::PostUpdate;
    EXPECT_TRUE(Registry.TryRegister(Shadow));
    EXPECT_TRUE(!AcsRegisterGameFrameworkSubsystems());
    EXPECT_TRUE(Registry.Unregister(Shadow));
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
}

ACS_TEST(Subsystem, GameStartRevalidatesCatalogAfterConstruction)
{
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();

    CStartCatalogProbeGame MissingGame;
    EXPECT_TRUE(MissingGame.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&MissingGame, ESubsystemOwnerKind::Application}));
    const FSubsystemFactory* const ClockActive =
        FindFactoryByKind(Registry, SubsystemKindOf<AWorldClockSubsystem>());
    EXPECT_TRUE(ClockActive != nullptr);
    if (ClockActive == nullptr) return;
    const FSubsystemFactory ClockExpected = *ClockActive;
    EXPECT_TRUE(Registry.Unregister(ClockExpected));
    MissingGame.InvokeStart();
    EXPECT_TRUE(FindFactoryByKind(Registry, ClockExpected.kind) != nullptr);
    EXPECT_EQ(MissingGame.initial_scene_calls, 1u);
    MissingGame.InvokeShutdown();

    CStartCatalogProbeGame ShadowGame;
    EXPECT_TRUE(ShadowGame.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&ShadowGame, ESubsystemOwnerKind::Application}));
    const FSubsystemFactory* const SpawnActive =
        FindFactoryByKind(Registry, SubsystemKindOf<ASpawn2DSubsystem>());
    EXPECT_TRUE(SpawnActive != nullptr);
    if (SpawnActive == nullptr) return;
    const FSubsystemFactory SpawnExpected = *SpawnActive;
    EXPECT_TRUE(Registry.Unregister(SpawnExpected));
    FSubsystemFactory Shadow = SpawnExpected;
    Shadow.phase = ESubsystemTickPhase::PostUpdate;
    EXPECT_TRUE(Registry.TryRegister(Shadow));
    ShadowGame.InvokeStart();
    EXPECT_EQ(ShadowGame.initial_scene_calls, 0u);
    EXPECT_TRUE(Registry.Unregister(Shadow));
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
}

ACS_TEST(Subsystem, GameDestructorKeepsParentAliveDuringWorldTeardown)
{
    const FSubsystemFactory Probe{
        SubsystemKindOf<CDestructorParentProbe>(), ESubsystemScope::World,
        "CDestructorParentProbe", &CreateDestructorParentProbe,
        ESubsystemTickPhase::None, 1100};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Probe));
    g_DestructorParentVisible = false;
    g_DestructorProbeDeinitializeCount = 0u;
    {
        CStartCatalogProbeGame Game;
        EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
            ESubsystemScope::Engine, nullptr,
            FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
        Game.InvokeStart();
        EXPECT_EQ(Game.initial_scene_calls, 1u);
    }
    EXPECT_EQ(g_DestructorProbeDeinitializeCount, 1u);
    EXPECT_TRUE(g_DestructorParentVisible);
    EXPECT_TRUE(Registry.Unregister(Probe));
}

ACS_TEST(Subsystem, LegacyWorldOwnersKeepSpawnInert)
{
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    CTransitionProbeGame Game;
    AScene OwnerScene;

    CSubsystemCollection LegacyOwned;
    LegacyOwned.Initialize(ESubsystemScope::World, nullptr, &OwnerScene);
    EXPECT_TRUE(LegacyOwned.IsInitialized());
    ASpawn2DSubsystem* const OwnedSpawn = LegacyOwned.Get<ASpawn2DSubsystem>();
    EXPECT_TRUE(OwnedSpawn != nullptr);
    if (OwnedSpawn != nullptr) {
        EXPECT_EQ(OwnedSpawn->OwnerKind(), ESubsystemOwnerKind::Unknown);
        EXPECT_TRUE(OwnedSpawn->Owner() == &OwnerScene);
        EXPECT_TRUE(OwnedSpawn->SpawnPrefabText("{}", FVec2{}) == nullptr);
    }

    CSubsystemCollection LegacyNull;
    LegacyNull.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(LegacyNull.IsInitialized());
    EXPECT_TRUE(LegacyNull.Get<ASpawn2DSubsystem>() != nullptr);

    const FSubsystemFactory Probe{
        SubsystemKindOf<COwnerValidationProbe>(), ESubsystemScope::World,
        "COwnerValidationProbe", &CreateOwnerValidationProbe,
        ESubsystemTickPhase::None, 400};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Probe));
    g_OwnerProbeInitializeCount = 0u;
    CSubsystemCollection WrongTypedOwner;
    EXPECT_TRUE(!WrongTypedOwner.TryInitialize(
        ESubsystemScope::World, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(WrongTypedOwner.Count(), 0u);
    EXPECT_EQ(g_OwnerProbeInitializeCount, 0u);
    EXPECT_TRUE(Registry.Unregister(Probe));
}

ACS_TEST(Subsystem, PreparationReentryCannotCommitOrResetBeforeUnwind)
{
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    int OwnerValue = 0;
    const FSubsystemOwner Owner{&OwnerValue, ESubsystemOwnerKind::Application};

    {
        CSubsystemCollection Collection;
        FFactoryReentryTrace Trace{&Collection, Owner};
        g_FactoryReentryTrace = &Trace;
        const FSubsystemFactory Factory{
            SubsystemKindOf<CFactoryReentryProbe>(), ESubsystemScope::Engine,
            "CFactoryReentryProbe", &CreateFactoryReentryProbe,
            ESubsystemTickPhase::None, 1300};
        EXPECT_TRUE(Registry.TryRegister(Factory));
        EXPECT_TRUE(!Collection.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
        EXPECT_TRUE(Trace.nested_attempted);
        EXPECT_TRUE(!Trace.nested_result);
        EXPECT_EQ(Trace.create_count, 1u);
        EXPECT_EQ(Trace.callback_count, 0u);
        EXPECT_EQ(Collection.Count(), 0u);
        EXPECT_TRUE(!Collection.IsInitialized());
        EXPECT_EQ(Collection.LifecycleGeneration(), 0u);
        EXPECT_TRUE(Registry.Unregister(Factory));
    }

    {
        CSubsystemCollection Collection;
        FFactoryReentryTrace Trace{&Collection, Owner};
        g_FactoryReentryTrace = &Trace;
        const FSubsystemFactory Factory{
            SubsystemKindOf<CKindFactoryReentryProbe>(), ESubsystemScope::Engine,
            "CKindFactoryReentryProbe", &CreateKindFactoryReentryProbe,
            ESubsystemTickPhase::None, 1300};
        EXPECT_TRUE(Registry.TryRegister(Factory));
        EXPECT_TRUE(!Collection.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
        EXPECT_TRUE(Trace.nested_attempted);
        EXPECT_TRUE(!Trace.nested_result);
        EXPECT_EQ(Trace.create_count, 1u);
        EXPECT_EQ(Trace.callback_count, 0u);
        EXPECT_EQ(Collection.Count(), 0u);
        EXPECT_TRUE(!Collection.IsInitialized());
        EXPECT_TRUE(Registry.Unregister(Factory));
    }

    {
        CSubsystemCollection Collection;
        FFactoryReentryTrace Trace{&Collection, Owner};
        g_FactoryReentryTrace = &Trace;
        static const int FailureKind = 0;
        const FSubsystemFactory StagedFactory{
            SubsystemKindOf<CDestructorFactoryReentryProbe>(), ESubsystemScope::Engine,
            "CDestructorFactoryReentryProbe", &CreateDestructorFactoryReentryProbe,
            ESubsystemTickPhase::None, 1300};
        const FSubsystemFactory FailureFactory{
            &FailureKind, ESubsystemScope::Engine, "FFailureAfterStagedProbe",
            &CreateFailureAfterStagedProbe, ESubsystemTickPhase::None, 1301};
        EXPECT_TRUE(Registry.TryRegister(StagedFactory));
        EXPECT_TRUE(Registry.TryRegister(FailureFactory));
        EXPECT_TRUE(!Collection.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
        EXPECT_TRUE(Trace.nested_attempted);
        EXPECT_TRUE(!Trace.nested_result);
        EXPECT_EQ(Trace.create_count, 2u);
        EXPECT_EQ(Trace.destructor_count, 1u);
        EXPECT_EQ(Trace.callback_count, 0u);
        EXPECT_EQ(Collection.Count(), 0u);
        EXPECT_TRUE(!Collection.IsInitialized());
        EXPECT_TRUE(Registry.Unregister(FailureFactory));
        EXPECT_TRUE(Registry.Unregister(StagedFactory));
    }

    g_FactoryReentryTrace = nullptr;
    CSubsystemCollection Recovery;
    EXPECT_TRUE(Recovery.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
    EXPECT_TRUE(Recovery.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
    EXPECT_TRUE(!Recovery.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Recovery, ESubsystemOwnerKind::Application}));
}

ACS_TEST(Subsystem, SuccessUnwindDefersActiveUntilAllocatorCallbacksFinish)
{
    CSubsystemReentrantFreeAllocator Allocator;
    CSubsystemDefaultAllocatorScope AllocatorScope(Allocator);
    CSubsystemCollection Collection;
    int OwnerValue = 0;
    const FSubsystemOwner Owner{&OwnerValue, ESubsystemOwnerKind::Application};
    Allocator.collection = &Collection;
    Allocator.owner = Owner;
    Allocator.armed = true;

    EXPECT_TRUE(!Collection.TryInitialize(ESubsystemScope::Engine, nullptr, Owner));
    EXPECT_TRUE(Allocator.triggered);
    EXPECT_TRUE(!Allocator.nested_initialize_result);
    EXPECT_TRUE(!Collection.IsInitialized());
    EXPECT_EQ(Collection.Count(), 0u);
    EXPECT_EQ(Collection.LifecycleGeneration(), 0u);
    Allocator.armed = false;
    EXPECT_EQ(Allocator.outstanding_allocations, 0u);
}

ACS_TEST(Subsystem, LookupUsesValidatedStoredKindWithoutVirtualReentry)
{
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    const FSubsystemFactory Factory{
        SubsystemKindOf<CStoredKindProbe>(), ESubsystemScope::Engine,
        "CStoredKindProbe", &CreateStoredKindProbe,
        ESubsystemTickPhase::None, 1300};
    EXPECT_TRUE(Registry.TryRegister(Factory));

    CSubsystemCollection Collection;
    int OwnerValue = 0;
    FStoredKindTrace Trace{&Collection};
    g_StoredKindTrace = &Trace;
    EXPECT_TRUE(Collection.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&OwnerValue, ESubsystemOwnerKind::Application}));
    EXPECT_EQ(Trace.kind_calls, 1u);
    Trace.kind_calls = 0u;
    Trace.deinitialize_on_kind = true;
    EXPECT_TRUE(Collection.Get<CStoredKindProbe>() != nullptr);
    EXPECT_EQ(Trace.kind_calls, 0u);
    EXPECT_TRUE(Collection.IsInitialized());
    Trace.deinitialize_on_kind = false;
    Collection.Deinitialize();
    g_StoredKindTrace = nullptr;
    EXPECT_TRUE(Registry.Unregister(Factory));
}

ACS_TEST(Subsystem, ParentGenerationRejectsCallbackTeardownAndReinitialize)
{
    const FSubsystemFactory Factory{
        SubsystemKindOf<CParentTeardownProbe>(), ESubsystemScope::GameInstance,
        "CParentTeardownProbe", &CreateParentTeardownProbe,
        ESubsystemTickPhase::None, 1300};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Factory));

    for (u32 StageIndex = 0u; StageIndex < 2u; ++StageIndex) {
        CSubsystemCollection Parent;
        CSubsystemCollection Child;
        FParentTeardownTrace Trace{};
        Trace.parent = &Parent;
        Trace.stage = StageIndex == 0u ? EParentTeardownStage::OwnerAssigned
                                      : EParentTeardownStage::Initialize;
        Trace.reinitialize_parent = true;
        Trace.parent_owner = FSubsystemOwner{&Trace, ESubsystemOwnerKind::Application};
        EXPECT_TRUE(Parent.TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace.parent_owner));
        const u64 ParentGeneration = Parent.LifecycleGeneration();
        EXPECT_TRUE(!Child.TryInitialize(
            ESubsystemScope::GameInstance, &Parent,
            FSubsystemOwner{&Trace, ESubsystemOwnerKind::Game}));
        EXPECT_TRUE(Trace.parent_reinitialize_result);
        EXPECT_TRUE(Parent.IsInitialized());
        EXPECT_TRUE(Parent.LifecycleGeneration() != ParentGeneration);
        EXPECT_TRUE(!Child.IsInitialized());
        EXPECT_EQ(Child.Count(), 0u);
        EXPECT_EQ(Child.LifecycleGeneration(), 0u);
        EXPECT_TRUE(Trace.owner_cleared_before_destroy);
        if (Trace.stage == EParentTeardownStage::OwnerAssigned) {
            EXPECT_EQ(Trace.callback_count, 2u);
            EXPECT_EQ(Trace.callbacks[0], 'A');
            EXPECT_EQ(Trace.callbacks[1], 'X');
        } else {
            EXPECT_EQ(Trace.callback_count, 4u);
            EXPECT_EQ(Trace.callbacks[0], 'A');
            EXPECT_EQ(Trace.callbacks[1], 'I');
            EXPECT_EQ(Trace.callbacks[2], 'D');
            EXPECT_EQ(Trace.callbacks[3], 'X');
        }
    }
    EXPECT_TRUE(Registry.Unregister(Factory));
}

ACS_TEST(Subsystem, TickStopsAndTearsDownChildWhenParentLifecycleChanges)
{
    const FSubsystemFactory FirstFactory{
        SubsystemKindOf<CTickParentTeardownFirst>(), ESubsystemScope::GameInstance,
        "CTickParentTeardownFirst", &CreateTickParentTeardownFirst,
        ESubsystemTickPhase::PreUpdate, 1300};
    const FSubsystemFactory SecondFactory{
        SubsystemKindOf<CTickParentTeardownSecond>(), ESubsystemScope::GameInstance,
        "CTickParentTeardownSecond", &CreateTickParentTeardownSecond,
        ESubsystemTickPhase::PreUpdate, 1301};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(FirstFactory));
    EXPECT_TRUE(Registry.TryRegister(SecondFactory));

    for (u32 Reinitialize = 0u; Reinitialize < 2u; ++Reinitialize) {
        CSubsystemCollection Parent;
        CSubsystemCollection Child;
        FTickParentTeardownTrace Trace{};
        Trace.parent = &Parent;
        Trace.parent_owner = FSubsystemOwner{&Trace, ESubsystemOwnerKind::Application};
        Trace.reinitialize_parent = Reinitialize != 0u;
        EXPECT_TRUE(Parent.TryInitialize(
            ESubsystemScope::Engine, nullptr, Trace.parent_owner));
        EXPECT_TRUE(Child.TryInitialize(
            ESubsystemScope::GameInstance, &Parent,
            FSubsystemOwner{&Trace, ESubsystemOwnerKind::Game}));

        Child.TickFrame(FSubsystemFrameContext{
            0.25f, 0.5f, 7u, ESubsystemTickPhase::PreUpdate});
        EXPECT_EQ(Trace.first_ticks, 1u);
        EXPECT_EQ(Trace.second_ticks, 0u);
        EXPECT_EQ(Trace.deinitialize_count, 2u);
        EXPECT_EQ(Trace.deinitialize_order[0], 'B');
        EXPECT_EQ(Trace.deinitialize_order[1], 'A');
        EXPECT_TRUE(!Child.IsInitialized());
        EXPECT_EQ(Child.Count(), 0u);
        EXPECT_TRUE(Child.Get<CTickParentTeardownFirst>() == nullptr);
        if (Reinitialize == 0u) {
            EXPECT_TRUE(!Parent.IsInitialized());
        } else {
            EXPECT_TRUE(Trace.parent_reinitialize_result);
            EXPECT_TRUE(Parent.IsInitialized());
        }
    }
    EXPECT_TRUE(Registry.Unregister(SecondFactory));
    EXPECT_TRUE(Registry.Unregister(FirstFactory));
}

ACS_TEST(Subsystem, TickRequestImmediatelyHidesSelfAndCommittedChild)
{
    /** Engine更新中に終了要求を出すfactory。 */
    const FSubsystemFactory ParentFactory{
        SubsystemKindOf<CTickVisibilityParent>(), ESubsystemScope::Engine,
        "CTickVisibilityParent", &CreateTickVisibilityParent,
        ESubsystemTickPhase::PreUpdate, 1310};
    /** GameInstance更新が遮断されることを観測するfactory。 */
    const FSubsystemFactory ChildFactory{
        SubsystemKindOf<CTickVisibilityChild>(), ESubsystemScope::GameInstance,
        "CTickVisibilityChild", &CreateTickVisibilityChild,
        ESubsystemTickPhase::PreUpdate, 1310};
    /** process共通factory registry。 */
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(ParentFactory));
    EXPECT_TRUE(Registry.TryRegister(ChildFactory));

    /** Engine collection。 */
    CSubsystemCollection Parent;
    /** GameInstance collection。 */
    CSubsystemCollection Child;
    /** callbackから参照する観測状態。 */
    FTickVisibilityTrace Trace{};
    Trace.parent = &Parent;
    Trace.child = &Child;
    Trace.parent_owner = FSubsystemOwner{&Trace, ESubsystemOwnerKind::Application};
    EXPECT_TRUE(Parent.TryInitialize(
        ESubsystemScope::Engine, nullptr, Trace.parent_owner));
    EXPECT_TRUE(Child.TryInitialize(
        ESubsystemScope::GameInstance, &Parent,
        FSubsystemOwner{&Trace, ESubsystemOwnerKind::Game}));

    Parent.TickFrame(FSubsystemFrameContext{
        0.25f, 0.5f, 8u, ESubsystemTickPhase::PreUpdate});
    EXPECT_TRUE(Trace.parent_visible_before_request);
    EXPECT_TRUE(Trace.child_visible_before_request);
    EXPECT_TRUE(Trace.exact_initialize_before_request);
    EXPECT_TRUE(Trace.parent_hidden_after_request);
    EXPECT_TRUE(Trace.child_hidden_after_request);
    EXPECT_TRUE(!Trace.exact_initialize_after_request);
    EXPECT_TRUE(!Parent.IsInitialized());

    Child.TickFrame(FSubsystemFrameContext{
        0.25f, 0.5f, 9u, ESubsystemTickPhase::PreUpdate});
    EXPECT_EQ(Trace.child_ticks, 0u);
    EXPECT_EQ(Trace.child_deinitializes, 1u);
    EXPECT_TRUE(!Child.IsInitialized());
    EXPECT_EQ(Child.Count(), 0u);
    EXPECT_TRUE(Child.Get<CTickVisibilityChild>() == nullptr);

    EXPECT_TRUE(Registry.Unregister(ChildFactory));
    EXPECT_TRUE(Registry.Unregister(ParentFactory));
}

ACS_TEST(Subsystem, DeepCommittedChainRejectsAncestorReinitializeBeforeParentTick)
{
    /** World更新の実行有無を観測するfactory。 */
    const FSubsystemFactory WorldFactory{
        SubsystemKindOf<CAncestorWorldProbe>(), ESubsystemScope::World,
        "CAncestorWorldProbe", &CreateAncestorWorldProbe,
        ESubsystemTickPhase::PreUpdate, 1310};
    /** process共通factory registry。 */
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(WorldFactory));

    /** Engine collection。 */
    CSubsystemCollection Engine;
    /** GameInstance collection。 */
    CSubsystemCollection GameInstance;
    /** World collection。 */
    CSubsystemCollection World;
    /** typed ownerへ使うEngine識別値。 */
    int EngineOwner = 0;
    /** typed ownerへ使うGameInstance識別値。 */
    int GameOwner = 0;
    /** typed ownerへ使うWorld識別値。 */
    int WorldOwner = 0;
    /** 同一Engine再初期化に使うowner。 */
    const FSubsystemOwner EngineDescriptor{
        &EngineOwner, ESubsystemOwnerKind::Application};
    EXPECT_TRUE(Engine.TryInitialize(
        ESubsystemScope::Engine, nullptr, EngineDescriptor));
    EXPECT_TRUE(GameInstance.TryInitialize(
        ESubsystemScope::GameInstance, &Engine,
        FSubsystemOwner{&GameOwner, ESubsystemOwnerKind::Game}));
    EXPECT_TRUE(World.TryInitialize(
        ESubsystemScope::World, &GameInstance,
        FSubsystemOwner{&WorldOwner, ESubsystemOwnerKind::Scene}));
    EXPECT_TRUE(World.Get<CAncestorWorldProbe>() != nullptr);

    Engine.Deinitialize();
    EXPECT_TRUE(Engine.TryInitialize(
        ESubsystemScope::Engine, nullptr, EngineDescriptor));
    EXPECT_TRUE(Engine.IsInitialized());
    EXPECT_TRUE(!GameInstance.IsInitialized());
    EXPECT_TRUE(!World.IsInitialized());
    EXPECT_EQ(World.Count(), 0u);
    EXPECT_TRUE(World.Get<CAncestorWorldProbe>() == nullptr);

    g_AncestorWorldTicks = 0u;
    World.TickFrame(FSubsystemFrameContext{
        0.25f, 0.5f, 10u, ESubsystemTickPhase::PreUpdate});
    EXPECT_EQ(g_AncestorWorldTicks, 0u);
    EXPECT_TRUE(!World.IsInitialized());
    EXPECT_EQ(World.Count(), 0u);
    EXPECT_TRUE(World.Get<CAncestorWorldProbe>() == nullptr);
    EXPECT_TRUE(!GameInstance.IsInitialized());
    EXPECT_TRUE(Engine.IsInitialized());

    GameInstance.Deinitialize();
    Engine.Deinitialize();
    EXPECT_TRUE(Registry.Unregister(WorldFactory));
}

ACS_TEST(Subsystem, SceneLifecycleWrapperWiresRootAndHandlesMissingCamera)
{
    CTransitionProbeGame Game;
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));

    FSceneRootLifecycleTrace Trace{};
    Game.Scenes().PushScene(MakeUnique<ANonCameraLifecycleScene>(&Trace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    ANonCameraLifecycleScene* const Scene = static_cast<ANonCameraLifecycleScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Scene != nullptr);
    if (Scene == nullptr) return;

    EXPECT_TRUE(Trace.entered);
    EXPECT_TRUE(Trace.root_services_wired);
    EXPECT_TRUE(Trace.component_services_attached);
    EXPECT_TRUE(Scene->HasServices());
    EXPECT_TRUE(Scene->Services().Has(ESvc::Clock));
    EXPECT_TRUE(!Scene->Services().Has(ESvc::Camera2D));
    EXPECT_EQ(Scene->Root().ChildCount(), 1u);
    EXPECT_EQ(Scene->Services().Physics().ShapeCount(), 1u);
    EXPECT_EQ(Scene->Services().Tweens().ActiveCount(), 1u);
    EXPECT_EQ(Scene->ViewCenter().x, 0.0f);
    EXPECT_EQ(Scene->ViewCenter().y, 0.0f);
    EXPECT_EQ(Scene->ViewZoom(), 1.0f);

    Game.Scenes().ExecutionAccess().Update(0.25f, 0.25f, 1u);
    Game.Scenes().ExecutionAccess().FixedUpdate(0.5f);
    EXPECT_EQ(Trace.scene_updates, 1u);
    EXPECT_EQ(Trace.scene_fixed_updates, 1u);
    EXPECT_EQ(Trace.component_updates, 1u);
    EXPECT_EQ(Trace.component_fixed_updates, 1u);

    Game.Scenes().ChangeScene(MakeUnique<AScene>());
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    EXPECT_TRUE(Trace.exited);
    EXPECT_EQ(Scene->Root().ChildCount(), 0u);
    EXPECT_EQ(Scene->Services().Physics().ShapeCount(), 0u);
    EXPECT_EQ(Scene->Services().Tweens().ActiveCount(), 0u);
    Game.Scenes().ExecutionAccess().ShutdownAll();
    Game.GameInstanceSubsystems().Deinitialize();
    Game.EngineSubsystems().Deinitialize();
}

ACS_TEST(Subsystem, SceneServiceAllocationFailureNeverCommitsPartialCandidate)
{
    CTransitionProbeGame Game;
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    FSceneTransitionTrace PreviousTrace{};
    Game.Scenes().PushScene(MakeUnique<ATransitionProbeScene>(&PreviousTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    AScene* const Previous = Game.Scenes().Top();
    EXPECT_TRUE(Previous != nullptr);

    for (u32 FailureCall = 1u; FailureCall <= 8u; ++FailureCall) {
        FServiceAllocationTrace Trace{};
        TUniquePtr<AScene> Candidate = MakeUnique<AAllServicesProbeScene>(&Trace);
        EXPECT_TRUE(Candidate.Get() != nullptr);
        CSubsystemCountingAllocator Allocator;
        Allocator.fail_on_allocation = FailureCall;
        {
            CSubsystemDefaultAllocatorScope AllocatorScope(Allocator);
            Game.Scenes().ChangeScene(Move(Candidate));
            Game.Scenes().ExecutionAccess().ApplyPending(Game);
        }
        EXPECT_TRUE(Game.Scenes().Top() == Previous);
        EXPECT_EQ(Game.Scenes().Depth(), 1u);
        EXPECT_EQ(PreviousTrace.paused, 0u);
        EXPECT_EQ(PreviousTrace.exited, 0u);
        EXPECT_TRUE(Trace.destroyed);
        EXPECT_TRUE(!Trace.had_services);
        EXPECT_EQ(Trace.entered, 0u);
        EXPECT_EQ(Allocator.outstanding_allocations, 0u);
    }
}

ACS_TEST(Subsystem, Scene2DRootAllocationFailureRejectsPushAndChange)
{
    CTransitionProbeGame Game;
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    FSceneTransitionTrace PreviousTrace{};
    Game.Scenes().PushScene(MakeUnique<ATransitionProbeScene>(&PreviousTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    AScene* const Previous = Game.Scenes().Top();
    EXPECT_TRUE(Previous != nullptr);

    const FSubsystemFactory WorldProbe{
        SubsystemKindOf<COwnerValidationProbe>(), ESubsystemScope::World,
        "FRootReadinessWorldProbe", &CreateOwnerValidationProbe,
        ESubsystemTickPhase::None, 1300};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(WorldProbe));
    g_OwnerProbeInitializeCount = 0u;

    for (u32 Operation = 0u; Operation < 2u; ++Operation) {
        CSubsystemCountingAllocator Allocator;
        Allocator.fail_on_allocation = 2u;
        FRootAllocationTrace Trace{};
        {
            CSubsystemDefaultAllocatorScope AllocatorScope(Allocator);
            TUniquePtr<AScene> Candidate = MakeUnique<ARootAllocationProbeScene>(&Trace);
            EXPECT_TRUE(Candidate.Get() != nullptr);
            if (Operation == 0u)
                Game.Scenes().PushScene(Move(Candidate));
            else
                Game.Scenes().ChangeScene(Move(Candidate));
            Game.Scenes().ExecutionAccess().ApplyPending(Game);
        }
        EXPECT_TRUE(Game.Scenes().Top() == Previous);
        EXPECT_EQ(Game.Scenes().Depth(), 1u);
        EXPECT_EQ(PreviousTrace.paused, 0u);
        EXPECT_EQ(PreviousTrace.exited, 0u);
        EXPECT_TRUE(Trace.destroyed);
        EXPECT_EQ(Trace.entered, 0u);
        EXPECT_EQ(g_OwnerProbeInitializeCount, 0u);
        EXPECT_EQ(Allocator.outstanding_allocations, 0u);
    }
    EXPECT_TRUE(Registry.Unregister(WorldProbe));
}

ACS_TEST(Subsystem, WorldReadyHookGenerationRollbackKeepsPreviousTop)
{
    CTransitionProbeGame Game;
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    FSceneTransitionTrace PreviousTrace{};
    Game.Scenes().PushScene(MakeUnique<ATransitionProbeScene>(&PreviousTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    AScene* const Previous = Game.Scenes().Top();
    EXPECT_TRUE(Previous != nullptr);

    const FSubsystemFactory FactoryA{
        SubsystemKindOf<CWorldHookProbeA>(), ESubsystemScope::World,
        "CWorldHookProbeA", &CreateWorldHookProbeA,
        ESubsystemTickPhase::None, 1300};
    const FSubsystemFactory FactoryB{
        SubsystemKindOf<CWorldHookProbeB>(), ESubsystemScope::World,
        "CWorldHookProbeB", &CreateWorldHookProbeB,
        ESubsystemTickPhase::None, 1301};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(FactoryA));
    EXPECT_TRUE(Registry.TryRegister(FactoryB));

    for (u32 Operation = 0u; Operation < 2u; ++Operation) {
        FWorldHookReentryTrace Trace{};
        Trace.parent = &Game.GameInstanceSubsystems();
        Trace.reinitialize_world = Operation != 0u;
        g_WorldHookReentryTrace = &Trace;
        TUniquePtr<AScene> Candidate = MakeUnique<AWorldHookReentryScene>(&Trace);
        EXPECT_TRUE(Candidate.Get() != nullptr);
        if (Operation == 0u)
            Game.Scenes().PushScene(Move(Candidate));
        else
            Game.Scenes().ChangeScene(Move(Candidate));
        Game.Scenes().ExecutionAccess().ApplyPending(Game);
        EXPECT_TRUE(Game.Scenes().Top() == Previous);
        EXPECT_EQ(Game.Scenes().Depth(), 1u);
        EXPECT_EQ(PreviousTrace.paused, 0u);
        EXPECT_EQ(PreviousTrace.exited, 0u);
        EXPECT_EQ(Trace.entered, 0u);
        if (Operation == 0u) {
            EXPECT_EQ(Trace.callback_count, 4u);
            EXPECT_EQ(Trace.callbacks[0], 'A');
            EXPECT_EQ(Trace.callbacks[1], 'B');
            EXPECT_EQ(Trace.callbacks[2], 'b');
            EXPECT_EQ(Trace.callbacks[3], 'a');
        } else {
            EXPECT_TRUE(Trace.world_reinitialize_result);
            EXPECT_EQ(Trace.callback_count, 8u);
            EXPECT_EQ(Trace.callbacks[0], 'A');
            EXPECT_EQ(Trace.callbacks[1], 'B');
            EXPECT_EQ(Trace.callbacks[2], 'b');
            EXPECT_EQ(Trace.callbacks[3], 'a');
            EXPECT_EQ(Trace.callbacks[4], 'A');
            EXPECT_EQ(Trace.callbacks[5], 'B');
            EXPECT_EQ(Trace.callbacks[6], 'b');
            EXPECT_EQ(Trace.callbacks[7], 'a');
        }
    }
    g_WorldHookReentryTrace = nullptr;
    EXPECT_TRUE(Registry.Unregister(FactoryB));
    EXPECT_TRUE(Registry.Unregister(FactoryA));
}

ACS_TEST(Subsystem, FailedScenePreparationKeepsPreviousTopUnchanged)
{
    CTransitionProbeGame Game;
    EXPECT_TRUE(Game.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game}));
    FSceneTransitionTrace PreviousTrace{};
    Game.Scenes().PushScene(MakeUnique<ATransitionProbeScene>(&PreviousTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    AScene* const Previous = Game.Scenes().Top();
    EXPECT_TRUE(Previous != nullptr);
    EXPECT_EQ(PreviousTrace.entered, 1u);

    static const int InvalidWorldKind = 0;
    const FSubsystemFactory Invalid{
        &InvalidWorldKind, ESubsystemScope::World, "InvalidWorld",
        &CreateEmptySubsystem, ESubsystemTickPhase::PreUpdate, 1200};
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    EXPECT_TRUE(Registry.TryRegister(Invalid));

    FSceneTransitionTrace PushTrace{};
    Game.Scenes().PushScene(MakeUnique<ATransitionProbeScene>(&PushTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    EXPECT_TRUE(Game.Scenes().Top() == Previous);
    EXPECT_EQ(Game.Scenes().Depth(), 1u);
    EXPECT_EQ(PreviousTrace.paused, 0u);
    EXPECT_EQ(PushTrace.entered, 0u);

    FSceneTransitionTrace ChangeTrace{};
    Game.Scenes().ChangeScene(MakeUnique<ATransitionProbeScene>(&ChangeTrace));
    Game.Scenes().ExecutionAccess().ApplyPending(Game);
    EXPECT_TRUE(Game.Scenes().Top() == Previous);
    EXPECT_EQ(PreviousTrace.exited, 0u);
    EXPECT_EQ(ChangeTrace.entered, 0u);
    EXPECT_TRUE(Registry.Unregister(Invalid));
}
