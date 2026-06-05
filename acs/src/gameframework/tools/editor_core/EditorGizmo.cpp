// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorGizmo 実装
//
// 設計のポイント (詳細はヘッダ参照):
//   ・state は GizmoState (POD) + snap step / ハンドル形状 / callback のみ。
//   ・ProcessInput → hot axis 更新 + drag 開始/終了の遷移。drag 中は hot axis を
//     ロックし続ける (途中で別軸が hot になっても掴んだ軸を保持)。
//   ・Manipulate → drag 中のとき限定で、現マウス ray と hot 軸/平面の交点を
//     drag_start_world と比較して delta を計算し inout_* に加算する。
//   ・DrawGizmo → FDebugDraw (FVec2) に対し、3D 入力を XY 平面へ射影して描画。
//     Z 軸ハンドルは「中心からマーカー一個」だけ表示 (top-down view では Z が
//     画面に向くため、線として描けない)。
//   ・ray-axis hit test = 「ray と無限直線の最近接距離 ≤ m_HandleRadius」かつ
//     「沿軸方向の射影 t が [0, m_AxisLength]」。完全な ray-finite-cylinder では
//     ないが、軸の手先より遠い場所をクリックして反応してしまう問題は防げる。
//   ・ray-plane hit test = ray-plane 交点を計算 → 中央付近の正方形内判定。
//   ・全 noexcept / STL 不使用 / 例外なし — 失敗系は no-op / false 戻りで安全。

#include "gameframework/tools/editor_core/EditorGizmo.h"

#include "gameframework/DebugDraw.h"
#include "math/Collision2D.h"
#include "math/Math.h"
#include "math/Quat.h"

namespace acs::game::editor_core {

namespace {

/**
 * f32 の絶対値を返す。
 *
 * @param v 対象値。
 * @return |v|。
 */
ACS_FORCEINLINE f32 Abs32(f32 v) noexcept { return v < 0.0f ? -v : v; }

/**
 * FVec3 同士の内積を返す。
 *
 * @param a 左辺ベクトル。
 * @param b 右辺ベクトル。
 * @return a·b。
 */
ACS_FORCEINLINE f32 Dot3(acs::FVec3 a, acs::FVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/**
 * gizmo の X / Y / Z 軸ベース 3 本をまとめた構造体。
 *
 * @details World space では単位ベクトル、Local space では rotation_euler を適用した回転後の軸。
 */
struct AxisBasis {
    /** X 軸ベクトル。 */
    acs::FVec3 x;

    /** Y 軸ベクトル。 */
    acs::FVec3 y;

    /** Z 軸ベクトル。 */
    acs::FVec3 z;
};

/**
 * 操作座標系と euler 回転から軸ベース 3 本を構築する。
 *
 * @details World は単位軸、Local は euler (pitch=X, yaw=Y, roll=Z) を quat に変換して単位軸を回転する。
 * @param space 操作座標系 (World / Local)。
 * @param rot_euler Local space で軸を回転させる euler 角 (radians)。
 * @return 構築した AxisBasis。
 */
ACS_FORCEINLINE AxisBasis MakeBasis(EGizmoSpace space, acs::FVec3 rot_euler) noexcept {
    AxisBasis b{};
    if (space == EGizmoSpace::World) {
        b.x = acs::FVec3{1.0f, 0.0f, 0.0f};
        b.y = acs::FVec3{0.0f, 1.0f, 0.0f};
        b.z = acs::FVec3{0.0f, 0.0f, 1.0f};
    } else {
        // Local: euler (pitch, yaw, roll) を quat に → 単位軸を回転。
        // FQuat::Euler は (pitch=X, yaw=Y, roll=Z) のラジアン規約。
        const acs::FQuat q = acs::FQuat::Euler(rot_euler.x, rot_euler.y, rot_euler.z);
        b.x = acs::Rotate(q, acs::FVec3{1.0f, 0.0f, 0.0f});
        b.y = acs::Rotate(q, acs::FVec3{0.0f, 1.0f, 0.0f});
        b.z = acs::Rotate(q, acs::FVec3{0.0f, 0.0f, 1.0f});
    }
    return b;
}

/**
 * AxisBasis から EGizmoAxis::X/Y/Z に対応する軸ベクトルを取り出す。
 *
 * @param b 取り出し元の軸ベース。
 * @param a 取り出す軸 (X/Y/Z 以外はゼロベクトル)。
 * @return 対応する軸ベクトル (X/Y/Z 以外は (0,0,0))。
 */
ACS_FORCEINLINE acs::FVec3 PickAxisVec(const AxisBasis& b, EGizmoAxis a) noexcept {
    switch (a) {
        case EGizmoAxis::X: return b.x;
        case EGizmoAxis::Y: return b.y;
        case EGizmoAxis::Z: return b.z;
        default:            return acs::FVec3{0.0f, 0.0f, 0.0f};
    }
}

/**
 * ray と直線の最近接計算の結果。
 *
 * @details
 * dist_sq は最近接距離の二乗、ray_t は ray 上の最近接パラメータ、line_s は line 上の
 * 最近接パラメータ。line_s が [0, axis_length] の外側かどうかは呼び出し側で判定する。
 */
struct ClosestRayLine {
    /** 最近接ペア間の距離の二乗。 */
    f32 dist_sq;

    /** ray 上の最近接パラメータ t。 */
    f32 ray_t;

    /** line 上の最近接パラメータ s (= 沿軸距離)。 */
    f32 line_s;

    /** ray と line が平行なら true (ray_t=0、line_s は projection)。 */
    bool parallel;
};

/**
 * ray と直線の最近接点ペアを計算する。
 *
 * @details
 * ray L1(t)=ray_o+ray_d*t と line L2(s)=line_p+line_d*s の最近接パラメータを解く。
 * 平行 (denom ≈ 0) のときは ray_t=0、line_s を line 上への projection とする。
 * @param ray_o ray の始点。
 * @param ray_d ray の方向。
 * @param line_p 直線上の 1 点。
 * @param line_d 直線の方向 (正規化想定)。
 * @return 最近接距離・両パラメータ・平行フラグを格納した ClosestRayLine。
 */
ClosestRayLine ClosestPointsRayLine(acs::FVec3 ray_o, acs::FVec3 ray_d,
                                    acs::FVec3 line_p, acs::FVec3 line_d) noexcept {
    // Real-Time Rendering / Eberly "3D FGame Engine Design" 5.1.1 を参照。
    // 2 直線 L1(t)=ray_o + ray_d*t, L2(s)=line_p + line_d*s の最近接ペアは
    //   d.dd  = dot(ray_d, ray_d)
    //   d.de  = dot(ray_d, line_d)
    //   d.ee  = dot(line_d, line_d)
    //   d.dr  = dot(ray_d, line_p - ray_o)
    //   d.er  = dot(line_d, line_p - ray_o)
    //   denom = d.dd * d.ee - d.de * d.de
    //   t = (d.ee * d.dr - d.de * d.er) / denom
    //   s = (d.de * d.dr - d.dd * d.er) / denom
    ClosestRayLine r{};
    const acs::FVec3 diff = line_p - ray_o;
    const f32 a = Dot3(ray_d, ray_d);
    const f32 b = Dot3(ray_d, line_d);
    const f32 c = Dot3(line_d, line_d);
    const f32 d = Dot3(ray_d, diff);
    const f32 e = Dot3(line_d, diff);
    const f32 denom = a * c - b * b;
    if (denom < 1e-8f) {
        // 平行 — ray_t = 0、line_s = e / c (line 上の projection)
        r.parallel = true;
        r.ray_t = 0.0f;
        r.line_s = (c > 1e-8f) ? (e / c) : 0.0f;
    } else {
        r.parallel = false;
        r.ray_t  = (c * d - b * e) / denom;
        r.line_s = (b * d - a * e) / denom;
    }
    // 最近接点
    const acs::FVec3 p_ray  = ray_o  + ray_d  * r.ray_t;
    const acs::FVec3 p_line = line_p + line_d * r.line_s;
    const acs::FVec3 diff2  = p_ray - p_line;
    r.dist_sq = Dot3(diff2, diff2);
    return r;
}

/**
 * ray と平面の交差パラメータ t を計算する。
 *
 * @details ray が平面と平行 (|n·d| ≈ 0) または交点が背面 (t < 0) なら false。
 * @param ray_o ray の始点。
 * @param ray_d ray の方向。
 * @param plane_p 平面上の 1 点。
 * @param plane_n 平面の法線。
 * @param out_t 交差パラメータ t の格納先。
 * @return 交差したら true (out_t 有効)、平行・背面なら false。
 */
bool RayPlaneIntersect(acs::FVec3 ray_o, acs::FVec3 ray_d,
                       acs::FVec3 plane_p, acs::FVec3 plane_n,
                       f32& out_t) noexcept {
    const f32 nd = Dot3(plane_n, ray_d);
    if (Abs32(nd) < 1e-8f) {
        return false;
    }
    const f32 t = Dot3(plane_n, plane_p - ray_o) / nd;
    if (t < 0.0f) {
        return false;
    }
    out_t = t;
    return true;
}

/**
 * 平面ハンドルの法線と平面上の u/v 軸をまとめた構造体。
 */
struct PlaneFrame {
    /** 平面の法線。 */
    acs::FVec3 n;

    /** 平面上の「横」軸。 */
    acs::FVec3 u;

    /** 平面上の「縦」軸。 */
    acs::FVec3 v;
};

/**
 * 平面ハンドル EGizmoAxis::XY/XZ/YZ の PlaneFrame を構築する。
 *
 * @details AxisBasis を渡すことで World / Local の両座標系に対応する。未対応軸は XY を既定とする。
 * @param b 軸ベース (World は単位軸、Local は回転後の軸)。
 * @param axis 平面ハンドル軸 (XY/XZ/YZ)。
 * @return 構築した PlaneFrame (法線 + u/v 軸)。
 */
PlaneFrame MakePlaneFrame(const AxisBasis& b, EGizmoAxis axis) noexcept {
    PlaneFrame f{};
    switch (axis) {
        case EGizmoAxis::XY: f.n = b.z; f.u = b.x; f.v = b.y; break;
        case EGizmoAxis::XZ: f.n = b.y; f.u = b.x; f.v = b.z; break;
        case EGizmoAxis::YZ: f.n = b.x; f.u = b.y; f.v = b.z; break;
        default:             f.n = b.z; f.u = b.x; f.v = b.y; break;
    }
    return f;
}

} // namespace

/** state とドラッグ関連の内部状態を default に戻す (callback / snap / 形状は維持)。 */
void FEditorGizmo::Init() noexcept {
    // state を default に戻す (mode = Translate, hot = None_, dragging = false)。
    // callback / snap step / ハンドル形状は意図的に維持 (= editor セッションを
    // またいだ復帰を想定)。完全初期化したい場合は Shutdown を呼ぶ。
    _state                = GizmoState{};
    m_DragOriginPos      = acs::FVec3{};
    m_DragOriginRot      = acs::FVec3{};
    m_DragOriginScl      = acs::FVec3{};
    m_bDragOriginSet      = false;
    m_LastRayOrigin      = acs::FVec3{};
    m_LastRayDirection   = acs::FVec3{0.0f, 0.0f, 1.0f};
}

/** state + callback + snap step + ハンドル形状をすべて default に戻す。 */
void FEditorGizmo::Shutdown() noexcept {
    // 完全初期化: state + callback + snap step + ハンドル形状をすべて default に。
    _state                = GizmoState{};
    m_DragOriginPos      = acs::FVec3{};
    m_DragOriginRot      = acs::FVec3{};
    m_DragOriginScl      = acs::FVec3{};
    m_bDragOriginSet      = false;
    m_LastRayOrigin      = acs::FVec3{};
    m_LastRayDirection   = acs::FVec3{0.0f, 0.0f, 1.0f};
    m_SnapTranslate       = 0.0f;
    m_SnapRotate          = 0.0f;
    m_SnapScale           = 0.0f;
    m_AxisLength          = 1.0f;
    m_HandleRadius        = 0.05f;
    m_PlaneHandleSize    = 0.2f;
    m_Cb                   = nullptr;
    m_CbUser              = nullptr;
}

/** 操作モードを設定する (drag 中なら確定してから切替)。 */
void FEditorGizmo::SetMode(EGizmoMode mode) noexcept {
    // drag 中のモード切替は不整合の元 (= 軸ハンドルの意味が変わる)。
    // drag 中なら強制終了 (= drag を確定させてから mode を変える)。
    if (_state.dragging) {
        FireDragEnd();
        _state.dragging  = false;
        _state.hot_axis  = EGizmoAxis::None_;
    }
    _state.mode = mode;
}

/** 操作座標系を設定する (drag 中なら確定してから切替)。 */
void FEditorGizmo::SetSpace(EGizmoSpace space) noexcept {
    // 座標系切替も drag 中は強制終了 (= 操作中の delta 基準が変わるため)。
    if (_state.dragging) {
        FireDragEnd();
        _state.dragging  = false;
        _state.hot_axis  = EGizmoAxis::None_;
    }
    _state.space = space;
}

/** 移動 snap step を設定する (負値は 0 = 無効に倒す)。 */
void FEditorGizmo::SetSnapTranslate(f32 step) noexcept {
    // 負値は誤代入と見なして 0 (snap 無効) に倒す。
    m_SnapTranslate = step > 0.0f ? step : 0.0f;
}

/** 回転 snap step を degrees で受け、radians 換算で格納する (負値は 0 = 無効)。 */
void FEditorGizmo::SetSnapRotate(f32 step_deg) noexcept {
    // 度 → ラジアン換算して保持。負値は 0 に倒す。
    m_SnapRotate = step_deg > 0.0f ? (step_deg * acs::kDeg2Rad) : 0.0f;
}

/** 拡縮 snap step を設定する (負値は 0 = 無効に倒す)。 */
void FEditorGizmo::SetSnapScale(f32 step) noexcept {
    m_SnapScale = step > 0.0f ? step : 0.0f;
}

/** 内部 radians の回転 snap を表示用 degrees に戻して返す。 */
f32 FEditorGizmo::SnapRotateDeg() const noexcept {
    // 内部 radians → 表示用 deg に戻す (= ImGui 等で表示する際に便利)。
    return m_SnapRotate * acs::kRad2Deg;
}

/** 軸ハンドルの世界長を設定する (1e-3 で下限 clamp、退化防止)。 */
void FEditorGizmo::SetAxisLength(f32 length) noexcept {
    // 0 以下は退化 (= 描画も hit test も成立しない)。最小 1e-3 で clamp。
    m_AxisLength = length > 1e-3f ? length : 1e-3f;
}

/** 軸 hit 円柱半径を設定する (1e-4 で下限 clamp)。 */
void FEditorGizmo::SetHandleRadius(f32 radius) noexcept {
    m_HandleRadius = radius > 1e-4f ? radius : 1e-4f;
}

/** 平面 hit 矩形の半サイズを設定する (1e-3 で下限 clamp)。 */
void FEditorGizmo::SetPlaneHandleSize(f32 size) noexcept {
    m_PlaneHandleSize = size > 1e-3f ? size : 1e-3f;
}

/** drag 終了通知 callback とユーザポインタを登録する。 */
void FEditorGizmo::SetOnManipulateCallback(ManipulateCallback cb, void* user) noexcept {
    m_Cb      = cb;
    m_CbUser = user;
}

/** マウス ray と LMB エッジ/レベルから drag state machine を 1 フレーム駆動する。 */
void FEditorGizmo::ProcessInput(acs::FVec3 mouse_ray_origin,
                                acs::FVec3 mouse_ray_direction,
                                bool lmb_down,
                                bool lmb_held,
                                bool lmb_up) noexcept {
    // mode == None なら gizmo そのものを無効化 (描画も hit test もしない)。
    if (_state.mode == EGizmoMode::None) {
        _state.hot_axis = EGizmoAxis::None_;
        _state.dragging = false;
        return;
    }

    // 直近 ray は Manipulate / DrawGizmo で使い回す。
    m_LastRayOrigin    = mouse_ray_origin;
    m_LastRayDirection = mouse_ray_direction;

    // lmb_held は契約上の存在意義 (drag 継続は state で表現) はあるが、本実装の
    // 制御は dragging フラグ + lmb_up で完結するため明示的には参照しない。
    (void)lmb_held;

    if (_state.dragging) {
        // drag 中: hot axis を維持。lmb_up で終了。
        if (lmb_up) {
            // 終了直前に accumulated delta を callback に通知。
            FireDragEnd();
            _state.dragging = false;
            // hot_axis は終了後にもう一度 PickAxis で再判定する
            // (現フレームの ray 位置に応じて hover ハイライトを更新)。
            _state.hot_axis = PickAxis(mouse_ray_origin, mouse_ray_direction);
        }
        // lmb_held 中は何もしない (= hot_axis lock 維持)。
    } else {
        // 非 drag: hot_axis を毎フレーム判定。
        _state.hot_axis = PickAxis(mouse_ray_origin, mouse_ray_direction);

        // drag 開始判定: lmb_down + 何か hot ならスタート。
        if (lmb_down && _state.hot_axis != EGizmoAxis::None_) {
            // drag_start_world (= 現マウス ray と hot 軸/平面の交点) を保存。
            // 失敗時 (= ray 平行) は drag 開始しない (= no-op で保留)。
            acs::FVec3 hit{};
            if (RaycastToHot(mouse_ray_origin, mouse_ray_direction, hit)) {
                _state.dragging         = true;
                _state.drag_start_world = hit;
                // m_DragOriginPos/rot/scl は Manipulate 初回で transform 値を
                // 受け取った瞬間にセットする (= 本関数は値を知らない)。
                // m_bDragOriginSet フラグで「初回セット待ち」を識別。
                m_bDragOriginSet = false;
            }
        }
    }
}

/** drag 中のみ hot 軸/平面と現マウス ray の交点から delta を計算し transform を更新する。 */
bool FEditorGizmo::Manipulate(acs::FVec3& inout_position,
                              acs::FVec3& inout_rotation_euler,
                              acs::FVec3& inout_scale) noexcept {
    if (!_state.dragging) {
        return false;
    }
    if (_state.mode == EGizmoMode::None || _state.hot_axis == EGizmoAxis::None_) {
        return false;
    }

    // drag 開始直後 (= 初回 Manipulate 呼び出し) なら m_DragOrigin* を確定。
    // ProcessInput で drag 開始時に `m_bDragOriginSet = false` にしているので、
    // ここで「未セット ⇒ 今フレームの inout_* をコピーして以後の delta 計算の
    // 基準にする」分岐を行う。bool フラグなので legitimate な (0,0,0) 値でも
    // 正しく動作する (= 値ベース判定の罠を回避)。
    if (!m_bDragOriginSet) {
        m_DragOriginPos = inout_position;
        m_DragOriginRot = inout_rotation_euler;
        m_DragOriginScl = inout_scale;
        m_bDragOriginSet = true;
    }

    // 現フレームの hot 軸 / 平面と現マウス ray の交点を取得。
    acs::FVec3 hit_now{};
    if (!RaycastToHot(m_LastRayOrigin, m_LastRayDirection, hit_now)) {
        // 平行などで hit が取れないフレームは inout を触らず true を返す
        // (drag 状態は維持される)。
        return true;
    }

    const acs::FVec3 raw_delta = hit_now - _state.drag_start_world;
    const AxisBasis basis     = MakeBasis(_state.space, m_DragOriginRot);

    switch (_state.mode) {
        case EGizmoMode::Translate: {
            acs::FVec3 delta = raw_delta;
            // 軸ハンドルなら 1 軸へ射影、平面ハンドルなら 2 軸分そのまま使う。
            if (_state.hot_axis == EGizmoAxis::X ||
                _state.hot_axis == EGizmoAxis::Y ||
                _state.hot_axis == EGizmoAxis::Z) {
                const acs::FVec3 axis = PickAxisVec(basis, _state.hot_axis);
                const f32 t = Dot3(raw_delta, axis);
                delta = axis * t;
            }
            // snap (各成分独立に量子化)。
            if (m_SnapTranslate > 0.0f) {
                delta = ApplySnap(delta, m_SnapTranslate);
            }
            inout_position = m_DragOriginPos + delta;
            return true;
        }
        case EGizmoMode::Rotate: {
            // 単純実装: drag 開始点 → 現 hit 点の "ベクトル角度差" を hot 軸の
            // euler 成分に加算する。pivot は m_DragOriginPos。
            // 厳密には軸まわりの平面投影が必要だが、ここでは sweep 角度
            // の符号付き大きさで近似する。
            const acs::FVec3 v0 = _state.drag_start_world - m_DragOriginPos;
            const acs::FVec3 v1 = hit_now                 - m_DragOriginPos;
            // 軸ベクトル取得 (rotate モードでは X/Y/Z ハンドルのみ有効、
            // 平面ハンドルは hot にならない設計だが念のため default で X)。
            const acs::FVec3 axis = PickAxisVec(basis,
                _state.hot_axis == EGizmoAxis::None_ ? EGizmoAxis::X : _state.hot_axis);
            // 軸まわりの符号付き角度: atan2(dot(axis, cross(v0,v1)), dot(v0,v1))
            const acs::FVec3 cr = acs::Cross(v0, v1);
            const f32 sin_a = Dot3(axis, cr);
            const f32 cos_a = Dot3(v0, v1);
            f32 angle = acs::ATan2(sin_a, cos_a);
            // snap (radians 単位)。
            if (m_SnapRotate > 0.0f) {
                angle = ApplySnap(angle, m_SnapRotate);
            }
            // 累積回転を m_DragOriginRot に加算。hot 軸の euler 成分にのみ加算
            // (= 簡易だが editor 用途では実用範囲)。
            acs::FVec3 result = m_DragOriginRot;
            switch (_state.hot_axis) {
                case EGizmoAxis::X: result.x += angle; break;
                case EGizmoAxis::Y: result.y += angle; break;
                case EGizmoAxis::Z: result.z += angle; break;
                default:            break;
            }
            inout_rotation_euler = result;
            return true;
        }
        case EGizmoMode::Scale: {
            // 軸方向の drag 距離を axis_length で正規化し、倍率を計算。
            // 平面ハンドルは scale モードでは hit を取らない (= 非対応軸は no-op)。
            if (_state.hot_axis != EGizmoAxis::X &&
                _state.hot_axis != EGizmoAxis::Y &&
                _state.hot_axis != EGizmoAxis::Z) {
                return true;
            }
            const acs::FVec3 axis = PickAxisVec(basis, _state.hot_axis);
            const f32 signed_dist = Dot3(raw_delta, axis);
            f32 factor = signed_dist / m_AxisLength;  // 0 で 1.0x、+1 で 2.0x 相当
            // snap (倍率刻み)
            if (m_SnapScale > 0.0f) {
                factor = ApplySnap(factor, m_SnapScale);
            }
            acs::FVec3 result = m_DragOriginScl;
            switch (_state.hot_axis) {
                case EGizmoAxis::X: result.x += factor; break;
                case EGizmoAxis::Y: result.y += factor; break;
                case EGizmoAxis::Z: result.z += factor; break;
                default:            break;
            }
            // 退化防止: scale が 0 以下にならないよう小値で clamp。
            if (result.x < 1e-3f) result.x = 1e-3f;
            if (result.y < 1e-3f) result.y = 1e-3f;
            if (result.z < 1e-3f) result.z = 1e-3f;
            inout_scale = result;
            return true;
        }
        case EGizmoMode::None:
        default:
            return false;
    }
}

/** 3D ハンドルを XY 平面へ射影して FDebugDraw に軸 line + ハンドルを積む。 */
void FEditorGizmo::DrawGizmo(FDebugDraw& dd,
                             acs::FVec3 position,
                             acs::FVec3 rotation_euler,
                             acs::FVec3 scale) noexcept {
    // mode None は描画しない。
    if (_state.mode == EGizmoMode::None) {
        return;
    }
    (void)scale;  // 描画スケールは axis_length で固定 (= 対象の scale 値とは独立)。

    const AxisBasis basis = MakeBasis(_state.space, rotation_euler);

    // 3D → 2D 射影 (XY 平面、Z 捨てる)。
    auto to2 = [](acs::FVec3 v) noexcept -> acs::FVec2 {
        return acs::FVec2{v.x, v.y};
    };

    // 色定義: 通常色 / hot 時の白ハイライト。
    constexpr acs::FVec4 kColX     = {1.0f, 0.2f, 0.2f, 1.0f};   // 赤
    constexpr acs::FVec4 kColY     = {0.2f, 1.0f, 0.2f, 1.0f};   // 緑
    constexpr acs::FVec4 kColZ     = {0.2f, 0.4f, 1.0f, 1.0f};   // 青
    constexpr acs::FVec4 kColHot   = {1.0f, 1.0f, 1.0f, 1.0f};   // 白
    constexpr acs::FVec4 kColPlane = {1.0f, 1.0f, 0.0f, 0.5f};   // 半透明黄

    const auto axis_color = [this](EGizmoAxis a, acs::FVec4 base) noexcept -> acs::FVec4 {
        return (_state.hot_axis == a) ? kColHot : base;
    };

    // 軸 line (X / Y / Z)。
    const acs::FVec2 center2 = to2(position);
    {
        const acs::FVec3 ex = position + basis.x * m_AxisLength;
        dd.DrawLine(center2, to2(ex), axis_color(EGizmoAxis::X, kColX));
    }
    {
        const acs::FVec3 ey = position + basis.y * m_AxisLength;
        dd.DrawLine(center2, to2(ey), axis_color(EGizmoAxis::Y, kColY));
    }
    {
        // Z 軸: 2D 射影では「中央から短い + マーカー」で表現。
        const acs::FVec3 ez = position + basis.z * m_AxisLength;
        const acs::FVec2 ez2 = to2(ez);
        const acs::FVec4 col = axis_color(EGizmoAxis::Z, kColZ);
        // 通常 line も描く (basis.z の XY 成分が 0 でなければ可視)。
        dd.DrawLine(center2, ez2, col);
        // 加えて + マーカー (Z 軸先端の手がかり)。
        dd.DrawCross(ez2, m_HandleRadius * 4.0f, col);
    }

    // 平面ハンドル (translate モードのみ)。
    if (_state.mode == EGizmoMode::Translate) {
        // 各平面 (XY / XZ / YZ) を中央の小正方形として描画。サイズは
        // m_PlaneHandleSize の半サイズ。
        const f32 h = m_PlaneHandleSize;
        // XY 平面: basis.x / basis.y で形成。
        {
            const acs::FVec3 p00 = position;
            const acs::FVec3 p10 = position + basis.x * h;
            const acs::FVec3 p11 = position + basis.x * h + basis.y * h;
            const acs::FVec3 p01 = position + basis.y * h;
            const acs::FVec4 col = axis_color(EGizmoAxis::XY, kColPlane);
            dd.DrawLine(to2(p00), to2(p10), col);
            dd.DrawLine(to2(p10), to2(p11), col);
            dd.DrawLine(to2(p11), to2(p01), col);
            dd.DrawLine(to2(p01), to2(p00), col);
        }
        // XZ 平面
        {
            const acs::FVec3 p00 = position;
            const acs::FVec3 p10 = position + basis.x * h;
            const acs::FVec3 p11 = position + basis.x * h + basis.z * h;
            const acs::FVec3 p01 = position + basis.z * h;
            const acs::FVec4 col = axis_color(EGizmoAxis::XZ, kColPlane);
            dd.DrawLine(to2(p00), to2(p10), col);
            dd.DrawLine(to2(p10), to2(p11), col);
            dd.DrawLine(to2(p11), to2(p01), col);
            dd.DrawLine(to2(p01), to2(p00), col);
        }
        // YZ 平面
        {
            const acs::FVec3 p00 = position;
            const acs::FVec3 p10 = position + basis.y * h;
            const acs::FVec3 p11 = position + basis.y * h + basis.z * h;
            const acs::FVec3 p01 = position + basis.z * h;
            const acs::FVec4 col = axis_color(EGizmoAxis::YZ, kColPlane);
            dd.DrawLine(to2(p00), to2(p10), col);
            dd.DrawLine(to2(p10), to2(p11), col);
            dd.DrawLine(to2(p11), to2(p01), col);
            dd.DrawLine(to2(p01), to2(p00), col);
        }
    }

    // 回転モード: 各軸まわりの円リング。
    if (_state.mode == EGizmoMode::Rotate) {
        // Z 軸まわり (= XY 平面の円) のみ正しい円として 2D 描画可能。
        // X/Y 軸まわりは 2D 射影では楕円 → 線になるため、近似で短い line を出す。
        const f32 r = m_AxisLength;

        // Z 軸まわりの円 (XY 平面)
        {
            acs::Circle c{};
            c.center = to2(position);
            c.radius = r;
            dd.DrawCircle(c, axis_color(EGizmoAxis::Z, kColZ), 36u);
        }
        // X 軸まわりの "axis 表示" line (= 2D では円が見えないので直線で代用)
        {
            const acs::FVec3 ex = position + basis.x * r;
            dd.DrawLine(to2(position), to2(ex), axis_color(EGizmoAxis::X, kColX));
        }
        // Y 軸まわりの "axis 表示" line
        {
            const acs::FVec3 ey = position + basis.y * r;
            dd.DrawLine(to2(position), to2(ey), axis_color(EGizmoAxis::Y, kColY));
        }
    }

    // スケールモード: 軸端のキューブを + マーカーで代用。
    if (_state.mode == EGizmoMode::Scale) {
        // 既に軸 line は描画済み。先端に "box っぽい" + マーカーを足す。
        const f32 m = m_HandleRadius * 3.0f;
        dd.DrawCross(to2(position + basis.x * m_AxisLength), m, axis_color(EGizmoAxis::X, kColX));
        dd.DrawCross(to2(position + basis.y * m_AxisLength), m, axis_color(EGizmoAxis::Y, kColY));
        dd.DrawCross(to2(position + basis.z * m_AxisLength), m, axis_color(EGizmoAxis::Z, kColZ));
    }
}

/** 現マウス ray がどの軸/平面にホバーしているか判定する (平面ハンドル優先)。 */
EGizmoAxis FEditorGizmo::PickAxis(acs::FVec3 ray_origin,
                                   acs::FVec3 ray_direction) const noexcept {
    if (_state.mode == EGizmoMode::None) {
        return EGizmoAxis::None_;
    }

    const AxisBasis basis = MakeBasis(_state.space, m_DragOriginRot);
    // 位置中心 (drag 中の origin、または 直近 Manipulate に渡された値) は本関数では
    // 不明なので、相対計算用に world 原点扱い。実用上 PickAxis は ProcessInput の
    // 直前に Manipulate でセットされる「対象 transform の position」が必要だが、
    // 本クラスは ProcessInput → Manipulate → DrawGizmo の 3 段構成で「ProcessInput
    // 時点で対象 position を知らない」。妥協として、PickAxis は
    // ray_origin から見て m_DragOriginPos (= 直近 Manipulate でセットされた値) を
    // gizmo 中心と仮定する。drag 開始前は (0,0,0) を中心と仮定するため、editor
    // 統合層が「ProcessInput 直前に SetCenter(node.WorldPosition()) を呼ぶ」運用が
    // 望ましいが、現状は API 簡略化のため省略。
    const acs::FVec3 center = m_DragOriginPos;

    EGizmoAxis best        = EGizmoAxis::None_;
    f32        best_metric = 1e30f;  // 軸: dist_sq、平面: t (近い方が勝ち)

    // 平面ハンドル先判定 (translate モードのみ)。
    if (_state.mode == EGizmoMode::Translate) {
        const EGizmoAxis planes[3] = { EGizmoAxis::XY, EGizmoAxis::XZ, EGizmoAxis::YZ };
        for (u32 i = 0; i < 3u; ++i) {
            const PlaneFrame pf = MakePlaneFrame(basis, planes[i]);
            f32 t = 0.0f;
            if (!RayPlaneIntersect(ray_origin, ray_direction, center, pf.n, t)) {
                continue;
            }
            const acs::FVec3 hit = ray_origin + ray_direction * t;
            const acs::FVec3 rel = hit - center;
            const f32 u = Dot3(rel, pf.u);
            const f32 v = Dot3(rel, pf.v);
            const f32 h = m_PlaneHandleSize;
            if (u >= 0.0f && u <= h && v >= 0.0f && v <= h) {
                if (t < best_metric) {
                    best_metric = t;
                    best        = planes[i];
                }
            }
        }
        // 平面が hit していたら確定 (= 軸より優先)。
        if (best != EGizmoAxis::None_) {
            return best;
        }
    }

    // 軸ハンドル判定 (ray-line 最近接 + 沿軸 t 範囲)。
    // scale モードでも軸ハンドル (X/Y/Z) は有効。
    // rotate モードでも X/Y/Z は有効 (= 各軸まわりの回転)。
    const EGizmoAxis axes[3]      = { EGizmoAxis::X,  EGizmoAxis::Y,  EGizmoAxis::Z  };
    const acs::FVec3  axis_vecs[3] = { basis.x,        basis.y,        basis.z       };
    const f32 r_sq = m_HandleRadius * m_HandleRadius;
    f32 best_dist_sq = 1e30f;
    EGizmoAxis best_axis = EGizmoAxis::None_;
    for (u32 i = 0; i < 3u; ++i) {
        const ClosestRayLine cr = ClosestPointsRayLine(ray_origin, ray_direction,
                                                       center, axis_vecs[i]);
        if (cr.parallel) {
            continue;
        }
        // 沿軸 t (= line_s) が [0, axis_length] の範囲内であること。
        if (cr.line_s < 0.0f || cr.line_s > m_AxisLength) {
            continue;
        }
        if (cr.ray_t < 0.0f) {
            continue;  // ray の後方
        }
        if (cr.dist_sq <= r_sq && cr.dist_sq < best_dist_sq) {
            best_dist_sq = cr.dist_sq;
            best_axis    = axes[i];
        }
    }
    return best_axis;
}

/** 現マウス ray と hot 軸/平面の world space 交点を算出する (軸は最近接点、平面は交点)。 */
bool FEditorGizmo::RaycastToHot(acs::FVec3 ray_origin,
                                acs::FVec3 ray_direction,
                                acs::FVec3& out_hit) const noexcept {
    if (_state.hot_axis == EGizmoAxis::None_) {
        return false;
    }
    const AxisBasis basis  = MakeBasis(_state.space, m_DragOriginRot);
    const acs::FVec3 center = m_DragOriginPos;

    switch (_state.hot_axis) {
        case EGizmoAxis::X:
        case EGizmoAxis::Y:
        case EGizmoAxis::Z: {
            const acs::FVec3 axis = PickAxisVec(basis, _state.hot_axis);
            const ClosestRayLine cr = ClosestPointsRayLine(ray_origin, ray_direction,
                                                           center, axis);
            if (cr.parallel) {
                return false;
            }
            // hit = line 上の最近接点 (= center + axis * line_s)。
            out_hit = center + axis * cr.line_s;
            return true;
        }
        case EGizmoAxis::XY:
        case EGizmoAxis::XZ:
        case EGizmoAxis::YZ: {
            const PlaneFrame pf = MakePlaneFrame(basis, _state.hot_axis);
            f32 t = 0.0f;
            if (!RayPlaneIntersect(ray_origin, ray_direction, center, pf.n, t)) {
                return false;
            }
            out_hit = ray_origin + ray_direction * t;
            return true;
        }
        case EGizmoAxis::ScreenAlign:
            // 未実装 (rotate モードの screen-aligned trackball)。
            return false;
        case EGizmoAxis::None_:
        default:
            return false;
    }
}

/** スカラー値を snap step で四捨五入量子化する (step<=0 はそのまま)。 */
f32 FEditorGizmo::ApplySnap(f32 value, f32 step) noexcept {
    if (step <= 0.0f) {
        return value;
    }
    // 四捨五入で量子化 (符号保持)。
    return acs::Round(value / step) * step;
}

/** FVec3 の各成分に独立して snap step を適用する。 */
acs::FVec3 FEditorGizmo::ApplySnap(acs::FVec3 v, f32 step) noexcept {
    return acs::FVec3{ApplySnap(v.x, step), ApplySnap(v.y, step), ApplySnap(v.z, step)};
}

/** drag 終了時に ManipulateCallback へ mode と代表 delta (Zero) を通知する。 */
void FEditorGizmo::FireDragEnd() noexcept {
    if (m_Cb == nullptr) {
        return;
    }
    // 代表 delta は Zero。editor は「drag 終了通知」だけを受け取り、
    // 自前で node の現 transform と m_DragOrigin* を比較する想定。
    m_Cb(m_CbUser, _state.mode, acs::FVec3{});
}

} // namespace acs::game::editor_core
