// SPDX-License-Identifier: Apache-2.0
#include "asset/InternedAssetPath.h"

namespace acs {

FInternedAssetPath::FInternedAssetPath(FAllocator& Allocator, FAssetId Id) noexcept
    : m_Units(Allocator), m_Id(Id)
{
}

bool FInternedAssetPath::TryInitialize(const wchar_t* Path, usize Length) noexcept
{
    if (Path == nullptr || !m_Units.IsEmpty()) return false;
    if (!m_Units.TryResize(Length + 1u)) return false;
    for (usize Index = 0u; Index < Length; ++Index) {
        m_Units[Index] = Path[Index];
    }
    m_Units[Length] = L'\0';
    return true;
}

const wchar_t* FInternedAssetPath::Path() const noexcept
{
    return m_Units.IsEmpty() ? L"" : m_Units.Data();
}

usize FInternedAssetPath::Length() const noexcept
{
    return m_Units.IsEmpty() ? 0u : m_Units.Size() - 1u;
}

FAssetId FInternedAssetPath::Id() const noexcept
{
    return m_Id;
}

bool FInternedAssetPath::Equals(const wchar_t* Path, usize Length) const noexcept
{
    if (Path == nullptr || Length != this->Length()) return false;
    for (usize Index = 0u; Index < Length; ++Index) {
        if (m_Units[Index] != Path[Index]) return false;
    }
    return true;
}

} // namespace acs
