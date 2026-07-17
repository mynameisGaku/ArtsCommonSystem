// SPDX-License-Identifier: Apache-2.0
// FCameraStack (Pillar E) の動作確認テスト — 特に Push/Pop 遷移の後勝ち契約
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Camera2D.h"
#include "gameframework/CameraStack.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(CameraStack, PushPopBasics) {
    FCamera2D a, b;
    a.SetPosition({0, 0});
    b.SetPosition({100, 0});

    FCameraStack st;
    st.PushCamera(a, 0.0f);
    EXPECT_EQ(st.Depth(), 1u);
    EXPECT_TRUE(st.Active() == &a);

    st.PushCamera(b, 0.5f);
    EXPECT_EQ(st.Depth(), 2u);
    EXPECT_TRUE(st.Active() == &b);
    EXPECT_TRUE(st.IsBlending());

    // blend 完了まで進めると top のみ残って blending 終了。
    st.Tick(1.0f);
    EXPECT_FALSE(st.IsBlending());

    st.PopCamera(0.0f);          // 即時 pop 要求 → 次の Tick で除去
    st.Tick(0.0f);
    EXPECT_EQ(st.Depth(), 1u);
    EXPECT_TRUE(st.Active() == &a);
}

ACS_TEST(CameraStack, PushDuringPopFinalizesPendingPop) {
    FCamera2D a, b, c;
    a.SetPosition({0, 0});
    b.SetPosition({100, 0});
    c.SetPosition({200, 0});

    FCameraStack st;
    st.PushCamera(a, 0.0f);
    st.PushCamera(b, 0.0f);
    EXPECT_EQ(st.Depth(), 2u);

    // b をフェードアウト中に c を push (後勝ち契約)。
    st.PopCamera(1.0f);
    st.PushCamera(c, 0.0f);

    // pop 中だった b はここで確定除去され、埋もれて残留しない。
    EXPECT_EQ(st.Depth(), 2u);       // a + c (b は除去済み)
    EXPECT_TRUE(st.Active() == &c);

    // 修正前の再現: b が埋もれたまま c を pop すると、b が is_in=true に
    // リセットされて Pop 要求が静かに取り消されていた。今は c を pop すると
    // 正しく a まで戻る。
    st.PopCamera(0.0f);
    st.Tick(0.0f);
    EXPECT_EQ(st.Depth(), 1u);
    EXPECT_TRUE(st.Active() == &a);

    // 長時間 Tick しても層数は安定 (残留 entry 無し)。
    for (int i = 0; i < 10; ++i) st.Tick(0.1f);
    EXPECT_EQ(st.Depth(), 1u);
}
