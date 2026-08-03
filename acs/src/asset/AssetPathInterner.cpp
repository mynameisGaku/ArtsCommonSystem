// SPDX-License-Identifier: Apache-2.0
#include "asset/AssetPathInterner.h"

#include "foundation/Move.h"
#include "threading/ScopedLock.h"

namespace acs {

namespace {

/** UTF-16 path から intern table の識別値を作る。 */
FAssetId MakeInternedAssetPathId(const wchar_t* Path, usize Length) noexcept
{
    return MakeAssetId(FStringView(reinterpret_cast<const char*>(Path), Length * sizeof(wchar_t)));
}

} // namespace

CAssetPathInterner::CAssetPathInterner() noexcept : CAssetPathInterner(DefaultAllocator())
{
}

CAssetPathInterner::CAssetPathInterner(IAllocator& Allocator) noexcept : m_Allocator(&Allocator), m_Paths(Allocator)
{
}

TResult<TSharedPtr<FInternedAssetPath>> CAssetPathInterner::Intern(const wchar_t* Path, usize Length) noexcept
{
    if (Path == nullptr || Length == 0u) {
        return ACS_ERR(Asset, kAssetPathInternerSubInvalidPath, "CAssetPathInterner::Intern: invalid path");
    }

    /** path の完全一致候補を探す識別値。 */
    const FAssetId Id = MakeInternedAssetPathId(Path, Length);
    /** table と診断値を同じ区間で保護する lock。 */
    FScopedLock Lock(m_Lock);
    ++m_Diagnostics.request_count;

    /** 同じ識別値で保持済みの path。 */
    const TSharedPtr<FInternedAssetPath>* Existing = m_Paths.Find(Id);
    if (Existing != nullptr && Existing->Get() != nullptr) {
        if (!(*Existing)->Equals(Path, Length)) {
            return ACS_ERR(Asset, kAssetPathInternerSubHashCollision, "CAssetPathInterner::Intern: path hash collision");
        }
        ++m_Diagnostics.hit_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, *Existing);
    }

    ++m_Diagnostics.miss_count;
    /** 呼び出し元へ返す新規 intern path。 */
    TSharedPtr<FInternedAssetPath> Created = MakeSharedIn<FInternedAssetPath>(*m_Allocator, *m_Allocator, Id);
    if (!Created.Get() || !Created->TryInitialize(Path, Length)) {
        return ACS_ERR(Memory, kAssetPathInternerSubOutOfMemory, "CAssetPathInterner::Intern: allocation failed");
    }

    /** 末尾 NUL を含む保持 code unit 数。 */
    const usize RequiredCodeUnits = Length + 1u;
    if (RequiredCodeUnits > kAssetPathInternerMaxCodeUnits) {
        ++m_Diagnostics.bypass_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
    }
    EvictUnusedUntilFit(RequiredCodeUnits);
    /** entry 数と code unit 予算の両方へ収まるか。 */
    const bool Fits = RequiredCodeUnits <= kAssetPathInternerMaxCodeUnits && m_Paths.Num() < kAssetPathInternerMaxEntries && m_Diagnostics.retained_code_units <= kAssetPathInternerMaxCodeUnits - RequiredCodeUnits;
    if (!Fits) {
        ++m_Diagnostics.bypass_count;
        return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
    }

    if (!m_Paths.TryAdd(Id, Created)) {
        return ACS_ERR(Memory, kAssetPathInternerSubOutOfMemory, "CAssetPathInterner::Intern: table allocation failed");
    }
    ++m_Diagnostics.retained_path_count;
    m_Diagnostics.retained_code_units += RequiredCodeUnits;
    return TResult<TSharedPtr<FInternedAssetPath>>(OkInit, Move(Created));
}

FAssetPathInternerDiagnostics CAssetPathInterner::Diagnostics() const noexcept
{
    /** 診断値の一貫した snapshot を守る lock。 */
    FScopedLock Lock(m_Lock);
    return m_Diagnostics;
}

void CAssetPathInterner::Reset() noexcept
{
    /** table と診断値の同時初期化を守る lock。 */
    FScopedLock Lock(m_Lock);
    m_Paths.Empty();
    m_Diagnostics = {};
}

void CAssetPathInterner::EvictUnusedUntilFit(usize RequiredCodeUnits) noexcept
{
    while (!m_Paths.IsEmpty() && (m_Paths.Num() >= kAssetPathInternerMaxEntries || RequiredCodeUnits > kAssetPathInternerMaxCodeUnits || m_Diagnostics.retained_code_units > kAssetPathInternerMaxCodeUnits - RequiredCodeUnits)) {
        /** 今回 table から外す未参照 path の識別値。 */
        FAssetId EvictionId{};
        /** eviction で解放する code unit 数。 */
        usize EvictionUnits = 0u;
        /** eviction 候補を発見したか。 */
        bool Found = false;
        /** table 内の保持 path を順に調べる pair。 */
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
