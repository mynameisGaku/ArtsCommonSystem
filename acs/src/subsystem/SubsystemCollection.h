// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "subsystem/Subsystem.h"
#include "subsystem/SubsystemFactory.h"
#include "subsystem/SubsystemFrameContext.h"
#include "subsystem/SubsystemOwner.h"
#include "subsystem/SubsystemScope.h"

namespace acs {

/** 1 owner スコープのサブシステムを決定順序で所有・更新する。 */
class FSubsystemCollection {
public:
    /** 未初期化の空コレクションを構築する。 */
    FSubsystemCollection() noexcept = default;

    /** 初期化済み要素を逆順に終了して破棄する。 */
    ~FSubsystemCollection() noexcept;

    FSubsystemCollection(const FSubsystemCollection&) = delete;
    FSubsystemCollection& operator=(const FSubsystemCollection&) = delete;

    /**
     * 登録 snapshot から対象スコープを全生成し、owner 検証後に初期化する。
     * factory、確保、生成、owner 検証の失敗時は callback を開始せず元の状態を保つ。
     * parent は callback 外で Active な直上スコープだけを受け入れ、null は standalone とする。
     * parent object はこのcollectionより長く生存する。parent lifecycle世代が変わると即座に
     * childを不可視とし、次のTickFrameで初期化済み要素を逆順に終了する。
     */
    bool TryInitialize(
        ESubsystemScope scope,
        FSubsystemCollection* parent = nullptr,
        FSubsystemOwner owner = {}) noexcept;

    /** 種別を持たない旧 owner ポインタで初期化を試みる互換 API。 */
    bool TryInitialize(
        ESubsystemScope scope, FSubsystemCollection* parent, void* owner) noexcept;

    /** 失敗結果を破棄する旧初期化 API。 */
    void Initialize(
        ESubsystemScope scope,
        FSubsystemCollection* parent = nullptr,
        void* owner = nullptr) noexcept;

    /** 初期化済み要素を逆順に終了する。callback 中の要求は callback 後へ延期する。 */
    void Deinitialize() noexcept;

    /** 指定段階と一致する要素だけを決定順序で 1 回更新する。 */
    void TickFrame(const FSubsystemFrameContext& context) noexcept;

    /**
     * 旧更新 API。
     * None 以外を PreUpdate、PostUpdate の順でそれぞれ 1 回だけ更新する。
     */
    void Tick(f32 delta_seconds) noexcept;

    /** 自スコープの可視要素、続いて parent から種別 ID を検索する。 */
    FSubsystem* GetByKind(const void* kind) const noexcept;

    /** 自スコープから parent の順に派生型を検索する。 */
    template<typename T>
    T* Get() const noexcept
    {
        return static_cast<T*>(GetByKind(SubsystemKindOf<T>()));
    }

    /** 現在 callback から安全に参照できる自スコープ要素数を返す。 */
    u32 Count() const noexcept
    {
        if ((m_State == EState::Active || m_State == EState::Ticking) &&
            (!IsLogicallyActive() || !CommittedParentMatches())) return 0u;
        return m_VisibleCount;
    }

    /** 最後に指定されたスコープを返す。 */
    ESubsystemScope Scope() const noexcept { return m_Scope; }

    /** 成功した初期化と終了ごとに変わる非循環lifecycle世代を返す。 */
    u64 LifecycleGeneration() const noexcept { return m_LifecycleGeneration; }

    /** 通常利用または更新 callback 中なら true を返す。 */
    bool IsInitialized() const noexcept;

private:
    /** lifecycle callback の再入を制御する状態。 */
    enum class EState : u8 {
        /** callback と所有要素がない。 */
        Uninitialized = 0,
        /** owner 検証または初期化 callback を実行中。 */
        Initializing = 1,
        /** 通常利用できる。 */
        Active = 2,
        /** 更新 callback を実行中。 */
        Ticking = 3,
        /** 終了 callback を実行中。 */
        Deinitializing = 4,
    };

    /** 通常利用中または終了要求前の更新 callback 中か判定する。 */
    bool IsLogicallyActive() const noexcept;

    /** 所有実体と更新順序をまとめる内部要素。 */
    struct FEntry {
        /** サブシステム実体。 */
        TUniquePtr<FSubsystem> instance{};
        /** factory生成時に一度だけ照合した種別。 */
        const void* kind = nullptr;
        /** 自動更新する段階。 */
        ESubsystemTickPhase phase = ESubsystemTickPhase::None;
        /** 初期化と更新の第 1 決定キー。 */
        i32 order = 0;
        /** 初期化と更新の第 2 決定キー。 */
        const char* name = "";
    };

    /** typed ownerの責務種別とscopeが一致するか判定する。 */
    static bool IsValidOwner(
        ESubsystemScope scope, const FSubsystemOwner& owner) noexcept;

    /** parent が自分または循環済み chain を含むか判定する。 */
    bool HasInvalidParent(
        ESubsystemScope scope, const FSubsystemCollection* parent) const noexcept;

    /** commit済みchainがcallback中を含め論理的に有効か判定する。 */
    bool HasInvalidCommittedParent(
        ESubsystemScope scope, const FSubsystemCollection* parent) const noexcept;

    /** parentのscope、Active状態、lifecycle世代が開始時点と一致するか判定する。 */
    bool ParentMatches(
        ESubsystemScope scope, const FSubsystemCollection* parent,
        u64 lifecycle_generation) const noexcept;

    /** commit済みparentが同じActive lifecycleを保つか判定する。 */
    bool CommittedParentMatches() const noexcept;

    /** left を right より先に置くか order、name の順で判定する。 */
    static bool EntryLess(const FEntry& left, const FEntry& right) noexcept;

    /** 初期化済みの可視 prefix だけを逆順 callback で終了する。 */
    void TeardownVisibleEntries() noexcept;

    /** 全要素の owner descriptor を空へ戻す。 */
    void ClearOwners() noexcept;

    /** 所有するサブシステムと決定順序。 */
    TArray<FEntry> m_Subsystems;
    /** 上位スコープの非所有コレクション。 */
    FSubsystemCollection* m_Parent = nullptr;
    /** callback から安全に参照できる先頭要素数。 */
    u32 m_VisibleCount = 0u;
    /** このコレクションの owner 寿命スコープ。 */
    ESubsystemScope m_Scope = ESubsystemScope::World;
    /** 現在の lifecycle 状態。 */
    EState m_State = EState::Uninitialized;
    /** callback 終了後に Deinitialize する要求。 */
    bool m_DeinitializeRequested = false;
    /** commit済み初期化を識別するowner descriptor。 */
    FSubsystemOwner m_Owner{};
    /** parent再初期化を識別する非循環lifecycle世代。 */
    u64 m_LifecycleGeneration = 0u;
    /** commit時に捕捉したparent lifecycle世代。 */
    u64 m_ParentGeneration = 0u;
};

static_assert(sizeof(FSubsystemCollection) == 80u);
static_assert(alignof(FSubsystemCollection) == 8u);

} // namespace acs
