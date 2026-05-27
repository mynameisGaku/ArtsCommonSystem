// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FAssetBundle 実装 (bridge スケルトン)
//
// 現フェーズ (G-1): bundle 表層 API + state machine だけを実装。
// FAssetRegistry::LoadAsync / FAssetFuture / Unload の実 API 接続は TODO に集約し、
// Phase G-2 でまとめて差し替える。スケルトンでも進捗 0→1 の遷移は確認可能 (Add 後に
// BeginLoad を呼ぶと全 entry が即座に Loaded 扱いになる仮想完了モードで動作)。
//
// 設計メモ:
//   ・「シーン破棄 → bundle 破棄 → TRc<Asset> drop → refcount 0 で実体解放」の流れは
//     Phase G-2 で FAssetFuture から取った TRc を Entry に保持して実現する。
//     現スケルトンでは TRc を持たないため、Unload は status リセットのみで十分。
//   ・進捗計算は entries が空の場合 1.0 を返す (=「読むものが無いので即完了」)。
//     これは BeginLoad 前後で挙動が変わらない (Add 0 件で BeginLoad しても 1.0)。
#include "gameframework/AssetBundle.h"

namespace acs::game {

void FAssetBundle::Add(const char* asset_path) noexcept {
    if (asset_path == nullptr) {
        ACS_LOG_WARN("FAssetBundle::Add: null path ignored");
        return;
    }
    if (m_Begun) {
        // BeginLoad 済 bundle への追加はサポートしない (進捗計算の意味が破綻するため)。
        ACS_LOG_WARN("FAssetBundle::Add: bundle already began loading, ignored '%s'",
                     asset_path);
        return;
    }
    Entry e{};
    e.path   = asset_path;
    e.status = LoadStatus::Pending;
    m_Entries.PushBack(e);
}

void FAssetBundle::BeginLoad() noexcept {
    if (m_Begun) {
        ACS_LOG_WARN("FAssetBundle::BeginLoad: already begun, ignored");
        return;
    }
    m_Begun = true;

    // TODO(Phase G-2): 各 entry について
    //   FAssetFuture fut = FAssetRegistry::Instance().LoadAsync(WidenPath(entry.path));
    //   entry.future = Move(fut);
    //   entry.status = LoadStatus::Loading;
    // を発行する。完了 polling は Tick() を別途追加するか、Progress() 呼び出し時に
    // future.IsReady() を見て status を Loaded/Failed に遷移させる。
    // wchar_t 化が必要なため、PathConv ユーティリティを foundation 側に追加予定。
    //
    // bridge スケルトン: 同期完了扱いで全 entry を Loaded に。これにより呼び出し側
    // (Scene) は「BeginLoad 直後に IsLoaded() == true」と見えて先に進める。
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        m_Entries[i].status = LoadStatus::Loaded;
    }
}

f32 FAssetBundle::Progress() const noexcept {
    const usize n = m_Entries.Size();
    if (n == 0) {
        // 空 bundle は「読むものが無い = 100%」とみなす (UI のプログレスバー実装を簡潔に)。
        return 1.0f;
    }
    if (!m_Begun) {
        // 未開始: Add しただけの段階は 0%。
        return 0.0f;
    }
    u32 done = 0;
    for (usize i = 0; i < n; ++i) {
        const LoadStatus s = m_Entries[i].status;
        if (s == LoadStatus::Loaded || s == LoadStatus::Failed) {
            ++done;
        }
    }
    return static_cast<f32>(done) / static_cast<f32>(n);
}

bool FAssetBundle::IsLoaded() const noexcept {
    const usize n = m_Entries.Size();
    if (n == 0) return true;       // 空 bundle は即 loaded
    if (!m_Begun) return false;     // 未開始なら未完了
    for (usize i = 0; i < n; ++i) {
        const LoadStatus s = m_Entries[i].status;
        if (s != LoadStatus::Loaded && s != LoadStatus::Failed) {
            return false;
        }
    }
    return true;
}

bool FAssetBundle::HasFailed() const noexcept {
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Entries[i].status == LoadStatus::Failed) {
            return true;
        }
    }
    return false;
}

u32 FAssetBundle::AssetCount() const noexcept {
    return static_cast<u32>(m_Entries.Size());
}

u32 FAssetBundle::LoadedCount() const noexcept {
    u32 done = 0;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Entries[i].status == LoadStatus::Loaded) {
            ++done;
        }
    }
    return done;
}

void FAssetBundle::Unload() noexcept {
    // TODO(Phase G-2): 各 entry の TRc<Asset> を drop し、必要なら
    //   FAssetRegistry::Instance().Unload(AssetIdFromPath(entry.path));
    // を呼んでキャッシュからも外す。他 Scene がまだ参照していれば refcount > 0 で
    // 実体は残り、最後の参照消失時に自動解放される (TRc の決定的破棄)。
    //
    // bridge スケルトン: 内部 state を Pending に戻し、m_Begun フラグもクリアして
    // 再利用可能な状態にする (同一 bundle インスタンスの再 Add + 再 BeginLoad を許す)。
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        m_Entries[i].status = LoadStatus::Pending;
    }
    m_Entries.Clear();
    m_Begun = false;
}

} // namespace acs::game
