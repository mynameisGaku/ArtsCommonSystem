// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FPhotoMode (Polish & GameFeel)
//
// プレイヤーが任意の瞬間で「ゲーム時間を停止して景色を撮影する」モード。
// 近年の AAA タイトル (Horizon / God of War / Spider-Man など) で
// 定着した機能で、Pillar R (Polish & GameFeel) の中核要素。
//
// 役割は **状態 holder** のみ:
//   ・active フラグ (止まっているか否か)
//   ・カメラの自由操作オフセット (panning / zoom / rotation)
//   ・色フィルタ種別 (None / Sepia / Grayscale / Vivid / Vignette)
//   ・撮影リクエスト flag (poll-and-consume パターン)
//   ・「Enter 時の time scale」保存値 (Exit 時に caller が復元する用)
//
// 設計選択:
//   ・**caller が時間操作を行う**: Enter() / Exit() は本クラスの状態を
//     変えるだけ。実際の `FGame::SetTimeScale(0)` 呼び出しは外側の
//     ゲームコードが SavedTimeScale() を見て行う。これにより
//     GameFramework モジュールから FGame への依存を切る (Pillar 規約)。
//   ・**capture flag は poll-and-consume**: 描画側が 1 フレームの末尾で
//     `ConsumeCaptureRequest()` を呼んで rear (= 落とす)。連打しても
//     1 フレームに 1 枚しか保存しない自然な仕様。
//   ・**zoom clamp [0.1, 10.0]**: 1/10 〜 10 倍。これより外は被写界深度や
//     LOD が破綻するので機械的に clamp。
//   ・**rotation は累積 radians**: 元のゲームカメラ rotation に **加算** する
//     用途。FCamera2D::Rotation() と直接競合しない設計。
//   ・**filter は enum のみ持つ**: 実 LUT / シェーダパラメータの解決は
//     ポストプロセスパスの責務。本クラスは「どのフィルタが選ばれているか」
//     だけを保持する。
//
// 非コピー・非ムーブ:
//   FPhotoMode は FGame / Scene のメンバとして 1 インスタンスだけ存在する
//   想定。複製可能にすると saved_time_scale 等の整合性管理が破綻する。
//
// 範囲外 (Phase 2+):
//   ・depth of field マニュアル制御
//   ・キャラポーズ・表情変更
//   ・hide HUD / sticker / frame overlay
//   ・複数フィルタの blend
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FPhotoMode {
public:
    // フィルタ種別。実 LUT 解決はポストプロセスパスが担当。
    enum FilterKind {
        None      = 0,   // フィルタなし (素通し)
        Sepia     = 1,   // セピア調 (warm tone + 彩度落とし)
        Grayscale = 2,   // モノクロ
        Vivid     = 3,   // 高彩度 / 高コントラスト
        Vignette  = 4,   // 周辺光量落とし
    };

    FPhotoMode() noexcept = default;
    ~FPhotoMode() noexcept = default;

    // 非コピー・非ムーブ (state holder の唯一性を担保)
    FPhotoMode(const FPhotoMode&)            = delete;
    FPhotoMode& operator=(const FPhotoMode&) = delete;
    FPhotoMode(FPhotoMode&&)                 = delete;
    FPhotoMode& operator=(FPhotoMode&&)      = delete;

    // ----- モード遷移 -----
    // 現在の time_scale を保存して active=true、カメラオフセットも初期化。
    // 実際に `FGame::SetTimeScale(0)` を呼ぶのは caller の責任。
    void Enter(f32 current_time_scale = 1.0f) noexcept;

    // active=false に戻す。caller が SavedTimeScale() を見て復元する。
    void Exit() noexcept;

    bool IsActive() const noexcept { return m_Active; }

    // ----- カメラ自由操作 (累積 offset) -----
    // パン: 入力 delta を累積。座標系は world units 想定。
    void MoveCamera(FVec2 delta) noexcept;

    // ズーム倍率の乗算的変更。zoom_delta=0.1 で +10% ズームイン。
    // 結果は [0.1, 10.0] に clamp される。
    void ZoomCamera(f32 zoom_delta) noexcept;

    // 回転を累積 (radians)。
    void RotateCamera(f32 rad_delta) noexcept;

    // ----- accessor -----
    FVec2 CameraOffset()    const noexcept { return m_Offset; }
    f32  ZoomMultiplier()  const noexcept { return m_ZoomMult; }
    f32  RotationOffset()  const noexcept { return m_Rot; }

    // ----- フィルタ -----
    void       SetFilter(FilterKind f) noexcept { m_Filter = f; }
    FilterKind GetFilter() const noexcept       { return m_Filter; }

    // ----- 撮影リクエスト (poll-and-consume) -----
    // ユーザがシャッターボタンを押した時に呼ぶ。flag が立つだけ。
    void RequestCapture() noexcept { m_CapturePending = true; }

    // 描画側が 1 フレーム末尾で呼ぶ。立っていれば true を返して同時に rear。
    bool ConsumeCaptureRequest() noexcept;

    // ----- time scale 連携 -----
    // Enter() 時に保存した値。caller が Exit() 後に
    // `FGame::SetTimeScale(photo.SavedTimeScale())` で復元する用。
    f32 SavedTimeScale() const noexcept { return m_SavedTimeScale; }

private:
    bool       m_Active            = false;
    FVec2       m_Offset            {0.0f, 0.0f};
    f32        m_ZoomMult         = 1.0f;
    f32        m_Rot               = 0.0f;
    FilterKind m_Filter            = None;
    bool       m_CapturePending   = false;
    f32        m_SavedTimeScale  = 1.0f;
};

} // namespace acs::game
