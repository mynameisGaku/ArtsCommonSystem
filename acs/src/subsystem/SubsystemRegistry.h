// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "memory/SystemAllocator.h"
#include "subsystem/SubsystemFactory.h"

namespace acs {

struct CSubsystemAutoRegister;

/** owner thread から利用する、同一 link image 内のサブシステム登録簿。 */
class CSubsystemRegistry {
public:
    /** 同一 link image 内の process lifetime 登録簿を返す。 */
    static CSubsystemRegistry& Get() noexcept;

    /**
     * 手動 factory の登録を試みる。
     * 同じ実装の再登録は成功として扱い、容量不足または確保失敗で false を返す。
     * factory.nameは成功から対応Unregister完了までimmutableなNUL終端storageを借用する。
     */
    bool TryRegister(const FSubsystemFactory& factory) noexcept;

    /** TryRegisterと同じborrowed name寿命で登録し、kindの現在有効な実装ならtrueを返す。 */
    bool TryRegisterActive(const FSubsystemFactory& factory) noexcept;

    /** TryRegisterと同じborrowed name寿命で登録する旧API。失敗はTryRegisterで検査できる。 */
    void Register(const FSubsystemFactory& factory) noexcept;

    /** 一致する手動factoryを1件解除し、成功後にborrowed name寿命を呼出側へ戻す。 */
    bool Unregister(const FSubsystemFactory& factory) noexcept;

    /** 現在選択されている factory 数を返す。 */
    u32 Count() const noexcept;

    /** 指定位置の現在選択されている factory を返す。 */
    const FSubsystemFactory& At(u32 index) const noexcept;

    /**
     * 現在選択されている factory を all-or-none で複製する。
     * 登録・解除・snapshot は同じ owner thread で直列に呼ぶ。
     */
    bool TrySnapshot(TArray<FSubsystemFactory>& output) const noexcept;

private:
    friend struct CSubsystemAutoRegister;

    /** 同じ kind へ同時登録できる module/source 数。 */
    static constexpr u32 kMaxSourcesPerFactory = 8u;

    /** 1 つの登録元と解除用 token。 */
    struct FFactorySource {
        /** 登録された factory 値。 */
        FSubsystemFactory factory{};
        /** 自動登録元の識別子。手動登録は nullptr。 */
        const void* token = nullptr;
    };

    /** 同じ kind に対する優先順付き登録元。 */
    struct FFactoryEntry {
        /** 最初の要素を現在の factory とする登録元一覧。 */
        FFactorySource sources[kMaxSourcesPerFactory]{};
        /** 使用中の登録元数。 */
        u8 source_count = 0u;
    };

    /** process lifetime allocator へ登録簿を接続する。 */
    CSubsystemRegistry() noexcept;

    CSubsystemRegistry(const CSubsystemRegistry&) = delete;
    CSubsystemRegistry& operator=(const CSubsystemRegistry&) = delete;

    /** null を含む C 文字列を比較する。 */
    static bool StrEq(const char* left, const char* right) noexcept;

    /** 同じ登録実装を表すか判定する。 */
    static bool SameImplementation(
        const FSubsystemFactory& left, const FSubsystemFactory& right) noexcept;

    /** token 付き factory の登録を試みる。 */
    bool RegisterSource(const FSubsystemFactory& factory, const void* token) noexcept;

    /** token 付き factory を 1 件解除する。 */
    bool UnregisterSource(const FSubsystemFactory& factory, const void* token) noexcept;

    /** 一時 DefaultAllocator から独立した process lifetime allocator。 */
    FSystemAllocator m_Allocator;

    /** kind ごとの登録元一覧。 */
    TArray<FFactoryEntry> m_Entries;
};

/** 旧公開名を正規サブシステム登録簿型へ接続する互換別名。 */
using FSubsystemRegistry = CSubsystemRegistry;

/** マクロによる登録元を寿命終了時に解除する補助値。 */
struct CSubsystemAutoRegister {
    /** factory を token 付きで登録する。 */
    explicit CSubsystemAutoRegister(const FSubsystemFactory& factory) noexcept;

    /** 登録に成功した factory を解除する。 */
    ~CSubsystemAutoRegister() noexcept;

    CSubsystemAutoRegister(const CSubsystemAutoRegister&) = delete;
    CSubsystemAutoRegister& operator=(const CSubsystemAutoRegister&) = delete;

private:
    /** 解除時に照合する factory 値。 */
    FSubsystemFactory m_Factory{};
    /** 登録が完了した場合だけ true。 */
    bool m_Registered = false;
};

/** 旧公開名を正規サブシステム自動登録型へ接続する互換別名。 */
using FSubsystemAutoRegister = CSubsystemAutoRegister;

} // namespace acs

/** phase と order を明示してサブシステム factory を自動登録する。 */
#define ACS_REGISTER_SUBSYSTEM_EX(T, SCOPE, PHASE, ORDER)                                      \
    namespace acs::game {                                                                        \
    namespace {                                                                                  \
    const ::acs::CSubsystemAutoRegister g_AcsSubsysReg_##T{::acs::FSubsystemFactory{             \
        ::acs::SubsystemKindOf<T>(), (SCOPE), #T,                                                \
        []() noexcept -> ::acs::TUniquePtr<::acs::ASubsystem> { return ::acs::MakeUnique<T>(); }, \
        (PHASE), (ORDER)}};                                                                       \
    }                                                                                            \
    }

/** 旧登録を PreUpdate、order 0 の factory として自動登録する。 */
#define ACS_REGISTER_SUBSYSTEM(T, SCOPE)                                                        \
    ACS_REGISTER_SUBSYSTEM_EX(T, SCOPE, ::acs::ESubsystemTickPhase::PreUpdate, 0)
