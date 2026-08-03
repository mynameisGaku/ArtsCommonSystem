// SPDX-License-Identifier: Apache-2.0
#include "subsystem/SubsystemRegistry.h"

#include "foundation/Move.h"

namespace acs {

CSubsystemRegistry& CSubsystemRegistry::Get() noexcept
{
    /** この link image が所有する登録簿。 */
    static CSubsystemRegistry Instance;
    return Instance;
}

CSubsystemRegistry::CSubsystemRegistry() noexcept : m_Entries(m_Allocator)
{
}

bool CSubsystemRegistry::TryRegister(const FSubsystemFactory& factory) noexcept
{
    return RegisterSource(factory, nullptr);
}

bool CSubsystemRegistry::TryRegisterActive(const FSubsystemFactory& factory) noexcept
{
    if (!TryRegister(factory)) return false;
    for (usize Index = 0u; Index < m_Entries.Num(); ++Index) {
        if (m_Entries[Index].sources[0].factory.kind == factory.kind) {
            return SameImplementation(m_Entries[Index].sources[0].factory, factory);
        }
    }
    return false;
}

void CSubsystemRegistry::Register(const FSubsystemFactory& factory) noexcept
{
    (void)TryRegister(factory);
}

bool CSubsystemRegistry::Unregister(const FSubsystemFactory& factory) noexcept
{
    return UnregisterSource(factory, nullptr);
}

u32 CSubsystemRegistry::Count() const noexcept
{
    return static_cast<u32>(m_Entries.Num());
}

const FSubsystemFactory& CSubsystemRegistry::At(u32 index) const noexcept
{
    return m_Entries[index].sources[0].factory;
}

bool CSubsystemRegistry::TrySnapshot(TArray<FSubsystemFactory>& output) const noexcept
{
    TArray<FSubsystemFactory> Staged(*output.GetAllocator());
    if (!Staged.TryReserve(m_Entries.Num())) return false;
    for (usize Index = 0u; Index < m_Entries.Num(); ++Index) {
        if (!Staged.TryAdd(m_Entries[Index].sources[0].factory)) return false;
    }
    output = Move(Staged);
    return true;
}

bool CSubsystemRegistry::StrEq(const char* left, const char* right) noexcept
{
    if (left == nullptr || right == nullptr) return left == right;
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

bool CSubsystemRegistry::SameImplementation(
    const FSubsystemFactory& left, const FSubsystemFactory& right) noexcept
{
    return left.kind == right.kind && left.scope == right.scope &&
           left.create == right.create && left.phase == right.phase &&
           left.order == right.order && StrEq(left.name, right.name);
}

bool CSubsystemRegistry::RegisterSource(
    const FSubsystemFactory& factory, const void* token) noexcept
{
    if (!IsValidSubsystemFactory(factory)) return false;
    for (usize EntryIndex = 0u; EntryIndex < m_Entries.Num(); ++EntryIndex) {
        FFactoryEntry& Entry = m_Entries[EntryIndex];
        if (Entry.sources[0].factory.kind != factory.kind) continue;

        for (u32 SourceIndex = 0u; SourceIndex < Entry.source_count; ++SourceIndex) {
            if (token != nullptr && Entry.sources[SourceIndex].token == token) return true;
            if (token == nullptr && Entry.sources[SourceIndex].token == nullptr &&
                SameImplementation(Entry.sources[SourceIndex].factory, factory)) {
                return true;
            }
        }
        if (Entry.source_count >= kMaxSourcesPerFactory) return false;
        Entry.sources[Entry.source_count] = FFactorySource{factory, token};
        ++Entry.source_count;
        return true;
    }

    FFactoryEntry Entry{};
    Entry.sources[0] = FFactorySource{factory, token};
    Entry.source_count = 1u;
    return m_Entries.TryAdd(Entry);
}

bool CSubsystemRegistry::UnregisterSource(
    const FSubsystemFactory& factory, const void* token) noexcept
{
    for (usize EntryIndex = 0u; EntryIndex < m_Entries.Num(); ++EntryIndex) {
        FFactoryEntry& Entry = m_Entries[EntryIndex];
        if (Entry.sources[0].factory.kind != factory.kind) continue;

        u32 SourceIndex = Entry.source_count;
        for (u32 Candidate = 0u; Candidate < Entry.source_count; ++Candidate) {
            const bool TokenMatches = token != nullptr
                                          ? Entry.sources[Candidate].token == token
                                          : Entry.sources[Candidate].token == nullptr &&
                                                SameImplementation(
                                                    Entry.sources[Candidate].factory, factory);
            if (TokenMatches) {
                SourceIndex = Candidate;
                break;
            }
        }
        if (SourceIndex == Entry.source_count) return false;

        for (u32 Remaining = SourceIndex; Remaining + 1u < Entry.source_count; ++Remaining) {
            Entry.sources[Remaining] = Entry.sources[Remaining + 1u];
        }
        Entry.sources[Entry.source_count - 1u] = FFactorySource{};
        --Entry.source_count;
        if (Entry.source_count != 0u) return true;

        for (usize Remaining = EntryIndex; Remaining + 1u < m_Entries.Num(); ++Remaining) {
            m_Entries[Remaining] = m_Entries[Remaining + 1u];
        }
        m_Entries.Pop();
        return true;
    }
    return false;
}

CSubsystemAutoRegister::CSubsystemAutoRegister(const FSubsystemFactory& factory) noexcept
    : m_Factory(factory), m_Registered(CSubsystemRegistry::Get().RegisterSource(m_Factory, this))
{
}

CSubsystemAutoRegister::~CSubsystemAutoRegister() noexcept
{
    if (m_Registered) {
        (void)CSubsystemRegistry::Get().UnregisterSource(m_Factory, this);
    }
}

} // namespace acs
