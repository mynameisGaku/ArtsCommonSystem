// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FAssetBundle 実装 (FAssetRegistry 実接続)
//
// BeginLoad(registry) で各 path を registry 経由で実ロードし、TSharedPtr<Asset> を Entry に
// 保持する (同期 Load)。bundle が TSharedPtr を持つことで Unload まで生存を保証し、Unload /
// bundle 破棄で TSharedPtr が drop → refcount 0 で実体解放 (GC 不要・決定的解放)。
//   ・進捗計算は entries が空の場合 1.0 を返す (=「読むものが無いので即完了」)。
//   ・同期 Load を採用 (シーン開始時の一括ロード用途)。ストリーミング/非同期分割は
//     FStreamingDirector + LoadAsync 側で扱う (本 bundle は確定的な集合ロード)。
#include "gameframework/AssetBundle.h"
#include "asset/AssetRegistry.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace acs::game {

namespace {
/**
 * UTF-8 の path を wide 文字列に変換する (FAssetRegistry が wide path を取るため)。
 *
 * @param utf8 変換元の UTF-8 path。
 * @param out 変換結果を書き込むバッファ。
 * @param cap out バッファの容量 (wchar_t 数)。
 * @return 変換成功なら true (失敗時は out[0] を 0 にして false)。
 */
bool WidenPath(const char* utf8, wchar_t* out, int cap) noexcept
{
    if (!utf8 || cap <= 0) return false;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    if (n <= 0) {
        out[0] = 0;
        return false;
    } // 変換失敗 / バッファ不足
    return true;
}
} // namespace

void FAssetBundle::Add(const char* asset_path) noexcept
{
    if (asset_path == nullptr) {
        ACS_LOG_WARN("FAssetBundle::Add: null path ignored");
        return;
    }
    if (m_bBegun) {
        // BeginLoad 済 bundle への追加はサポートしない (進捗計算の意味が破綻するため)。
        ACS_LOG_WARN("FAssetBundle::Add: bundle already began loading, ignored '%s'", asset_path);
        return;
    }
    FEntry e{};
    e.path = asset_path;
    e.status = ELoadStatus::Pending;
    m_Entries.PushBack(e);
}

void FAssetBundle::BeginLoad(FAssetRegistry& registry) noexcept
{
    if (m_bBegun) {
        ACS_LOG_WARN("FAssetBundle::BeginLoad: already begun, ignored");
        return;
    }
    m_bBegun = true;

    // 各 entry を registry 経由で同期ロードし、TSharedPtr を保持。失敗は Failed としてマーク
    // し他 entry のロードは続ける (bundle 全体は HasFailed() で判定できる)。
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        FEntry& e = m_Entries[i];
        wchar_t wpath[260];
        if (!WidenPath(e.path, wpath, 260)) {
            ACS_LOG_WARN("FAssetBundle::BeginLoad: path 変換失敗 '%s'", e.path ? e.path : "(null)");
            e.status = ELoadStatus::Failed;
            continue;
        }
        auto r = registry.Load(wpath);
        if (r.IsErr()) {
            ACS_LOG_WARN("FAssetBundle::BeginLoad: load 失敗 '%s'", e.path);
            e.status = ELoadStatus::Failed;
            continue;
        }
        e.asset = Move(r.Value());
        e.status = ELoadStatus::Loaded;
    }
}

TSharedPtr<FAsset> FAssetBundle::GetAsset(u32 index) const noexcept
{
    if (index >= m_Entries.Size()) return TSharedPtr<FAsset>();
    return m_Entries[index].asset;
}

TSharedPtr<FAsset> FAssetBundle::FindAsset(const char* asset_path) const noexcept
{
    if (!asset_path) return TSharedPtr<FAsset>();
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        const char* p = m_Entries[i].path;
        if (p) {
            const char* a = p;
            const char* b = asset_path;
            while (*a && *b && *a == *b) {
                ++a;
                ++b;
            }
            if (*a == 0 && *b == 0) return m_Entries[i].asset;
        }
    }
    return TSharedPtr<FAsset>();
}

f32 FAssetBundle::Progress() const noexcept
{
    const usize n = m_Entries.Size();
    if (n == 0) {
        // 空 bundle は「読むものが無い = 100%」とみなす (UI のプログレスバー実装を簡潔に)。
        return 1.0f;
    }
    if (!m_bBegun) {
        // 未開始: Add しただけの段階は 0%。
        return 0.0f;
    }
    u32 done = 0;
    for (usize i = 0; i < n; ++i) {
        const ELoadStatus s = m_Entries[i].status;
        if (s == ELoadStatus::Loaded || s == ELoadStatus::Failed) {
            ++done;
        }
    }
    return static_cast<f32>(done) / static_cast<f32>(n);
}

bool FAssetBundle::IsLoaded() const noexcept
{
    const usize n = m_Entries.Size();
    if (n == 0) return true;     // 空 bundle は即 loaded
    if (!m_bBegun) return false; // 未開始なら未完了
    for (usize i = 0; i < n; ++i) {
        const ELoadStatus s = m_Entries[i].status;
        if (s != ELoadStatus::Loaded && s != ELoadStatus::Failed) {
            return false;
        }
    }
    return true;
}

bool FAssetBundle::HasFailed() const noexcept
{
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Entries[i].status == ELoadStatus::Failed) {
            return true;
        }
    }
    return false;
}

u32 FAssetBundle::AssetCount() const noexcept
{
    return static_cast<u32>(m_Entries.Size());
}

u32 FAssetBundle::LoadedCount() const noexcept
{
    u32 done = 0;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Entries[i].status == ELoadStatus::Loaded) {
            ++done;
        }
    }
    return done;
}

void FAssetBundle::Unload() noexcept
{
    // m_Entries.Clear() で各 Entry が破棄され、保持していた TSharedPtr<Asset> が drop される。
    // 他 bundle / registry cache がまだ参照していれば refcount > 0 で実体は残り、最後の
    // 参照消失時に自動解放される (TSharedPtr の決定的破棄、GC 不要)。registry cache から完全に
    // 外したい場合は app 側で registry.Unload(id) / Clear() を呼ぶ (bundle は共有参照の
    // 解放のみ担当し、cache 寿命管理には踏み込まない)。
    m_Entries.Clear();
    m_bBegun = false;
}

} // namespace acs::game
