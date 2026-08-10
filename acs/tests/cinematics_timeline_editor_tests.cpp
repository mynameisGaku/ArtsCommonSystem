// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"
#include "gameframework/CinematicsDirector.h"
#include "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h"

#include <limits>

using namespace acs;
using namespace acs::game;
using namespace acs::game::cinetimeline;

ACS_TEST(CinematicsTimelineEditor, RejectsNonFiniteEditsAndKeepsFiniteClamps)
{
    // Editorとruntime directorの状態を検証する対象。
    ACinematicsTimelineEditorPanel panel;
    CCinematicsDirector director;
    panel.Init();
    panel.SetCinematicsDirector(&director);
    panel.AddKeyframe(ETimelineKeyKind::CameraCut, 0.0f);
    EXPECT_EQ(director.KeyframeCount(), 1u);

    // 不正入力前のpanel状態を保持する基準値。
    const f32 before_time = panel.CurrentTimeSec();
    const f32 before_duration = panel.DurationSec();
    const i32 before_selected = panel.SelectedKeyframeIndex();

    // 各APIへ渡す非有限値の組み合わせ。
    const f32 invalid_values[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };
    for (const f32 invalid : invalid_values) {
        panel.SetCurrentTimeSec(invalid);
        panel.SetDurationSec(invalid);
        panel.AddKeyframe(ETimelineKeyKind::FadeColor, invalid);
    }
    EXPECT_EQ(panel.CurrentTimeSec(), before_time);
    EXPECT_EQ(panel.DurationSec(), before_duration);
    EXPECT_EQ(panel.SelectedKeyframeIndex(), before_selected);
    EXPECT_EQ(director.KeyframeCount(), 1u);

    // 再生中の非有限stepが時刻と再生状態を変えないことを検証する基準値。
    panel.Play();
    const f32 before_step = panel.CurrentTimeSec();
    const bool before_playing = panel.IsPlaying();
    for (const f32 invalid : invalid_values) {
        panel.Step(invalid);
    }
    EXPECT_EQ(panel.CurrentTimeSec(), before_step);
    EXPECT_EQ(panel.IsPlaying(), before_playing);
    panel.Stop();

    // 有限値の範囲外入力は既存のclamp契約で処理する。
    panel.SetDurationSec(-1.0f);
    EXPECT_EQ(panel.DurationSec(), ACinematicsTimelineEditorPanel::kMinDurationSec);
    panel.SetCurrentTimeSec(10.0f);
    EXPECT_EQ(panel.CurrentTimeSec(), ACinematicsTimelineEditorPanel::kMinDurationSec);
    panel.AddKeyframe(ETimelineKeyKind::TriggerCallback, -1.0f);
    EXPECT_TRUE(panel.SelectedKeyframeIndex() >= 0);
    EXPECT_EQ(director.KeyframeCount(), 2u);
}

ACS_TEST(CinematicsTimelineEditor, RejectsFiniteStepOverflowAtomically)
{
    // 非バインドpanelで有限加算のoverflowを検証する対象。
    ACinematicsTimelineEditorPanel panel;
    panel.Init();
    const f32 max_time = std::numeric_limits<f32>::max();
    // 2回目の加算でoverflowする有限step。
    const f32 step = max_time * 0.75f;
    panel.SetDurationSec(max_time);
    panel.Play();
    panel.Step(step);
    // overflow拒否前に保持するpanel時刻。
    const f32 before = panel.CurrentTimeSec();
    panel.Step(step);
    EXPECT_EQ(panel.CurrentTimeSec(), before);
    EXPECT_TRUE(panel.IsPlaying());
}

ACS_TEST(CinematicsTimelineEditor, RejectsBoundStepOverflowAtomically)
{
    // directorとpanelの時刻同期を検証する対象。
    ACinematicsTimelineEditorPanel panel;
    CCinematicsDirector director;
    panel.Init();
    panel.SetCinematicsDirector(&director);
    panel.SetDurationSec(std::numeric_limits<f32>::max());
    panel.Play();
    const f32 step = std::numeric_limits<f32>::max() * 0.75f;
    panel.Step(step);
    // panelとdirectorの同期前に保持するdirector時刻。
    const f32 director_before = director.CurrentTime();

    // panelだけを編集して同期ずれを作る。
    panel.SetCurrentTimeSec(0.0f);
    // overflow拒否前に保持するpanel時刻。
    const f32 panel_before = panel.CurrentTimeSec();
    panel.Step(step);
    EXPECT_EQ(panel.CurrentTimeSec(), panel_before);
    EXPECT_EQ(director.CurrentTime(), director_before);
    EXPECT_TRUE(panel.IsPlaying());
}
