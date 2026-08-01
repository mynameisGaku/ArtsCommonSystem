// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CPhotoMode (Polish & GameFeel)
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
//     変えるだけ。実際の `CGame::SetTimeScale(0)` 呼び出しは外側の
//     ゲームコードが SavedTimeScale() を見て行う。これにより
//     GameFramework モジュールから CGame への依存を切る (Pillar 規約)。
//   ・**capture flag は poll-and-consume**: 描画側が 1 フレームの末尾で
//     `ConsumeCaptureRequest()` を呼んで rear (= 落とす)。連打しても
//     1 フレームに 1 枚しか保存しない自然な仕様。
//   ・**zoom clamp [0.1, 10.0]**: 1/10 〜 10 倍。これより外は被写界深度や
//     LOD が破綻するので機械的に clamp。
//   ・**rotation は累積 radians**: 元のゲームカメラ rotation に **加算** する
//     用途。CCamera2D::Rotation() と直接競合しない設計。
//   ・**filter は enum のみ持つ**: 実 LUT / シェーダパラメータの解決は
//     ポストプロセスパスの責務。本クラスは「どのフィルタが選ばれているか」
//     だけを保持する。
//
// 非コピー・非ムーブ:
//   CPhotoMode は CGame / AScene のメンバとして 1 インスタンスだけ存在する
//   想定。複製可能にすると saved_time_scale 等の整合性管理が破綻する。
//
// 範囲外:
//   ・depth of field マニュアル制御
//   ・キャラポーズ・表情変更
//   ・hide HUD / sticker / frame overlay
//   ・複数フィルタの blend
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * ゲーム時間を停止して景色を撮影する撮影モードの状態 holder。
 *
 * @details
 * active フラグ・カメラ自由操作オフセット (pan / zoom / rotation)・色フィルタ種別・
 * 撮影リクエスト flag・Enter 時の time scale 保存値だけを保持する。実際の時間停止や
 * カメラ移動・フィルタ適用は caller (CGame / AScene / ポストプロセスパス) が状態を見て
 * 行うため、GameFramework から CGame への依存を持たない。state holder の唯一性を保つ
 * ため非コピー・非ムーブ。
 */
class CPhotoMode {
public:
    /**
     * 色フィルタの種別。
     *
     * @details 実 LUT / シェーダパラメータの解決はポストプロセスパスが担当し、
     * 本クラスはどのフィルタが選ばれているかだけを保持する。
     */
    enum EFilterKind {
        /** フィルタなし (素通し)。 */
        None      = 0,

        /** セピア調 (warm tone + 彩度落とし)。 */
        Sepia     = 1,

        /** モノクロ。 */
        Grayscale = 2,

        /** 高彩度 / 高コントラスト。 */
        Vivid     = 3,

        /** 周辺光量落とし。 */
        Vignette  = 4,
    };

    /** 非アクティブ状態で構築する (フィルタ None、オフセット 0)。 */
    CPhotoMode() noexcept = default;

    /** デストラクタ。 */
    ~CPhotoMode() noexcept = default;

    /** コピー禁止 (state holder の唯一性を担保するため)。 */
    CPhotoMode(const CPhotoMode&)            = delete;

    /** コピー代入も禁止。 */
    CPhotoMode& operator=(const CPhotoMode&) = delete;

    /** ムーブ禁止 (state holder の唯一性を担保するため)。 */
    CPhotoMode(CPhotoMode&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPhotoMode& operator=(CPhotoMode&&)      = delete;

    /**
     * 撮影モードに入る。
     *
     * @details
     * 二重 Enter は no-op (保存値を上書きしない)。現在の time_scale を保存し、カメラ
     * オフセット・ズーム・回転・撮影 flag をリセットする。フィルタは前回値を保持する。
     * 実際に `CGame::SetTimeScale(0)` を呼ぶのは caller の責任。
     * @param current_time_scale Enter 時点の time scale (Exit 後の復元用に保存される)。
     */
    void Enter(f32 current_time_scale = 1.0f) noexcept;

    /**
     * 撮影モードを抜ける。
     *
     * @details active=false に戻し撮影 flag を落とすだけ。time scale の復元は
     * caller が SavedTimeScale() を見て行う。カメラオフセット等は保持される。
     */
    void Exit() noexcept;

    /**
     * 撮影モード中かを返す。
     *
     * @return active なら true。
     */
    bool IsActive() const noexcept { return m_Active; }

    /**
     * カメラのパン入力を累積する (active 中のみ反映)。
     *
     * @param delta 加算するオフセット (world units 想定)。
     */
    void MoveCamera(FVec2 delta) noexcept;

    /**
     * ズーム倍率を変更する (active 中のみ反映)。
     *
     * @details zoom_delta を現在倍率に加算し、結果を [0.1, 10.0] に clamp する。
     * @param zoom_delta ズーム倍率への加算量 (+0.1 で +10% ズームイン)。
     */
    void ZoomCamera(f32 zoom_delta) noexcept;

    /**
     * 回転オフセットを累積する (active 中のみ反映)。
     *
     * @param rad_delta 加算する回転量 (ラジアン)。
     */
    void RotateCamera(f32 rad_delta) noexcept;

    /**
     * 累積されたカメラオフセットを返す。
     *
     * @return MoveCamera で累積したオフセット。
     */
    FVec2 CameraOffset()    const noexcept { return m_Offset; }

    /**
     * 現在のズーム倍率を返す。
     *
     * @return [0.1, 10.0] に clamp 済みのズーム倍率。
     */
    f32  ZoomMultiplier()  const noexcept { return m_ZoomMult; }

    /**
     * 累積された回転オフセットを返す。
     *
     * @return RotateCamera で累積した回転 (ラジアン)。
     */
    f32  RotationOffset()  const noexcept { return m_Rot; }

    /**
     * 色フィルタ種別を設定する。
     *
     * @param f 適用するフィルタ種別。
     */
    void       SetFilter(EFilterKind f) noexcept { m_Filter = f; }

    /**
     * 現在の色フィルタ種別を返す。
     *
     * @return 設定済みのフィルタ種別。
     */
    EFilterKind GetFilter() const noexcept       { return m_Filter; }

    /** 撮影リクエスト flag を立てる (シャッターボタン押下時に呼ぶ)。 */
    void RequestCapture() noexcept { m_bCapturePending = true; }

    /**
     * 撮影リクエストを読み取って同時に消費する (描画側が 1 フレーム末尾で呼ぶ)。
     *
     * @details active 外では常に false。flag が立っていれば true を返し同時に落とすため、
     * 同一フレームで複数回呼んでも 1 枚しか撮影されない。
     * @return 撮影リクエストが立っていれば true。
     */
    bool ConsumeCaptureRequest() noexcept;

    /**
     * Enter 時に保存した time scale を返す。
     *
     * @details caller が Exit 後に `CGame::SetTimeScale(photo.SavedTimeScale())` で
     * 復元する用。
     * @return Enter 時点の time scale。
     */
    f32 SavedTimeScale() const noexcept { return m_SavedTimeScale; }

private:
    /** 撮影モード中フラグ。 */
    bool       m_Active            = false;

    /** 累積カメラオフセット (world units)。 */
    FVec2       m_Offset            {0.0f, 0.0f};

    /** 累積ズーム倍率 ([0.1, 10.0] に clamp)。 */
    f32        m_ZoomMult         = 1.0f;

    /** 累積回転オフセット (ラジアン)。 */
    f32        m_Rot               = 0.0f;

    /** 選択中の色フィルタ種別。 */
    EFilterKind m_Filter            = None;

    /** 撮影リクエストの保留フラグ (poll-and-consume)。 */
    bool       m_bCapturePending   = false;

    /** Enter 時に保存した time scale (Exit 後の復元用)。 */
    f32        m_SavedTimeScale  = 1.0f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPhotoMode = CPhotoMode;

} // namespace acs::game
