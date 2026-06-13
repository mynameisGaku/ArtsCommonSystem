// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode3D
//
// 3D シーングラフのノード (FNode2D の 3D 版)。親子ツリーで階層 transform を持ち、
// 各ノードが OnSpawn/OnUpdate/OnFixedUpdate/OnDespawn を override してロジックを書く。
// メッシュ描画などは FComponent3D を attach して合成する。
//
// 設計選択 (FNode2D と同一):
//   ・non-copy / non-move: `TUniquePtr<FNode3D>` で所有、`FNode3D*` で参照。
//   ・lifecycle: AddChild 即時 OnSpawn、Destroy() で pending マーク → フレーム境界の
//     ResolveStructuralChanges() で OnDespawn → 配列除去。子ツリーが先に reap。
//   ・transform: m_Local を真値、World() は親をたどってオンザフライ合成。
//   ・iteration safety: UpdateTree は index ベース走査 (走査中 AddChild は同フレーム実行)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/Transform3D.h"
#include "gameframework/Component3D.h"
#include "gameframework/NodeId.h"

namespace acs::game {

/**
 * 3D シーングラフのノード (FNode2D の 3D 版)。
 *
 * @details
 * 親子ツリーで階層 transform を持ち、各ノードが OnSpawn/OnUpdate/OnFixedUpdate/OnDespawn
 * を override してロジックを書く。`TUniquePtr<FNode3D>` 所有・`FNode3D*` 参照の
 * non-copy / non-move 型で、`AddChild(MakeUnique<FNode3D>())` が標準パターン。transform は
 * m_Local を真値とし、World() は親をたどって FTransform3D::Compose で合成する。
 */
class FNode3D {
public:
    /** 空のノードを構築する (transform は単位、親なし)。 */
    FNode3D() noexcept = default;

    /**
     * 名前を指定してノードを構築する。
     *
     * @param name ノード名 (ヒエラルキー表示やルックアップに使う)。
     */
    explicit FNode3D(FStringView name) noexcept : m_Name(name) {}

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FNode3D() noexcept = default;

    /** コピー禁止 (ノードは TUniquePtr で単独所有するため)。 */
    FNode3D(const FNode3D&)            = delete;

    /** コピー代入も禁止。 */
    FNode3D& operator=(const FNode3D&) = delete;

    /** ムーブ禁止 (親が保持するポインタの安定性を保つため)。 */
    FNode3D(FNode3D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNode3D& operator=(FNode3D&&)      = delete;

    /** AddChild でツリーに入った直後に 1 回呼ばれる初期化フック。 */
    virtual void OnSpawn() noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/) noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /** ツリーから除去される直前に 1 回呼ばれる後始末フック。 */
    virtual void OnDespawn() noexcept {}

    /**
     * ローカル transform への可変参照を返す (位置・回転・スケールを直接書き換える)。
     *
     * @return ローカル transform への参照。
     */
    FTransform3D&       Local()       noexcept { return m_Local; }

    /**
     * ローカル transform への const 参照を返す。
     *
     * @return ローカル transform への const 参照。
     */
    const FTransform3D& Local() const noexcept { return m_Local; }

    /**
     * 親をたどって world transform を合成して返す (オンザフライ計算、キャッシュなし)。
     *
     * @return root からこのノードまで合成した world transform。
     */
    FTransform3D World() const noexcept;

    /**
     * ノード名を返す。
     *
     * @return ノード名の文字列ビュー。
     */
    FStringView Name() const noexcept { return m_Name.View(); }

    /**
     * ノード名を設定する。
     *
     * @param name 新しいノード名。
     */
    void SetName(FStringView name) noexcept { m_Name = FString(name); }

    /**
     * 有効フラグを設定する。
     *
     * @param b false なら subtree ごと update をスキップする。
     */
    void SetEnabled(bool b) noexcept { m_Enabled = b; }

    /**
     * 有効フラグを返す。
     *
     * @return 有効なら true。
     */
    bool IsEnabled() const noexcept { return m_Enabled; }

    /**
     * 可視フラグを設定する。
     *
     * @param b false なら subtree ごと描画をスキップする (描画側が参照)。
     */
    void SetVisible(bool b) noexcept { m_Visible = b; }

    /**
     * 可視フラグを返す。
     *
     * @return 可視なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * 親ノードを返す。
     *
     * @return 親ノード (root なら nullptr)。
     */
    FNode3D* Parent() const noexcept { return m_Parent; }

    /**
     * 直接の子の数を返す。
     *
     * @return 子の数。
     */
    u32 ChildCount() const noexcept { return static_cast<u32>(m_Children.Size()); }

    /**
     * i 番目の子を返す。
     *
     * @param i 子のインデックス。
     * @return i 番目の子 (範囲外なら nullptr)。
     */
    FNode3D* Child(u32 i) const noexcept {
        return i < m_Children.Size() ? m_Children[i].Get() : nullptr;
    }

    /**
     * 直接の子 `child` を、子配列内のインデックス `to` の位置へ即時に移動する (兄弟並べ替え)。
     *
     * @param child 移動する直接の子。
     * @param to 移動先インデックス (配列サイズ-1 にクランプ)。
     * @return 移動したら true、`child` が子でなければ false。
     */
    bool MoveChild(FNode3D& child, u32 to) noexcept {
        const u32 n = static_cast<u32>(m_Children.Size());
        u32 from = n;
        for (u32 i = 0; i < n; ++i) { if (m_Children[i].Get() == &child) { from = i; break; } }
        if (from >= n) return false;
        if (to >= n) to = n - 1;
        if (from == to) return true;
        TUniquePtr<FNode3D> moved = static_cast<TUniquePtr<FNode3D>&&>(m_Children[from]);
        if (from < to) { for (u32 i = from; i < to; ++i) m_Children[i] = static_cast<TUniquePtr<FNode3D>&&>(m_Children[i + 1]); }
        else           { for (u32 i = from; i > to; --i) m_Children[i] = static_cast<TUniquePtr<FNode3D>&&>(m_Children[i - 1]); }
        m_Children[to] = static_cast<TUniquePtr<FNode3D>&&>(moved);
        return true;
    }

    /**
     * 子を追加して所有権を奪い、OnSpawn を即時に呼ぶ。
     *
     * @param child 追加する子 (所有権が移る)。
     * @return 追加した子への参照 (チェイン記述用)。
     */
    FNode3D& AddChild(TUniquePtr<FNode3D> child) noexcept;

    /**
     * 自身を「破棄予定」にマークする (実際の破棄は次の ResolveStructuralChanges)。
     */
    void Destroy() noexcept { m_PendingDestroy = true; }

    /**
     * 破棄予定フラグが立っているかを返す。
     *
     * @return 破棄予定なら true。
     */
    bool IsPendingDestroy() const noexcept { return m_PendingDestroy; }

    /**
     * 自分を `new_parent` の子に移動するよう要求する (フレーム境界で適用、ワールド位置保持なし)。
     *
     * @details
     * new_parent が自分自身 or 子孫 (cycle) / root の reparent は不正 (警告 + 無視)。
     * ResolveStructuralChanges 内で m_Children 間を Move し parent を書き換える。OnSpawn/
     * OnDespawn は呼ばれない。
     * @param new_parent 移動先の親ノード。
     */
    void Reparent(FNode3D& new_parent) noexcept;

    /**
     * 親付け替え予定が立っているかを返す。
     *
     * @return 付け替え予定なら true。
     */
    bool IsPendingReparent() const noexcept { return m_PendingReparentTarget != nullptr; }

    /**
     * ノード単位に振られる generational handle を返す。
     *
     * @return ノードの FNodeId (既定は invalid)。
     */
    FNodeId Id() const noexcept { return m_Id; }

    /**
     * ノード ID を設定する (内部用。生成側が割り当てる)。
     *
     * @param id 割り当てる FNodeId。
     */
    void _SetId(FNodeId id) noexcept { m_Id = id; }

    /**
     * T の FComponent3D を構築・attach し、参照を返す。
     *
     * @details OnAttach は即時呼出。依存コンポーネントは OnRequire で先に確保される。
     * @tparam T 追加する FComponent3D 派生型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return attach した T への参照。
     */
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) noexcept {
        TUniquePtr<T> comp = MakeUnique<T>(Forward<Args>(args)...);
        T* ref = comp.Get();
        ref->_SetOwner(this);
        ref->OnRequire(*this);   // 依存を先に確保 (Unity の RequireComponent 相当)
        m_Components.PushBack(TUniquePtr<FComponent3D>(comp.Release(), comp.GetAllocator()));
        ref->OnAttach(*this);
        return *ref;
    }

    /**
     * T があれば返し、無ければ追加して返す (RequireComponent の自動追加に使う)。
     *
     * @tparam T 取得または追加する FComponent3D 派生型。
     * @tparam Args 新規追加時に T のコンストラクタへ渡す引数型。
     * @param args 新規追加時に T のコンストラクタへ転送する引数。
     * @return 既存または新規に追加した T への参照。
     */
    template<typename T, typename... Args>
    T& GetOrAddComponent(Args&&... args) noexcept {
        if (T* existing = GetComponent<T>()) return *existing;
        return AddComponent<T>(Forward<Args>(args)...);
    }

    /**
     * 最初に見つかった T 型コンポーネントを返す。
     *
     * @tparam T 探す FComponent3D 派生型。
     * @return 見つかった T へのポインタ (無ければ nullptr)。
     */
    template<typename T>
    T* GetComponent() noexcept {
        const void* k = Component3DKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                return static_cast<T*>(m_Components[i].Get());
            }
        }
        return nullptr;
    }

    /**
     * T 型コンポーネントを持っているかを返す。
     *
     * @tparam T 探す FComponent3D 派生型。
     * @return 持っていれば true。
     */
    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = Component3DKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) return true;
        }
        return false;
    }

    /**
     * 最初に見つかった T 型コンポーネントを 1 つ除去する (OnDetach → 破棄)。
     *
     * @tparam T 除去する FComponent3D 派生型。
     * @return 除去したら true、見つからなければ false。
     */
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = Component3DKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                m_Components[i]->OnDetach();
                m_Components[i].Reset();
                if (i + 1 < m_Components.Size()) {
                    m_Components[i] = Move(m_Components[m_Components.Size() - 1]);
                }
                m_Components.PopBack();
                return true;
            }
        }
        return false;
    }

    /** 全コンポーネントを除去する (各 OnDetach → 破棄)。 */
    void RemoveAllComponents() noexcept {
        for (u32 i = 0; i < m_Components.Size(); ++i)
            if (m_Components[i]) { m_Components[i]->OnDetach(); m_Components[i].Reset(); }
        m_Components.Clear();
    }

    /**
     * attach 済みコンポーネントの数を返す。
     *
     * @return コンポーネント数。
     */
    u32 ComponentCount() const noexcept { return static_cast<u32>(m_Components.Size()); }

    /**
     * i 番目のコンポーネントを返す (型を知らない汎用列挙。範囲外は nullptr)。
     *
     * @param i コンポーネントのインデックス。
     * @return i 番目のコンポーネント (範囲外なら nullptr)。
     */
    FComponent3D*       ComponentAt(u32 i)       noexcept {
        return i < m_Components.Size() ? m_Components[i].Get() : nullptr;
    }

    /** i 番目のコンポーネントを返す (const 版)。 */
    const FComponent3D* ComponentAt(u32 i) const noexcept {
        return i < m_Components.Size() ? m_Components[i].Get() : nullptr;
    }

    /**
     * 構築済みのコンポーネントを非テンプレートで attach する (factory 生成物の取り付け)。
     *
     * @param comp 取り付けるコンポーネント (所有権が移る、非 null 前提)。
     * @return attach したコンポーネントへの参照。
     */
    FComponent3D& AttachComponent(TUniquePtr<FComponent3D> comp) noexcept {
        FComponent3D* ref = comp.Get();
        ref->_SetOwner(this);
        ref->OnRequire(*this);
        m_Components.PushBack(Move(comp));
        ref->OnAttach(*this);
        return *ref;
    }

    /**
     * subtree 全体に可変刻み update を伝播する (root から呼ぶ)。
     *
     * @param dt 前フレームからの経過秒。
     */
    void UpdateTree(f32 dt) noexcept;

    /**
     * subtree 全体に固定刻み update を伝播する。
     *
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdateTree(f32 fixed_dt) noexcept;

    /**
     * フレーム境界で 1 回呼び、保留中の構造変更 (destroy / reparent) を適用する。
     *
     * @details
     * pending_destroy なノードを subtree ごと OnDespawn 呼んで子配列から除去 (子から先に
     * reap)。pending_reparent_target がセットされた子は target の m_Children へ Move する。
     */
    void ResolveStructuralChanges() noexcept;

private:
    /**
     * Reparent 操作で cycle が生じないかを確認する。
     *
     * @param candidate 移動先候補のノード。
     * @return candidate が自分の子孫 (= cycle になる) なら true。
     */
    bool IsAncestorOf(const FNode3D* candidate) const noexcept;

    /** ローカル transform (真値。world は親から合成)。 */
    FTransform3D m_Local{};

    /** ノード名 (ヒエラルキー表示 / ルックアップ用)。 */
    FString      m_Name;

    /** 親ノード (root なら nullptr)。 */
    FNode3D*     m_Parent = nullptr;

    /** 直接の子 (所有権を持つ)。 */
    TArray<TUniquePtr<FNode3D>>      m_Children;

    /** attach されたコンポーネント (所有権を持つ)。 */
    TArray<TUniquePtr<FComponent3D>> m_Components;

    /** ノードの generational handle (default = invalid)。 */
    FNodeId      m_Id{};

    /** 非 null なら次の resolve でこの target 配下へ移動する。 */
    FNode3D*     m_PendingReparentTarget = nullptr;

    /** 有効フラグ (false で subtree の update をスキップ)。 */
    bool         m_Enabled = true;

    /** 可視フラグ (false で subtree の描画をスキップ)。 */
    bool         m_Visible = true;

    /** OnSpawn 済みフラグ (二重 spawn 防止)。 */
    bool         m_Spawned = false;

    /** 破棄予定フラグ (次の resolve で reap)。 */
    bool         m_PendingDestroy = false;
};

} // namespace acs::game
