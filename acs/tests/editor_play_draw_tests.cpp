// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// インプロセス Play の «コンポーネント OnDraw をエディタ viewport に描く» 経路の検証。
//   GPU 非依存で、editor_abi の acs_game_scene_draw が依存する 2 つの土台を固める:
//   (1) FNode2D::DrawTree が各コンポーネントの OnDraw を、SpriteBatch + view を配線した
//       RenderContext で呼ぶこと (= シムが DrawTree に委譲すれば OnDraw が走る)。
//   (2) エディタカメラ (screen = world*zoom + pan) に一致する SpriteBatch world view の
//       パラメータ式 cam = ((w/2,h/2) - pan)/zoom が、world→screen を厳密に再現すること
//       (SpriteBatch シェーダの screen = (world-cam)*zoom + (w/2,h/2) と突き合わせ)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "math/Vec.h"

using namespace acs;
using namespace acs::game;

namespace {

// OnDraw 呼び出しと、その時 RenderContext に何が配線されていたかを記録するプローブ。
// rc.Sprites() は呼ばない (fake ポインタを deref しない) ので GPU 不要。
struct FProbeDraw : public FComponent2D {
    const void* Kind() const noexcept override { return ComponentKindOf<FProbeDraw>(); }

    int   draws        = 0;
    bool  had_sprites  = false;
    FVec2 view_center{ -1.0f, -1.0f };
    f32   view_scale   = -1.0f;

    void OnDraw(RenderContext& rc) noexcept override {
        ++draws;
        had_sprites = rc.HasSprites();
        view_center = rc.ViewCenter();
        view_scale  = rc.ViewScale();
    }
};

// SpriteBatch シェーダの world→screen 変換 (SpriteBatch.cpp VSMain と同式)。
FVec2 BatchWorldToScreen(FVec2 world, FVec2 cam, f32 zoom, f32 w, f32 h) noexcept {
    return FVec2{ (world.x - cam.x) * zoom + w * 0.5f,
                  (world.y - cam.y) * zoom + h * 0.5f };
}

// editor_abi が acs_editor_render で計算する world view cam (= GameReflectShim へ渡す値)。
FVec2 EditorWorldViewCam(f32 pan_x, f32 pan_y, f32 zoom, f32 w, f32 h) noexcept {
    return FVec2{ (w * 0.5f - pan_x) / zoom, (h * 0.5f - pan_y) / zoom };
}

} // namespace

// (1) DrawTree が OnDraw を、配線済み RenderContext で呼ぶ (シムが委譲する経路の土台)。
ACS_TEST(EditorPlayDraw, DrawTreeInvokesComponentOnDrawWithWiredContext) {
    FNode2D root;
    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FProbeDraw& probe = child.AddComponent<FProbeDraw>();

    // acs_game_scene_draw と同じ配線: SpriteBatch + world view。
    // 実 FSpriteBatch を stack に置く (Init しないので GPU 不要、LightsActive()=0 で安全)。
    // DrawTree はマテリアル無しノードで rc.Sprites().LightsActive() を問い合わせるため、
    // fake ポインタではなく CPU 側だけで成立する実体が要る。
    FSpriteBatch sprites;
    RenderContext rc;
    rc._SetSpriteBatch(&sprites);
    rc._SetView2D(FVec2{ 12.0f, 34.0f }, 2.0f);

    EXPECT_EQ(probe.draws, 0);
    root.DrawTree(rc);

    EXPECT_EQ(probe.draws, 1);                       // OnDraw が 1 回呼ばれた
    EXPECT_TRUE(probe.had_sprites);                  // SpriteBatch が配線されていた
    EXPECT_NEAR(probe.view_center.x, 12.0f, 1e-4f);  // view が伝わっていた
    EXPECT_NEAR(probe.view_center.y, 34.0f, 1e-4f);
    EXPECT_NEAR(probe.view_scale, 2.0f, 1e-4f);
}

// 非表示ノードの OnDraw はスキップされる (DrawTree の m_Visible ガード)。
ACS_TEST(EditorPlayDraw, HiddenNodeSkipsOnDraw) {
    FNode2D root;
    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FProbeDraw& probe = child.AddComponent<FProbeDraw>();
    child.SetVisible(false);

    FSpriteBatch sprites;
    RenderContext rc;
    rc._SetSpriteBatch(&sprites);
    root.DrawTree(rc);
    EXPECT_EQ(probe.draws, 0);
}

// (2) editor cam 式が world→screen を厳密再現する: SpriteBatch world view で描いた
//     コンポーネントの screen 位置 == エディタの screen=world*zoom+pan と一致する。
ACS_TEST(EditorPlayDraw, WorldViewMatchesEditorCamera) {
    const f32 w = 1280.0f, h = 720.0f;
    struct Cam { f32 pan_x, pan_y, zoom; };
    const Cam cams[] = { { 0.0f, 0.0f, 1.0f }, { 120.0f, -80.0f, 1.0f },
                         { 40.0f, 200.0f, 2.5f }, { -300.0f, 60.0f, 0.5f } };
    const FVec2 pts[] = { { 0, 0 }, { 100, -50 }, { -250, 175 }, { 640, 360 } };

    for (const Cam& c : cams) {
        const FVec2 cam = EditorWorldViewCam(c.pan_x, c.pan_y, c.zoom, w, h);
        for (const FVec2 p : pts) {
            const FVec2 sb_screen = BatchWorldToScreen(p, cam, c.zoom, w, h);   // SpriteBatch 経由
            const FVec2 ed_screen{ p.x * c.zoom + c.pan_x, p.y * c.zoom + c.pan_y };  // エディタ式
            EXPECT_NEAR(sb_screen.x, ed_screen.x, 1e-3f);
            EXPECT_NEAR(sb_screen.y, ed_screen.y, 1e-3f);
        }
    }
}

// カメラ追従の round-trip: Play 開始時 editor cam → play camera center、毎フレーム play camera →
// editor cam。ユーザーがカメラを触らなければ view は不変 (round-trip 一致)、触れば追従する。
ACS_TEST(EditorPlayDraw, CameraFollowRoundTrips) {
    const f32 w = 1280.0f, h = 720.0f;
    struct Cam { f32 pan_x, pan_y, zoom; };
    const Cam cams[] = { { 0.0f, 0.0f, 1.0f }, { 120.0f, -80.0f, 1.0f },
                         { 40.0f, 200.0f, 2.5f }, { -300.0f, 60.0f, 0.5f } };
    for (const Cam& c : cams) {
        // logic_play_start: editor cam → play camera center (vcx,vcy)。
        const f32 vcx = (w * 0.5f - c.pan_x) / c.zoom;
        const f32 vcy = (h * 0.5f - c.pan_y) / c.zoom;
        // EditorTickLogic: play camera (vcx,vcy,zoom) → editor cam pan。
        const f32 pan_x2 = w * 0.5f - vcx * c.zoom;
        const f32 pan_y2 = h * 0.5f - vcy * c.zoom;
        EXPECT_NEAR(pan_x2, c.pan_x, 1e-2f);   // カメラ未操作なら view 不変
        EXPECT_NEAR(pan_y2, c.pan_y, 1e-2f);
    }
}
