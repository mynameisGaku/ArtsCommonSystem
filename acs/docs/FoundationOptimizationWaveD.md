# エンジン基盤最適化 Wave D/H 検証記録

## 対象

ルート担当の T16～T20 と T36～T40 をまとめた検証記録である。描画品質、
シミュレーション精度、入力の接続中レイテンシは変更せず、毎フレームの不要な
OS 呼び出し、バックグラウンド描画、ログ整形・起床、Release の最適化境界を扱った。

## 実装結果

### 入力

- 接続中の XInput ポートは従来どおり毎フレーム取得する。
- 未接続ポートは `TGamepadPollScheduler<4>` が1フレーム1ポートずつ確認する。
- 全ポート未接続時の失敗呼び出しは 4,000,000 回/1,000,000フレームから
  1,000,000 回へ減少した。
- 全ポートの再接続確認上限は4フレームである。

### ウィンドウとアプリケーションループ

- `WM_SIZE` から最小化状態を保持し、ムーブ構築・代入でも状態を失わない。
- 最小化時の 0x0 リサイズをレンダラへ渡さない。
- 最小化中は更新・描画を行わず、Win32 メッセージまたは既定100msの上限まで待つ。
- 待機中もフレーム時計を進め、復帰直後の `dt` が最小化時間分へ跳ねない。
- 表示中のループ、レンダリング設定、画質には変更を加えていない。

### 非同期ロガー

- リング投入処理を予約・公開へ分離し、公開位置が読み取り位置と一致する
  「空から非空」のときだけライターへ通知する。
- ライターの空判定と条件変数待機を同じ `wake_lock` 境界で再確認し、
  起床通知を取りこぼさない。
- 非ドレイン中の4096レコード投入モデルは、起床通知4096回から1回になった。
- 書式指定子を含まない文字列リテラルは、型特性と `if constexpr` で
  `vsnprintf` を通らず固定長コピーする。
- `ACS_COMPILED_LOG_MIN_SEVERITY` を0～6で設定でき、無効な固定レベルは
  ログ引数の評価とロガーへのリンク参照をコンパイル時に除去する。既定値0は従来互換。

### Release IPO/LTCG

- `ACS_ENABLE_RELEASE_IPO=ON` を既定とし、CMake が対応可否を検査する。
- ASan 有効時またはツールチェーン非対応時は安全に無効化する。
- Debug には適用せず、反復ビルドと診断性を維持する。
- Visual Studio 18 の生成結果で `WholeProgramOptimization=true` を確認した。

## 測定

同一ソース、Release、16並列、テスト用ターゲットで1回ずつクリーン相当の
ビルドを行った。時間はホスト負荷を含む診断値であり、回帰ゲートには使用しない。

| 指標 | IPO無効 | IPO有効 | 差 |
|---|---:|---:|---:|
| `acs_unit_tests.exe` | 5,620,736 B | 5,564,416 B | -56,320 B (-1.00%) |
| 初回 `acs_unit_tests` ビルド | 119.1 s | 113.5 s | -5.6 s |
| スケジューラ100万回・21回中央値 | 836,800 ns | 829,700 ns | -7,100 ns |

スケジューラは単一翻訳単位内の小さな測定なので、0.85%の差をIPO固有の効果とは
断定しない。採用根拠は、Release回帰が通ること、無効化可能であること、生成物が
縮小したこと、決定的な作業量が悪化しないことである。

## 検証コマンド

```pwsh
cmake -S acs/engine -B acs/Intermediate/foundation-ipo `
  -G "Visual Studio 18 2026" -A x64 `
  -DACS_BUILD_TESTS=ON -DACS_BUILD_SAMPLES=OFF `
  -DACS_BUILD_TOOLS=OFF -DACS_ENABLE_RELEASE_IPO=ON
cmake --build acs/Intermediate/foundation-ipo --config Release `
  --target acs_unit_tests acs_foundation_performance `
           acs_foundation_log_compile_gate --parallel 16
ctest --test-dir acs/Intermediate/foundation-ipo -C Release `
  --output-on-failure `
  -R "^(ACS\.FoundationPerformance.*|ACS\.FoundationLogCompileGate|ACS\.UnitTests)$"
```

確認済み結果:

- `ACS.FoundationPerformance`: pass
- `ACS.FoundationPerformanceVerifierSelfTest`: pass
- `ACS.FoundationPerformanceContract`: pass
- `ACS.FoundationLogCompileGate`: pass
- `ACS.FoundationEndToEndAggregatorSelfTest`: pass
- `ACS.ChangedCppRulesAuditSelfTest`: pass
- `ACS.UnitTests`: pass

## 回帰ゲート

`verify_foundation_performance.py` はタイミング値ではなく次の決定値を検査する。

- XInput 未接続呼び出しが正確に75%減る。
- 再接続確認上限が4フレーム以内である。
- 非ドレイン中のログバーストが起床1回になる。
- 生産側JSONが `status=pass` を返す。

このため、CIホストの一時的な負荷やCPU周波数変動でテストを不安定にせず、
最適化そのものが失われた場合だけ失敗する。

## 共通C++規約と統合証跡

`audit_changed_cpp_rules.py` は `origin/main` など指定した基準との差分だけを対象にし、
追加C++行の括弧・初期化子一行規約、日本語コメント、変更header冒頭の
SPDX/include guard規約を検査する。既存コード全体を一括整形せず、今回の変更が
新たな違反を増やさないことを機械的に確認できる。
既定では複数headerを連結して生成する `dist/` を除外し、配布物は
`ACS.DistributionConventions`、`ACS.DistributionHeaderSyntax`、配布drift検査へ
委ねる。生成物自体を同じ規則で調査する場合だけ `--include-generated` を指定する。

`run_foundation_end_to_end.py` は任意のC++差分規約監査、DebugまたはReleaseの
build、全CTest、決定的な基盤性能契約を順に実行し、終了コード・所要時間・
失敗出力末尾を単一JSONへ原子的に保存する。schema 2 はsourceのHEAD SHAと
比較基点SHA、tracked差分の有無、`CMakeCache.txt` のSHA-256、raw/Diligent・
tests/tools/samples構成、性能実行fileと`--artifact`指定fileのsize・SHA-256を
同時に保存する。cacheの`CMAKE_HOME_DIRECTORY`は検証対象checkoutの
`acs/engine`とfile identityまで照合し、別worktreeのbuild結果を現在SHAへ
誤帰属させない。`--expect-cache KEY=VALUE` はreleaseで要求した構成とcacheが
異なる場合に実行前で失敗し、`--require-clean-source`は実行前後のtracked差分と
途中のcommit切替を拒否する。
公開型のlayout変更を含む統合では `--clean-first` を指定し、古いMSBuild中間物と
新しいheaderが混在するABI不整合を排除する。
最終統合時はこのJSONをT64/T80の再現可能な証跡として使用する。
