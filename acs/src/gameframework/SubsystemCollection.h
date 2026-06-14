// SPDX-License-Identifier: Apache-2.0
// GameFramework — FSubsystemCollection (1 スコープぶんのサブシステム束)
//
// 1 つのスコープ(Engine / GameInstance / World)に属するサブシステム群を所有する。
// Initialize() で登録簿から該当スコープのサブシステムを «全て» 生成し OnInitialize。
// Get<T>() は自スコープに無ければ parent(上位スコープ)へフォールバック検索する
// (World → GameInstance → Engine)。Deinitialize() は生成の逆順で OnDeinitialize。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemRegistry.h"

namespace acs::game {

/**
 * 1 スコープぶんのサブシステムを所有・駆動するコレクション。
 *
 * @details
 * FGame が Engine / GameInstance を、各 Scene が World を 1 つずつ持つ。parent を辿る
 * フォールバック検索で、下位スコープから上位スコープのサブシステムも透過的に取得できる。
 */
class FSubsystemCollection {
public:
    /** 空(未初期化)で構築する。 */
    FSubsystemCollection() noexcept = default;

    /** 破棄時に Deinitialize する。 */
    ~FSubsystemCollection() noexcept { Deinitialize(); }

    FSubsystemCollection(const FSubsystemCollection&)            = delete;
    FSubsystemCollection& operator=(const FSubsystemCollection&) = delete;

    /**
     * 指定スコープのサブシステムを登録簿から全て生成し、順に OnInitialize する。
     *
     * @param scope  このコレクションのスコープ。
     * @param parent 上位スコープのコレクション(Get<T> のフォールバック先。null 可)。
     */
    void Initialize(ESubsystemScope scope, FSubsystemCollection* parent = nullptr) noexcept {
        if (m_Initialized) return;
        m_Scope  = scope;
        m_Parent = parent;
        FSubsystemRegistry& reg = FSubsystemRegistry::Get();
        for (u32 i = 0; i < reg.Count(); ++i) {
            const FSubsystemFactory& f = reg.At(i);
            if (f.scope != scope || f.create == nullptr) continue;
            TUniquePtr<FSubsystem> s = f.create();
            if (!s) continue;
            FSubsystem* raw = s.Get();
            m_Subsystems.PushBack(Move(s));
            raw->OnInitialize();             // 生成順に初期化(リスト末尾に積んでから fire)
        }
        m_Initialized = true;
    }

    /** 生成の逆順で OnDeinitialize して破棄する(依存の逆解体)。冪等。 */
    void Deinitialize() noexcept {
        if (!m_Initialized) return;
        for (u32 i = m_Subsystems.Size(); i > 0; --i) {
            if (m_Subsystems[i - 1]) m_Subsystems[i - 1]->OnDeinitialize();
        }
        m_Subsystems.Clear();
        m_Initialized = false;
        m_Parent = nullptr;
    }

    /** 所有する全サブシステムを 1 フレーム進める。 */
    void Tick(f32 dt) noexcept {
        for (u32 i = 0; i < m_Subsystems.Size(); ++i) {
            if (m_Subsystems[i]) m_Subsystems[i]->OnTick(dt);
        }
    }

    /**
     * 種別 ID でサブシステムを引く。自スコープに無ければ parent を辿る。
     *
     * @param kind SubsystemKindOf<T>()。
     * @return 見つかったサブシステム(無ければ nullptr)。
     */
    FSubsystem* GetByKind(const void* kind) const noexcept {
        for (u32 i = 0; i < m_Subsystems.Size(); ++i) {
            if (m_Subsystems[i] && m_Subsystems[i]->Kind() == kind) return m_Subsystems[i].Get();
        }
        return (m_Parent != nullptr) ? m_Parent->GetByKind(kind) : nullptr;
    }

    /**
     * 型でサブシステムを引く(自 → 上位スコープへフォールバック)。
     *
     * @tparam T FSubsystem 派生型。
     * @return T*(未登録/未初期化なら nullptr)。
     */
    template<typename T>
    T* Get() const noexcept {
        return static_cast<T*>(GetByKind(SubsystemKindOf<T>()));
    }

    /** このスコープで生成済みのサブシステム数(parent は含まない)。 */
    u32 Count() const noexcept { return static_cast<u32>(m_Subsystems.Size()); }

    /** このコレクションのスコープ。 */
    ESubsystemScope Scope() const noexcept { return m_Scope; }

    /** 初期化済みか。 */
    bool IsInitialized() const noexcept { return m_Initialized; }

private:
    TArray<TUniquePtr<FSubsystem>> m_Subsystems;                  ///< 所有するサブシステム群。
    FSubsystemCollection*          m_Parent      = nullptr;       ///< 上位スコープ(フォールバック先)。
    ESubsystemScope                m_Scope       = ESubsystemScope::World;
    bool                           m_Initialized = false;
};

} // namespace acs::game
