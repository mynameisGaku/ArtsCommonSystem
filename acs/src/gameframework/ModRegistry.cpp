// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — FModRegistry 実装
//
// 「メタデータの登録・列挙・並び替え」だけを担う。実際の `.acpak` mount /
// hook 適用は AssetPack 統合と Lua 5.4 統合で埋める予定で、本ファイルは未実装。
#include "gameframework/ModRegistry.h"

#include <cstring>  // strcmp

namespace acs::game {

/** 2 つの id 文字列が等しいかを返す (両者 nullptr / 同一ポインタも安全に判定)。 */
bool FModRegistry::IdEquals(const char* a, const char* b) noexcept {
    if (a == b)                 return true;       // 同一ポインタ (or 両 nullptr)
    if (a == nullptr || b == nullptr) return false;
    return std::strcmp(a, b) == 0;
}

/** ModInfo を登録する。id == nullptr / 同 id 重複は警告して無視する。 */
void FModRegistry::Register(const FModInfo& info) noexcept {
    if (info.id == nullptr) {
        // id 無しは管理不能 (Find/Enable で参照できない) ので拒否。
        ACS_LOG_WARN("FModRegistry::Register: skipped entry with null id (name=%s)",
                     info.name ? info.name : "(null)");
        return;
    }

    // 同 id 重複は警告 (既存エントリは残す = 先勝ち)。manifest loader 側で
    // 検出するのが本来だが、二重防御として警告ログだけ出す。
    for (u32 i = 0; i < m_Mods.Size(); ++i) {
        if (IdEquals(m_Mods[i].id, info.id)) {
            ACS_LOG_WARN("FModRegistry::Register: duplicate id '%s' ignored", info.id);
            return;
        }
    }

    m_Mods.PushBack(info);

    // AssetPack 統合後は、info.pack_path が非 nullptr なら VirtualFileSystem に
    // mount 予約する (enable=true のときだけ実 mount)。現状は path を持つだけ。
}

/** mod_id に一致する Mod の enabled を true にする。見つかれば true。 */
bool FModRegistry::Enable(const char* mod_id) noexcept {
    for (u32 i = 0; i < m_Mods.Size(); ++i) {
        if (IdEquals(m_Mods[i].id, mod_id)) {
            m_Mods[i].enabled = true;
            // 実 mount + hook 適用 (Lua 5.4 / C++ plugin) は未実装。flag だけ立てる。
            return true;
        }
    }
    return false;
}

/** mod_id に一致する Mod の enabled を false にする。見つかれば true。 */
bool FModRegistry::Disable(const char* mod_id) noexcept {
    for (u32 i = 0; i < m_Mods.Size(); ++i) {
        if (IdEquals(m_Mods[i].id, mod_id)) {
            m_Mods[i].enabled = false;
            // unmount + hook 解除は未実装。flag だけ下げる。
            return true;
        }
    }
    return false;
}

/** mod_id に一致する Mod の load_order を変更する (未登録 id は no-op)。 */
void FModRegistry::SetLoadOrder(const char* mod_id, i32 order) noexcept {
    for (u32 i = 0; i < m_Mods.Size(); ++i) {
        if (IdEquals(m_Mods[i].id, mod_id)) {
            m_Mods[i].load_order = order;
            return;
        }
    }
    // 未登録 id は noop (UI 同期ループの都合上、警告は出さない)。
}

/** 登録済み Mod の個数を返す。 */
u32 FModRegistry::Count() const noexcept {
    return static_cast<u32>(m_Mods.Size());
}

/** mod_id に一致する Mod を返す (見つからなければ nullptr)。 */
const FModInfo* FModRegistry::Find(const char* mod_id) const noexcept {
    for (u32 i = 0; i < m_Mods.Size(); ++i) {
        if (IdEquals(m_Mods[i].id, mod_id)) return &m_Mods[i];
    }
    return nullptr;
}

/** 登録済み Mod の生バッファ先頭を返す (長さは Count())。 */
const FModInfo* FModRegistry::All() const noexcept {
    return m_Mods.Data();
}

/** load_order 昇順に安定 insertion sort する (同値は登録順を保つ)。 */
void FModRegistry::SortByLoadOrder() noexcept {
    // Insertion sort: N (= mod 数) は実用上 < 64 と想定。安定 sort なので
    // 同 load_order は登録順を保ち、UI の見え方に予測可能性が出る。
    const u32 n = static_cast<u32>(m_Mods.Size());
    for (u32 i = 1; i < n; ++i) {
        FModInfo key = m_Mods[i];
        u32 j = i;
        while (j > 0 && m_Mods[j - 1].load_order > key.load_order) {
            m_Mods[j] = m_Mods[j - 1];
            --j;
        }
        m_Mods[j] = key;
    }
}

/** 全 Mod を削除する。 */
void FModRegistry::Clear() noexcept {
    // enabled な Mod に対する Disable 相当の hook 解除は未実装。現状は単純クリア。
    m_Mods.Clear();
}

} // namespace acs::game
