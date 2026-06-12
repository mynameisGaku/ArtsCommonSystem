// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode2D
//
// シーンの中身を表す唯一のノードクラス (2D 専用、抽象 Node 基底は作らない)。
// 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
// OnDespawn を override してロジック・描画を書く。
//
// 設計選択:
//   ・**non-copy / non-move**: `TUniquePtr<FNode2D>` で所有、`FNode2D*` で参照。
//     `AddChild(MakeUnique<MyNode>(args))` が標準パターン。
//   ・**lifecycle**: `AddChild` 即時 `OnSpawn`、`Destroy()` で
//     `m_PendingDestroy` をマーク、フレーム境界の `ResolveStructuralChanges()`
//     で OnDespawn を呼んで TArray から除去。子ツリーが先に reap される。
//   ・**transform**: `m_Local` を真値、`World()` は親をたどってオンザフライ計算。
//   ・**iteration safety**: UpdateTree/DrawTree は index ベースで走査。
//     AddChild が走査中に呼ばれた場合の新規子は同フレームで走らせる (Unity 互換)。
//     Destroy は遅延 reap なので走査中の即時除去はしない。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "gameframework/Transform2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class RenderContext;
class FSceneServices;   // 非所有ポインタで参照 (include しない = ノードヘッダを軽く保つ)
struct FMaterial2D;     // 使用マテリアル (効果プリセット)。実体は TUniquePtr で所有 (ヘッダを軽く保つ)

/**
 * シーンの中身を表す唯一のノード (2D 専用)。
 *
 * @details
 * 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
 * OnDespawn を override してロジック・描画を書く。`TUniquePtr<FNode2D>` で所有し
 * `FNode2D*` で参照する non-copy / non-move 型で、`AddChild(MakeUnique<MyNode>())`
 * が標準パターン。transform は m_Local を真値とし、World() は親をたどって合成する。
 */
class FNode2D {
public:
    /** 空のノードを構築する (transform は単位、親なし)。 */
    FNode2D() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FNode2D() noexcept = default;

    /** コピー禁止 (ノードは TUniquePtr で単独所有するため)。 */
    FNode2D(const FNode2D&)            = delete;

    /** コピー代入も禁止。 */
    FNode2D& operator=(const FNode2D&) = delete;

    /** ムーブ禁止 (親が保持するポインタの安定性を保つため)。 */
    FNode2D(FNode2D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNode2D& operator=(FNode2D&&)      = delete;

    /** AddChild でツリーに入った直後に 1 回呼ばれる初期化フック。 */
    virtual void OnSpawn()                noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/)     noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @details
     * 同フレームで 0..max_fixed_steps 回呼ばれる。FGame::SetFixedTimeStep が 0 のとき
     * (= 固定 update 無効) は呼ばれない。
     * @param fixed_dt 固定刻みの秒 (SetFixedTimeStep で指定した値)。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。スプライト等を積む。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}

    /** ツリーから除去される直前に 1 回呼ばれる後始末フック。 */
    virtual void OnDespawn()              noexcept {}

    /**
     * ローカル transform への可変参照を返す (位置・回転・スケールを直接書き換える)。
     *
     * @return ローカル transform への参照。
     */
    FTransform2D&       Local()       noexcept { return m_Local; }

    /**
     * ローカル transform への const 参照を返す。
     *
     * @return ローカル transform への const 参照。
     */
    const FTransform2D& Local() const noexcept { return m_Local; }

    /**
     * 親をたどって world transform を合成して返す (オンザフライ計算、キャッシュなし)。
     *
     * @return root からこのノードまで合成した world transform。
     */
    FTransform2D World() const noexcept;

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
     * @param b false なら subtree ごと描画をスキップする。
     */
    void SetVisible(bool b) noexcept { m_Visible = b; }

    /**
     * 可視フラグを返す。
     *
     * @return 可視なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * 子の描画順モード。
     *
     * @details
     * 既定 Tree = 配列追加順 (従来挙動・ゼロオーバーヘッド)。見下ろしゲームの Y 遮蔽や、
     * 背景/ワールド/前景/HUD のレイヤ分離に使う。
     */
    enum class EChildDrawOrder : u8 {
        /** 配列追加順 (従来)。ソートしない。 */
        Tree       = 0,

        /** SortLayer 昇順 (低い層が奥=先に描画)。同層は配列順で安定。 */
        Layer      = 1,

        /** SortLayer 昇順 → 同層内は (world.y + YSortBias) 昇順。+Y=画面下なので小さい y を先に描画 = 見下ろし遮蔽。 */
        LayerThenY = 2,
    };

    /**
     * この node が「自分の子」を描画する順序を設定する (兄弟間のみ。木の階層は維持)。
     *
     * @param o 適用する子描画順モード。
     */
    void            SetChildDrawOrder(EChildDrawOrder o) noexcept { m_ChildOrder = o; }

    /**
     * 現在の子描画順モードを返す。
     *
     * @return 設定済みの EChildDrawOrder。
     */
    EChildDrawOrder ChildDrawOrder() const noexcept { return m_ChildOrder; }

    /**
     * この node に焼き込んだマテリアル効果の状態 (SpriteBatch 型に依存しない軽量 POD)。
     *
     * @details Node2D.h を軽く保つため、効果プリセットの値だけをここに持つ
     *          (ESpriteEffect の整数値 + パラメータ)。DrawTree が rc.Sprites().SetEffect へ渡す。
     */
    struct FMaterialState {
        i32   kind     = 0;            ///< 0 = Lit/PBR, 1 = Effect。
        bool  active   = false;        ///< マテリアルが設定されているか。
        // --- Effect ---
        i32   effect   = 0;            ///< ESpriteEffect の整数値 (0 = None)。
        f32   strength = 1.0f;         ///< 主効果量。
        f32   p0 = 0.0f, p1 = 0.0f, p2 = 0.0f;  ///< 補助パラメータ。
        FVec4 color{ 1, 1, 1, 1 };     ///< 染め色 / 縁色。
        bool  animated = false;        ///< true で描画時に MaterialClock() を time に流す。
        // --- Lit (PBR) ---
        FVec4 baseColor{ 1, 1, 1, 1 }; ///< アルベド tint + 不透明度。
        f32   metallic = 0.0f, roughness = 0.5f, normalStrength = 1.0f, ao = 1.0f;
        FVec3 emissive{ 0, 0, 0 };     ///< 自己発光色。
        f32   emissiveStrength = 0.0f; ///< 発光強度。
        void* normalTex = nullptr;     ///< 法線マップ (非所有 IRhiTexture*、シーンが所有)。null=平面。
        // --- シェーディングモード + トゥーン ---
        i32   shadingMode = 0;         ///< 0=PBR, 1=Toon。
        FVec3 shadow1Color{ 0.55f, 0.52f, 0.62f }; f32 shadow1Threshold = 0.5f;
        FVec3 shadow2Color{ 0.32f, 0.30f, 0.40f }; f32 shadow2Threshold = 0.2f;
        FVec3 rimColor{ 1, 1, 1 };     f32 rimPower = 4.0f;
        FVec3 specColor{ 1, 1, 1 };    f32 specThreshold = 0.85f;
        f32   toonSoftness = 0.05f;
        // --- Substrate 風 拡張ロブ (PBR のみ) ---
        f32   clearcoat = 0.0f, clearcoatRoughness = 0.1f, anisotropy = 0.0f;
        f32   specularLevel = 0.5f, specularTint = 0.0f;
        f32   sheen = 0.0f, sheenRoughness = 0.3f;   FVec3 sheenColor{ 1, 1, 1 };
        f32   subsurface = 0.0f;                       FVec3 subsurfaceColor{ 1.0f, 0.3f, 0.2f };
    };

    /**
     * この node に使用マテリアル (効果プリセット) を設定する。
     *
     * @details
     * DrawTree がこの node 自身の描画 (OnDraw + 自分の component の OnDraw) を効果で包む
     * (子には及ばない = 各 node が自分のマテリアルを持つ)。アニメ付き効果は MaterialClock()
     * を参照する。マテリアルの値はコピーして焼き込む。
     * @param mat 設定するマテリアル。
     */
    void SetMaterial(const FMaterial2D& mat) noexcept;

    /** 使用マテリアルを外す (効果なしに戻す)。 */
    void ClearMaterial() noexcept { m_Mat.active = false; }

    /** PBR マテリアルの法線マップ (非所有 IRhiTexture*) を設定する。シーンが所有・遅延ロードする。 */
    void SetMaterialNormalTex(void* tex) noexcept { m_Mat.normalTex = tex; }

    /** この node の使用マテリアルの法線マップパス長が 0 でないか (シーンの遅延ロード判定用)。 */
    const FMaterialState& MaterialState() const noexcept { return m_Mat; }

    /** マテリアルが設定されているか。 */
    bool HasMaterial() const noexcept { return m_Mat.active; }

    /** PBR (Lit) マテリアルが設定されているか (シーンのライト収集要否の判定用)。 */
    bool IsLitMaterial() const noexcept { return m_Mat.active && m_Mat.kind == 0; }

    /** 自分自身のオクルーダー番号を設定する (シーンの影収集が毎フレーム設定。-1=自分は影源でない)。
     *  lit 描画時にこの番号のオクルーダーをスキップし、自スプライトの自己影 (リング/直線) を防ぐ。 */
    void SetSelfOccluder(i32 k) noexcept { m_SelfOccluder = k; }
    /** 自分自身のオクルーダー番号 (-1=無し)。 */
    i32  SelfOccluder() const noexcept { return m_SelfOccluder; }

    /**
     * この node の描画レイヤを設定する。
     *
     * @details 親が Layer/LayerThenY のとき兄弟ソートの第1キー。低いほど奥 (先に描画)。
     * @param layer 描画レイヤ (既定 0)。
     */
    void SetSortLayer(i32 layer) noexcept { m_SortLayer = layer; }

    /**
     * 描画レイヤを返す。
     *
     * @return 設定済みの描画レイヤ。
     */
    i32  SortLayer() const noexcept { return m_SortLayer; }

    /**
     * Y-sort の pivot バイアスを設定する。
     *
     * @details world.y に加算してソートキーにする。足元で遮蔽したいときは正値 (+Y=画面下=足元)。
     * @param bias world.y に加算するバイアス (既定 0)。
     */
    void SetYSortBias(f32 bias) noexcept { m_YSortBias = bias; }

    /**
     * Y-sort の pivot バイアスを返す。
     *
     * @return 設定済みのバイアス。
     */
    f32  YSortBias() const noexcept { return m_YSortBias; }

    /**
     * 親ノードを返す。
     *
     * @return 親ノード (root なら nullptr)。
     */
    FNode2D* Parent() const noexcept { return m_Parent; }

    /**
     * 直接の子の数を返す。
     *
     * @return 子の数。
     */
    u32     ChildCount() const noexcept { return static_cast<u32>(m_Children.Size()); }

    /**
     * i 番目の子を返す。
     *
     * @param i 子のインデックス。
     * @return i 番目の子 (範囲外なら nullptr)。
     */
    FNode2D* Child(u32 i) const noexcept {
        return i < m_Children.Size() ? m_Children[i].Get() : nullptr;
    }

    /**
     * 直接の子 `child` を、子配列内のインデックス `to` の位置へ即時に移動する (兄弟の並べ替え)。
     *
     * @details エディタのヒエラルキーで兄弟順を入れ替える / 隙間へ挿入するのに使う。他の子の相対
     * 順序は保たれる。`child` が直接の子でなければ false。`to` は配列サイズ-1 にクランプされる。
     * 構造変更の予約ではなく即時適用 (描画/更新ループ外から呼ぶこと)。
     * @param child 移動する直接の子。
     * @param to 移動先インデックス。
     * @return 移動したら true、`child` が子でなければ false。
     */
    bool MoveChild(FNode2D& child, u32 to) noexcept {
        const u32 n = static_cast<u32>(m_Children.Size());
        u32 from = n;
        for (u32 i = 0; i < n; ++i) { if (m_Children[i].Get() == &child) { from = i; break; } }
        if (from >= n) return false;
        if (to >= n) to = n - 1;
        if (from == to) return true;
        TUniquePtr<FNode2D> moved = static_cast<TUniquePtr<FNode2D>&&>(m_Children[from]);
        if (from < to) { for (u32 i = from; i < to; ++i) m_Children[i] = static_cast<TUniquePtr<FNode2D>&&>(m_Children[i + 1]); }
        else           { for (u32 i = from; i > to; --i) m_Children[i] = static_cast<TUniquePtr<FNode2D>&&>(m_Children[i - 1]); }
        m_Children[to] = static_cast<TUniquePtr<FNode2D>&&>(moved);
        return true;
    }

    /**
     * 子を追加して所有権を奪い、OnSpawn を即時に呼ぶ。
     *
     * @param child 追加する子 (所有権が移る)。
     * @return 追加した子への参照 (チェイン記述用)。
     */
    FNode2D& AddChild(TUniquePtr<FNode2D> child) noexcept;

    /**
     * 自身を「破棄予定」にマークする。
     *
     * @details
     * 実際の破棄は次の ResolveStructuralChanges で起こる
     * (OnDespawn 呼出 → TArray から除去 → デストラクタで memory release)。
     */
    void Destroy() noexcept { m_PendingDestroy = true; }

    /**
     * 破棄予定フラグが立っているかを返す。
     *
     * @return 破棄予定なら true。
     */
    bool IsPendingDestroy() const noexcept { return m_PendingDestroy; }

    /**
     * 自分を `new_parent` の子に移動するよう要求する (フレーム境界で適用)。
     *
     * @details
     * new_parent == nullptr は不正 (警告ログ + 無視)、自分自身 or 子孫を指定した場合も
     * 不正 (cycle 検出、警告 + 無視)。ResolveStructuralChanges 内で m_Children TArray 間を
     * Move し、parent ポインタを書き換える。OnSpawn/OnDespawn は呼ばれない
     * (= 既に生きているノードの移動)。
     * @param new_parent 移動先の親ノード。
     */
    void Reparent(FNode2D& new_parent) noexcept;

    /**
     * 親付け替え予定が立っているかを返す。
     *
     * @return 付け替え予定なら true。
     */
    bool IsPendingReparent() const noexcept { return m_PendingReparentTarget != nullptr; }

    /**
     * ノード単位に振られる generational handle を返す。
     *
     * @details
     * Scene 内で唯一であることは保証されない (生成側が一意性を管理)。
     * default は invalid (m_Packed == 0)。
     * @return ノードの FNodeId。
     */
    FNodeId Id() const noexcept { return m_Id; }

    /**
     * ノード ID を設定する (内部用。生成側が割り当てる)。
     *
     * @param id 割り当てる FNodeId。
     */
    void   _SetId(FNodeId id) noexcept { m_Id = id; }

    /**
     * T の FComponent2D を構築・attach し、参照を返す。
     *
     * @details OnAttach は即時呼出。依存コンポーネントは OnRequire で先に確保される。
     * @tparam T 追加する FComponent2D 派生型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return attach した T への参照。
     */
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) noexcept {
        TUniquePtr<T> comp = MakeUnique<T>(Forward<Args>(args)...);
        T* ref = comp.Get();
        ref->_SetOwner(this);
        // 依存コンポーネントを先に確保 (Unity の RequireComponent 相当)。
        // 依存が m_Components に先に積まれるので、この後の OnAttach から
        // GetComponent<Dep>() で確実に取得できる。
        ref->OnRequire(*this);
        m_Components.PushBack(TUniquePtr<FComponent2D>(comp.Release(), comp.GetAllocator()));
        ref->OnAttach(*this);
        ref->_MaybeAttachServices(SceneServices());   // ツリーが既に services 配線済なら即 fire
        return *ref;
    }

    /**
     * T があれば返し、無ければ追加して返す (RequireComponent の自動追加に使う)。
     *
     * @tparam T 取得または追加する FComponent2D 派生型。
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
     * @tparam T 探す FComponent2D 派生型。
     * @return 見つかった T へのポインタ (無ければ nullptr)。
     */
    template<typename T>
    T* GetComponent() noexcept {
        const void* k = ComponentKindOf<T>();
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
     * @tparam T 探す FComponent2D 派生型。
     * @return 持っていれば true。
     */
    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) return true;
        }
        return false;
    }

    /**
     * 最初に見つかった T 型コンポーネントを 1 つ除去する (OnDetach → 破棄)。
     *
     * @tparam T 除去する FComponent2D 派生型。
     * @return 除去したら true、見つからなければ false。
     */
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                m_Components[i]->OnDetach();
                m_Components[i].Reset();
                // compact: 末尾を i に詰める (順序は壊れる)
                if (i + 1 < m_Components.Size()) {
                    m_Components[i] = Move(m_Components[m_Components.Size() - 1]);
                }
                m_Components.PopBack();
                return true;
            }
        }
        return false;
    }

    /**
     * 全コンポーネントを除去する (各 OnDetach → 破棄)。Play 終了時のクリーンアップ等に使う。
     */
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
     * @details シリアライズ / Play モード等が `Kind()`/`ReflectName()` 越しに横断利用する。
     * @param i コンポーネントのインデックス。
     * @return i 番目のコンポーネント (範囲外なら nullptr)。
     */
    FComponent2D*       ComponentAt(u32 i)       noexcept {
        return i < m_Components.Size() ? m_Components[i].Get() : nullptr;
    }

    /** i 番目のコンポーネントを返す (const 版)。 */
    const FComponent2D* ComponentAt(u32 i) const noexcept {
        return i < m_Components.Size() ? m_Components[i].Get() : nullptr;
    }

    /**
     * 構築済みのコンポーネントを非テンプレートで attach する (factory 生成物の取り付けに使う)。
     *
     * @details
     * AddComponent<T> はコンパイル時に型が要るため、reflection factory で生成した
     * `TUniquePtr<FComponent2D>` を取り付けられない。これはその受け口。OnRequire→OnAttach の
     * lifecycle は AddComponent と同じ。`comp` はエンジンアロケータ所有であること
     * (CreateComponentByName が満たす)。
     * @param comp 取り付けるコンポーネント (所有権が移る、非 null 前提)。
     * @return attach したコンポーネントへの参照。
     */
    FComponent2D& AttachComponent(TUniquePtr<FComponent2D> comp) noexcept {
        FComponent2D* ref = comp.Get();
        ref->_SetOwner(this);
        ref->OnRequire(*this);
        m_Components.PushBack(Move(comp));
        ref->OnAttach(*this);
        ref->_MaybeAttachServices(SceneServices());   // ツリーが既に services 配線済なら即 fire
        return *ref;
    }

    /**
     * ツリーに配線された FSceneServices を返す (root まで遡る。未配線なら nullptr)。
     *
     * @details services は root ノードにのみ設定され、子は root まで歩いて解決する
     * (m_Owner/m_Parent と同じ「親が pointee を生かす」非所有ポインタ規約)。
     * @return 配線済み FSceneServices ポインタ (未配線は nullptr)。
     */
    FSceneServices* SceneServices() const noexcept;

    /**
     * services ポインタを設定する (内部用。root ノードでのみ意味を持つ)。
     *
     * @param svc 設定する FSceneServices (非所有、nullptr で解除)。
     */
    void _SetSceneServices(FSceneServices* svc) noexcept { m_Services = svc; }

    /**
     * root に services を設定し、subtree の全コンポーネントの OnAttachServices を一度発火する。
     *
     * @details ツリー構築完了後・services 生成後に 1 回呼ぶ (FScene2D / editor Play)。
     * 既に発火済のコンポーネントは二重発火しない (各コンポーネントの内部フラグでガード)。
     * @param svc 配線する FSceneServices。
     */
    void _ActivateServices(FSceneServices& svc) noexcept;

    /**
     * subtree 全体に可変刻み update を伝播する (root から呼ぶ)。
     *
     * @param dt 前フレームからの経過秒。
     */
    void UpdateTree(f32 dt) noexcept;

    /**
     * subtree 全体に固定刻み update を伝播する。
     *
     * @details Scene の OnFixedUpdate から root に対して 1 回呼ぶ。
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdateTree(f32 fixed_dt) noexcept;

    /**
     * subtree 全体を描画する (root から呼ぶ)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawTree(RenderContext& rc) noexcept;

    /**
     * フレーム境界で 1 回呼び、保留中の構造変更を適用する。
     *
     * @details
     * pending_destroy なノードを subtree ごと OnDespawn 呼んで子配列から除去する
     * (子から先に reap、その後に自分が抜ける)。また pending_reparent_target が
     * セットされた子があれば、その子を target の m_Children へ Move する (cycle 検出済)。
     */
    void ResolveStructuralChanges() noexcept;

private:
    /**
     * Reparent 操作で cycle が生じないかを確認する。
     *
     * @param candidate 移動先候補のノード。
     * @return candidate が自分の子孫 (= cycle になる) なら true。
     */
    bool IsAncestorOf(const FNode2D* candidate) const noexcept;

    /**
     * m_ChildOrder != Tree のとき、子を (SortLayer, world.y) で安定ソートして描画する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawChildrenSorted(RenderContext& rc) noexcept;

    /** ローカル transform (真値。world は親から合成)。 */
    FTransform2D m_Local{};

    /**
     * subtree に services を発火だけする (ポインタは設定しない、_ActivateServices の再帰部)。
     *
     * @param svc 発火に使う FSceneServices ポインタ。
     */
    void _ActivateSubtreeServices(FSceneServices* svc) noexcept;

    /** 親ノード (root なら nullptr)。 */
    FNode2D*     m_Parent          = nullptr;

    /** 配線された FSceneServices (root にのみ設定、非所有。子は walk-to-root で解決)。 */
    FSceneServices* m_Services     = nullptr;

    /** 直接の子 (所有権を持つ)。 */
    TArray<TUniquePtr<FNode2D>>      m_Children;

    /** attach されたコンポーネント (所有権を持つ)。 */
    TArray<TUniquePtr<FComponent2D>> m_Components;

    /** ノードの generational handle (default = invalid)。 */
    FNodeId      m_Id{};

    /** 非 null なら次の resolve でこの target 配下へ移動する。 */
    FNode2D*     m_PendingReparentTarget = nullptr;

    /** 描画レイヤ (親ソート時の第1キー)。 */
    i32          m_SortLayer       = 0;

    /** Y-sort の pivot バイアス。 */
    f32          m_YSortBias       = 0.0f;

    /** 子の描画順モード。 */
    EChildDrawOrder m_ChildOrder   = EChildDrawOrder::Tree;

    /** 使用マテリアル (効果プリセット) の焼き込み状態。active=false なら効果なし。 */
    FMaterialState m_Mat;

    /** 自分自身のオクルーダー番号 (シーンの影収集が毎フレーム設定。-1=影源でない)。自己影スキップ用。 */
    i32 m_SelfOccluder = -1;

    /** 有効フラグ (false で subtree の update をスキップ)。 */
    bool        m_Enabled         = true;

    /** 可視フラグ (false で subtree の描画をスキップ)。 */
    bool        m_Visible         = true;

    /** OnSpawn 済みフラグ (二重 spawn 防止)。 */
    bool        m_Spawned         = false;

    /** 破棄予定フラグ (次の resolve で reap)。 */
    bool        m_PendingDestroy = false;
};

} // namespace acs::game
