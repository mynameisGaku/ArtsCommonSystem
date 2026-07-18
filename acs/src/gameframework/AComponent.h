// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — AComponent
//
// ANode に attach する「振る舞いパーツ」の基底 (旧 FComponent2D / FComponent3D を統一)。
// sprite / メッシュ描画 / 当たり判定 / アニメーション / 音再生 / カスタムロジックを
// **継承ではなく合成** で組み上げる (composition over inheritance)。
//
// 命名規約 (docs/NodeUnification.md):
//   F = 構造体・素のクラス / A = ACS のオブジェクト基底 (FObject) を継承するオブジェクト。
//
// 使い方:
//   class RotateComponent : public AComponent {
//   public:
//       ACS_GAME_COMPONENT_KIND(RotateComponent)
//       explicit RotateComponent(f32 speed_rps) noexcept : m_Speed(speed_rps) {}
//       void OnUpdate(f32 dt) noexcept override {
//           Owner().SetRotation2D(Owner().Rotation2D() + m_Speed * dt);
//       }
//   private:
//       f32 m_Speed;
//   };
//
//   ANode& node = root.AddChild(NewObject<ANode>());
//   node.AddComponent<RotateComponent>(/*speed_rps=*/1.0f);
//
// 設計選択:
//   ・**RTTI 不使用の型 ID**: `template static const int` のアドレスを使う
//     (`ComponentKindOf<T>()`)。`virtual Kind()` で返して `ANode::GetComponent<T>()`
//     の static_cast に使う。
//   ・**Owner&** アクセス: `OnAttach(ANode&)` で owner ref を保存。以降 `Owner()` で
//     取り出す (raw pointer、stale はあり得ない = コンポーネントは owner の寿命より
//     長く生きない)。
//   ・**multiple components per kind**: 同じ型を 1 ノードに複数 attach 可能。
//     `GetComponent<T>()` は最初の一致を返す (線形探索)。
//   ・**lifecycle**: AddComponent 即時 `OnAttach`、Node 破棄時に `OnDetach`。
//     OnUpdate/OnDraw は Node の対応フックの後に呼ばれる。
//   ・**virtual は append-only**: 3D 向け追加フック等は必ず末尾に追加する
//     (途中挿入は game DLL との vtable ABI 不整合)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"   // FVec2 / FVec3 (QueryLight 等)
#include "memory/ObjectPtr.h"                    // FObject (ACS オブジェクト基底)
#include "gameframework/SubsystemCollection.h"   // GetSubsystem<T>() (軽量ヘッダ)

namespace acs::game {

class ANode;
class RenderContext;
class FSceneServices;

/**
 * 型 T ごとに一意なコンポーネント種別 ID を返す (RTTI 不使用)。
 *
 * @details
 * `template static int` のアドレスを ID に使う。T ごとに別 instantiation =
 * 別アドレスとなるため、static_cast の安全性チェックに使える。
 * @tparam T 種別 ID を取りたい AComponent 派生型。
 * @return T に固有の安定したポインタ ID。
 */
template<typename T>
const void* ComponentKindOf() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

/**
 * 2D ライトの記述子 (AComponent::QueryLight が埋める。位置は呼び出し側が owner world から)。
 */
struct FLightDesc2D {
    i32   type      = 0;            ///< 0=Point, 1=Spot, 2=Directional。
    f32   radius    = 256.0f;       ///< 届く半径 (px、Point/Spot)。
    FVec3 color{ 1, 1, 1 };         ///< 光の色。
    f32   intensity = 1.5f;         ///< 明るさ。
    FVec2 dir{ 0, 1 };              ///< スポット軸 / 平行光方向 (正規化)。
    f32   coneInner = 0.92f;        ///< スポット内円錐 (cos)。
    f32   coneOuter = 0.70f;        ///< スポット外円錐 (cos)。
};

/**
 * ANode に attach する「振る舞いパーツ」の基底 (旧 FComponent2D/FComponent3D を統一)。
 *
 * @details
 * 描画 / 当たり判定 / アニメーション / 音再生 / カスタムロジックを継承ではなく合成で
 * 組み上げる。owner ノードを `Owner()` で参照し、必要な lifecycle フックだけ override
 * する。同じ型を 1 ノードに複数 attach 可能で、種別 ID は ComponentKindOf<T>() による
 * RTTI 不使用の型タグで識別する。FObject 継承 (A プレフィックス規約)。
 */
class AComponent : public FObject {
public:
    /** 空のコンポーネントを構築する (owner は attach 時に設定)。 */
    AComponent() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~AComponent() noexcept = default;

    /** コピー禁止 (コンポーネントは owner が単独所有するため)。 */
    AComponent(const AComponent&)            = delete;

    /** コピー代入も禁止。 */
    AComponent& operator=(const AComponent&) = delete;

    /** ムーブ禁止 (owner が保持するポインタの安定性を保つため)。 */
    AComponent(AComponent&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AComponent& operator=(AComponent&&)      = delete;

    /**
     * このコンポーネントの種別 ID を返す (派生は ACS_GAME_COMPONENT_KIND で実装)。
     *
     * @return ComponentKindOf<派生型>() の安定ポインタ ID。
     */
    virtual const void* Kind() const noexcept = 0;

    /**
     * リフレクション登録名 (= クラス名) を返す。インスタンス→FTypeRegistry の橋渡し。
     *
     * @details
     * Kind() は per-process の void* タグで実行をまたいで安定せず、FTypeId (名前ハッシュ)
     * とも別物。シリアライズ / Play モード実体化は「生きたコンポーネント → 反射型」を
     * 名前で解決する必要があるため、ACS_GAME_COMPONENT_KIND が #T を返す ReflectName を
     * 自動実装する。未対応 (この基底のまま) は nullptr。
     * @return クラス名文字列 (反射カタログの登録名と一致)。未対応は nullptr。
     */
    virtual const char* ReflectName() const noexcept { return nullptr; }

    /**
     * 依存コンポーネント宣言フック (Unity の [RequireComponent] 相当)。
     *
     * @details
     * AddComponent が OnAttach の前に 1 度だけ呼ぶ。実装側で
     * `owner.GetOrAddComponent<Dep>()` を呼べば必要な兄弟コンポーネントを自動確保
     * できる。依存は本コンポーネントより先に attach されるので、OnAttach/OnUpdate から
     * GetComponent<Dep>() で必ず取れる。既定 no-op。
     * @param owner このコンポーネントが attach される先のノード。
     */
    virtual void OnRequire(ANode& /*owner*/) noexcept {}

    /**
     * attach 直後に 1 回呼ばれる初期化フック。
     *
     * @details 既定 no-op。必要な派生のみ override する。
     * @param owner このコンポーネントの owner ノード。
     */
    virtual void OnAttach(ANode& /*owner*/) noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @details Node の OnUpdate の後に呼ばれる。既定 no-op。
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/)        noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @details
     * FGame の fixed-step accumulator から Scene 経由で呼ばれ、同フレームで複数回
     * (catch-up) または 0 回 (slow-down clamp) 呼ばれ得る。既定 no-op。
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。スプライト等を積む。
     *
     * @details 既定 no-op。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}

    /**
     * OnDraw と子ツリー描画の「後」に呼ばれる後処理フック。
     *
     * @details
     * OnDraw で設定した描画状態 (ステンシルマスク等) を子ツリーの描画後に解除する
     * のに使う。これにより 1 コンポーネントで「子ツリーをマスクで囲う」が書ける。既定 no-op。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDrawPostChildren(RenderContext& /*rc*/) noexcept {}

    /**
     * シーンサービスが配線され Play 開始されたとき 1 回呼ばれる opt-in フック。
     *
     * @details
     * OnAttach の後・services が存在するときだけ発火する。ここで
     * `services.Input()/Physics()/Camera()` の参照をキャッシュしておけば、OnUpdate は
     * 分岐なしで使える。既定 no-op。1 コンポーネントにつき高々 1 回 (二重発火しない)。
     * @param services 配線済みの FSceneServices。
     */
    virtual void OnAttachServices(FSceneServices& /*services*/) noexcept {}

    /**
     * detach (owner 破棄) 直前に 1 回呼ばれる後始末フック。
     *
     * @details 既定 no-op。
     */
    virtual void OnDetach()                  noexcept {}

    // 注: 以下の Query 系は «末尾に追加» すること。途中に挿入すると後続 virtual の vtable
    // スロットがずれ、古いヘッダでビルドされた game DLL と ABI 不整合になる (append-only)。

    /**
     * 2D 点光源コンポーネントなら、その半径/色/強度を返す (FLight2DComponent が override)。
     *
     * @details シーンが lit スプライトのためにライトを収集するのに使う。位置は owner ノードの world。
     * @param out ライト記述子の出力 (type/radius/color/intensity/dir/cone)。
     * @return ライトコンポーネントなら true。
     */
    virtual bool QueryLight(FLightDesc2D& /*out*/) const noexcept { return false; }

    /**
     * 影を落とすコンポーネントなら占有半径スケールを返す (FShadowCaster2DComponent が override)。
     *
     * @param radius_scale ノード半径に対する占有半径スケールの出力。
     * @param shape 影の形状の出力 (0=円, 1=箱)。
     * @param self_shadow 自分自身にも影を落とすか (false=自己影スキップ、既定)。
     * @return 影キャスターなら true。
     */
    virtual bool QueryShadowCaster(f32& /*radius_scale*/, i32& /*shape*/,
                                   bool& /*self_shadow*/) const noexcept { return false; }

    /**
     * プリミティブ形状コンポーネントなら shape(0=Box,1=Circle,2=Triangle) とローカル半サイズを返す。
     *
     * @details 影オクルーダーを «見た目の形» に合わせるため、影収集が同じノードのこれを問い合わせる。
     *   末尾に追加 (append-only。途中挿入禁止)。
     * @return プリミティブレンダラなら true。
     */
    virtual bool QueryPrimitive(i32& /*shape*/, FVec2& /*half_size*/) const noexcept { return false; }

    /**
     * owner の subtree を «原子的な描画単位» として扱うべきかを返す。
     *
     * @details
     * OnDraw / OnDrawPostChildren で subtree 全体を包む描画状態 (ステンシルマスク等) を
     * 設定するコンポーネントが true を返す。true のノードはグローバル描画順ソート
     * (ANode::DrawTreeSorted) でフラット化されず、subtree ごと 1 個の描画単位として
     * ノード自身の DrawLayer/DrawPriority で整列し、内部は従来のツリー順で描かれる。
     * 既定 false。末尾に追加 (append-only)。
     * @return subtree を原子扱いするなら true。
     */
    virtual bool WantsAtomicSubtree() const noexcept { return false; }

    /**
     * owner ツリーに配線された FSceneServices を返す (未配線なら nullptr)。
     *
     * @return 配線済み services ポインタ (owner 未設定/未配線は nullptr)。
     */
    FSceneServices* SceneServices() const noexcept;

    /**
     * services が配線済みかを返す。
     *
     * @return SceneServices() が非 null なら true。
     */
    bool HasSceneServices() const noexcept;

    /**
     * owner ツリーに配線された World サブシステム束を返す (未配線なら nullptr)。
     *
     * @return World スコープのコレクション (GameInstance → Engine へフォールバック)。
     */
    FSubsystemCollection* Subsystems() const noexcept;

    /**
     * 型でサブシステムを取得する (World → GameInstance → Engine の順に検索)。
     *
     * @details オブジェクト同士のやり取り(スコア加算・イベント送出 等)はこれ経由で行う。
     * @tparam T FSubsystem 派生型。
     * @return T*(未配線/未登録なら nullptr)。
     */
    template<typename T>
    T* GetSubsystem() const noexcept {
        FSubsystemCollection* s = Subsystems();
        return (s != nullptr) ? s->Get<T>() : nullptr;
    }

    /**
     * services が利用可能なら OnAttachServices を一度だけ発火する (内部用、ガード付き)。
     *
     * @param svc 配線された services (nullptr なら何もしない)。
     */
    void _MaybeAttachServices(FSceneServices* svc) noexcept {
        if (svc != nullptr && !m_ServicesAttached) {
            m_ServicesAttached = true;
            OnAttachServices(*svc);
        }
    }

    /**
     * owner ノードへの可変参照を返す。
     *
     * @return owner ノードへの参照。
     */
    ANode& Owner() noexcept { return *m_Owner; }

    /**
     * owner ノードへの const 参照を返す。
     *
     * @return owner ノードへの const 参照。
     */
    const ANode& Owner() const noexcept { return *m_Owner; }

    /**
     * owner が設定済みかを返す。
     *
     * @return owner が非 null なら true。
     */
    bool HasOwner() const noexcept { return m_Owner != nullptr; }

    /**
     * owner ポインタを設定する (内部用。ANode::AddComponent が呼ぶ)。
     *
     * @param o 設定する owner ノード。
     */
    void _SetOwner(ANode* o) noexcept { m_Owner = o; }

private:
    /** owner ノード (attach 前は nullptr)。 */
    ANode* m_Owner = nullptr;

    /** OnAttachServices 発火済みフラグ (二重発火防止)。 */
    bool   m_ServicesAttached = false;
};

/** 派生クラスで Kind() + ReflectName() を 1 行で override するためのマクロ。 */
#define ACS_GAME_COMPONENT_KIND(T)                                                  \
    const void* Kind() const noexcept override                                       \
    { return ::acs::game::ComponentKindOf<T>(); }                                     \
    const char* ReflectName() const noexcept override { return #T; }

} // namespace acs::game
