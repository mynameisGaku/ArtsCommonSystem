// SPDX-License-Identifier: Apache-2.0
#include "asset/AssetPathInterner.h"

#include "foundation/Move.h"
#include "threading/ScopedLock.h"

namespace acs {

namespace {

FAssetId MakeInternedAssetPathId(const wchar_t* Path, usize Length) noexcept
{
    return MakeAssetId(FStringView(reinterpret_cast<const char*>(Path), Length * sizeof(wchar_t)));
}

} // namespace

FAssetPathInterner::FAssetPathInterner() noexcept : FAssetPathInterner(DefaultAllocator())
{
}

FAssetPathInterner::FAssetPathInterner(FAllocator& Allocator) noexcept : m_Allocator(&Allocator), m_Paths(Allocator)
{
}

TResult<TSharedPtr<FInternedAssetPath>> FAssetPathInterner::Intern(const wchar_t* Path, usize Length) noexcept
{
    if (Path == nullptr || Length == 0u) {
        return ACS_ERR(Asset, kAssetPathInternerSubInvalidPath, "FAssetPathInterner::Intern: invalid path");
    }

    const FAssetId Id = MakeInternedAssetPathId(Path, Length);
    FScopedLock Lock(m_Lock);
    ++m_Diagnostics.request_count;

    const TSharedPtr<FInternedAssetPath>* Existing = m_Paths.Find(Id);
    if (Existing != nullptr && Existing->Get() != nullptr) {
        if (!(*Existing)->Equals(Path, Length)) {
            return ACS_ERR(Asset, kAssetPathInternerSubHashCollision, "FAssetPathInterner::Intern: path hash collision");
        }
        ++m_Diagnostics.hit_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, *Existing);
    }

    ++m_Diagnostics.miss_count;
    TSharedPtr<FInternedAssetPath> Created = MakeSharedIn<FInternedAssetPath>(*m_Allocator, *m_Allocator, Id);
    if (!Created.Get() || !Created->TryInitialize(Path, Length)) {
        return ACS_ERR(Memory, kAssetPathInternerSubOutOfMemory, "FAssetPathInterner::Intern: allocation failed");
    }

    const usize RequiredCodeUnits = Length + 1u;
    if (RequiredCodeUnits > kAssetPathInternerMaxCodeUnits) {
        ++m_Diagnostics.bypass_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
    }
    EvictUnusedUntilFit(RequiredCodeUnits);
    const bool Fits = RequiredCodeUnits <= kAssetPathInternerMaxCodeUnits && m_Paths.Size() < kAssetPathInternerMaxEntries && m_Diagnostics.retained_code_units <= kAssetPathInternerMaxCodeUnits - RequiredCodeUnits;
    if (!Fits) {
        ++m_Diagnostics.bypass_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
    }

    if (!m_Paths.TryInsert(Id, Created)) {
        return ACS_ERR(Memory, kAssetPathInternerSubOutOfMemory, "FAssetPathInterner::Intern: table allocation failed");
    }
    ++m_Diagnostics.retained_path_count;
    m_Diagnostics.retained_code_units += RequiredCodeUnits;
    return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
}

FAssetPathInternerDiagnostics FAssetPathInterner::Diagnostics() const noexcept
{
    FScopedLock Lock(m_Lock);
    return m_Diagnostics;
}

void FAssetPathInterner::Reset() noexcept
{
    FScopedLock Lock(m_Lock);
    m_Paths.ReleaseStorage();
    m_Diagnostics = {};
}

void FAssetPathInterner::EvictUnusedUntilFit(usize RequiredCodeUnits) noexcept
{
    while (!m_Paths.IsEmpty() && (m_Paths.Size() >= kAssetPathInternerMaxEntries || RequiredCodeUnits > kAssetPathInternerMaxCodeUnits || m_Diagnostics.retained_code_units > kAssetPathInternerMaxCodeUnits - RequiredCodeUnits)) {
        FAssetId EvictionId{};
        usize EvictionUnits = 0u;
        bool Found = false;
        for (const auto& Pair : m_Paths) {
            if (Pair.second.UseCount() == 1u) {
                EvictionId = Pair.first;
                EvictionUnits = Pair.second->Length() + 1u;
                Found = true;
                break;
            }
        }
        if (!Found) break;
        m_Paths.Remove(EvictionId);
        ++m_Diagnostics.eviction_count;
        --m_Diagnostics.retained_path_count;
        m_Diagnostics.retained_code_units -= EvictionUnits;
    }
}

} // namespace acs
