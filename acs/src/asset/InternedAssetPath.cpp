// SPDX-License-Identifier: Apache-2.0
#include "asset/InternedAssetPath.h"

#include "foundation/Limits.h"

namespace acs {

FInternedAssetPath::FInternedAssetPath(IAllocator& Allocator, FAssetId Id) noexcept
    : m_Units(Allocator), m_Id(Id)
{
}

bool FInternedAssetPath::TryInitialize(const wchar_t* Path, usize Length) noexcept
{
    /** 末尾 NUL 込み配列の byte 数を安全に表現できる最大 path 長。 */
    constexpr usize MaximumOwnedPathLength = TNumLimits<usize>::Max() / sizeof(wchar_t);
    if (Path == nullptr || !m_Units.IsEmpty() || Length >= MaximumOwnedPathLength) return false;
    /** 末尾 NUL を含む所有要素数。 */
    const usize RequiredCodeUnits = Length + 1u;
    if (!m_Units.TrySetNum(RequiredCodeUnits)) return false;
    /** 所有配列へ複写する path の添字。 */
    for (usize Index = 0u; Index < Length; ++Index) {
        m_Units[Index] = Path[Index];
    }
    m_Units[Length] = L'\0';
    return true;
}

const wchar_t* FInternedAssetPath::Path() const noexcept
{
    return m_Units.IsEmpty() ? L"" : m_Units.GetData();
}

usize FInternedAssetPath::Length() const noexcept
{
    return m_Units.IsEmpty() ? 0u : m_Units.Num() - 1u;
}

FAssetId FInternedAssetPath::Id() const noexcept
{
    return m_Id;
}

bool FInternedAssetPath::Equals(const wchar_t* Path, usize Length) const noexcept
{
    if (Path == nullptr || Length != this->Length()) return false;
    /** 完全一致を調べる path の添字。 */
    for (usize Index = 0u; Index < Length; ++Index) {
        if (m_Units[Index] != Path[Index]) return false;
    }
    return true;
}

} // namespace acs
