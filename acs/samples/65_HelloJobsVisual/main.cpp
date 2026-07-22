// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// HelloJobsVisual — ジョブ/並列を「見える化」する (初学者向け)
// ----------------------------------------------------------------------------
//   ・2500 個のパーティクルが「流れ場」に沿って渦巻きながら動く。その位置計算を
//     毎フレーム ParallelFor で並列実行する → 群れがヌルヌル動く = 並列処理の様子。
//   ・Space を押すと、ゲージ A と B が「並列に」伸び、両方が満タンになってから
//     ゲージ C が伸びる → 「A,B 並列 → C 依存」を進捗バーで可視化。
//
// ★ジョブの中(ラムダ)は別スレッドで走る。中では計算だけして結果を変数に書き、
//   描画はメインスレッド(while ループの中)で行う。これがジョブの基本ルール。
// ============================================================================
#include "easy/Easy.h"

#include <cstdio>

using namespace acs::easy;

namespace {

constexpr int   N    = 2500;       // パーティクル数
constexpr float W    = 960.0f, H = 600.0f;
constexpr float kTop = 64.0f, kBot = 504.0f;   // パーティクルが動く帯 (上端ヘッダ/下端ゲージを避ける)

// ジョブが触る状態は file スコープに置く。ウィンドウを閉じても、走行中のジョブが
// 完了するまで安全に生存させるため(ローカルだとダングリングし得る)。
float px[N], py[N], vx[N], vy[N];  // パーティクルの位置と速度
float gaugeA = 0.0f, gaugeB = 0.0f, gaugeC = 0.0f;

// 重い計算をしながら進捗 g を 0→1 に進める(別スレッドで走る)。
// g への書き込みは描画用の概算で、多少の競合は見た目だけの問題なので許容する。
void HeavyFill(float& g) {
    volatile long long acc = 0;
    for (int k = 0; k <= 100; ++k) {
        for (int j = 0; j < 2000000; ++j) acc += j;   // 実際の計算負荷
        g = k * 0.01f;                                 // 進捗
    }
    (void)acc;
}

// ラベル + 進捗バー(背景 + 充填)を描く小ヘルパ。
void DrawGauge(float x, float y, const char* label, float v, FColor col) {
    DrawString(x, y, label, FColor::White);
    const float bx = x + 180.0f, bw = 280.0f, bh = 20.0f;
    DrawRect(bx, y - 2.0f, bw, bh, Rgb(40, 44, 52));               // 背景
    DrawRect(bx, y - 2.0f, bw * Clamp(v, 0.0f, 1.0f), bh, col);    // 充填
}

} // namespace

int main() {
    OpenWindow(static_cast<int>(W), static_cast<int>(H),
               "ACS ジョブ可視化  (Space: A,B 並列 → C)");
    std::printf("並列ワーカ数: %d\n", WorkerCount());

    // パーティクル初期化 (速度は流れ場が決めるので 0 から)
    for (int i = 0; i < N; ++i) {
        px[i] = RandomFloat(0.0f, W);
        py[i] = RandomFloat(kTop, kBot);
        vx[i] = 0.0f; vy[i] = 0.0f;
    }

    FJobBatch ja{}, jb{}, jc{};
    int   phase = 0;       // 0:待機  1:A&B 実行中  2:C 実行中  3:完了
    float t     = 0.0f;    // 経過時間 (流れ場のアニメ用)

    while (NextFrame()) {
        const float dt = DeltaTime();
        t += dt;

        // (1) 並列 for: 全パーティクルの移動を毎フレーム「並列に」計算する。
        //     各 i は自分の要素だけを読み書きするので競合しない。grain を大きめに
        //     指定して 1 フレームあたりのタスク数を抑え、投入オーバーヘッドを下げる。
        ParallelFor(0, N, 256, [&](int i) {
            // 位置と時間から「流れ場」の角度を作る(sin/cos は度)。渦巻く動きになる。
            const float ang = (Sin(px[i] * 0.5f + t * 50.0f) +
                               Cos(py[i] * 0.5f + t * 40.0f)) * 180.0f;
            const float spd = 140.0f;
            vx[i] += (Cos(ang) * spd - vx[i]) * 2.5f * dt;   // 流れ場の向きへ滑らかに
            vy[i] += (Sin(ang) * spd - vy[i]) * 2.5f * dt;
            px[i] += vx[i] * dt;
            py[i] += vy[i] * dt;
            // 帯の中をトーラス状にラップ(端から反対側へ)
            if (px[i] < 0.0f)      px[i] += W;          else if (px[i] > W)    px[i] -= W;
            if (py[i] < kTop)      py[i] += (kBot - kTop); else if (py[i] > kBot) py[i] -= (kBot - kTop);
        });

        // 描画はメインスレッドで(ジョブの中では描画しない)。
        for (int i = 0; i < N; ++i)
            DrawCircle(px[i], py[i], 2.5f, Rgb(110, static_cast<acs::u8>(120 + (i % 120)), 255));

        // (2)(3) Space で A,B を並列起動 → 両方完了後に C(依存)。進捗をゲージで可視化。
        if (IsKeyPressed(EKey::Space) && (phase == 0 || phase == 3)) {
            gaugeA = gaugeB = gaugeC = 0.0f;
            phase = 1;
            ja = RunAsync([&]{ HeavyFill(gaugeA); });   // A と B は…
            jb = RunAsync([&]{ HeavyFill(gaugeB); });   // …別スレッドで同時に走る
        }
        if (phase == 1 && JobsDone(ja) && JobsDone(jb)) {
            WaitJobs(ja); WaitJobs(jb);                 // 完了済 → 即返り + クロージャ解放
            phase = 2;
            jc = RunAsync([&]{ HeavyFill(gaugeC); });    // 両方終わってから C
        }
        if (phase == 2 && JobsDone(jc)) { WaitJobs(jc); phase = 3; }

        // ヘッダ + ゲージ描画
        DrawString(20.0f, 18.0f, "ParallelFor: 2500 個のパーティクルを毎フレーム並列更新", FColor::White);
        DrawString(20.0f, 40.0f, "Space: ゲージ A,B を並列実行 → 両方終わってから C", FColor::Sky);

        DrawGauge(20.0f, H - 90.0f, "A (並列)",       gaugeA, Rgb(90, 210, 130));
        DrawGauge(20.0f, H - 60.0f, "B (並列)",       gaugeB, Rgb(90, 170, 230));
        DrawGauge(20.0f, H - 30.0f, "C (A,B の後)",   gaugeC, Rgb(235, 175, 70));

        const char* st = (phase == 0) ? "状態: 待機中 (Space で開始)"
                       : (phase == 1) ? "状態: A,B を並列実行中..."
                       : (phase == 2) ? "状態: C を実行中 (A,B 完了後)"
                                      : "状態: 完了! (Space でもう一度)";
        DrawString(540.0f, H - 60.0f, st, FColor::Yellow);
    }
    return 0;
}
