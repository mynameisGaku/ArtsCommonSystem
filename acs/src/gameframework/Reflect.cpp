// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FTypeRegistry 実装 (統一リフレクション / 型レジストリ)
// -----------------------------------------------------------------------------
// Reflect.h で宣言した FTypeRegistry の本体。固定長 (kMax) 配列に FTypeDesc*
// (静的寿命・非所有) を貯め、名前 / ID / カテゴリで横断検索し、ファクトリで生成する。
// 全 ACS_REFLECT* 型は静的初期化時に FTypeAutoRegister 経由でここへ登録される。
//
// 規約: no-STL container / no-exceptions / 全 noexcept。同 ID は列挙上 1 件のまま登録元を追跡する。
// =============================================================================
#include "gameframework/Reflect.h"

namespace acs::game {

FTypeRegistry& FTypeRegistry::Get() noexcept {
    // Meyers singleton: 最初の Get() で 1 度だけ構築。静的初期化順序問題を避けるため、
    // 自動登録 (FTypeAutoRegister) からの Register も必ずこの関数経由で実体を得る。
    static FTypeRegistry s_Instance;
    return s_Instance;
}

bool FTypeRegistry::Register(const FTypeDesc* d) noexcept {
    if (d == nullptr) return false;

    // 同じ ID の記述子は列挙上 1 型のまま、登録元だけを固定長で保持する。
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->id != d->id) continue;
        const u32 source_count = m_SourceCounts[i];
        for (u32 source = 0; source < source_count; ++source) {
            if (m_Sources[i][source] == d) return true;
        }
        if (source_count >= kMaxSourcesPerType) return false;
        m_Sources[i][source_count] = d;
        m_SourceCounts[i] = static_cast<u8>(source_count + 1u);
        return true;
    }

    if (m_Count >= kMax) return false;   // 上限到達。
    m_Types[m_Count] = d;
    m_Sources[m_Count][0] = d;
    m_SourceCounts[m_Count] = 1u;
    ++m_Count;
    return true;
}

bool FTypeRegistry::Unregister(const FTypeDesc* descriptor) noexcept
{
    if (descriptor == nullptr) return false;

    for (u32 type_index = 0; type_index < m_Count; ++type_index) {
        const u32 source_count = m_SourceCounts[type_index];
        u32 source_index = source_count;
        for (u32 source = 0; source < source_count; ++source) {
            if (m_Sources[type_index][source] == descriptor) {
                source_index = source;
                break;
            }
        }
        if (source_index == source_count) continue;

        for (u32 source = source_index; source + 1u < source_count; ++source) {
            m_Sources[type_index][source] = m_Sources[type_index][source + 1u];
        }
        m_Sources[type_index][source_count - 1u] = nullptr;
        m_SourceCounts[type_index] = static_cast<u8>(source_count - 1u);

        if (source_count > 1u) {
            m_Types[type_index] = m_Sources[type_index][0];
            return true;
        }

        // 最後の登録元を外した型は列挙からも除く。列挙順を保つため後続を前へ詰める。
        for (u32 type = type_index; type + 1u < m_Count; ++type) {
            m_Types[type] = m_Types[type + 1u];
            m_SourceCounts[type] = m_SourceCounts[type + 1u];
            for (u32 source = 0; source < kMaxSourcesPerType; ++source) {
                m_Sources[type][source] = m_Sources[type + 1u][source];
            }
        }
        --m_Count;
        m_Types[m_Count] = nullptr;
        m_SourceCounts[m_Count] = 0u;
        for (u32 source = 0; source < kMaxSourcesPerType; ++source) {
            m_Sources[m_Count][source] = nullptr;
        }
        return true;
    }
    return false;
}

const FTypeDesc* FTypeRegistry::FindByName(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->name != nullptr && std::strcmp(m_Types[i]->name, name) == 0)
            return m_Types[i];
    }
    return nullptr;
}

const FTypeDesc* FTypeRegistry::FindById(FTypeId id) const noexcept {
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->id == id) return m_Types[i];
    }
    return nullptr;
}

u32 FTypeRegistry::CountOfCategory(ETypeCategory cat) const noexcept {
    u32 n = 0;
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->category == cat) ++n;
    }
    return n;
}

const FTypeDesc* FTypeRegistry::AtOfCategory(ETypeCategory cat, u32 nth) const noexcept {
    u32 seen = 0;
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->category != cat) continue;
        if (seen == nth) return m_Types[i];
        ++seen;
    }
    return nullptr;
}

void* FTypeRegistry::Create(const char* name) const noexcept {
    const FTypeDesc* d = FindByName(name);
    if (d == nullptr || d->construct == nullptr) return nullptr;
    return d->construct();
}

void* FTypeRegistry::CreateById(FTypeId id) const noexcept {
    const FTypeDesc* d = FindById(id);
    if (d == nullptr || d->construct == nullptr) return nullptr;
    return d->construct();
}

void FTypeRegistry::Destroy(FTypeId id, void* obj) const noexcept {
    if (obj == nullptr) return;
    const FTypeDesc* d = FindById(id);
    if (d != nullptr && d->destruct != nullptr) d->destruct(obj);
}

} // namespace acs::game
