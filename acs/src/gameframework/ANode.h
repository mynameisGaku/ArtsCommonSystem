// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "memory/ObjectPtr.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/Transform2D.h"
#include "gameframework/Transform3D.h"
#include "gameframework/AComponent.h"
#include "gameframework/NodeId.h"
// Rotation2Dの抽出でAtan2を使う。
#include "math/Math.h"

namespace acs::game {

class FRenderContext;
// 使用する効果プリセット。値は描画materialへ反映する。
struct FMaterial2D;

/**
 * ノードツリーで許容する最大深度 (root=0、親子 edge 数)。
 *
 * @details update / draw / 構造変更 / 破棄の一部は再帰的なため、敵対的または意図しない
 * 極端な深さでネイティブスタックを枯渇させない共通の不変条件として使う。
 */
inline constexpr u32 kNodeMaxTreeDepth = 512u;

/** TryAddChild の結果。失敗時は渡した強参照の所有権を変更しない。 */
enum class EAddChildResult : u8 {
    Added = 0,              ///< 追加成功。入力 TObjectPtr は空になる。
    NullChild,              ///< 空の TObjectPtr。
    SelfChild,              ///< 自分自身を追加しようとした。
    AlreadyParented,        ///< 既に別の親 (または同じ親) に所属している。
    WouldCreateCycle,       ///< 祖先を子にして循環を作ろうとした。
    ParentPendingDestroy,   ///< 追加先が破棄予定。
    ChildPendingDestroy,    ///< 子が破棄予定。
    TreeDepthLimitExceeded, ///< 追加後の subtree が kNodeMaxTreeDepth を超える。
};

/**
 * シーンの中身を表す唯一の統一ノード (旧 FNode2D / FNode3D を統合)。
 *
 * @details
 * 親子ツリーで階層的な transform (FTransform3D) を持ち、各ノードが
 * OnSpawn/OnUpdate/OnDraw/OnDespawn を override してロジック・描画を書く。
 * `NewObject<MyNode>()` で生成して `AddChild` で所有させ、参照は
 * `TWeakObjectPtr<ANode>` を使う。描画順は DrawLayer / DrawPriority / YSort を
 * ノードに設定し、シーンが自動で並べる。
 */
class ANode : public AObject {
public:
    /** 空のノードを構築する (transform は単位、親なし)。 */
    ANode() noexcept = default;

    /**
     * 名前を指定してノードを構築する。
     *
     * @param name ノード名 (ヒエラルキー表示やルックアップに使う)。
     */
    explicit ANode(FStringView name) noexcept : m_Name(name) {}

    /**
     * 派生クラスを正しく破棄し、自身を監視する遅延 Reparent 要求を無効化する。
     */
    virtual ~ANode() noexcept;

    /** コピー禁止 (ノードは親が単独所有するため)。 */
    ANode(const ANode&)            = delete;

    /** コピー代入も禁止。 */
    ANode& operator=(const ANode&) = delete;

    /** ムーブ禁止 (親が保持するポインタの安定性を保つため)。 */
    ANode(ANode&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ANode& operator=(ANode&&)      = delete;

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
     * 同フレームで 0..max_fixed_steps 回呼ばれる。CGame::SetFixedTimeStep が 0 のとき
     * (= 固定 update 無効) は呼ばれない。
     * @param fixed_dt 固定刻みの秒 (SetFixedTimeStep で指定した値)。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。スプライト等を積む。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDraw(FRenderContext& /*rc*/) noexcept {}

    /** ツリーから除去される直前に 1 回呼ばれる後始末フック。 */
    virtual void OnDespawn()              noexcept {}

    // ------------------------------------------------------------------ 名前

    /**
     * ノード名を返す。
     *
     * @return ノード名 (未設定なら空)。
     */
    FStringView Name() const noexcept { return m_Name.View(); }

    /**
     * ノード名を設定する。
     *
     * @param name 新しいノード名。
     */
    void SetName(FStringView name) noexcept { m_Name = FString{name}; }

    // ------------------------------------------------------------- transform

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
     * 親をたどって合成した world transform を返す (キャッシュなし、O(深さ))。
     *
     * @return world transform。
     */
    FTransform3D World() const noexcept;


    // ---- 3D の位置・向き・大きさ -------------------------------------------
    //
    // 2D にはヘルパが在るのに 3D は `Local().position = ...` を毎回書かせていた。
    // 3D が前提の作りなので、同じだけの手数で書けるようにしておく。
    //
    // **どれもローカル (親の座標系)。** 親を持たないノードでは world と同じ。

    /**
     * ローカル位置を返す。
     *
     * @return 親の座標系での位置。
     */
    FVec3 Position() const noexcept { return m_Local.position; }

    /**
     * ローカル位置を設定する。
     *
     * @param p 新しい位置。
     */
    void SetPosition(FVec3 p) noexcept { m_Local.position = p; }

    /**
     * ローカル位置をずらす。
     *
     * @details **向きは見ない。** 前へ進めたいなら `Position() + ForwardVector() * 距離` を渡す。
     * @param delta 加える量。
     */
    void Translate(FVec3 delta) noexcept { m_Local.position += delta; }

    /**
     * ローカル回転を度で返す。
     *
     * @details X→Y→Z の順に適用したときの角度。Y が ±90 度付近では Z が 0 に寄る
     * (ジンバルロック)。
     * @return X/Y/Z の角度 (度)。
     */
    FVec3 RotationDeg() const noexcept { return m_Local.EulerDeg(); }

    /**
     * ローカル回転を度で設定する。
     *
     * @details **ラジアンに直す必要は無い。** 度のまま渡す。
     * @param deg X/Y/Z の角度 (度)。
     */
    void SetRotationDeg(FVec3 deg) noexcept { m_Local.SetEulerDeg(deg); }

    /**
     * ローカル回転に度を加える。
     *
     * @details
     * 毎フレーム回し続ける用途向け。**角度で足すので、積み重ねるとジンバルロックの影響を
     * 受ける。** 1 軸だけ回すなら問題にならない。
     * @param deg 加える角度 (度)。
     */
    void RotateDeg(FVec3 deg) noexcept { SetRotationDeg(RotationDeg() + deg); }

    /**
     * ローカルスケールを返す。
     *
     * @return 各軸の倍率。
     */
    FVec3 Scale() const noexcept { return m_Local.scale; }

    /**
     * ローカルスケールを設定する。
     *
     * @param s 各軸の倍率。
     */
    void SetScale(FVec3 s) noexcept { m_Local.scale = s; }

    /**
     * ローカルスケールを一様に設定する。
     *
     * @param s 全軸に使う倍率。
     */
    void SetScale(f32 s) noexcept { m_Local.scale = FVec3{s, s, s}; }

    /**
     * このノードが向いている方向を返す。
     *
     * @details
     * 回転していないときは +Z。
     *
     * `Forward` ではなく `ForwardVector` なのは、**同じ名前の完全転送 `Forward<T>` が
     * この class の中で使われている**ため (`AddComponent` など)。member が勝って
     * template の呼び出しが壊れる。3 つとも揃えて `...Vector` にしてある。
     * @return 正規化された前方向 (親の座標系)。
     */
    FVec3 ForwardVector() const noexcept { return Rotate(m_Local.rotation, FVec3{0.0f, 0.0f, 1.0f}); }

    /**
     * このノードの右方向を返す。
     *
     * @return 正規化された右方向 (親の座標系)。
     */
    FVec3 RightVector() const noexcept { return Rotate(m_Local.rotation, FVec3{1.0f, 0.0f, 0.0f}); }

    /**
     * このノードの上方向を返す。
     *
     * @return 正規化された上方向 (親の座標系)。
     */
    FVec3 UpVector() const noexcept { return Rotate(m_Local.rotation, FVec3{0.0f, 1.0f, 0.0f}); }

    /**
     * ある点の方を向く。
     *
     * @details
     * `ForwardVector()` が `target` を指すように回転を置き換える。**`target` は親の座標系**で
     * 解釈する (親を持たないノードなら world と同じ)。
     *
     * 自分と同じ位置を指したときは何もしない (向きが決まらないため)。
     * @param target 向く先。
     * @param up 上として使う向き。既定は +Y。
     */
    void LookAt(FVec3 target, FVec3 up = FVec3{0.0f, 1.0f, 0.0f}) noexcept;

    /**
     * ある点へ、決めた距離だけ近づく。
     *
     * @details
     * **行き過ぎない。** 残りが `max_distance` より近ければ、そこで止まる。
     * 毎フレーム `速さ * dt` を渡す使い方を想定している。
     * @param target 目指す点 (親の座標系)。
     * @param max_distance この呼び出しで進む上限。
     * @return 着いたら true。
     */
    bool MoveToward(FVec3 target, f32 max_distance) noexcept;

    /**
     * ローカル位置の x,y を返す (2D ヘルパ。z は温存される)。
     *
     * @return ローカル位置の 2D 成分。
     */
    FVec2 Position2D() const noexcept { return FVec2{m_Local.position.x, m_Local.position.y}; }

    /**
     * ローカル位置の x,y を設定する (2D ヘルパ。z は温存される)。
     *
     * @param p 新しい 2D 位置 (Y-down / 左上原点の 2D 規約)。
     */
    void SetPosition2D(FVec2 p) noexcept { m_Local.position.x = p.x; m_Local.position.y = p.y; }

    /**
     * ローカル回転の Z 軸角を返す (2D ヘルパ)。
     *
     * @details クォータニオンから Z-twist (ZYX オイラーの yaw) を抽出する。X/Y 軸の
     * 回転が混ざった 3D 姿勢では「Z 軸まわり成分」の近似になる。
     * @return Z 軸回転角 (ラジアン)。
     */
    f32 Rotation2D() const noexcept { return RotationZOf(m_Local.rotation); }

    /**
     * ローカル回転を Z 軸回転で置き換える (2D ヘルパ)。
     *
     * @param rad Z 軸回転角 (ラジアン)。
     */
    void SetRotation2D(f32 rad) noexcept { m_Local.rotation = FQuat::AxisAngle(FVec3{0, 0, 1}, rad); }

    /**
     * ローカルスケールの x,y を返す (2D ヘルパ)。
     *
     * @return スケールの 2D 成分。
     */
    FVec2 Scale2D() const noexcept { return FVec2{m_Local.scale.x, m_Local.scale.y}; }

    /**
     * ローカルスケールの x,y を設定する (2D ヘルパ。z は温存される)。
     *
     * @param s 新しい 2D スケール。
     */
    void SetScale2D(FVec2 s) noexcept { m_Local.scale.x = s.x; m_Local.scale.y = s.y; }

    /**
     * ローカル transform の 2D 射影を返す。
     *
     * @details x,y 位置 / Z 軸回転角 / x,y スケールを FTransform2D に詰める。
     * @return local transform の 2D 射影。
     */
    FTransform2D Local2D() const noexcept {
        FTransform2D out;
        out.position = Position2D();
        out.rotation = Rotation2D();
        out.scale    = Scale2D();
        return out;
    }

    /**
     * ローカル transform の 2D 成分をまとめて設定する。
     *
     * @details position.z / scale.z は温存し、rotation は Z 軸回転で置き換える。
     * @param t 新しい 2D local transform。
     */
    void SetLocal2D(const FTransform2D& t) noexcept {
        SetPosition2D(t.position);
        SetRotation2D(t.rotation);
        SetScale2D(t.scale);
    }

    /**
     * world transform の 2D 射影を返す (2D 描画パス用)。
     *
     * @details x,y 位置 / Z 軸回転角 / x,y スケールを FTransform2D に詰める。
     * @return world の 2D 射影。
     */
    FTransform2D World2D() const noexcept {
        const FTransform3D w = World();
        FTransform2D out;
        out.position = FVec2{w.position.x, w.position.y};
        out.rotation = RotationZOf(w.rotation);
        out.scale    = FVec2{w.scale.x, w.scale.y};
        return out;
    }

    // ------------------------------------------------------------ 有効/可視

    /**
     * update の有効フラグを設定する。
     *
     * @param b false なら subtree ごと update をスキップする。
     */
    void SetEnabled(bool b) noexcept { m_Enabled = b; }

    /**
     * update の有効フラグを返す。
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

    // ------------------------------------------------------------------ 描画順

    /**
     * 描画レイヤーを設定する (第 1 ソートキー。小さい層 = 奥 = 先に描画)。
     *
     * @param layer 描画レイヤー (負値可)。
     */
    void SetDrawLayer(i32 layer) noexcept { m_DrawLayer = layer; }

    /**
     * 描画レイヤーを返す。
     *
     * @return 描画レイヤー。
     */
    i32  DrawLayer() const noexcept { return m_DrawLayer; }

    /**
     * 層内の描画プライオリティを設定する (第 2 ソートキー。小さい値 = 奥 = 先に描画)。
     *
     * @param priority 層内順序 (負値可)。
     */
    void SetDrawPriority(i32 priority) noexcept { m_DrawPriority = priority; }

    /**
     * 層内の描画プライオリティを返す。
     *
     * @return 描画プライオリティ。
     */
    i32  DrawPriority() const noexcept { return m_DrawPriority; }

    /**
     * Y ソート参加フラグを設定する (見下ろし遮蔽)。
     *
     * @details 有効なノードは同レイヤー内で (world.y + YSortBias) 昇順が
     * DrawPriority より優先される (+Y=画面下なので小さい y が奥)。
     * @param b Y ソートに参加するなら true。
     */
    void SetYSortEnabled(bool b) noexcept { m_YSortEnabled = b; }

    /**
     * Y ソート参加フラグを返す。
     *
     * @return 参加するなら true。
     */
    bool IsYSortEnabled() const noexcept { return m_YSortEnabled; }

    /**
     * Y-sort の pivot バイアスを設定する。
     *
     * @details 足元基準にしたい場合などに world.y へ加算するオフセット。
     * @param bias 加算するバイアス (px)。
     */
    void SetYSortBias(f32 bias) noexcept { m_YSortBias = bias; }

    /**
     * Y-sort の pivot バイアスを返す。
     *
     * @return 設定済みバイアス。
     */
    f32  YSortBias() const noexcept { return m_YSortBias; }

    // ---------------------------------------------------------- マテリアル

    /**
     * この node に焼き込んだマテリアル効果の状態 (CSpriteBatch 型に依存しない軽量 POD)。
     *
     * @details ANode.h を軽く保つため、効果プリセットの値だけをここに持つ
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

    /** この node に使用マテリアル (効果 or PBR) の値を焼き込む。 */
    void SetMaterial(const FMaterial2D& mat) noexcept;

    /** 使用マテリアルを解除する。 */
    void ClearMaterial() noexcept { m_Mat.active = false; }

    /** 法線マップテクスチャを差し込む (PBR 用、非所有)。 */
    void SetMaterialNormalTex(void* tex) noexcept { m_Mat.normalTex = tex; }

    /** 焼き込み済みマテリアル状態を返す。 */
    const FMaterialState& MaterialState() const noexcept { return m_Mat; }

    /** マテリアルが設定されているかを返す。 */
    bool HasMaterial() const noexcept { return m_Mat.active; }

    /** Lit (PBR) マテリアルかを返す。 */
    bool IsLitMaterial() const noexcept { return m_Mat.active && m_Mat.kind == 0; }

    /** 自己影スキップ用のオクルーダー番号を設定する (シーンの影収集が毎フレーム設定)。 */
    void SetSelfOccluder(i32 k) noexcept { m_SelfOccluder = k; }

    /** 自己オクルーダー番号を返す (-1 = 影源でない)。 */
    i32  SelfOccluder() const noexcept { return m_SelfOccluder; }

    // ------------------------------------------------------------------ 階層

    /**
     * 親ノードを返す (root は nullptr)。
     *
     * @return 親ノード。
     */
    ANode* Parent() const noexcept { return m_Parent; }

    /**
     * 直接の子の数を返す。
     *
     * @return 子の数。
     */
    u32     ChildCount() const noexcept { return static_cast<u32>(m_Children.Num()); }

    /** root から自身までの深度 (root=0) を返す。 */
    u32 TreeDepth() const noexcept;

    /**
     * i 番目の子を返す。
     *
     * @param i 子のインデックス。
     * @return i 番目の子 (範囲外なら nullptr)。
     */
    ANode* Child(u32 i) const noexcept {
        return i < m_Children.Num() ? m_Children[i].Get() : nullptr;
    }

    /**
     * 直接の子 `child` を、子配列内のインデックス `to` の位置へ即時に移動する (兄弟の並べ替え)。
     *
     * @details エディタのヒエラルキーで兄弟順を入れ替えるのに使う。他の子の相対順序は
     * 保たれる。`child` が直接の子でなければ false。`to` は配列サイズ-1 にクランプ。
     * 構造変更の予約ではなく即時適用 (描画/更新ループ外から呼ぶこと)。
     * @param child 移動する直接の子。
     * @param to 移動先インデックス。
     * @return 移動したら true、`child` が子でなければ false。
     */
    bool MoveChild(ANode& child, u32 to) noexcept {
        const u32 n = static_cast<u32>(m_Children.Num());
        u32 from = n;
        for (u32 i = 0; i < n; ++i) { if (m_Children[i].Get() == &child) { from = i; break; } }
        if (from >= n) return false;
        if (to >= n) to = n - 1;
        if (from == to) return true;
        TObjectPtr<ANode> moved = Move(m_Children[from]);
        if (from < to) { for (u32 i = from; i < to; ++i) m_Children[i] = Move(m_Children[i + 1]); }
        else           { for (u32 i = from; i > to; --i) m_Children[i] = Move(m_Children[i - 1]); }
        m_Children[to] = Move(moved);
        return true;
    }

    /**
     * 子を追加して強参照を保持し、未 spawn なら OnSpawn を即時に呼ぶ。
     *
     * @details `AddChild(NewObject<MyNode>(args))` が標準パターン。
     * @param child 追加する子 (強参照が移る)。
     * @return 追加した子への参照 (チェイン記述用。null 入力時は自身)。
     */
    ANode& AddChild(TObjectPtr<ANode> child) noexcept;

    /**
     * 検証に成功した場合だけ子の所有権を受け取り、未 spawn なら OnSpawn を呼ぶ。
     *
     * @details
     * 自己追加、祖先追加による循環、既所属ノードの多重親化、破棄予定ノード、
     * `kNodeMaxTreeDepth` 超過を拒否する。失敗時は `child` を変更しないため、
     * 呼び出し側は結果を検査して別の処理へ安全に回せる。
     * @param child 追加候補。成功時のみ Move されて空になる。
     * @return 追加結果。
     */
    EAddChildResult TryAddChild(TObjectPtr<ANode>& child) noexcept;

    /**
     * 自身を「破棄予定」にマークする。
     *
     * @details
     * 実際の破棄は次の ResolveStructuralChanges で起こる (OnDespawn 呼出 → 配列から
     * 除去 → 最後の強参照が切れた時点で破棄)。
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
     * 自分自身 / 子孫 / root の指定は不正 (警告ログ + 無視)。OnSpawn/OnDespawn は
     * 呼ばれない (= 既に生きているノードの移動)。
     * @param new_parent 移動先の親ノード。
     */
    void Reparent(ANode& new_parent) noexcept;

    /**
     * 親付け替え予定が立っているかを返す。
     *
     * @return 付け替え予定なら true。
     */
    bool IsPendingReparent() const noexcept {
        return m_PendingReparentTarget.Get() != nullptr;
    }

    /**
     * ノード単位に振られる generational handle を返す。
     *
     * @return ノードの FNodeId (default は invalid)。
     */
    FNodeId Id() const noexcept { return m_Id; }

    // ---- ここから下は «組み立て» のためのもの。ゲーム側から呼ぶものではない ----
    //
    // AScene と CNodePool はクラスなので friend で本当に閉じられる。
    // ローダとエディタ ABI の呼び元は**自由関数**なので friend にできず、
    // `FNodeInternals` (このファイルの下) を通す形にした。**止められてはいないが、
    // ノードの補完候補には出てこないし、呼ぶには内部用の型を名指しする必要がある。**
private:
    friend class CNodePool;
    friend class AScene;
    friend class FNodeInternals;

    /** ノード ID を設定する (CNodePool が割り当てる)。 */
    void   SetId_Internal(FNodeId id) noexcept { m_Id = id; }

    /** シーン直列化 ID を設定する (ローダ/エディタが割り当てる)。 */
    void SetSerialId_Internal(i32 id) noexcept { m_SerialId = id; }

    /** services ポインタを設定する (root ノードでのみ意味を持つ)。 */
    void SetSceneServices_Internal(CSceneServices* svc) noexcept { m_Services = svc; }

    /** サブシステム束を設定する (root ノードでのみ意味を持つ)。 */
    void SetSubsystems_Internal(CSubsystemCollection* subs) noexcept { m_Subsystems = subs; }

    /** root に services を設定し、subtree 全コンポーネントの OnAttachServices を一度発火する。 */
    void ActivateServices_Internal(CSceneServices& svc) noexcept;

public:

    /**
     * シーン直列化 ID (エディタ id) を返す (-1 = 未設定)。
     *
     * @return 直列化 ID。
     */
    i32 SerialId() const noexcept { return m_SerialId; }

    /**
     * subtree (this + 子孫) から直列化 ID 一致のノードを探す (DFS、無ければ nullptr)。
     *
     * @param id 探す SerialId。
     * @return 一致ノード (無ければ nullptr)。id<0 は常に nullptr。
     */
    ANode* FindBySerialId(i32 id) noexcept;

    // ------------------------------------------------------ コンポーネント

    /**
     * T の AComponent を構築・attach し、参照を返す。
     *
     * @details OnAttach は即時呼出。依存コンポーネントは OnRequire で先に確保される。
     * @tparam T 追加する AComponent 派生型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return attach した T への参照。
     */
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) noexcept {
        TUniquePtr<T> comp = MakeUnique<T>(Forward<Args>(args)...);
        T* ref = comp.Get();
        ref->SetOwner_Internal(this);
        // 依存コンポーネントを先に確保する。
        ref->OnRequire(*this);
        m_Components.Add(TUniquePtr<AComponent>(comp.Release(), comp.GetAllocator()));
        ref->OnAttach(*this);
        ref->MaybeAttachServices_Internal(SceneServices());   // ツリーが既に services 配線済なら即 fire
        return *ref;
    }

    /**
     * T があれば返し、無ければ追加して返す (RequireComponent の自動追加に使う)。
     *
     * @tparam T 取得または追加する AComponent 派生型。
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
     * @tparam T 探す AComponent 派生型。
     * @return 見つかった T へのポインタ (無ければ nullptr)。
     */
    template<typename T>
    T* GetComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Num(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                return static_cast<T*>(m_Components[i].Get());
            }
        }
        return nullptr;
    }

    /**
     * T 型コンポーネントを持っているかを返す。
     *
     * @tparam T 探す AComponent 派生型。
     * @return 持っていれば true。
     */
    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Num(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) return true;
        }
        return false;
    }

    /**
     * 最初に見つかった T 型コンポーネントを 1 つ除去する (OnDetach → 破棄)。
     *
     * @tparam T 除去する AComponent 派生型。
     * @return 除去したら true、見つからなければ false。
     */
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Num(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                m_Components[i]->OnDetach();
                m_Components[i].Reset();
                // compact: 末尾を i に詰める (順序は壊れる)
                if (i + 1 < m_Components.Num()) {
                    m_Components[i] = Move(m_Components[m_Components.Num() - 1]);
                }
                m_Components.Pop();
                return true;
            }
        }
        return false;
    }

    /**
     * 全コンポーネントを除去する (各 OnDetach → 破棄)。Play 終了時のクリーンアップ等に使う。
     */
    void RemoveAllComponents() noexcept {
        for (u32 i = 0; i < m_Components.Num(); ++i)
            if (m_Components[i]) { m_Components[i]->OnDetach(); m_Components[i].Reset(); }
        m_Components.Reset();
    }

    /**
     * attach 済みコンポーネントの数を返す。
     *
     * @return コンポーネント数。
     */
    u32 ComponentCount() const noexcept { return static_cast<u32>(m_Components.Num()); }

    /**
     * i 番目のコンポーネントを返す (型を知らない汎用列挙。範囲外は nullptr)。
     *
     * @param i コンポーネントのインデックス。
     * @return i 番目のコンポーネント (範囲外なら nullptr)。
     */
    AComponent*       ComponentAt(u32 i)       noexcept {
        return i < m_Components.Num() ? m_Components[i].Get() : nullptr;
    }

    /** i 番目のコンポーネントを返す (const 版)。 */
    const AComponent* ComponentAt(u32 i) const noexcept {
        return i < m_Components.Num() ? m_Components[i].Get() : nullptr;
    }

    /**
     * 構築済みのコンポーネントを非テンプレートで attach する (factory 生成物の取り付けに使う)。
     *
     * @details
     * reflection factory で生成した `TUniquePtr<AComponent>` の受け口。
     * OnRequire→OnAttach の lifecycle は AddComponent と同じ。`comp` はエンジン
     * アロケータ所有であること (CreateComponentByName が満たす)。
     * @param comp 取り付けるコンポーネント (所有権が移る、非 null 前提)。
     * @return attach したコンポーネントへの参照。
     */
    AComponent& AttachComponent(TUniquePtr<AComponent> comp) noexcept {
        AComponent* ref = comp.Get();
        ref->SetOwner_Internal(this);
        ref->OnRequire(*this);
        m_Components.Add(Move(comp));
        ref->OnAttach(*this);
        ref->MaybeAttachServices_Internal(SceneServices());
        return *ref;
    }

    // --------------------------------------------- services / subsystems

    /**
     * ツリーに配線された CSceneServices を返す (root まで遡る。未配線なら nullptr)。
     *
     * @return 配線済み CSceneServices ポインタ (未配線は nullptr)。
     */
    CSceneServices* SceneServices() const noexcept;

    /**
     * ツリーに配線された World サブシステム束を返す (root まで遡る。未配線なら nullptr)。
     *
     * @return 配線済み CSubsystemCollection (未配線は nullptr)。
     */
    CSubsystemCollection* Subsystems() const noexcept;

    /**
     * 型でサブシステムを取得する (root のコレクションから解決)。
     *
     * @tparam T ASubsystem 派生型。
     * @return T* (未配線/未登録なら nullptr)。
     */
    template<typename T>
    T* GetSubsystem() const noexcept {
        CSubsystemCollection* s = Subsystems();
        return (s != nullptr) ? s->Get<T>() : nullptr;
    }

    // ------------------------------------------------------------ ツリー実行

    /**
     * 自身と components の OnUpdate を呼び、子へ可変刻み update を伝播する。
     *
     * @param dt 前フレームからの経過秒。
     */
    void UpdateTree(f32 dt) noexcept;

    /**
     * 自身と components の OnFixedUpdate を呼び、子へ固定刻み update を伝播する。
     *
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdateTree(f32 fixed_dt) noexcept;

    /**
     * 自身と components を描画し、子ツリーをツリー順で描く。
     *
     * @details 描画順の並べ替え (DrawLayer/DrawPriority/YSort) は DrawTreeSorted が
     * フラット収集 + 安定ソートで行う。DrawTree 自体はツリー順再帰 (原子グループの
     * 内部描画にも使われる)。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawTree(FRenderContext& rc) noexcept;

    /**
     * subtree をグローバル描画順で描く (シーンの標準描画経路)。
     *
     * @details
     * 可視ノードをフラット収集し `(DrawLayer, DrawPriority, [YSort: world.y+bias],
     * ツリー出現順)` の安定ソートで並べてから各ノードの自前描画を実行する。階層は
     * transform 専用になり、描画順はレイヤー+プライオリティで自由に制御できる。
     *   ・Y ソートは同 layer・同 priority のノード間で適用される (足元遮蔽は
     *     キャラ群を同 priority + YSortEnabled にする)。
     *   ・WantsAtomicSubtree なコンポーネントを持つノード (ステンシルマスク等) は
     *     subtree ごと 1 個の描画単位として整列し、内部はツリー順 (DrawTree)。
     *   ・全ノードがキー 0 (layer=priority=0, YSort 無効, 原子なし) のフレームは
     *     ソートを省略してツリー順で描く = 従来挙動と完全一致・ゼロオーバーヘッド。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawTreeSorted(FRenderContext& rc) noexcept;

    /**
     * このノード自身 (OnDraw + components、子は含まない) を描画する。
     *
     * @details
     * DrawTreeSorted のフラット実行が使う内部寄り API (通常はシーン経由で
     * DrawTreeSorted を使う)。マテリアル/ライトの包み込みは DrawTree と同一。
     * 非原子ノードの OnDrawPostChildren は直後に呼ぶ。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawSelf(FRenderContext& rc) noexcept;

    /**
     * WantsAtomicSubtree なコンポーネントを持つかを返す (DrawTreeSorted の収集用)。
     *
     * @return 原子 subtree として扱うなら true。
     */
    bool HasAtomicSubtreeComponent() const noexcept {
        for (u32 i = 0; i < m_Components.Num(); ++i) {
            if (m_Components[i] && m_Components[i]->WantsAtomicSubtree()) return true;
        }
        return false;
    }

    /**
     * pending 中の Destroy / Reparent をフレーム境界で確定する。
     */
    void ResolveStructuralChanges() noexcept;

    /**
     * this が candidate の祖先かを返す。
     *
     * @param candidate 判定対象。
     * @return 祖先なら true。
     */
    bool IsAncestorOf(const ANode* candidate) const noexcept;

private:
    /**
     * 遅延 Reparent 先の寿命を監視する、割り当て不要の侵入型 observer。
     *
     * @details
     * ANode は NewObject 所有だけでなく AScene root の値所有も正式に許すため、
     * TWeakObjectPtr だけでは全対象を監視できない。observer を対象側のリストへ
     * 接続し、対象のデストラクタから無効化することで両方を stale-safe に扱う。
     */
    class FReparentTargetObserver final {
    public:
        FReparentTargetObserver() noexcept = default;
        ~FReparentTargetObserver() noexcept;

        FReparentTargetObserver(const FReparentTargetObserver&) = delete;
        FReparentTargetObserver& operator=(const FReparentTargetObserver&) = delete;
        FReparentTargetObserver(FReparentTargetObserver&&) = delete;
        FReparentTargetObserver& operator=(FReparentTargetObserver&&) = delete;

        /** target の監視を開始する。既存の監視は先に解除する。 */
        void Observe(ANode& target) noexcept;

        /** 現在の監視を解除して空にする。 */
        void Reset() noexcept;

        /** 対象が生存中ならそのポインタ、破棄済みまたは未設定なら nullptr。 */
        ANode* Get() const noexcept { return m_Target; }

    private:
        ANode* m_Target = nullptr;
        FReparentTargetObserver* m_Previous = nullptr;
        FReparentTargetObserver* m_Next = nullptr;

        friend class ANode;
    };

    /**
     * クォータニオンの Z 軸回転成分 (ZYX オイラーの yaw) を返す。
     *
     * @param q 抽出元の回転。
     * @return Z 軸回転角 (ラジアン)。
     */
    static f32 RotationZOf(const FQuat& q) noexcept {
        return ATan2(2.0f * (q.w * q.z + q.x * q.y),
                     1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    }

    /** 自身を root とした subtree の最大深度 (自身=0) を反復走査で返す。 */
    u32 SubtreeHeight() const noexcept;

    /** subtree を DFS し各コンポーネントの OnAttachServices をガード付きで発火する。 */
    void ActivateSubtreeServices_Internal(CSceneServices* svc) noexcept;

    /** ローカル transform (真値。world は親から合成)。 */
    FTransform3D m_Local{};

    /** ノード名 (ヒエラルキー表示 / ルックアップ用)。 */
    FString      m_Name;

    /** 親ノード (root は nullptr。非所有 = 親が自分を所有している)。 */
    ANode*       m_Parent          = nullptr;

    /** 子ノード群 (強参照所有)。 */
    TArray<TObjectPtr<ANode>> m_Children;

    /** attach 済みコンポーネント群 (単独所有)。 */
    TArray<TUniquePtr<AComponent>> m_Components;

    /** update 有効フラグ。 */
    bool         m_Enabled         = true;

    /** 可視フラグ。 */
    bool         m_Visible         = true;

    /** OnSpawn 発火済みフラグ。 */
    bool         m_Spawned         = false;

    /** 破棄予定フラグ (ResolveStructuralChanges で確定)。 */
    bool         m_PendingDestroy  = false;

    /** Y ソート参加フラグ。 */
    bool         m_YSortEnabled    = false;

    /** シーン直列化 ID (-1 = 未設定)。 */
    i32          m_SerialId        = -1;

    /** ノード ID (generational handle)。 */
    FNodeId      m_Id{};

    /** 親付け替え先。対象破棄時はデストラクタから自動的に無効化される。 */
    FReparentTargetObserver m_PendingReparentTarget;

    /** 自身を遅延 Reparent 先として監視している observer のリスト先頭。 */
    FReparentTargetObserver* m_ReparentObserverHead = nullptr;

    /** 描画レイヤー (第 1 ソートキー、小 = 奥)。 */
    i32          m_DrawLayer       = 0;

    /** 層内描画プライオリティ (第 2 ソートキー、小 = 奥)。 */
    i32          m_DrawPriority    = 0;

    /** Y-sort の pivot バイアス。 */
    f32          m_YSortBias       = 0.0f;

    /** 使用マテリアル (効果プリセット) の焼き込み状態。active=false なら効果なし。 */
    FMaterialState m_Mat;

    /** 自分自身のオクルーダー番号 (シーンの影収集が毎フレーム設定。-1=影源でない)。 */
    i32 m_SelfOccluder = -1;

    /** ツリー root に配線される services (root のみ設定、子は walk-to-root)。 */
    CSceneServices* m_Services = nullptr;

    /** ツリー root に配線されるサブシステム束 (root のみ設定)。 */
    CSubsystemCollection* m_Subsystems = nullptr;
};


/**
 * ノードの «組み立て» だけを通す窓口。
 *
 * @details
 * ローダ (`SceneTextLoader` / `Scene3DSerialize`) とエディタ ABI は自由関数なので
 * `friend` にできない。かといって組み立て用の関数を `ANode` の公開面へ戻すと、
 * **ゲームを書く人の補完候補に «呼んではいけないもの» が並ぶ。**
 *
 * 呼べてしまうことは変わらないが、
 *
 * - `ANode` の公開 API からは消える
 * - 呼ぶには内部用の型を名指しする必要がある
 * - grep 一発で «組み立てに触っている場所» が全部出る
 *
 * ゲームのコードからは**呼ばない**。
 */
class FNodeInternals {
public:
    /** 直列化 ID を割り当てる (.acscene の id / editor_id)。 */
    static void SetSerialId(ANode& node, i32 id) noexcept { node.SetSerialId_Internal(id); }

    /** root ノードへ services を配線する。 */
    static void SetSceneServices(ANode& node, CSceneServices* services) noexcept {
        node.SetSceneServices_Internal(services);
    }

    /** root ノードへ World サブシステム束を配線する。 */
    static void SetSubsystems(ANode& node, CSubsystemCollection* subsystems) noexcept {
        node.SetSubsystems_Internal(subsystems);
    }

    /** subtree の OnAttachServices を一度だけ発火する。 */
    static void ActivateServices(ANode& node, CSceneServices& services) noexcept {
        node.ActivateServices_Internal(services);
    }
};

} // namespace acs::game

namespace acs {

/** node追加結果をトップレベルから参照する正規入口。 */
using game::EAddChildResult;
/** node階層の最大深度をトップレベルから参照する正規入口。 */
using game::kNodeMaxTreeDepth;
/** node描画コンテキストをトップレベルから参照する正規入口。 */
using game::FRenderContext;
/** nodeへ設定する2D materialをトップレベルから参照する正規入口。 */
using game::FMaterial2D;
/** GameFramework 内の実装型をトップレベルから参照する正規入口。 */
using game::ANode;

} // namespace acs
