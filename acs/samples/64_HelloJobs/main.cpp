// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// HelloJobs — acs::easy の「ジョブ / 並列」入門 (初学者向け)
// ----------------------------------------------------------------------------
// やりたい 3 つを、関数ポインタや手動初期化を意識せず「ラムダ」で書ける:
//   (1) ParallelFor … 重い配列処理を全コアで分担
//   (2) RunAsync/WaitJobs … 重い処理をバックグラウンドへ投げて、後で待つ
//   (3) Job/Then/RunJobs … A と B を並列に走らせ、両方終わってから C
//
// 中身は本格的なワークスチール CThreadPool / 依存グラフ CJobGraph。初学者はそれを
// 知らなくてよい (内部は「めちゃくちゃちゃんと」動く)。初期化も不要 (初回使用で自動)。
//
// ★唯一の注意: ジョブの中 (ラムダ) は別スレッドで走る。中では計算だけして、
//   結果は捕捉した変数へ書くこと。描画など他の easy 関数を中で呼んではいけない。
// ============================================================================
#include "easy/Easy.h"

#include <cstdio>

using namespace acs::easy;

int main() {
    std::puts("=== ACS HelloJobs ===");
    std::printf("並列ワーカ数: %d\n", WorkerCount());

    bool all_ok = true;

    // --- (1) 並列 for: 10 万要素を全コアで埋める -----------------------------
    static int data[100000];
    ParallelFor(0, 100000, [&](int i) {
        data[i] = i * 2;                       // i ごとに独立した計算のみ (競合なし)
    });
    long long sum = 0;
    bool ok1 = true;
    for (int i = 0; i < 100000; ++i) { if (data[i] != i * 2) ok1 = false; sum += data[i]; }
    std::printf("(1) 並列 for : data[i]=i*2 を 10万件、合計=%lld  %s\n", sum, ok1 ? "OK" : "NG");
    all_ok = all_ok && ok1;

    // --- (2) 非同期 1 発: 重い処理を投げて、後で待つ -------------------------
    long long result = 0;
    FJobBatch job = RunAsync([&] {
        long long acc = 0;
        for (int i = 1; i <= 1000000; ++i) acc += i;   // 1〜100万の総和
        result = acc;                                  // 結果は捕捉変数へ
    });
    // ここでメインスレッドは別の作業を続けられる (デモでは省略)。
    WaitJobs(job);                                     // 完了を待つ
    const bool ok2 = (result == 500000500000LL);
    std::printf("(2) 非同期   : sum(1..1000000)=%lld  %s\n", result, ok2 ? "OK" : "NG");
    all_ok = all_ok && ok2;

    // --- (3) A→B 依存: A と B を並列、両方終わってから C --------------------
    int partA = 0, partB = 0, merged = -1;
    FJobNode a = Job([&] { for (int i = 0; i < 500000; ++i) ++partA; });   // → 500000
    FJobNode b = Job([&] { for (int i = 0; i < 300000; ++i) ++partB; });   // → 300000
    FJobNode c = Job([&] { merged = partA + partB; });                    // 両方の後で実行
    Then(a, c);                                        // c は a の後
    Then(b, c);                                        // かつ b の後 (a と b は並列)
    RunJobs();                                         // 依存順に実行し、全完了まで待つ
    const bool ok3 = (partA == 500000 && partB == 300000 && merged == 800000);
    std::printf("(3) 依存 A,B→C: A=%d B=%d merged=%d  %s\n", partA, partB, merged, ok3 ? "OK" : "NG");
    all_ok = all_ok && ok3;

    std::puts(all_ok ? "result: === ALL PASS ===" : "result: FAIL");
    return all_ok ? 0 : 1;
}
