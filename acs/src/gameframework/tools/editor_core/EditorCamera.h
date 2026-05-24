// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — EditorCamera (Phase 21a editor_core 共通基盤)
//
// ModelViewer (3D) / LevelEditor (2D top-down) / TilemapEditor (2D) / 各種
// viewport editor が共有する **camera コントローラ**。1 個のクラスで 2D と 3D
// 双方の操作モデルをハンドルし、editor 側はマウス入力をそのまま流し込むだけで
// 3D orbit / 2D pan-zoom が成立するようにする。
//
// 使い方:
//   acs::game::editor_core::EditorCamera cam;
//   cam.Init(EEditorCameraMode::Mode3D);
//
//   // 毎フレームの editor tick:
//   cam.HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel);
//   cam.Tick(dt);
//   Mat4 view = cam.ViewMatrix();
//   Mat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 1000.0f);
//
//   // 選択へ寄せる (将来 SelectionService + bounds 計算経由):
//   cam.FrameToBoundingSphere(node_center, node_radius);
//
// 設計選択 (Phase 21a = editor_core Phase 1):
//   ・**2 モード 1 クラス**: 2D / 3D を別 class にすると panel 側で switch
//     文が増え、共通の "Frame to selection" や "Tick smoothing" を 2 回
//     書くハメになる。1 クラス内 mode フラグで管理し、API は両モード共通形に
//     揃える (Pan / Dolly は意味を mode で読み替え)。
//   ・**3D orbit = yaw + pitch + distance + target**: industry-standard な
//     Maya / Blender 風の orbit。eye 位置は (target + spherical(yaw,pitch)
//     * distance) で算出。pitch は ±89° にクランプして極点で gimbal flip
//     しないようにする。
//   ・**2D = position + zoom_2d**: Camera2D と同じ pan/zoom モデル。orthographic
//     投影で width = base_ortho_size / zoom_2d とすれば「ズームイン = 拡大」
//     が直感通り。Camera2D の rotation までは持たない (editor は通常 axis-aligned)。
//   ・**HandleMouseInput で操作系を集約**: panel 側は ImGui の IO から取った
//     delta / button / wheel をそのまま渡せばよい。マウス操作の規約:
//       - 3D: LMB drag = orbit, MMB drag = pan, RMB drag = orbit (Maya 風代替),
//             wheel = dolly
//       - 2D: LMB drag = pan, MMB drag = pan, wheel = zoom
//     この規約は editor 全体で統一すべきもの。
//   ・**Tick で smoothing**: Camera2D と同じ `1 - exp(-rate*dt)` の framerate
//     independent な指数補間で「狙った target / zoom」へ追従させる。マウス
//     入力は raw target を直接書き換え、Tick が actual state を寄せる構造。
//     rate = 0 で即時、5.0 で約 0.2s で 63% 詰める典型。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用**: ACS 規約。内部 state
//     は POD のみ。Init / Reset で完全に初期化可能。
//
// 将来拡張余地 (Phase 21a の範囲外、ヘッダ末尾の TODO 参照):
//   ・preset (Top/Front/Side/3-quarter 等の標準視点を 1 ボタン)
//   ・camera lock (Y 軸固定で yaw のみ自由 = ARPG style)
//   ・focus to selection (現選択 NodeId にカメラを向ける、SelectionService 経由)
//   ・camera shake disable (PhotoMode 同様、editor では shake 抑止)
//   ・rotation 2D (Camera2D と等価の axis 回転)
//   ・FPS フリールック (orbit ではなく eye 中心の look-around)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"

namespace acs::game::editor_core {

// ---- カメラモード -----------------------------------------------------------
// `Mode2D`: pan / zoom のみの 2D 操作 (top-down editor / tilemap editor 等)。
// `Mode3D`: target を中心に orbit + pan + dolly する 3D 操作 (model viewer 等)。
enum class EEditorCameraMode : u8 {
    Mode2D = 0,
    Mode3D = 1,
};

// ---- 内部 state (公開のため struct で外出し) --------------------------------
// editor 側で「現在の position / target」等を直接覗きたいユースケース
// (debug overlay / serializer) のため、可視 struct で公開する。`State()`
// で const 参照を返す。直接書き換える場合は accessor (`SetTarget` 等) 経由を
// 推奨 — そうしないと smooth_target との 2 重管理が崩れる。
struct EditorCameraState {
    // 3D 用: orbit center / eye 算出に使用
    Vec3 position{0.0f, 0.0f, 0.0f};      // 実 eye position (3D); 2D では (x,y) を使用
    Vec3 target  {0.0f, 0.0f, 0.0f};      // orbit center (3D); 2D では未使用
    Vec3 up      {0.0f, 1.0f, 0.0f};      // up vector (LookAt 用)

    // 投影系
    f32  fov_deg = 60.0f;                  // 3D perspective FoV (vertical, degree)
    f32  zoom_2d = 1.0f;                   // 2D ortho zoom (1.0 で基本、>1 で拡大)

    // 3D orbit パラメータ (eye を target からの球面座標で表現)
    f32  yaw_rad   = 0.0f;                 // Y 軸まわり (0 = +Z 方向から見る)
    f32  pitch_rad = 0.0f;                 // 水平面からの仰角 (- で見下ろし)
    f32  distance  = 10.0f;                // target からの距離
};

// ---- EditorCamera -----------------------------------------------------------
class EditorCamera {
public:
    EditorCamera() noexcept = default;
    ~EditorCamera() noexcept = default;

    // 非コピー / 非ムーブ (内部 state の所有を曖昧にしない)
    EditorCamera(const EditorCamera&)            = delete;
    EditorCamera& operator=(const EditorCamera&) = delete;
    EditorCamera(EditorCamera&&)                 = delete;
    EditorCamera& operator=(EditorCamera&&)      = delete;

    // ----- 初期化 / モード -----------------------------------------------
    // 指定モードで完全初期化 (Reset 相当 + mode 設定)。多重呼び出し可。
    void Init(EEditorCameraMode mode) noexcept;

    // mode を切り替える。state は保持 (3D ↔ 2D 間で position/zoom は
    // それぞれ独立に保たれる)。
    void SetMode(EEditorCameraMode mode) noexcept;
    EEditorCameraMode Mode() const noexcept { return _mode; }

    // ----- 入力 ----------------------------------------------------------
    // editor 側がマウス IO をそのまま流し込むエントリポイント。
    // `mouse_delta`: フレーム間のマウス移動 (screen pixel)
    // `lmb / rmb / mmb`: ボタン押下中フラグ (down state)
    // `wheel_delta`: ホイール量 (>0 で前進)
    // 3D: LMB or RMB drag = orbit / MMB drag = pan / wheel = dolly
    // 2D: LMB or MMB drag = pan / wheel = zoom
    void HandleMouseInput(Vec2 mouse_delta,
                          bool lmb, bool rmb, bool mmb,
                          f32 wheel_delta) noexcept;

    // 2D: screen 上での delta だけ world position を平行移動
    // 3D: orbit target を camera right / up 方向に平行移動
    //     (screen 上の見た目 delta と一致するよう distance / zoom スケール)
    void Pan(Vec2 screen_delta) noexcept;

    // 3D 専用: yaw / pitch を加算 (2D では no-op)
    // pitch は ±89° にクランプして極点 gimbal flip を防止。
    void Orbit(f32 yaw_delta, f32 pitch_delta) noexcept;

    // 3D: distance を倍率変化 (delta > 0 で寄る = distance 縮)
    // 2D: zoom_2d を倍率変化 (delta > 0 で拡大 = zoom_2d 増)
    void Dolly(f32 delta) noexcept;

    // ----- リセット / フレーミング ---------------------------------------
    // 初期状態に戻す:
    //   3D: target=origin, distance=10, yaw=0, pitch=-30°
    //   2D: position=origin, zoom_2d=1.0
    void Reset() noexcept;

    // 3D: bounding sphere が画面に収まるよう target を center / distance を
    //     radius / sin(fov/2) で配置 (margin 1.2x)
    // 2D: sphere → ortho 全幅 fit (z は無視)
    void FrameToBoundingSphere(Vec3 center, f32 radius) noexcept;

    // 2D 専用: axis-aligned bounding box (XY 平面) 全体が見える zoom / pos
    // を計算 (3D では Y を 0 として近似的に Frame、distance は radius 経由)。
    void FrameToBoundingBox2D(Vec2 min_xy, Vec2 max_xy) noexcept;

    // ----- 行列 ---------------------------------------------------------
    // LookAt ベースの view 行列 (LH)。3D は (eye, target, up)、2D は
    // (eye=(pos.x, pos.y, -1), target=(pos.x, pos.y, 0), up=+Y)。
    Mat4 ViewMatrix() const noexcept;

    // perspective (3D) / orthographic (2D) を mode に応じて生成。
    // 2D の ortho 幅は `base_ortho_size / zoom_2d`、高さは aspect から導出。
    Mat4 ProjectionMatrix(f32 aspect, f32 near_plane, f32 far_plane) const noexcept;

    // ----- state アクセサ -----------------------------------------------
    const EditorCameraState& State() const noexcept { return _state; }

    void SetFovDeg(f32 deg) noexcept;
    f32  GetFovDeg() const noexcept { return _state.fov_deg; }

    // 3D: orbit target を直接設定 (smooth_target にも反映 = 即追従)
    // 2D: position の (x,y) を target.xy として扱い、(pos, smooth) 共に更新
    void SetTarget(Vec3 target) noexcept;
    Vec3 GetTarget() const noexcept { return _state.target; }

    // eye position を直接設定 (3D は target + spherical 計算で逆算 = distance
    // / yaw / pitch も更新)。2D は単に position の (x,y) を書き換える。
    void SetPosition(Vec3 position) noexcept;
    Vec3 GetPosition() const noexcept { return _state.position; }

    // ----- driver --------------------------------------------------------
    // smooth target follow + smooth zoom 補間 (Camera2D と同じ
    // framerate-independent な `1 - exp(-rate*dt)` モデル)。
    // editor 側で毎フレーム呼ぶ。
    void Tick(f32 dt) noexcept;

    // smoothing rate を設定 (0 = 即時 / 5.0 = デフォルト、Camera2D と同方式)。
    void SetSmoothing(f32 rate) noexcept;
    f32  GetSmoothing() const noexcept { return _smoothing; }

    // ----- 基本 ortho サイズ (2D の zoom_2d=1 時の表示幅 world units) ----
    // 既定 20.0 (= 画面に 20 単位幅が映る)。editor 起動時に project の単位
    // 系に合わせて調整できるよう公開。
    void SetBaseOrthoSize(f32 world_width) noexcept;
    f32  GetBaseOrthoSize() const noexcept { return _base_ortho_size; }

    // 3D orbit の現 eye 位置を spherical 計算で算出 (target + dir*distance)。
    // ViewMatrix の内部実装と等価。debug overlay や ray pick 用に公開。
    Vec3 ComputeEye() const noexcept;

private:
    // 3D pitch をクランプする上限 (rad)。±89° = gimbal flip 防止の典型値。
    static constexpr f32 kPitchLimit = 1.55334303f;  // 89° in rad

    // 3D orbit の dir を yaw / pitch から計算 (Y-up, +Z を yaw=0 とする LH)。
    // (eye - target) の正規化ベクトルと等価。
    Vec3 OrbitDirection() const noexcept;

    EditorCameraState _state{};                           // 公開 state (実値)
    EditorCameraState _smooth_target{};                   // 補間先 (Tick が _state を寄せる)
    EEditorCameraMode _mode             = EEditorCameraMode::Mode3D;
    f32               _smoothing        = 5.0f;            // 0 = 即時, 5.0 = 既定
    f32               _base_ortho_size  = 20.0f;           // 2D の zoom_2d=1 時の幅 (world)
};

} // namespace acs::game::editor_core
