/* ACS リファレンス — timing モジュール。 */
ACS_REF.modules.push({
  id: "timing",
  order: 18,
  title: "timing — 決定論的な時間変換",
  blurb: "利用側が値として所有する固定更新時計。可変な経過秒を有界な更新回数と描画補間率へ変換し、ゲームループや共有サービスの寿命から切り離します。",
  types: [
    {
      name: "FFixedStepOptions",
      kind: "構造体", header: "timing/FixedStepOptions.h",
      summary: "固定更新の刻み秒、一回の最大更新回数、一回の最大蓄積秒をまとめる設定値。",
      when: "simulationごとに固定更新の精度と過負荷時の上限を指定する時。"
    },
    {
      name: "FFixedStepAdvanceResult",
      kind: "構造体", header: "timing/FixedStepAdvanceResult.h",
      summary: "一回の経過入力で確定した更新回数、補間率、破棄秒、受付状態を返す値。",
      when: "固定更新を指定回数だけ実行し、描画を残り時間で補間する時。"
    },
    {
      name: "FFixedStepClockSnapshot",
      kind: "構造体", header: "timing/FixedStepClockSnapshot.h",
      summary: "時計の設定、持ち越し秒、累積破棄秒、累積更新回数をまとめた保存値。",
      when: "rollbackや再試行のために同じACS版の時計位置を一時保存する時。"
    },
    {
      name: "CFixedStepClock",
      kind: "クラス", header: "timing/FixedStepClock.h",
      summary: "可変な経過秒を固定更新回数へ決定論的に変換する、利用側所有の値。",
      when: "物理や規則処理を描画frameとは独立した刻み幅で進める時。",
      members: [
        { sig: "bool Configure(FFixedStepOptions options)", ret: "適用できたか", desc: "設定を検証して適用し、補間位置と累積統計を初期化する。" },
        { sig: "bool TryReconfigurePreservingProgress(FFixedStepOptions options)", ret: "適用できたか", desc: "補間率と累積統計を維持して刻み幅を変更する。" },
        { sig: "FFixedStepAdvanceResult Advance(f64 delta_seconds)", ret: "今回の更新回数と補間情報", desc: "負値と有限でない値を拒否し、上限を適用して時計を進める。" },
        { sig: "bool TryAdvanceBatch(deltas, count, results, capacity, result_count)", ret: "全件処理できたか", desc: "最大4096件を先に検証し、失敗時は時計と全出力を維持する。" },
        { sig: "TryCaptureSnapshot / TryRestoreSnapshot", ret: "保存または復元できたか", desc: "内容、整列、領域重複を検証し、失敗時は出力または時計を維持する。" },
        { sig: "Reset / InterpolationAlpha / TotalStepCount / TotalDroppedSeconds", desc: "設定を維持した初期化と、現在補間率、累積更新回数、累積破棄秒の取得を行う。" }
      ]
    },
    {
      name: "TryCaptureFixedStepClockSnapshots / TryRestoreFixedStepClockSnapshots",
      kind: "自由関数", header: "timing/FixedStepClockBatch.h",
      summary: "複数時計の保存または復元を最大4096件まで一括で確定する関数。",
      when: "複数のsimulation時計を同じtransactionとして保存または復元する時。",
      members: [
        { sig: "bool TryCaptureFixedStepClockSnapshots(clocks, count, snapshots, capacity, snapshot_count)", ret: "全件保存できたか", desc: "全領域と全時計を先に検証し、失敗時は保存先と件数を維持する。" },
        { sig: "bool TryRestoreFixedStepClockSnapshots(clocks, snapshots, count)", ret: "全件復元できたか", desc: "全領域と全保存値を先に検証し、失敗時はどの時計も変更しない。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "CFixedStepClock": "可変な経過秒を固定更新回数と描画補間率へ変換する、利用側所有の決定論的な時計。",
  "固定更新": "描画frameの長さに左右されず、一定の秒数ずつsimulationを進める方法。"
});
