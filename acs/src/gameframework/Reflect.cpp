// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FTypeRegistry 実装 (統一リフレクション / 型レジストリ)
// -----------------------------------------------------------------------------
// Reflect.h で宣言した FTypeRegistry の本体。固定長 (kMax) 配列に FTypeDesc*
// (静的寿命・非所有) を貯め、名前 / ID / カテゴリで横断検索し、ファクトリで生成する。
// 全 ACS_REFLECT* 型は静的初期化時に FTypeAutoRegister 経由でここへ登録される。
//
// 規約: no-STL / no-exceptions / 全 noexcept。重複登録 (同 ID) は最初の 1 件のみ。
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

    // 同 ID は既登録とみなして無視する (ヘッダ inline 変数が複数 TU から来ても 1 件)。
    for (u32 i = 0; i < m_Count; ++i) {
        if (m_Types[i]->id == d->id) return true;
    }

    if (m_Count >= kMax) return false;   // 上限到達。
    m_Types[m_Count++] = d;
    return true;
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
