// SPDX-License-Identifier: Apache-2.0
// GameFramework — FSubsystemRegistry (サブシステム型のファクトリ登録簿)
//
// ACS_REGISTER_SUBSYSTEM(T, scope) で「この型をこのスコープで自動生成する」ことを
// 静的初期化時に登録する。スコープ開始時に FSubsystemCollection がこの登録簿を走査して
// 該当スコープのサブシステムを «全て» 生成する(UE の自動インスタンス化と同じ思想)。
//
// ヘッダオンリー: 登録簿は inline 関数内 static で 1 インスタンス。マクロは無名名前空間に
// 静的オブジェクトを置き、その ctor で登録する(TU ごとに 1 回)。
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "memory/SystemAllocator.h"
#include "container/Array.h"
#include "gameframework/Subsystem.h"

namespace acs::game {

/** サブシステム 1 体を生成するファクトリ関数(非捕捉ラムダ → 関数ポインタ)。 */
using FSubsystemCreateFn = TUniquePtr<FSubsystem> (*)();

/** 1 つのサブシステム型の登録情報(種別 ID・スコープ・名前・生成関数)。 */
struct FSubsystemFactory {
    const void*        kind   = nullptr;                    ///< SubsystemKindOf<T>()。
    ESubsystemScope    scope  = ESubsystemScope::World;     ///< 生成スコープ。
    const char*        name   = "";                         ///< 型名(デバッグ用)。
    FSubsystemCreateFn create = nullptr;                    ///< 1 体生成する関数。
};

struct FSubsystemAutoRegister;

/**
 * サブシステム型のファクトリ登録簿(グローバル単一)。ACS_REGISTER_SUBSYSTEM が登録する。
 */
class FSubsystemRegistry {
public:
    /** 単一インスタンスを返す(初回呼び出しで遅延構築 = 静的初期化順に非依存)。 */
    static FSubsystemRegistry& Get() noexcept {
        static FSubsystemRegistry s_instance;
        return s_instance;
    }

    /** ファクトリを登録する。同じ kind の異なる登録元は併存させる。 */
    void Register(const FSubsystemFactory& f) noexcept {
        RegisterSource(f, nullptr);
    }

    /**
     * 手動登録したファクトリを解除する。
     *
     * @return 一致する手動登録を 1 件解除できたら true。
     */
    bool Unregister(const FSubsystemFactory& factory) noexcept
    {
        return UnregisterSource(factory, nullptr);
    }

    /** 登録数。 */
    u32 Count() const noexcept
    {
        return static_cast<u32>(m_Entries.Size());
    }

    /** i 番目の登録情報。 */
    const FSubsystemFactory& At(u32 i) const noexcept
    {
        return m_Entries[i].sources[0].factory;
    }

private:
    friend struct FSubsystemAutoRegister;

    /** 同じ kind へ同時登録できる module/source 数。 */
    static constexpr u32 kMaxSourcesPerFactory = 8;

    struct FFactorySource {
        FSubsystemFactory factory{};
        const void* token = nullptr;
    };

    struct FFactoryEntry {
        FFactorySource sources[kMaxSourcesPerFactory]{};
        u8 source_count = 0;
    };

    /** process lifetime の登録簿が一時的な DefaultAllocator を捕捉しないようにする。 */
    FSubsystemRegistry() noexcept : m_Entries(m_Allocator)
    {
    }

    static bool StrEq(const char* a, const char* b) noexcept
    {
        if (a == nullptr || b == nullptr) return a == b;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        return *a == *b;
    }

    static bool SameImplementation(const FSubsystemFactory& a, const FSubsystemFactory& b) noexcept
    {
        return a.kind == b.kind && a.scope == b.scope && a.create == b.create && StrEq(a.name, b.name);
    }

    void RegisterSource(const FSubsystemFactory& factory, const void* token) noexcept
    {
        for (u32 entry_index = 0; entry_index < m_Entries.Size(); ++entry_index) {
            FFactoryEntry& entry = m_Entries[entry_index];
            if (entry.sources[0].factory.kind != factory.kind) continue;

            for (u32 source = 0; source < entry.source_count; ++source) {
                if (token != nullptr && entry.sources[source].token == token) return;
                if (token == nullptr && entry.sources[source].token == nullptr &&
                    SameImplementation(entry.sources[source].factory, factory))
                    return;
            }
            if (entry.source_count >= kMaxSourcesPerFactory) return;
            entry.sources[entry.source_count++] = FFactorySource{factory, token};
            return;
        }

        FFactoryEntry entry{};
        entry.sources[0] = FFactorySource{factory, token};
        entry.source_count = 1;
        m_Entries.PushBack(entry);
    }

    bool UnregisterSource(const FSubsystemFactory& factory, const void* token) noexcept
    {
        for (u32 entry_index = 0; entry_index < m_Entries.Size(); ++entry_index) {
            FFactoryEntry& entry = m_Entries[entry_index];
            if (entry.sources[0].factory.kind != factory.kind) continue;

            u32 source_index = entry.source_count;
            for (u32 source = 0; source < entry.source_count; ++source) {
                const bool token_matches = token != nullptr
                                               ? entry.sources[source].token == token
                                               : entry.sources[source].token == nullptr &&
                                                     SameImplementation(entry.sources[source].factory, factory);
                if (token_matches) {
                    source_index = source;
                    break;
                }
            }
            if (source_index == entry.source_count) return false;

            for (u32 source = source_index; source + 1u < entry.source_count; ++source) {
                entry.sources[source] = entry.sources[source + 1u];
            }
            entry.sources[entry.source_count - 1u] = FFactorySource{};
            --entry.source_count;
            if (entry.source_count != 0) return true;

            for (u32 remaining = entry_index; remaining + 1u < m_Entries.Size(); ++remaining) {
                m_Entries[remaining] = m_Entries[remaining + 1u];
            }
            m_Entries.PopBack();
            return true;
        }
        return false;
    }

    /** 登録簿より後に破棄される、登録簿専用の process-lifetime allocator。 */
    FSystemAllocator m_Allocator;

    /** kind ごとに登録元を束ねた factory 一覧。 */
    TArray<FFactoryEntry> m_Entries;
};

/** ACS_REGISTER_SUBSYSTEM が生成する登録元追跡付き RAII helper。 */
struct FSubsystemAutoRegister {
    explicit FSubsystemAutoRegister(const FSubsystemFactory& factory) noexcept : m_Factory(factory)
    {
        FSubsystemRegistry::Get().RegisterSource(m_Factory, this);
    }

    ~FSubsystemAutoRegister() noexcept
    {
        (void)FSubsystemRegistry::Get().UnregisterSource(m_Factory, this);
    }

    FSubsystemAutoRegister(const FSubsystemAutoRegister&) = delete;
    FSubsystemAutoRegister& operator=(const FSubsystemAutoRegister&) = delete;

private:
    FSubsystemFactory m_Factory{};
};

} // namespace acs::game

/**
 * サブシステム型 T を スコープ SCOPE で自動生成するよう登録する(静的初期化時)。
 * 翻訳単位の末尾(クラス定義後)に 1 回書く。namespace acs::game を開くので、T は
 * 外側ルックアップで acs::game 内型もグローバルなユーザー型も解決される。
 */
#define ACS_REGISTER_SUBSYSTEM(T, SCOPE)                                                                  \
    namespace acs::game {                                                                                 \
    namespace {                                                                                           \
    const ::acs::game::FSubsystemAutoRegister g_AcsSubsysReg_##T{::acs::game::FSubsystemFactory{          \
        ::acs::game::SubsystemKindOf<T>(), (SCOPE), #T,                                                   \
        []() noexcept -> ::acs::TUniquePtr<::acs::game::FSubsystem> { return ::acs::MakeUnique<T>(); }}}; \
    }                                                                                                     \
    }
