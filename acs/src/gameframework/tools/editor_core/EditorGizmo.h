// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorGizmo
//
// 役割:
//   選択中の ANode の FTransform3D を viewport 上で直接ドラッグ操作する
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
//   // 2) Manipulate: 選択中の ANode の transform を渡して、drag 中なら値を
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
// 設計選択:
//   ・**EGizmoMode / EGizmoSpace / EGizmoAxis の 3 enum**: ACS 規約の E prefix
//     enum、`u8` 基底で表のレイアウトに優しい。`None_` は EGizmoAxis 側で
//     キーワード衝突回避のためアンダースコア付き (foundation/Limits.h 等で
//     既に確立した方針)。
//   ・**FGizmoState を struct として公開**: テストや editor 上の inspector で
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
//     スケーリングは外側責務、本クラスは固定の `m_HandleRadius` を持つ)。
//   ・**ray-plane hit test**: 平面ハンドル = 中央付近の小正方形。ray-plane 交差
//     → 矩形内判定。サイズは `m_PlaneHandleSize`。
//   ・**hot axis 永続性**: drag 中は hot axis を新しい hit に書き換えない
//     (= 一度掴んだ軸を離すまで保持)。drag 中でない時のみ毎フレーム判定する。
//   ・**Manipulate は delta ベースで動く**: drag 開始時の world-space 座標
//     (`drag_start_world`) を覚え、現フレームの mouse ray と軸/平面の交点との
//     差分から delta を計算する。これにより「クリック位置とハンドル中心がずれて
//     いても」drag が滑らかになる (Unity / Blender と同じ感覚)。
//   ・**rotate モードは euler 角度差分**: quat 補間は使わず、
//     軸ごとに「現マウス角度 - 開始マウス角度」を計算して inout_rotation_euler の
//     対応軸に加算する単純実装。Gimbal lock の心配があるが、editor 上の手動
//     編集なので許容範囲。
//   ・**scale モードは axis-aligned**: 軸ハンドルで「その軸方向の uniform scale
//     倍率」を計算。XY/XZ/YZ 平面 + ScreenAlign は scale モードでは hit を取らない
//     (= 各軸ごとの非一様 scale を意図的に強制、Unity と同じ)。
//   ・**DrawGizmo は FDebugDraw (FVec2 ベース) に出力**: FDebugDraw は 2D
//     ラインバッファ。本ヘッダでは「Z 軸を捨てて XY 平面に
//     射影する」simple projection を採用する (= 2D top-down view を想定)。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。state は POD FGizmoState + パラメータ群のみ。
//
// 範囲外:
//   ・vertex / face 編集 (mesh edit gizmo)
//   ・animation キーフレーム上のドラッグ操作 (TimelineEditor 側責務)
//   ・collider shape edit
//   ・camera viewport 操作 (= Pan / Orbit / Zoom は別 input handler)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {
class FDebugDraw;        // 前方宣言 — .cpp で gameframework/DebugDraw.h を include
}

namespace acs::game::editor_core {

/**
 * gizmo の現在の操作モード。
 *
 * @details
 * None は「ハンドル一切なし」(= シーン上で gizmo を描画も操作もしない) 用。
 * editor の Q/W/E/R 系キーバインドで Translate/Rotate/Scale を切り替える想定。
 */
enum class EGizmoMode : u8 {
    /** gizmo 完全 OFF (描画も hit test もしない)。 */
    None      = 0,

    /** 移動モード (axis arrow + plane handle)。 */
    Translate = 1,

    /** 回転モード (axis ring)。 */
    Rotate    = 2,

    /** 拡縮モード (axis box)。 */
    Scale     = 3,
};

/**
 * gizmo の操作座標系。
 *
 * @details
 * World はワールド軸 (X/Y/Z 固定) を表示してその軸方向に move/scale する。
 * Local は対象 ANode の rotation を考慮した「ローカル軸」を表示・操作する。
 * Local space は inout_rotation_euler を quat に変換して軸を回転させる実装で、
 * Manipulate / DrawGizmo の両方に効く。
 */
enum class EGizmoSpace : u8 {
    /** ワールド軸 (X/Y/Z 固定、既定)。 */
    World = 0,

    /** ANode の rotation を考慮したローカル軸。 */
    Local = 1,
};

/**
 * どの軸/平面ハンドルが hot (ホバー or drag 中) かを表す。
 *
 * @details
 * None_ のアンダースコアは None キーワード化を避けるための ACS 規約
 * (foundation/Limits.h 等で既出)。XY / XZ / YZ は平面ハンドル (translate
 * モードで 2 軸同時 drag 用)。ScreenAlign は rotate モードの「画面平行 trackball
 * 風回転」用で、値だけ予約し実 hit test は未実装。
 */
enum class EGizmoAxis : u8 {
    /** どのハンドルもホバー / drag していない。 */
    None_       = 0,

    /** X 軸ハンドル。 */
    X           = 1,

    /** Y 軸ハンドル。 */
    Y           = 2,

    /** Z 軸ハンドル。 */
    Z           = 3,

    /** XY 平面ハンドル (translate)。 */
    XY          = 4,

    /** XZ 平面ハンドル (translate)。 */
    XZ          = 5,

    /** YZ 平面ハンドル (translate)。 */
    YZ          = 6,

    /** 画面平行ハンドル (rotate 用、値だけ予約)。 */
    ScreenAlign = 7,
};

/**
 * gizmo の現フレームの操作状態 (POD、テストで覗ける)。
 *
 * @details
 * 公開フィールドだが、書き換えは FEditorGizmo 内部からのみ行うこと。
 * drag_start_world は drag 開始時のワールド空間ヒット点 (delta 計算の基点)。
 */
struct FGizmoState {
    /** 現在の操作モード。 */
    EGizmoMode mode             = EGizmoMode::Translate;

    /** 現在の操作座標系。 */
    EGizmoSpace space           = EGizmoSpace::World;

    /** ホバー or drag 中の軸/平面ハンドル。 */
    EGizmoAxis  hot_axis        = EGizmoAxis::None_;

    /** LMB 押下中の drag 中フラグ。 */
    bool        dragging        = false;

    /** drag 開始時の world hit point (delta 計算の基点)。 */
    acs::FVec3   drag_start_world{};
};

/**
 * drag 終了時に外部へ delta を通知する callback 型。
 *
 * @details
 * drag 完了時に 1 度だけ呼ばれる (= FUndoStack に FMoveNodeCommand 等を push する
 * 適切なタイミング)。delta の意味はモード依存で、Translate は world space の
 * 移動量、Rotate は euler 角度の差分 (radians)、Scale は scale 倍率の差分
 * (1.0 を基準) を表す。user は SetOnManipulateCallback の第二引数で渡した
 * ポインタがそのまま戻る。
 * @param user SetOnManipulateCallback で登録したユーザポインタ。
 * @param mode drag 確定時の操作モード。
 * @param delta モード依存の代表 delta。
 */
using ManipulateCallback = void (*)(void* user, EGizmoMode mode, acs::FVec3 delta) noexcept;

/**
 * 選択 ANode の FTransform3D を viewport 上で直接操作するハンドル。
 *
 * @details
 * 1 個のインスタンスを editor が所有し、選択中 ANode の transform を毎フレーム
 * 流し込む。ハンドル本体は POD 状態のみで、レンダリングは FDebugDraw 経由
 * (= レンダラ非依存)。ProcessInput → Manipulate → DrawGizmo の 3 段で入力取得 /
 * 値更新 / 描画を完全に分離し、translate / rotate / scale の各モードで X/Y/Z 軸
 * ハンドルと平面ハンドルを提供する。
 */
class FEditorGizmo {
public:
    /** 空状態で構築する (state は default、ハンドル形状は既定値)。 */
    FEditorGizmo() noexcept  = default;

    /** 破棄する (所有リソースなし)。 */
    ~FEditorGizmo() noexcept = default;

    /** コピー禁止 (editor 内で 1 個だけ生存させる前提)。 */
    FEditorGizmo(const FEditorGizmo&)            = delete;

    /** コピー代入も禁止。 */
    FEditorGizmo& operator=(const FEditorGizmo&) = delete;

    /** ムーブ禁止 (editor 内で 1 個だけ生存させる前提)。 */
    FEditorGizmo(FEditorGizmo&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FEditorGizmo& operator=(FEditorGizmo&&)      = delete;

    /**
     * 初期化する。
     *
     * @details
     * state を default に戻す (mode = Translate, hot = None_, dragging = false)。
     * callback / snap step / ハンドル形状は意図的に維持する (= editor セッションを
     * またいだ復帰を想定)。完全初期化したい場合は Shutdown を呼ぶ。多重 Init 可。
     */
    void Init() noexcept;

    /**
     * 後片付けする。
     *
     * @details state + callback + snap step + ハンドル形状をすべて default に戻す。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * 操作モードを設定する。
     *
     * @details drag 中に呼ばれた場合は drag を確定 (callback 発火) してから mode を変える。
     * @param mode 設定する操作モード。
     */
    void SetMode(EGizmoMode mode) noexcept;

    /**
     * 現在の操作モードを返す。
     *
     * @return 設定済みの EGizmoMode。
     */
    EGizmoMode Mode() const noexcept { return _state.mode; }

    /**
     * 操作座標系を設定する。
     *
     * @details drag 中に呼ばれた場合は drag を確定 (callback 発火) してから space を変える。
     * @param space 設定する操作座標系。
     */
    void SetSpace(EGizmoSpace space) noexcept;

    /**
     * 現在の操作座標系を返す。
     *
     * @return 設定済みの EGizmoSpace。
     */
    EGizmoSpace Space() const noexcept { return _state.space; }

    /**
     * 移動モードの snap step を設定する (world units)。
     *
     * @details step <= 0 で snap 無効化。drag 中に step > 0 なら delta が step に量子化される。
     * @param step スナップ単位 (world units、負値・0 は無効)。
     */
    void SetSnapTranslate(f32 step) noexcept;

    /**
     * 回転モードの snap step を設定する (degrees)。
     *
     * @details 内部で radians に換算して保持する。step_deg <= 0 で snap 無効化。
     * @param step_deg スナップ角度 (degrees、負値・0 は無効)。
     */
    void SetSnapRotate(f32 step_deg) noexcept;

    /**
     * 拡縮モードの snap step を設定する (倍率刻み)。
     *
     * @details step <= 0 で snap 無効化。
     * @param step スナップ倍率刻み (例: 0.1 → 10% 刻み、負値・0 は無効)。
     */
    void SetSnapScale(f32 step) noexcept;

    /**
     * 移動モードの snap step を返す。
     *
     * @return スナップ単位 (world units、0 なら無効)。
     */
    f32 SnapTranslate() const noexcept { return m_SnapTranslate; }

    /**
     * 回転モードの snap step を degrees で返す。
     *
     * @return スナップ角度 (degrees、0 なら無効)。
     */
    f32 SnapRotateDeg() const noexcept;

    /**
     * 拡縮モードの snap step を返す。
     *
     * @return スナップ倍率刻み (0 なら無効)。
     */
    f32 SnapScale() const noexcept     { return m_SnapScale; }

    /**
     * 入力を処理して drag state machine を駆動する (毎フレーム呼ぶ)。
     *
     * @details
     * Manipulate / DrawGizmo を呼ぶ前にこの関数を呼ぶこと。遷移は、!dragging かつ
     * lmb_down かつ hot_axis != None_ で drag 開始、dragging 中は hot_axis を保持、
     * dragging かつ lmb_up で drag 終了 + callback 発火、非 drag では hot_axis を
     * 毎フレーム再判定する。
     * @param mouse_ray_origin カメラ位置 (world space)。
     * @param mouse_ray_direction マウス位置から伸びる方向 (正規化推奨)。
     * @param lmb_down 当フレームで LMB が押下された (edge: false→true)。
     * @param lmb_held 当フレームで LMB が押下中 (level)。
     * @param lmb_up 当フレームで LMB が離された (edge: true→false)。
     */
    void ProcessInput(acs::FVec3 mouse_ray_origin,
                      acs::FVec3 mouse_ray_direction,
                      bool lmb_down,
                      bool lmb_held,
                      bool lmb_up) noexcept;

    /**
     * drag 中であれば transform を in-place 更新する (ProcessInput の後に呼ぶ)。
     *
     * @details
     * drag 中でなければ false を返して inout_* は触らない。モード別に、Translate は
     * inout_position に delta を加算、Rotate は inout_rotation_euler に angular delta
     * を加算 (radians)、Scale は inout_scale に倍率 delta を加算する。pivot は
     * inout_position をそのまま使用する (= ANode のローカル原点が pivot)。
     * @param inout_position 更新対象の位置 (Translate で書き換え)。
     * @param inout_rotation_euler 更新対象の euler 回転 (Rotate で書き換え、radians)。
     * @param inout_scale 更新対象のスケール (Scale で書き換え)。
     * @return drag 中で何か更新したら true、drag 中でなければ false。
     */
    bool Manipulate(acs::FVec3& inout_position,
                    acs::FVec3& inout_rotation_euler,
                    acs::FVec3& inout_scale) noexcept;

    /**
     * FDebugDraw 経由で軸 line + ハンドルを描く。
     *
     * @details
     * 実描画は dd の消費側責務。FDebugDraw が 2D (FVec2) なので Z 軸は XY 平面へ
     * 射影される (top-down view)。描画色は X=赤 / Y=緑 / Z=青 / 平面=半透明黄色 /
     * 選択中 hot=白ハイライト。
     * @param dd 軸・ハンドルの line を積む先のデバッグ描画バッファ。
     * @param position gizmo 中心の world 座標。
     * @param rotation_euler 軸ベースを構築するための euler 回転 (Local space で使用)。
     * @param scale 対象のスケール (描画には未使用、引数のみ)。
     */
    void DrawGizmo(FDebugDraw& dd,
                   acs::FVec3 position,
                   acs::FVec3 rotation_euler,
                   acs::FVec3 scale) noexcept;

    /**
     * drag 中かを返す。
     *
     * @return drag 中なら true。
     */
    bool IsDragging() const noexcept    { return _state.dragging; }

    /**
     * 現在 hot な軸/平面ハンドルを返す。
     *
     * @return ホバー or drag 中の EGizmoAxis (なければ None_)。
     */
    EGizmoAxis HotAxis() const noexcept { return _state.hot_axis; }

    /**
     * FGizmoState の現値をまるごと参照で返す (テスト / inspector 表示用)。
     *
     * @return 現フレームの FGizmoState への const 参照。
     */
    const FGizmoState& State() const noexcept { return _state; }

    /**
     * drag 終了通知 callback を登録する。
     *
     * @details
     * 内部で dragging が true → false へ遷移したフレーム末に 1 度呼ばれる。
     * cb に null を渡すと解除 (= 既存 callback をクリア)。
     * @param cb 登録する callback (null で解除)。
     * @param user callback に渡されるユーザポインタ。
     */
    void SetOnManipulateCallback(ManipulateCallback cb, void* user) noexcept;

    /**
     * 軸ハンドルの世界長を設定する。
     *
     * @details 描画と hit test の沿軸範囲に使う。0 以下は 1e-3 に clamp。camera 距離スケーリングは外側責務。
     * @param length 軸の世界長 (default 1.0)。
     */
    void SetAxisLength(f32 length) noexcept;

    /**
     * 軸ハンドルの hit 円柱半径を設定する。
     *
     * @details ray-line 最近接距離がこの半径以下なら軸 hit。0 以下は 1e-4 に clamp。
     * @param radius 軸 hit 円柱の半径 (default 0.05)。
     */
    void SetHandleRadius(f32 radius) noexcept;

    /**
     * 平面ハンドルの hit 矩形の半サイズを設定する。
     *
     * @details 0 以下は 1e-3 に clamp。
     * @param size 平面 hit 矩形の半サイズ (default 0.2)。
     */
    void SetPlaneHandleSize(f32 size) noexcept;

    /**
     * 軸ハンドルの世界長を返す。
     *
     * @return 設定済みの軸長。
     */
    f32 AxisLength() const noexcept       { return m_AxisLength; }

    /**
     * 軸ハンドルの hit 円柱半径を返す。
     *
     * @return 設定済みの hit 半径。
     */
    f32 HandleRadius() const noexcept     { return m_HandleRadius; }

    /**
     * 平面ハンドルの hit 矩形の半サイズを返す。
     *
     * @return 設定済みの平面 hit 半サイズ。
     */
    f32 PlaneHandleSize() const noexcept  { return m_PlaneHandleSize; }

private:
    /**
     * 現マウス ray がどの軸/平面ハンドルにホバーしているか判定する。
     *
     * @details
     * drag 中は呼ばない (hot axis を保持するため)。優先順は 平面ハンドル > 軸ハンドル。
     * gizmo 中心は m_DragOriginPos を仮定する。
     * @param ray_origin マウス ray の始点 (world space)。
     * @param ray_direction マウス ray の方向。
     * @return hit した EGizmoAxis (どれも当たらなければ None_)。
     */
    EGizmoAxis PickAxis(acs::FVec3 ray_origin, acs::FVec3 ray_direction) const noexcept;

    /**
     * 現マウス ray と hot 軸/平面の world space 交点を算出する。
     *
     * @details 軸ハンドルは ray と axis-line の最近接点、平面ハンドルは ray-plane 交点を hit とする。
     * @param ray_origin マウス ray の始点 (world space)。
     * @param ray_direction マウス ray の方向。
     * @param out_hit hit 点 (world space) の格納先。
     * @return 交点が取れたら true、平行・未対応軸なら false (out_hit は不変)。
     */
    bool RaycastToHot(acs::FVec3 ray_origin,
                      acs::FVec3 ray_direction,
                      acs::FVec3& out_hit) const noexcept;

    /**
     * スカラー値に snap step を適用する (四捨五入で量子化)。
     *
     * @param value 量子化する値。
     * @param step スナップ単位 (0 以下なら value をそのまま返す)。
     * @return 量子化後の値。
     */
    static f32 ApplySnap(f32 value, f32 step) noexcept;

    /**
     * FVec3 の各成分に独立して snap step を適用する。
     *
     * @param v 量子化するベクトル。
     * @param step スナップ単位 (0 以下なら v をそのまま返す)。
     * @return 各成分を量子化したベクトル。
     */
    static acs::FVec3 ApplySnap(acs::FVec3 v, f32 step) noexcept;

    /**
     * drag 終了時に ManipulateCallback へ delta を通知する。
     *
     * @details callback 未登録なら no-op。代表 delta は Zero で渡し、editor 側で drag 確定だけ判定する想定。
     */
    void FireDragEnd() noexcept;

    /** 現フレームの操作状態 (mode / space / hot_axis / dragging / drag_start_world)。 */
    FGizmoState _state{};

    /** drag 開始時に記録した元の位置 (累積 delta の基点)。 */
    acs::FVec3 m_DragOriginPos{};

    /** drag 開始時に記録した元の euler 回転 (累積 delta の基点)。 */
    acs::FVec3 m_DragOriginRot{};

    /** drag 開始時に記録した元のスケール (累積 delta の基点)。 */
    acs::FVec3 m_DragOriginScl{};

    /** 初回 Manipulate で m_DragOrigin* をセット済みかフラグ (0 値判定の罠回避)。 */
    bool      m_bDragOriginSet = false;

    /** 直近マウス ray の始点 (ProcessInput で更新、Manipulate で参照)。 */
    acs::FVec3 m_LastRayOrigin{};

    /** 直近マウス ray の方向 (ProcessInput で更新、Manipulate で参照)。 */
    acs::FVec3 m_LastRayDirection{0.0f, 0.0f, 1.0f};

    /** 移動 snap step (world units、0 で無効)。 */
    f32 m_SnapTranslate = 0.0f;

    /** 回転 snap step (radians、SetSnapRotate(deg) で換算して格納、0 で無効)。 */
    f32 m_SnapRotate    = 0.0f;

    /** 拡縮 snap step (倍率刻み、0 で無効)。 */
    f32 m_SnapScale     = 0.0f;

    /** 軸ハンドルの世界長。 */
    f32 m_AxisLength        = 1.0f;

    /** 軸ハンドルの hit 円柱半径。 */
    f32 m_HandleRadius      = 0.05f;

    /** 平面ハンドルの hit 矩形の半サイズ。 */
    f32 m_PlaneHandleSize  = 0.2f;

    /** drag 終了通知 callback (未登録なら nullptr)。 */
    ManipulateCallback m_Cb      = nullptr;

    /** callback に渡すユーザポインタ。 */
    void*              m_CbUser = nullptr;
};

} // namespace acs::game::editor_core
