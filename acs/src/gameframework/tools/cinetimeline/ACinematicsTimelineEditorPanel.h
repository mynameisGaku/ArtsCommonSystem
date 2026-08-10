// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/cinetimeline/CCinematicTimelineDocument.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
class CCinematicsDirector;
namespace cinetimeline {

// 既存UIが使う編集種別名を正規の列挙型へ結び付けます。
using ETimelineKeyKind = ECinematicTimelineKeyKind;
// 既存UIが使う編集値名を正規の値型へ結び付けます。
using FEditorKeyframe = FCinematicTimelineKeyframe;

// Directorを参照して編集状態とタイムライン表示を管理するパネルです。
class ACinematicsTimelineEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    // 空の文書と初期表示状態を作成します。
    ACinematicsTimelineEditorPanel() noexcept = default;
    // 保持する文書と表示状態を解放します。
    ~ACinematicsTimelineEditorPanel() noexcept override = default;
    // パネルの複製を許可しません。
    ACinematicsTimelineEditorPanel(const ACinematicsTimelineEditorPanel&) = delete;
    // パネルの複製代入を許可しません。
    ACinematicsTimelineEditorPanel& operator=(const ACinematicsTimelineEditorPanel&) = delete;
    // パネルの移動を許可しません。
    ACinematicsTimelineEditorPanel(ACinematicsTimelineEditorPanel&&) = delete;
    // パネルの移動代入を許可しません。
    ACinematicsTimelineEditorPanel& operator=(ACinematicsTimelineEditorPanel&&) = delete;

    // 文書と表示状態を初期化します。
    void Init() noexcept;
    // 文書と表示状態を終了状態へ戻します。
    void Shutdown() noexcept;
    // Directorを参照先として設定し、現在の文書を反映します。
    void SetCinematicsDirector(acs::game::CCinematicsDirector* director) noexcept;
    // 現在のDirector参照を返します。
    acs::game::CCinematicsDirector* CurrentDirector() const noexcept;
    // 文書を反映できた場合だけ再生を開始します。
    void Play() noexcept;
    // 再生を一時停止します。
    void Pause() noexcept;
    // 時刻と再生状態を初期値へ戻します。
    void Stop() noexcept;
    // 再生中だけ指定時間を進め、無効な値は無視します。
    void Step(f32 dt) noexcept;
    // パネルの再生状態を返します。
    bool IsPlaying() const noexcept;
    // 現在時刻を秒で返します。
    f32 CurrentTimeSec() const noexcept;
    // 有限な時刻を範囲内へ収めて設定します。
    void SetCurrentTimeSec(f32 time_sec) noexcept;
    // 文書の継続時間を秒で返します。
    f32 DurationSec() const noexcept;
    // 有限な継続時間を設定し、失敗時は状態を変更しません。
    void SetDurationSec(f32 duration_sec) noexcept;
    // 選択中のキー番号を返し、未選択は負値です。
    i32 SelectedKeyframeIndex() const noexcept;
    // 有効なキー番号だけを選択します。
    void SelectKeyframe(i32 index) noexcept;
    // 指定種別のキーを追加し、失敗時は文書を変更しません。
    void AddKeyframe(ETimelineKeyKind kind, f32 time_sec) noexcept;
    // 選択中のキーを削除し、失敗時は文書を変更しません。
    void RemoveSelectedKeyframe() noexcept;
    // パネル名を返します。
    const char* Title() const noexcept override { return "Cinematics Timeline"; }
    // タイムラインとインスペクターを描画します。
    void DrawUI() noexcept override;

    // 選択なしを表す番号です。
    static constexpr i32 kNoKeySelected = -1;
    // 表示するトラック数です。
    static constexpr u32 kTrackCount = 5u;
    // 各トラックの表示高さです。
    static constexpr f32 kTrackRowHeightPx = 28.0f;
    // マーカーの表示幅です。
    static constexpr f32 kMarkerWidthPx = 10.0f;
    // マーカー判定に加える横方向の余白です。
    static constexpr f32 kMarkerHitSlackPx = 3.0f;
    // 文書が受け付ける最小継続時間です。
    static constexpr f32 kMinDurationSec = CCinematicTimelineDocument::kMinDurationSec;
    // 新規文書の継続時間です。
    static constexpr f32 kDefaultDurationSec = CCinematicTimelineDocument::kDefaultDurationSec;

private:
    // 文書のキーを読み取り専用で描画へ渡します。
    TSpan<const FEditorKeyframe> EditorKeyframes() const noexcept;
    // ツールバーの一回進行を処理します。
    void StepOnce(f32 dt) noexcept;
    // 文書をDirectorへ反映し、失敗時はDirectorを変更しません。
    bool BakeToDirector() noexcept;

    // 反映先として参照するDirectorです。
    acs::game::CCinematicsDirector* m_Director = nullptr;
    // キーと継続時間を所有する文書です。
    CCinematicTimelineDocument m_Document;
    // 選択中のキー番号です。
    i32 m_SelectedIdx = kNoKeySelected;
    // 表示中の時刻です。
    f32 m_CurrentTime = 0.0f;
    // パネルの再生状態です。
    bool m_Playing = false;
    // マーカーをドラッグ中かを示します。
    bool m_bDraggingMarker = false;
    // ドラッグ対象のキー番号です。
    i32 m_DragIdx = kNoKeySelected;
    // 追加するキー種別です。
    ETimelineKeyKind m_AddKind = ETimelineKeyKind::CameraCut;
};

// 旧パネル名を正規パネル型へ結び付けます。
using FCinematicsTimelineEditorPanel = ACinematicsTimelineEditorPanel;

}
}
