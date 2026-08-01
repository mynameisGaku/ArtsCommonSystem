// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — CInspectorSeam 実装
//
// 設計のポイント:
//   ・Provider レジストリは線形 `TArray<Provider*>`。想定の登録数は
//     数十オブジェクト程度なので、線形検索 (Register の重複判定 / Unregister の
//     探索) で十分。hash 化は必要になれば再考。
//   ・Provider の所有権は持たない。`ClearAll()` でも Provider 自体は破棄しない。
//   ・全 noexcept。エラーは現状なし (登録 / 取得 / 通知のみ)。

#include "gameframework/InspectorSeam.h"

namespace acs::game {

void CInspectorSeam::Init() noexcept {
    // 現状は何もしない (予約点)。
    // 多重呼び出し可: 何度呼んでも副作用なし。
}

void CInspectorSeam::RegisterProvider(IInspectableProvider* provider) noexcept {
    // node 紐付けなし = invalid FNodeId で登録 (GetProviderForNode の対象外)。
    RegisterProviderForNode(FNodeId{}, provider);
}

void CInspectorSeam::RegisterProviderForNode(FNodeId node_id,
                                             IInspectableProvider* provider) noexcept {
    if (provider == nullptr) {
        // nullptr は無視 (呼び出し側の境界条件を吸収する)。
        return;
    }
    // 重複登録は no-op で弾く。誤って同じ Provider を二重に Register しても
    // UI 側で同じオブジェクトが 2 回描画されるのを防ぐ。
    for (usize i = 0; i < m_Providers.Size(); ++i) {
        if (m_Providers[i] == provider) {
            return;
        }
    }
    // provider と node_id を同じ index で対応させる (parallel array)。
    m_Providers.PushBack(provider);
    m_NodeIds.PushBack(node_id);
}

void CInspectorSeam::UnregisterProvider(IInspectableProvider* provider) noexcept {
    if (provider == nullptr) {
        // nullptr は no-op (Register と対称)。
        return;
    }
    // 該当エントリを 1 件だけ削除する (Register が重複弾きしているので、
    // 同一ポインタは高々 1 件しか存在しない前提)。
    // 順序保存は不要なので、最後の要素を上書きして PopBack する swap-remove で
    // 削除コストを O(1) に。
    const usize n = m_Providers.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Providers[i] == provider) {
            const usize last = n - 1;
            if (i != last) {
                // parallel array なので provider / node_id を同じ index で swap。
                m_Providers[i] = m_Providers[last];
                m_NodeIds[i]   = m_NodeIds[last];
            }
            m_Providers.PopBack();
            m_NodeIds.PopBack();
            return;
        }
    }
    // 未登録の Provider を Unregister しても no-op (呼び出し側のライフサイクル
    // ミスを致命化しない)。
}

u32 CInspectorSeam::ProviderCount() const noexcept {
    return static_cast<u32>(m_Providers.Size());
}

IInspectableProvider* CInspectorSeam::GetProvider(u32 index) const noexcept {
    if (index >= m_Providers.Size()) {
        // 範囲外は nullptr を返す (TArray::operator[] の ASSERT を避ける防御)。
        return nullptr;
    }
    return m_Providers[index];
}

IInspectableProvider* CInspectorSeam::GetProviderForNode(FNodeId node_id) const noexcept {
    if (!node_id.IsValid()) {
        // invalid id (= 未紐付け sentinel) では引かない。RegisterProvider 由来の
        // 紐付けなし provider (m_NodeIds[i] == invalid) を誤ヒットさせないため。
        return nullptr;
    }
    // FNodeId 完全一致 (index + generation) で線形検索。登録数は数十想定なので
    // 線形で十分 (hash 化は必要になれば再考)。
    for (usize i = 0; i < m_Providers.Size(); ++i) {
        if (m_NodeIds[i] == node_id) {
            return m_Providers[i];
        }
    }
    return nullptr;
}

void CInspectorSeam::NotifyFieldChanged(u32 provider_index, u32 obj_index, u32 field_index) noexcept {
    if (provider_index >= m_Providers.Size()) {
        // 範囲外は no-op。UI 側のリストと本体側の登録が一瞬ずれることがあるため
        // 防御的にチェックする。
        return;
    }
    IInspectableProvider* prov = m_Providers[provider_index];
    if (prov == nullptr) {
        // 通常は到達しないが、念のための null チェック。
        return;
    }
    // obj_index / field_index の妥当性検証は Provider 側に委ねる
    // (Provider しか実際のオブジェクト / フィールド数を知らないため)。
    prov->OnFieldChanged(obj_index, field_index);
}

void CInspectorSeam::ClearAll() noexcept {
    // Provider は non-owning なので破棄せず、配列だけ空にする。
    // 容量は保持 (Reserve 状態を保つ ≒ 次回再登録時のアロケーション節約)。
    // parallel array なので node_id 側も必ず一緒に空にする。
    m_Providers.Clear();
    m_NodeIds.Clear();
}

} // namespace acs::game
