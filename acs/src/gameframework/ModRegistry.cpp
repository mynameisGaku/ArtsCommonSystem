// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — FModRegistry 実装 (Phase 1 skeleton)
//
// 現フェーズは「メタデータの登録・列挙・並び替え」だけを担う。実際の
// `.acpak` mount / hook 適用は Pillar G (FAssetPack 統合 Phase 2) と
// Lua 5.4 統合フェーズで埋めるため TODO に留めている。
#include "gameframework/ModRegistry.h"

#include <cstring>  // strcmp

namespace acs::game {

bool FModRegistry::IdEquals(const char* a, const char* b) noexcept {
    if (a == b)                 return true;       // 同一ポインタ (or 両 nullptr)
    if (a == nullptr || b == nullptr) return false;
    return std::strcmp(a, b) == 0;
}

void FModRegistry::Register(const ModInfo& info) noexcept {
    if (info.id == nullptr) {
        // id 無しは管理不能 (Find/Enable で参照できない) ので拒否。
        ACS_LOG_WARN("FModRegistry::Register: skipped entry with null id (name=%s)",
                     info.name ? info.name : "(null)");
        return;
    }

    // 同 id 重複は警告 (既存エントリは残す = 先勝ち)。manifest loader 側で
    // 検出するのが本来だが、二重防御として警告ログだけ出す。
    for (u32 i = 0; i < _mods.Size(); ++i) {
        if (IdEquals(_mods[i].id, info.id)) {
            ACS_LOG_WARN("FModRegistry::Register: duplicate id '%s' ignored", info.id);
            return;
        }
    }

    _mods.PushBack(info);

    // TODO(Pillar G FAssetPack Phase 2): info.pack_path が非 nullptr なら
    // VirtualFileSystem に mount 予約 (まだ enable=true のときだけ実 mount する)。
    // 現状は path を持つだけ。
}

bool FModRegistry::Enable(const char* mod_id) noexcept {
    for (u32 i = 0; i < _mods.Size(); ++i) {
        if (IdEquals(_mods[i].id, mod_id)) {
            _mods[i].enabled = true;
            // TODO(Pillar N hook): 実 mount + hook 適用 (Lua 5.4 / C++ plugin)
            // をここでトリガーする。スケルトンでは flag だけ。
            return true;
        }
    }
    return false;
}

bool FModRegistry::Disable(const char* mod_id) noexcept {
    for (u32 i = 0; i < _mods.Size(); ++i) {
        if (IdEquals(_mods[i].id, mod_id)) {
            _mods[i].enabled = false;
            // TODO(Pillar N hook): unmount + hook 解除。
            return true;
        }
    }
    return false;
}

void FModRegistry::SetLoadOrder(const char* mod_id, i32 order) noexcept {
    for (u32 i = 0; i < _mods.Size(); ++i) {
        if (IdEquals(_mods[i].id, mod_id)) {
            _mods[i].load_order = order;
            return;
        }
    }
    // 未登録 id は noop (UI 同期ループの都合上、警告は出さない)。
}

u32 FModRegistry::Count() const noexcept {
    return static_cast<u32>(_mods.Size());
}

const ModInfo* FModRegistry::Find(const char* mod_id) const noexcept {
    for (u32 i = 0; i < _mods.Size(); ++i) {
        if (IdEquals(_mods[i].id, mod_id)) return &_mods[i];
    }
    return nullptr;
}

const ModInfo* FModRegistry::All() const noexcept {
    return _mods.Data();
}

void FModRegistry::SortByLoadOrder() noexcept {
    // Insertion sort: N (= mod 数) は実用上 < 64 と想定。安定 sort なので
    // 同 load_order は登録順を保ち、UI の見え方に予測可能性が出る。
    const u32 n = static_cast<u32>(_mods.Size());
    for (u32 i = 1; i < n; ++i) {
        ModInfo key = _mods[i];
        u32 j = i;
        while (j > 0 && _mods[j - 1].load_order > key.load_order) {
            _mods[j] = _mods[j - 1];
            --j;
        }
        _mods[j] = key;
    }
}

void FModRegistry::Clear() noexcept {
    // TODO(Pillar N): enabled な Mod に対しては Disable と同等の hook 解除を
    // 内部で走らせる必要がある。現状は単純クリア。
    _mods.Clear();
}

} // namespace acs::game
