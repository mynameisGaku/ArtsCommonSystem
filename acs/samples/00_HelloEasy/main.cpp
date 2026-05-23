// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 00 — HelloEasy
//
// acs::easy レイヤを使った、最も簡単な入門サンプル。
// クラスも継承もエラー型も使わず、関数を呼ぶだけで 2D ゲームが書ける。
// （次の段階として、acs::Application を直接使う書き方は docs/QUICKSTART.md）
//
// 操作:
//   ・矢印キー / WASD … 四角を動かす
//   ・マウス          … 円がついてくる
//   ・Esc             … 終了

#include "easy/Easy.h"

using namespace acs::easy;

int main() {
    OpenWindow(1280, 720, "ACS Easy へようこそ");
    SetBackground(Rgb(28, 32, 44));

    // ゲームの状態は、ただのローカル変数でよい（グローバル変数は不要）
    float x = 600.0f;
    float y = 320.0f;
    const float speed = 360.0f;            // 1 秒あたりの移動ピクセル数

    while (NextFrame()) {                   // ウィンドウを閉じると終了
        if (IsKeyPressed(EKey::Escape)) Quit();

        // 入力で四角を動かす（DeltaTime を掛けてフレームレート非依存に）
        const float step = speed * DeltaTime();
        if (IsKeyDown(EKey::Right) || IsKeyDown(EKey::D)) x += step;
        if (IsKeyDown(EKey::Left)  || IsKeyDown(EKey::A)) x -= step;
        if (IsKeyDown(EKey::Down)  || IsKeyDown(EKey::S)) y += step;
        if (IsKeyDown(EKey::Up)    || IsKeyDown(EKey::W)) y -= step;

        // 描画
        DrawLine(0, 380, ScreenWidth(), 380, Color::Gray, 2.0f);
        DrawRect(x - 40, y - 40, 80, 80, Color::Sky);
        DrawCircle(MouseX(), MouseY(), 28, Color::Orange);

        DrawString(24, 20, "矢印キー / WASD で四角を動かす", Color::White);
        DrawString(24, 52, "マウスを動かすと円がついてくる", Color::White);
        DrawString(24, 84, "Esc で終了", Color::Gray);
        if (IsMouseDown())
            DrawString(24, 116, "マウス左ボタンを押している", Color::Yellow);
    }
}
