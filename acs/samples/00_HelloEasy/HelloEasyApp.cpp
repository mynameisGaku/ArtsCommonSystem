// SPDX-License-Identifier: Apache-2.0
// HelloEasy — acs::easy レイヤを使った 2D サンプルのメインループ本体。
//
// 操作:
//   ・矢印キー / WASD … 四角を動かす
//   ・マウス          … 円がついてくる
//   ・Esc             … 終了
#include "HelloEasyApp.h"

#include "easy/Easy.h"

namespace hello00 {

using namespace acs::easy;

int HelloEasyApp::Run() noexcept {
    OpenWindow(1280, 720, "ACS Easy へようこそ");
    SetBackground(Rgb(28, 32, 44));

    // ゲームの状態は、ただのローカル変数でよい（グローバル変数は不要）
    float x = 600.0f;
    float y = 320.0f;
    const float speed = 360.0f;            // 1 秒あたりの移動ピクセル数

    while (NextFrame()) {                   // ウィンドウを閉じると終了
        if (IsKeyPressed(EKey::Escape)) Quit();

        // DeltaTime を掛けてフレームレート非依存にする
        const float step = speed * DeltaTime();
        if (IsKeyDown(EKey::Right) || IsKeyDown(EKey::D)) x += step;
        if (IsKeyDown(EKey::Left)  || IsKeyDown(EKey::A)) x -= step;
        if (IsKeyDown(EKey::Down)  || IsKeyDown(EKey::S)) y += step;
        if (IsKeyDown(EKey::Up)    || IsKeyDown(EKey::W)) y -= step;

        DrawLine(0, 380, ScreenWidth(), 380, FColor::Gray, 2.0f);
        DrawRect(x - 40, y - 40, 80, 80, FColor::FSky);
        DrawCircle(MouseX(), MouseY(), 28, FColor::Orange);

        DrawString(24, 20, "矢印キー / WASD で四角を動かす", FColor::White);
        DrawString(24, 52, "マウスを動かすと円がついてくる", FColor::White);
        DrawString(24, 84, "Esc で終了", FColor::Gray);
        if (IsMouseDown())
            DrawString(24, 116, "マウス左ボタンを押している", FColor::Yellow);
    }
    return 0;
}

} // namespace hello00
