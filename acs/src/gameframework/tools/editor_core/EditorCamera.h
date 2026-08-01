// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — CEditorCamera (editor_core 共通基盤)
//
// ModelViewer (3D) / LevelEditor (2D top-down) / TilemapEditor (2D) / 各種
// viewport editor が共有する **camera コントローラ**。1 個のクラスで 2D と 3D
// 双方の操作モデルをハンドルし、editor 側はマウス入力をそのまま流し込むだけで
// 3D orbit / 2D pan-zoom が成立するようにする。
//
// 使い方:
//   acs::game::editor_core::CEditorCamera cam;
//   cam.Init(EEditorCameraMode::Mode3D);
//
//   // 毎フレームの editor tick:
//   cam.HandleMouseInput(mouse_delta, lmb, rmb, mmb, wheel);
//   cam.Tick(dt);
//   FMat4 view = cam.ViewMatrix();
//   FMat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 1000.0f);
//
//   // 選択へ寄せる (将来 CSelectionService + bounds 計算経由):
//   cam.FrameToBoundingSphere(node_center, node_radius);
//
// 設計選択 (editor_core):
//   ・**2 モード 1 クラス**: 2D / 3D を別 class にすると panel 側で switch
//     文が増え、共通の "Frame to selection" や "Tick smoothing" を 2 回
//     書くハメになる。1 クラス内 mode フラグで管理し、API は両モード共通形に
//     揃える (Pan / Dolly は意味を mode で読み替え)。
//   ・**3D orbit = yaw + pitch + distance + target**: industry-standard な
//     Maya / Blender 風の orbit。eye 位置は (target + spherical(yaw,pitch)
//     * distance) で算出。pitch は ±89° にクランプして極点で gimbal flip
//     しないようにする。
//   ・**2D = position + zoom_2d**: FCamera2D と同じ pan/zoom モデル。orthographic
//     投影で width = base_ortho_size / zoom_2d とすれば「ズームイン = 拡大」
//     が直感通り。FCamera2D の rotation までは持たない (editor は通常 axis-aligned)。
//   ・**HandleMouseInput で操作系を集約**: panel 側は ImGui の IO から取った
//     delta / button / wheel をそのまま渡せばよい。マウス操作の規約:
//       - 3D: LMB drag = orbit, MMB drag = pan, RMB drag = orbit (Maya 風代替),
//             wheel = dolly
//       - 2D: LMB drag = pan, MMB drag = pan, wheel = zoom
//     この規約は editor 全体で統一すべきもの。
//   ・**Tick で smoothing**: FCamera2D と同じ `1 - exp(-rate*dt)` の framerate
//     independent な指数補間で「狙った target / zoom」へ追従させる。マウス
//     入力は raw target を直接書き換え、Tick が actual state を寄せる構造。
//     rate = 0 で即時、5.0 で約 0.2s で 63% 詰める典型。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用**: ACS 規約。内部 state
//     は POD のみ。Init / Reset で完全に初期化可能。
//
// 将来拡張余地:
//   ・preset (Top/Front/Side/3-quarter 等の標準視点を 1 ボタン)
//   ・camera lock (Y 軸固定で yaw のみ自由 = ARPG style)
//   ・focus to selection (現選択 FNodeId にカメラを向ける、CSelectionService 経由)
//   ・camera shake disable (FPhotoMode 同様、editor では shake 抑止)
//   ・rotation 2D (FCamera2D と等価の axis 回転)
//   ・FPS フリールック (orbit ではなく eye 中心の look-around)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"

namespace acs::game::editor_core {

/**
 * editor カメラの操作モード (2D pan-zoom か 3D orbit か)。
 */
enum class EEditorCameraMode : u8 {
    /** pan / zoom のみの 2D 操作 (top-down editor / tilemap editor 等)。 */
    Mode2D = 0,

    /** target を中心に orbit + pan + dolly する 3D 操作 (model viewer 等)。 */
    Mode3D = 1,
};

/**
 * CEditorCamera の内部 state (公開のため struct で外出し)。
 *
 * @details
 * editor 側で「現在の position / target」等を直接覗きたいユースケース (debug overlay /
 * serializer) のため可視 struct で公開し、State() が const 参照を返す。直接書き換える場合は
 * accessor (SetTarget 等) 経由を推奨 — そうしないと smooth_target との 2 重管理が崩れる。
 */
struct FEditorCameraState {
    /** 実 eye position (3D)。2D では (x,y) を使用する。 */
    FVec3 position{0.0f, 0.0f, 0.0f};

    /** orbit center (3D)。2D では未使用。 */
    FVec3 target  {0.0f, 0.0f, 0.0f};

    /** up vector (LookAt 用)。 */
    FVec3 up      {0.0f, 1.0f, 0.0f};

    /** 3D perspective の垂直 FoV (degree)。 */
    f32  fov_deg = 60.0f;

    /** 2D ortho zoom (1.0 で基本、>1 で拡大)。 */
    f32  zoom_2d = 1.0f;

    /** 3D orbit の Y 軸まわり角 (0 = +Z 方向から見る)。 */
    f32  yaw_rad   = 0.0f;

    /** 3D orbit の仰角 (水平面から、- で見下ろし)。 */
    f32  pitch_rad = 0.0f;

    /** 3D orbit の target からの距離。 */
    f32  distance  = 10.0f;
};

/**
 * 2D pan-zoom と 3D orbit を 1 クラスで扱う editor 共通のカメラコントローラ。
 *
 * @details
 * ModelViewer (3D) / LevelEditor (2D) / TilemapEditor 等が共有する。editor 側はマウス入力を
 * HandleMouseInput にそのまま流し込むだけで、3D orbit (yaw + pitch + distance + target) /
 * 2D pan-zoom (position + zoom_2d) が成立する。Tick が `1 - exp(-rate*dt)` の framerate
 * independent な指数補間で raw target / zoom へ追従させる。全 noexcept / 非コピー / 非ムーブ /
 * STL 不使用で、内部 state は POD のみ。
 */
class CEditorCamera {
public:
    /** mode 未確定のまま構築する (Init で初期化する)。 */
    CEditorCamera() noexcept = default;

    /** デストラクタ (state は POD のみで解放処理なし)。 */
    ~CEditorCamera() noexcept = default;

    /** コピー禁止 (内部 state の所有を曖昧にしないため)。 */
    CEditorCamera(const CEditorCamera&)            = delete;

    /** コピー代入も禁止。 */
    CEditorCamera& operator=(const CEditorCamera&) = delete;

    /** ムーブ禁止。 */
    CEditorCamera(CEditorCamera&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEditorCamera& operator=(CEditorCamera&&)      = delete;

    /**
     * 指定モードで完全初期化する (Reset 相当 + mode 設定)。
     *
     * @details smoothing と base ortho size も既定値に戻す。多重呼び出し可。
     * @param mode 初期化するカメラモード。
     */
    void Init(EEditorCameraMode mode) noexcept;

    /**
     * mode を切り替える (state は保持)。
     *
     * @details 3D ↔ 2D 間で position / zoom はそれぞれ独立に保たれる。同一 mode 指定は no-op。
     * @param mode 切り替え先のカメラモード。
     */
    void SetMode(EEditorCameraMode mode) noexcept;

    /**
     * 現在のカメラモードを返す。
     *
     * @return 設定済みの EEditorCameraMode。
     */
    EEditorCameraMode Mode() const noexcept { return m_Mode; }

    /**
     * editor 側のマウス IO をそのまま流し込む入力エントリポイント。
     *
     * @details
     * 3D は LMB/RMB drag=orbit、MMB drag=pan、wheel=dolly。2D は LMB/MMB drag=pan、wheel=zoom。
     * @param mouse_delta フレーム間のマウス移動 (screen pixel)。
     * @param lmb 左ボタン押下中フラグ。
     * @param rmb 右ボタン押下中フラグ。
     * @param mmb 中ボタン押下中フラグ。
     * @param wheel_delta ホイール量 (>0 で前進)。
     */
    void HandleMouseInput(FVec2 mouse_delta,
                          bool lmb, bool rmb, bool mmb,
                          f32 wheel_delta) noexcept;

    /**
     * カメラを平行移動する (mode で意味が変わる)。
     *
     * @details
     * 2D は screen delta 分だけ world position を平行移動。3D は orbit target を camera
     * right / up 方向に平行移動する (screen 上の見た目 delta と一致するよう distance スケール)。
     * @param screen_delta screen 上のドラッグ量 (pixel)。
     */
    void Pan(FVec2 screen_delta) noexcept;

    /**
     * 3D orbit の yaw / pitch を加算する (2D では no-op)。
     *
     * @details pitch は ±89° にクランプして極点 gimbal flip を防止する。
     * @param yaw_delta yaw への加算量 (rad)。
     * @param pitch_delta pitch への加算量 (rad)。
     */
    void Orbit(f32 yaw_delta, f32 pitch_delta) noexcept;

    /**
     * ズーム / ドリーする (mode で意味が変わる)。
     *
     * @details 3D は distance を倍率変化 (delta>0 で寄る = distance 縮)、2D は zoom_2d を倍率変化 (delta>0 で拡大)。
     * @param delta ホイール等のズーム量 (>0 で寄る / 拡大)。
     */
    void Dolly(f32 delta) noexcept;

    /**
     * カメラを初期状態に戻す。
     *
     * @details 3D は target=origin / distance=10 / yaw=0 / pitch=-30°、2D は position=origin / zoom_2d=1.0。
     */
    void Reset() noexcept;

    /**
     * bounding sphere が画面に収まるよう target / distance を配置する。
     *
     * @details
     * 3D は target=center、distance=radius/sin(fov/2) に margin 1.2x を載せる。2D は直径が
     * ortho 全幅に収まる zoom を計算 (z は無視)。
     * @param center bounding sphere の中心。
     * @param radius bounding sphere の半径 (kEpsilon 未満は kEpsilon に補正)。
     */
    void FrameToBoundingSphere(FVec3 center, f32 radius) noexcept;

    /**
     * axis-aligned bounding box (XY 平面) 全体が見えるよう配置する。
     *
     * @details 2D は box の長辺基準で zoom / position を計算。3D は外接 sphere とみなして FrameToBoundingSphere へ再委譲する。
     * @param min_xy box の最小 (x,y)。
     * @param max_xy box の最大 (x,y)。
     */
    void FrameToBoundingBox2D(FVec2 min_xy, FVec2 max_xy) noexcept;

    /**
     * LookAt ベースの view 行列 (LH) を生成する。
     *
     * @details 3D は (eye, target, up)、2D は (eye=(pos.x,pos.y,-1), target=(pos.x,pos.y,0), up=+Y)。
     * @return mode に応じた view 行列。
     */
    FMat4 ViewMatrix() const noexcept;

    /**
     * mode に応じて投影行列を生成する。
     *
     * @details 3D は perspective、2D は orthographic (幅 = base_ortho_size / zoom_2d、高さは aspect から導出)。
     * @param aspect ビューポートのアスペクト比 (kEpsilon 未満は 1.0 に補正)。
     * @param near_plane near クリップ面。
     * @param far_plane far クリップ面。
     * @return mode に応じた投影行列。
     */
    FMat4 ProjectionMatrix(f32 aspect, f32 near_plane, f32 far_plane) const noexcept;

    /**
     * 現在の内部 state への const 参照を返す。
     *
     * @return 補間後の実値 state (debug overlay / serializer 用)。
     */
    const FEditorCameraState& State() const noexcept { return _state; }

    /**
     * 3D perspective の FoV を設定する。
     *
     * @details [10, 170] degree にクランプする (smooth_target にも反映 = 即追従)。
     * @param deg 設定する垂直 FoV (degree)。
     */
    void SetFovDeg(f32 deg) noexcept;

    /**
     * 現在の FoV を返す。
     *
     * @return 設定済みの垂直 FoV (degree)。
     */
    f32  GetFovDeg() const noexcept { return _state.fov_deg; }

    /**
     * カメラの注視 target を設定する。
     *
     * @details 3D は orbit target を直接設定 (smooth_target にも反映 = 即追従)。2D は target.xy を position(x,y) としても同期する。
     * @param target 設定する target 座標。
     */
    void SetTarget(FVec3 target) noexcept;

    /**
     * 現在の target を返す。
     *
     * @return 設定済みの target 座標。
     */
    FVec3 GetTarget() const noexcept { return _state.target; }

    /**
     * eye position を直接設定する。
     *
     * @details 3D は target + spherical 計算で distance / yaw / pitch を逆算する。2D は position の (x,y) を書き換える。
     * @param position 設定する eye position。
     */
    void SetPosition(FVec3 position) noexcept;

    /**
     * 現在の eye position を返す。
     *
     * @return 設定済みの eye position。
     */
    FVec3 GetPosition() const noexcept { return _state.position; }

    /**
     * smooth target / zoom 補間を 1 ステップ進める (editor 側で毎フレーム呼ぶ)。
     *
     * @details FCamera2D と同じ framerate-independent な `1 - exp(-rate*dt)` モデルで raw target へ追従させる。
     * @param dt 前フレームからの経過秒 (負値は 0 に補正)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * smoothing rate を設定する。
     *
     * @details 0 = 即時スナップ、5.0 = 既定 (FCamera2D と同方式)。負値は 0 に補正。
     * @param rate smoothing rate。
     */
    void SetSmoothing(f32 rate) noexcept;

    /**
     * 現在の smoothing rate を返す。
     *
     * @return 設定済みの smoothing rate。
     */
    f32  GetSmoothing() const noexcept { return m_Smoothing; }

    /**
     * 基本 ortho サイズ (2D の zoom_2d=1 時の表示幅 world units) を設定する。
     *
     * @details 既定 20.0。project の単位系に合わせて調整できるよう公開。0.01 未満は 0.01 に補正。
     * @param world_width zoom_2d=1 時に画面へ映る world 幅。
     */
    void SetBaseOrthoSize(f32 world_width) noexcept;

    /**
     * 現在の基本 ortho サイズを返す。
     *
     * @return 設定済みの基本 ortho サイズ (world units)。
     */
    f32  GetBaseOrthoSize() const noexcept { return m_BaseOrthoSize; }

    /**
     * 3D orbit の現 eye 位置を spherical 計算で算出する (target + dir*distance)。
     *
     * @details ViewMatrix の内部実装と等価。debug overlay や ray pick 用に公開する。
     * @return 算出した eye position。
     */
    FVec3 ComputeEye() const noexcept;

private:
    /** 3D pitch をクランプする上限 (rad、89° = gimbal flip 防止の典型値)。 */
    static constexpr f32 kPitchLimit = 1.55334303f;

    /**
     * 3D orbit の方向ベクトルを yaw / pitch から計算する。
     *
     * @details Y-up、+Z を yaw=0 とする LH 系。(eye - target) の正規化ベクトルと等価。
     * @return orbit 方向 (正規化済み)。
     */
    FVec3 OrbitDirection() const noexcept;

    /** 公開 state (補間後の実値)。 */
    FEditorCameraState _state{};

    /** 補間先 state (Tick が _state を寄せる)。 */
    FEditorCameraState m_SmoothTarget{};

    /** 現在のカメラモード。 */
    EEditorCameraMode m_Mode             = EEditorCameraMode::Mode3D;

    /** smoothing rate (0 = 即時、5.0 = 既定)。 */
    f32               m_Smoothing        = 5.0f;

    /** 2D の zoom_2d=1 時の表示幅 (world units)。 */
    f32               m_BaseOrthoSize  = 20.0f;
};

using FEditorCamera = CEditorCamera;

} // namespace acs::game::editor_core
