// SPDX-License-Identifier: Apache-2.0
#include "subsystem/SubsystemCollection.h"

#include "foundation/Move.h"
#include "subsystem/SubsystemRegistry.h"

namespace acs {
namespace {

/** lifecycle世代を循環させず退役させる上限。 */
constexpr u64 kMaximumLifecycleGeneration = ~static_cast<u64>(0);

/** left が right より辞書順で前なら true を返す。 */
bool StringLess(const char* left, const char* right) noexcept
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return static_cast<unsigned char>(*left) < static_cast<unsigned char>(*right);
}

/** 2つの非null C文字列が同じ内容かを判定する。 */
bool StringEquals(const char* Left, const char* Right) noexcept
{
    while (*Left != '\0' && *Left == *Right) {
        ++Left;
        ++Right;
    }
    return *Left == *Right;
}

} // namespace

FSubsystemCollection::~FSubsystemCollection() noexcept
{
    Deinitialize();
}

bool FSubsystemCollection::IsValidOwner(ESubsystemScope scope, const FSubsystemOwner& owner) noexcept
{
    if (owner.kind == ESubsystemOwnerKind::Unknown) return true;
    if (owner.pointer == nullptr) return false;
    if (scope == ESubsystemScope::Engine) {
        return owner.kind == ESubsystemOwnerKind::Application;
    }
    if (scope == ESubsystemScope::GameInstance) {
        return owner.kind == ESubsystemOwnerKind::Game;
    }
    if (scope == ESubsystemScope::World) {
        return owner.kind == ESubsystemOwnerKind::Scene;
    }
    return false;
}

bool FSubsystemCollection::HasInvalidParent(ESubsystemScope scope, const FSubsystemCollection* parent) const noexcept
{
    if (parent == nullptr) return false;
    /** 現在検査中のchild scope。 */
    ESubsystemScope ChildScope = scope;
    /** scope階層を上る非所有カーソル。 */
    const FSubsystemCollection* Current = parent;
    /** Engineまでの最大2段を超えるchainを拒否する。 */
    u32 Depth = 0u;
    while (Current != nullptr) {
        if (Current == this || Current->m_State != EState::Active || Depth >= 2u) return true;
        const bool DirectParent =
            (ChildScope == ESubsystemScope::GameInstance && Current->m_Scope == ESubsystemScope::Engine) ||
            (ChildScope == ESubsystemScope::World && Current->m_Scope == ESubsystemScope::GameInstance);
        if (!DirectParent) return true;
        if (Current->m_Parent == nullptr) {
            if (Current->m_ParentGeneration != 0u) return true;
        } else if (Current->m_ParentGeneration != Current->m_Parent->m_LifecycleGeneration) {
            return true;
        }
        ChildScope = Current->m_Scope;
        Current = Current->m_Parent;
        ++Depth;
    }
    return false;
}

bool FSubsystemCollection::ParentMatches(ESubsystemScope scope, const FSubsystemCollection* parent,
                                         u64 lifecycle_generation) const noexcept
{
    if (parent == nullptr) return lifecycle_generation == 0u;
    return !HasInvalidParent(scope, parent) && parent->m_LifecycleGeneration == lifecycle_generation;
}

bool FSubsystemCollection::HasInvalidCommittedParent(ESubsystemScope scope,
                                                     const FSubsystemCollection* parent) const noexcept
{
    if (parent == nullptr) return false;
    /** 現在検査中のchild scope。 */
    ESubsystemScope ChildScope = scope;
    /** scope階層を上る非所有カーソル。 */
    const FSubsystemCollection* Current = parent;
    /** Engineまでの最大2段を超えるchainを拒否する。 */
    u32 Depth = 0u;
    while (Current != nullptr) {
        const bool LifecycleActive = Current->m_State == EState::Active ||
                                     (Current->m_State == EState::Ticking && !Current->m_DeinitializeRequested);
        if (Current == this || !LifecycleActive || Depth >= 2u) return true;
        const bool DirectParent =
            (ChildScope == ESubsystemScope::GameInstance && Current->m_Scope == ESubsystemScope::Engine) ||
            (ChildScope == ESubsystemScope::World && Current->m_Scope == ESubsystemScope::GameInstance);
        if (!DirectParent) return true;
        if (Current->m_Parent == nullptr) {
            if (Current->m_ParentGeneration != 0u) return true;
        } else if (Current->m_ParentGeneration != Current->m_Parent->m_LifecycleGeneration) {
            return true;
        }
        ChildScope = Current->m_Scope;
        Current = Current->m_Parent;
        ++Depth;
    }
    return false;
}

bool FSubsystemCollection::CommittedParentMatches() const noexcept
{
    if (m_Parent == nullptr) return m_ParentGeneration == 0u;
    return !HasInvalidCommittedParent(m_Scope, m_Parent) && m_Parent->m_LifecycleGeneration == m_ParentGeneration;
}

bool FSubsystemCollection::IsLogicallyActive() const noexcept
{
    return m_State == EState::Active || (m_State == EState::Ticking && !m_DeinitializeRequested);
}

bool FSubsystemCollection::EntryLess(const FEntry& left, const FEntry& right) noexcept
{
    if (left.order != right.order) return left.order < right.order;
    return StringLess(left.name, right.name);
}

bool FSubsystemCollection::TryInitialize(ESubsystemScope scope, FSubsystemCollection* parent,
                                         FSubsystemOwner owner) noexcept
{
    if (m_State == EState::Active || m_State == EState::Ticking) {
        return IsLogicallyActive() && CommittedParentMatches() && scope == m_Scope && parent == m_Parent &&
               owner.pointer == m_Owner.pointer && owner.kind == m_Owner.kind;
    }
    if (m_State != EState::Uninitialized || m_LifecycleGeneration >= kMaximumLifecycleGeneration - 1u ||
        !IsValidSubsystemScope(scope) || !IsValidOwner(scope, owner) || HasInvalidParent(scope, parent)) {
        return false;
    }

    /** 初期化開始時点のparent lifecycle世代。 */
    const u64 ParentGeneration = parent != nullptr ? parent->m_LifecycleGeneration : 0u;
    /** 失敗時に戻す旧parent。 */
    FSubsystemCollection* const PreviousParent = m_Parent;
    /** 失敗時に戻す旧scope。 */
    const ESubsystemScope PreviousScope = m_Scope;
    /** 失敗時に戻す旧owner descriptor。 */
    const FSubsystemOwner PreviousOwner = m_Owner;
    /** 失敗時に戻す旧parent lifecycle世代。 */
    const u64 PreviousParentGeneration = m_ParentGeneration;

    // snapshotとfactory生成を含む全準備期間を再入不可にする。
    m_State = EState::Initializing;
    m_DeinitializeRequested = false;
    /** ローカル所有物の破棄後にだけ再初期化を許可するguard。 */
    struct FPreparationGuard {
        /** 復元するlifecycle状態。 */
        EState* state = nullptr;
        /** 復元する終了要求。 */
        bool* deinitialize_requested = nullptr;
        /** commit成功後は復元しない。 */
        bool dismissed = false;

        ~FPreparationGuard() noexcept
        {
            if (dismissed) return;
            *state = EState::Uninitialized;
            *deinitialize_requested = false;
        }
    } PreparationGuard{&m_State, &m_DeinitializeRequested, false};

    {
        /** 初期化開始時点の factory 一覧。以降の登録変更から分離する。 */
        TArray<FSubsystemFactory> Factories(*m_Subsystems.GetAllocator());
        if (!FSubsystemRegistry::Get().TrySnapshot(Factories)) return false;
        if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) return false;

        // create前に全factoryと決定順序キーを検証し、static初期化順への依存を拒否する。
        for (usize FactoryIndex = 0u; FactoryIndex < Factories.Size(); ++FactoryIndex) {
            const FSubsystemFactory& Factory = Factories[FactoryIndex];
            if (!IsValidSubsystemFactory(Factory)) return false;
            if (Factory.scope != scope) continue;
            for (usize PreviousIndex = 0u; PreviousIndex < FactoryIndex; ++PreviousIndex) {
                const FSubsystemFactory& Previous = Factories[PreviousIndex];
                if (Previous.scope == scope && Previous.order == Factory.order && Previous.name != nullptr &&
                    StringEquals(Previous.name, Factory.name)) {
                    return false;
                }
            }
        }

        /** factory 検証と生成を完了してから commit する一時所有配列。 */
        TArray<FEntry> Staged(*m_Subsystems.GetAllocator());
        if (!Staged.TryReserve(Factories.Size())) return false;
        if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) return false;
        for (usize FactoryIndex = 0u; FactoryIndex < Factories.Size(); ++FactoryIndex) {
            /** 今回の snapshot に含まれる登録値。 */
            const FSubsystemFactory& Factory = Factories[FactoryIndex];
            if (Factory.scope != scope) continue;
            /** factory から生成した未 commit 要素。 */
            FEntry Entry{};
            Entry.instance = Factory.create();
            if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration) || !Entry.instance)
                return false;
            /** factory契約と照合する生成実体の種別。 */
            const void* const CreatedKind = Entry.instance->Kind();
            if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration) ||
                CreatedKind != Factory.kind)
                return false;
            Entry.kind = Factory.kind;
            Entry.phase = Factory.phase;
            Entry.order = Factory.order;
            Entry.name = Factory.name;
            if (!Staged.TryPushBack(Move(Entry))) return false;
            if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) return false;
        }

        // factory callbackがparentを終了した場合はcommitしない。
        if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) return false;

        // 安定 insertion sort により同じ order/name の snapshot 順序も保持する。
        for (usize Index = 1u; Index < Staged.Size(); ++Index) {
            usize Position = Index;
            while (Position > 0u && EntryLess(Staged[Position], Staged[Position - 1u])) {
                Swap(Staged[Position], Staged[Position - 1u]);
                --Position;
            }
        }

        /** owner 検証失敗時に容量まで正確に戻す旧空配列。 */
        TArray<FEntry> Previous(Move(m_Subsystems));
        m_Subsystems = Move(Staged);
        m_Parent = parent;
        m_Scope = scope;
        m_Owner = owner;
        m_ParentGeneration = ParentGeneration;
        m_VisibleCount = 0u;
        m_DeinitializeRequested = false;

        for (usize Index = 0u; Index < m_Subsystems.Size(); ++Index) {
            m_Subsystems[Index].instance->_SetOwnerDescriptor(owner);
        }
        for (usize Index = 0u; Index < m_Subsystems.Size(); ++Index) {
            if (!m_Subsystems[Index].instance->OnOwnerAssigned() || m_DeinitializeRequested ||
                !ParentMatches(scope, parent, ParentGeneration)) {
                ClearOwners();
                m_Subsystems = Move(Previous);
                m_Parent = PreviousParent;
                m_Scope = PreviousScope;
                m_Owner = PreviousOwner;
                m_ParentGeneration = PreviousParentGeneration;
                m_VisibleCount = 0u;
                return false;
            }
        }

        for (usize Index = 0u; Index < m_Subsystems.Size(); ++Index) {
            m_VisibleCount = static_cast<u32>(Index + 1u);
            m_Subsystems[Index].instance->OnInitialize();
            if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) {
                TeardownVisibleEntries();
                m_Subsystems = Move(Previous);
                m_Parent = PreviousParent;
                m_Scope = PreviousScope;
                m_Owner = PreviousOwner;
                m_ParentGeneration = PreviousParentGeneration;
                m_VisibleCount = 0u;
                return false;
            }
        }

        if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) {
            TeardownVisibleEntries();
            m_Subsystems = Move(Previous);
            m_Parent = PreviousParent;
            m_Scope = PreviousScope;
            m_Owner = PreviousOwner;
            m_ParentGeneration = PreviousParentGeneration;
            m_VisibleCount = 0u;
            return false;
        }
    }

    // temporaryのdestructorやallocator FreeもInitializing中に完了させる。
    if (m_DeinitializeRequested || !ParentMatches(scope, parent, ParentGeneration)) {
        TeardownVisibleEntries();
        m_Parent = PreviousParent;
        m_Scope = PreviousScope;
        m_Owner = PreviousOwner;
        m_ParentGeneration = PreviousParentGeneration;
        m_VisibleCount = 0u;
        return false;
    }

    ++m_LifecycleGeneration;
    m_DeinitializeRequested = false;
    PreparationGuard.dismissed = true;
    m_State = EState::Active;
    return true;
}

bool FSubsystemCollection::TryInitialize(ESubsystemScope scope, FSubsystemCollection* parent, void* owner) noexcept
{
    return TryInitialize(scope, parent, FSubsystemOwner{owner, ESubsystemOwnerKind::Unknown});
}

void FSubsystemCollection::Initialize(ESubsystemScope scope, FSubsystemCollection* parent, void* owner) noexcept
{
    (void)TryInitialize(scope, parent, owner);
}

void FSubsystemCollection::ClearOwners() noexcept
{
    for (usize Index = 0u; Index < m_Subsystems.Size(); ++Index) {
        if (m_Subsystems[Index].instance) {
            m_Subsystems[Index].instance->_SetOwnerDescriptor(FSubsystemOwner{});
        }
    }
}

void FSubsystemCollection::TeardownVisibleEntries() noexcept
{
    m_State = EState::Deinitializing;
    while (m_VisibleCount > 0u) {
        /** まだ初期化済みで可視な末尾要素。 */
        FEntry& Entry = m_Subsystems[m_VisibleCount - 1u];
        if (Entry.instance) {
            Entry.instance->OnDeinitialize();
            Entry.instance->_SetOwnerDescriptor(FSubsystemOwner{});
        }
        --m_VisibleCount;
    }
    ClearOwners();
    m_Subsystems.ReleaseStorage();
}

void FSubsystemCollection::Deinitialize() noexcept
{
    if (m_State == EState::Uninitialized || m_State == EState::Deinitializing) return;
    if (m_State == EState::Initializing || m_State == EState::Ticking) {
        m_DeinitializeRequested = true;
        return;
    }

    TeardownVisibleEntries();
    m_Parent = nullptr;
    m_Owner = FSubsystemOwner{};
    m_ParentGeneration = 0u;
    m_State = EState::Uninitialized;
    m_DeinitializeRequested = false;
    ++m_LifecycleGeneration;
}

void FSubsystemCollection::TickFrame(const FSubsystemFrameContext& context) noexcept
{
    if (m_State == EState::Active && !CommittedParentMatches()) {
        Deinitialize();
        return;
    }
    if (m_State != EState::Active || context.phase == ESubsystemTickPhase::None ||
        !IsValidSubsystemTickPhase(context.phase)) {
        return;
    }

    m_State = EState::Ticking;
    m_DeinitializeRequested = false;
    for (u32 Index = 0u; Index < m_VisibleCount; ++Index) {
        if (!CommittedParentMatches()) {
            m_DeinitializeRequested = true;
            break;
        }
        FEntry& Entry = m_Subsystems[Index];
        if (Entry.phase == context.phase && Entry.instance) {
            Entry.instance->OnTickFrame(context);
            if (m_DeinitializeRequested || !CommittedParentMatches()) {
                m_DeinitializeRequested = true;
                break;
            }
        }
    }

    /** callback 後に実行する終了要求。 */
    const bool ShouldDeinitialize = m_DeinitializeRequested;
    m_State = EState::Active;
    m_DeinitializeRequested = false;
    if (ShouldDeinitialize) Deinitialize();
}

void FSubsystemCollection::Tick(f32 delta_seconds) noexcept
{
    /** 旧 API の更新前コンテキスト。 */
    const FSubsystemFrameContext PreContext{delta_seconds, delta_seconds, 0u, ESubsystemTickPhase::PreUpdate};
    TickFrame(PreContext);
    if (!IsInitialized()) return;

    /** 旧 API の更新後コンテキスト。 */
    const FSubsystemFrameContext PostContext{delta_seconds, delta_seconds, 0u, ESubsystemTickPhase::PostUpdate};
    TickFrame(PostContext);
}

FSubsystem* FSubsystemCollection::GetByKind(const void* kind) const noexcept
{
    if (kind == nullptr) return nullptr;
    /** commit済みparentを安全に辿れるか。 */
    const bool ParentAvailable = CommittedParentMatches();
    if ((m_State == EState::Active || m_State == EState::Ticking) && (!IsLogicallyActive() || !ParentAvailable)) {
        return nullptr;
    }
    for (u32 Index = 0u; Index < m_VisibleCount; ++Index) {
        const FEntry& Entry = m_Subsystems[Index];
        if (Entry.instance && Entry.kind == kind) return Entry.instance.Get();
    }
    return m_Parent != nullptr && ParentAvailable ? m_Parent->GetByKind(kind) : nullptr;
}

bool FSubsystemCollection::IsInitialized() const noexcept
{
    return IsLogicallyActive() && CommittedParentMatches();
}

} // namespace acs
