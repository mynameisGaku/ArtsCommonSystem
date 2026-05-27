// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorGizmo (Phase 21a)
//
// 役割:
//   選択中の FNode2D / Node3D の Transform を viewport 上で直接ドラッグ操作する
//   「ハンドル」。translate (移動) / rotate (回転) / scale (拡縮) の 3 モードを
//   持ち、各モードで X / Y / Z 軸ハンドル + 平面 (XY/XZ/YZ) ハンドル + 画面
//   並列ハンドル (rotate のみ) を提供する。Unity / Godot / UE のシーンビュー上の
//   操作系を最小集合で再実装したもの。
//
// 使い方 (典型):
//   acs::game::editor_core::FEditorGizmo gizmo;
//   gizmo.Init();
//   gizmo.SetMode(EGizmoMode::Translate);
//   gizmo.SetSpace(EGizmoSpace::World);
//   gizmo.SetSnapTranslate(0.5f);   // Shift キー押下中に 0.5 単位スナップ
//
//   // ---- 毎フレーム ----
//   // 1) ProcessInput: マウス ray と LMB 状態を渡す。hot axis 判定 / drag 開始/終了
//   //    の遷移を内部で管理する。
//   gizmo.ProcessInput(mouse_ray_origin, mouse_ray_direction,
//                      input.LmbDown(), input.LmbHeld(), input.LmbUp());
//
//   // 2) Manipulate: 選択中の Node の transform を渡して、drag 中なら値を
//   //    in-place で更新する。true 戻りなら何か変更があった (= UndoCommand 発火
//   //    タイミング判定にも使える)。
//   acs::FVec3 pos = node.WorldPosition();
//   acs::FVec3 rot = node.EulerRotation();
//   acs::FVec3 scl = node.WorldScale();
//   if (gizmo.Manipulate(pos, rot, scl)) {
//       node.SetWorldPosition(pos);
//       node.SetEulerRotation(rot);
//       node.SetWorldScale(scl);
//   }
//
//   // 3) DrawGizmo: FDebugDraw 経由で軸 / 平面 / ハンドルを描く (描画は外部の
//   //    FDebugDraw 消費側が ImGui / 自前 LineRenderer 等に転写する)。
//   gizmo.DrawGizmo(debug_draw, pos, rot, scl);
//
//   // 4) drag 終了時に FUndoStack へ push したい場合:
//   gizmo.SetOnManipulateCallback(&MyEditor::OnGizmoDelta, &editor);
//
// 設計選択 (Phase 21a):
//   ・**EGizmoMode / EGizmoSpace / EGizmoAxis の 3 enum**: ACS 規約の E prefix
//     enum、`u8` 基底で表のレイアウトに優しい。`None_` は EGizmoAxis 側で
//     キーワード衝突回避のためアンダースコア付き (foundation/Limits.h 等で
//     既に確立した方針)。
//   ・**GizmoState を struct として公開**: テストや editor 上の inspector で
//     「今 drag 中か」「どの軸が hot か」を読み取れるよう公開する。書き換えは
//     FEditorGizmo 内部からのみ行うが、struct 全体を public にしておけば
//     外部から ImGui::Text で覗くのが楽 (= debug / replay 性が高い)。
//   ・**ProcessInput → Manipulate → DrawGizmo の 3 段**: input 取得 / 値更新 /
//     描画を完全に分離する。これにより:
//       - headless test で ProcessInput + Manipulate だけ走らせて drag 結果を検証
//       - DrawGizmo 単体で「ハンドルだけ表示して操作はさせない」モード (replay /
//         動画キャプチャ) が可能
//   ・**raw 関数ポインタ callback**: ACS は std::function を使えないため、
//     ManipulateCallback は C スタイル `void(*)(void*, ...) noexcept` で揃える
//     (Input.h / FInspectorPanel と同形)。
//   ・**snap は Shift モディファイア前提**: SetSnap*(step) で step > 0 を渡すと
//     drag 中の Shift で snap が有効になる。step == 0 で snap 無効 (default)。
//     Shift キー検出は ProcessInput の呼び出し側で行い、本クラスは「snap step が
//     正 ≠ snap 有効」とだけ判定する (= 入力依存を抽象化)。
//   ・**ray-axis hit test**: 各軸ハンドル = 始点中心の細い円柱とみなし、ray と
//     円柱の最小距離が threshold 以下なら hit。閾値は world unit (= camera 距離
//     スケーリングは外側責務、本クラスは固定の `_handle_radius` を持つ)。
//   ・**ray-plane hit test**: 平面ハンドル = 中央付近の小正方形。ray-plane 交差
//     → 矩形内判定。サイズは `_plane_handle_size`。
//   ・**hot axis 永続性**: drag 中は hot axis を新しい hit に書き換えない
//     (= 一度掴んだ軸を離すまで保持)。drag 中でない時のみ毎フレーム判定する。
//   ・**Manipulate は delta ベースで動く**: drag 開始時の world-space 座標
//     (`drag_start_world`) を覚え、現フレームの mouse ray と軸/平面の交点との
//     差分から delta を計算する。これにより「クリック位置とハンドル中心がずれて
//     いても」drag が滑らかになる (Unity / Blender と同じ感覚)。
//   ・**rotate モードは euler 角度差分**: Phase 21a 範囲では quat 補間は使わず、
//     軸ごとに「現マウス角度 - 開始マウス角度」を計算して inout_rotation_euler の
//     対応軸に加算する単純実装。Gimbal lock の心配があるが、editor 上の手動
//     編集なので許容範囲。
//   ・**scale モードは axis-aligned**: 軸ハンドルで「その軸方向の uniform scale
//     倍率」を計算。XY/XZ/YZ 平面 + ScreenAlign は scale モードでは hit を取らない
//     (= 各軸ごとの非一様 scale を意図的に強制、Unity と同じ)。
//   ・**DrawGizmo は FDebugDraw (FVec2 ベース) に出力**: 現状 FDebugDraw は 2D
//     ラインバッファ (Pillar H Phase 1)。本ヘッダでは「Z 軸を捨てて XY 平面に
//     射影する」simple projection を採用する (= 2D top-down view を想定)。
//     完全 3D viewport 描画は将来 DebugDraw3D (= FVec3 ベース) を追加した上で
//     overload を生やす予定 (Phase 21b 以降)。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。state は POD GizmoState + パラメータ群のみ。
//
// 将来拡張余地 (Phase 21b 以降):
//   ・**multi-select**: 複数 Node を同時に操作する。pivot 計算 (centroid / first
//     selected / scene origin) と各 Node への delta 配布を追加。本クラスの
//     API シグネチャは 1 Node 想定なので、上位に MultiGizmo wrapper を載せる
//     形が綺麗。
//   ・**pivot mode (center / local origin / bounds center)**: 現状は「inout_*
//     で渡された point を pivot とみなす」一択。pivot 計算は上位責務 (上位が
//     bounds 計算した結果を pivot として渡す)。
//   ・**2D 専用 gizmo (top-down)**: Z 軸を完全に無視するモード。本クラスは
//     既に Z を捨てて 2D 描画している (= DrawGizmo の制約) ので、Manipulate
//     側でも Z 軸ハンドルを hide できる SetIgnoreZAxis(bool) を後付けする。
//   ・**collider 等の shape 編集**: AABB の各辺中央に resize handle を出す
//     ShapeGizmo として派生クラスで実装する想定。本クラスは transform 編集に
//     特化。
//   ・**DebugDraw3D (= FVec3 ベース) との overload**: 現状の DrawGizmo に加えて
//     `void DrawGizmo(DebugDraw3D& dd3, ...)` を追加し、3D viewport で正しい
//     遠近表示を提供する。
//   ・**screen-aligned ハンドル**: 既に EGizmoAxis::ScreenAlign を定義済みだが、
//     現状 Phase 21a は rotate モードの全軸回転として未使用 (= 値だけ予約)。
//
// 範囲外 (本フェーズで持たない):
//   ・vertex / face 編集 (mesh edit gizmo)
//   ・animation キーフレーム上のドラッグ操作 (TimelineEditor 側責務)
//   ・collider shape edit (= ShapeGizmo は後付け派生)
//   ・camera viewport 操作 (= Pan / Orbit / Zoom は別 input handler)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {
class FDebugDraw;        // 前方宣言 — .cpp で gameframework/FDebugDraw.h を include
}

namespace acs::game::editor_core {

// ============================================================================
// EGizmoMode — 現在の操作モード
// ----------------------------------------------------------------------------
// `None` は「ハンドル一切なし」(= シーン上で gizmo を描画も操作もしない) 用。
// editor の Q/W/E/R 系キーバインドで Translate/Rotate/Scale を切り替える想定。
// ============================================================================
enum class EGizmoMode : u8 {
    None      = 0,  // gizmo 完全 OFF
    Translate = 1,  // 移動 (axis arrow + plane handle)
    Rotate    = 2,  // 回転 (axis ring)
    Scale     = 3,  // 拡縮 (axis box)
};

// ============================================================================
// EGizmoSpace — 操作座標系
// ----------------------------------------------------------------------------
// World: ワールド軸 (X/Y/Z 固定) を表示し、その軸方向に move/scale する。
// Local: 対象 Node の rotation を考慮した「ローカル軸」を表示・操作する。
// Phase 21a 範囲では Local space は inout_rotation_euler を quat に変換して
// 軸を回転させる実装 (Manipulate / DrawGizmo の両方に効く)。
// ============================================================================
enum class EGizmoSpace : u8 {
    World = 0,  // world axis (default)
    Local = 1,  // node local axis (rotation を考慮)
};

// ============================================================================
// EGizmoAxis — どの軸/平面ハンドルが hot か
// ----------------------------------------------------------------------------
// `None_` のアンダースコアは `None` キーワード化を避けるための ACS 規約
// (foundation/Limits.h 等で既出)。
// XY / XZ / YZ は平面ハンドル (translate モードで 2 軸同時 drag 用)。
// ScreenAlign は rotate モードの「画面平行 trackball 風回転」用、Phase 21a では
// 値だけ予約 (実 hit test は将来追加)。
// ============================================================================
enum class EGizmoAxis : u8 {
    None_       = 0,  // どのハンドルもホバー / drag していない
    X           = 1,  // X 軸ハンドル
    Y           = 2,  // Y 軸ハンドル
    Z           = 3,  // Z 軸ハンドル
    XY          = 4,  // XY 平面ハンドル (translate)
    XZ          = 5,  // XZ 平面ハンドル (translate)
    YZ          = 6,  // YZ 平面ハンドル (translate)
    ScreenAlign = 7,  // 画面平行ハンドル (rotate 用、Phase 21a は予約)
};

// ============================================================================
// GizmoState — 現フレームの操作状態 (POD、テストで覗ける)
// ----------------------------------------------------------------------------
// 公開フィールドだが、書き換えは FEditorGizmo 内部からのみ行うこと。
// `drag_start_world` は drag 開始時のワールド空間ヒット点 (delta 計算の基点)。
// ============================================================================
struct GizmoState {
    EGizmoMode mode             = EGizmoMode::Translate;  // 現在のモード
    EGizmoSpace space           = EGizmoSpace::World;     // 現在の座標系
    EGizmoAxis  hot_axis        = EGizmoAxis::None_;      // ホバー or drag 中の軸
    bool        dragging        = false;                  // LMB 押下中の drag 中フラグ
    acs::FVec3   drag_start_world{};                       // drag 開始時の world hit point
};

// ============================================================================
// ManipulateCallback — drag 終了時 (or 各フレーム中) に外部へ delta を通知
// ----------------------------------------------------------------------------
// drag 完了時に 1 度だけ呼ばれる (= FUndoStack に MoveNodeCommand 等を push する
// 適切なタイミング)。`delta` の意味はモード依存:
//   Translate: world space の移動量 (FVec3)
//   Rotate   : euler 角度の差分 (radians; FVec3)
//   Scale    : scale 倍率の差分 (FVec3; 1.0 を基準)
// user は SetOnManipulateCallback の第二引数で渡したポインタがそのまま戻る。
// ============================================================================
using ManipulateCallback = void (*)(void* user, EGizmoMode mode, acs::FVec3 delta) noexcept;

// ============================================================================
// FEditorGizmo — 選択 Node の Transform を viewport 上で直接操作するハンドル
// ----------------------------------------------------------------------------
// 1 個のインスタンスを editor が所有し、選択中 Node の transform を毎フレーム
// 流し込む。ハンドル本体は POD 状態のみで、レンダリングは FDebugDraw 経由
// (= レンダラ非依存)。
// ============================================================================
class FEditorGizmo {
public:
    FEditorGizmo() noexcept  = default;
    ~FEditorGizmo() noexcept = default;

    // 非コピー / 非ムーブ: editor 内で 1 個だけ生存させる前提 (ACS 規約)。
    FEditorGizmo(const FEditorGizmo&)            = delete;
    FEditorGizmo& operator=(const FEditorGizmo&) = delete;
    FEditorGizmo(FEditorGizmo&&)                 = delete;
    FEditorGizmo& operator=(FEditorGizmo&&)      = delete;

    // ----- ライフサイクル ---------------------------------------------------

    // 初期化 (state を default に戻す、callback はクリアしない)。多重 Init 可。
    void Init() noexcept;

    // 後片付け (state + callback + snap step を完全初期化)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- モード / 座標系 -------------------------------------------------

    void SetMode(EGizmoMode mode) noexcept;
    EGizmoMode Mode() const noexcept { return _state.mode; }

    void SetSpace(EGizmoSpace space) noexcept;
    EGizmoSpace Space() const noexcept { return _state.space; }

    // ----- スナップ設定 -----------------------------------------------------
    // 各モードごとに独立した snap step。step <= 0 で snap 無効化。
    // Shift モディファイア検出は ProcessInput 呼び出し側責務 (= 呼ぶ側が
    // Shift 押下時のみ snap_active を true にして渡す API は将来検討)。
    // 現状は「step > 0 かつ drag 中なら常に snap する」シンプル動作。

    void SetSnapTranslate(f32 step) noexcept;   // world units
    void SetSnapRotate(f32 step_deg) noexcept;  // degrees (内部で radians 換算)
    void SetSnapScale(f32 step) noexcept;       // 倍率 (例: 0.1 → 10% 刻み)

    f32 SnapTranslate() const noexcept { return _snap_translate; }
    f32 SnapRotateDeg() const noexcept;
    f32 SnapScale() const noexcept     { return _snap_scale; }

    // ----- 入力処理 ---------------------------------------------------------
    // 毎フレーム呼ぶ。マウス ray と LMB 状態を渡して、drag 開始 / 継続 / 終了の
    // 遷移を更新する。Manipulate / DrawGizmo を呼ぶ前にこの関数を呼ぶこと。
    //
    // 引数:
    //   mouse_ray_origin     : カメラ位置 (world space)
    //   mouse_ray_direction  : マウス位置から伸びる方向 (正規化推奨)
    //   lmb_down             : 当フレームで LMB が押下された (edge: false→true)
    //   lmb_held             : 当フレームで LMB が押下中 (level)
    //   lmb_up               : 当フレームで LMB が離された (edge: true→false)
    //
    // 遷移:
    //   !dragging かつ lmb_down かつ hot_axis != None_ : drag 開始
    //   dragging  かつ lmb_held                       : drag 継続 (hot_axis 保持)
    //   dragging  かつ lmb_up                         : drag 終了 + callback 発火
    //   !dragging かつ !lmb_held                       : hot_axis を毎フレーム再判定
    void ProcessInput(acs::FVec3 mouse_ray_origin,
                      acs::FVec3 mouse_ray_direction,
                      bool lmb_down,
                      bool lmb_held,
                      bool lmb_up) noexcept;

    // ----- transform 操作 ---------------------------------------------------
    // ProcessInput の後に呼ぶ。drag 中であれば inout_* を in-place 更新し、
    // true を返す。drag 中でなければ false を返して inout_* は触らない。
    //
    // モード別の inout 解釈:
    //   Translate: inout_position に delta を加算
    //   Rotate   : inout_rotation_euler に angular delta を加算 (radians)
    //   Scale    : inout_scale に delta を加算 (1.0 を基準とした倍率)
    //
    // pivot は inout_position をそのまま使用 (= Node のローカル原点が pivot)。
    bool Manipulate(acs::FVec3& inout_position,
                    acs::FVec3& inout_rotation_euler,
                    acs::FVec3& inout_scale) noexcept;

    // ----- 描画 -------------------------------------------------------------
    // FDebugDraw 経由で軸 line + ハンドルを描く。実描画は dd の消費側責務。
    // FDebugDraw が 2D (FVec2) なので、Z 軸は XY 平面へ射影される (top-down view)。
    // 将来 DebugDraw3D が来たら overload を生やす予定。
    //
    // 描画色: X=red / Y=green / Z=blue / 平面=半透明黄色 / 選択中 hot=白ハイライト。
    void DrawGizmo(FDebugDraw& dd,
                   acs::FVec3 position,
                   acs::FVec3 rotation_euler,
                   acs::FVec3 scale) noexcept;

    // ----- 状態問い合わせ ---------------------------------------------------

    bool IsDragging() const noexcept    { return _state.dragging; }
    EGizmoAxis HotAxis() const noexcept { return _state.hot_axis; }

    // テスト / inspector 表示用に GizmoState の現値をまるごと参照で公開。
    const GizmoState& State() const noexcept { return _state; }

    // ----- callback 登録 ----------------------------------------------------
    // drag 終了時 (= 内部で `dragging: true → false` 遷移したフレーム末) に 1 度
    // 呼ばれる。null 渡しで解除 (= 既存 callback をクリア)。
    void SetOnManipulateCallback(ManipulateCallback cb, void* user) noexcept;

    // ----- ハンドル形状パラメータ (上位 editor から微調整可能) ---------------
    // 軸ハンドルの「太さ」(ray-cylinder hit test の半径) と、軸の長さ。
    // 平面ハンドルの半サイズ。camera 距離スケーリングは外側責務 (= camera 距離に
    // 応じて axis_length / handle_radius を毎フレーム書き換える運用)。

    void SetAxisLength(f32 length) noexcept;       // 軸の世界長 (default 1.0)
    void SetHandleRadius(f32 radius) noexcept;     // 軸 hit 円柱半径 (default 0.05)
    void SetPlaneHandleSize(f32 size) noexcept;    // 平面 hit 矩形半サイズ (default 0.2)

    f32 AxisLength() const noexcept       { return _axis_length; }
    f32 HandleRadius() const noexcept     { return _handle_radius; }
    f32 PlaneHandleSize() const noexcept  { return _plane_handle_size; }

private:
    // ProcessInput から呼ぶ「現マウス ray がどの軸/平面に当たっているか」判定。
    // drag 中は呼ばない (hot axis を保持するため)。
    EGizmoAxis PickAxis(acs::FVec3 ray_origin, acs::FVec3 ray_direction) const noexcept;

    // drag 開始時 / 継続時に呼ぶ「ray と hot 軸/平面の交点 (world space)」算出。
    // 失敗時 (= ray 平行など) は inout_hit を変えず false を返す。
    bool RaycastToHot(acs::FVec3 ray_origin,
                      acs::FVec3 ray_direction,
                      acs::FVec3& out_hit) const noexcept;

    // Manipulate 内で snap step を delta に適用するヘルパ (step==0 は no-op)。
    static f32 ApplySnap(f32 value, f32 step) noexcept;
    static acs::FVec3 ApplySnap(acs::FVec3 v, f32 step) noexcept;

    // drag 終了時 (lmb_up 検出時) に呼ぶ — 累積 delta を ManipulateCallback に渡す。
    void FireDragEnd() noexcept;

    // ----- 状態 -------------------------------------------------------------

    GizmoState _state{};

    // drag 開始時に記録される「直近の Manipulate 入力 transform 値」。
    // 一連の drag で累積 delta を計算するために使う (drag_start_world とは別)。
    // drag_start_world はワールド空間のヒット点、これは元の transform value。
    acs::FVec3 _drag_origin_pos{};
    acs::FVec3 _drag_origin_rot{};
    acs::FVec3 _drag_origin_scl{};
    // 初回 Manipulate で _drag_origin_* をセット済みかフラグ (= 0 判定の罠回避)。
    bool      _drag_origin_set = false;

    // 直近マウス ray (ProcessInput で更新、Manipulate で参照)
    acs::FVec3 _last_ray_origin{};
    acs::FVec3 _last_ray_direction{0.0f, 0.0f, 1.0f};

    // snap step (各モードごと、0 で disable)
    f32 _snap_translate = 0.0f;
    f32 _snap_rotate    = 0.0f;  // radians (SetSnapRotate(deg) で換算して格納)
    f32 _snap_scale     = 0.0f;

    // ハンドル形状
    f32 _axis_length        = 1.0f;
    f32 _handle_radius      = 0.05f;
    f32 _plane_handle_size  = 0.2f;

    // drag 終了通知 callback
    ManipulateCallback _cb      = nullptr;
    void*              _cb_user = nullptr;
};

} // namespace acs::game::editor_core
