# ACS 学習例バックログ

## 目的

ACS Engine の公開 API を段階的に学べる実行例を整備します。現在 `acs/samples` は
存在しません。この文書を学習例に関する唯一のバックログとし、実装優先度は低く保ちます。

## 責務の境界

- Engine の module は、実行時機能と公開 API を提供します。
- Framework は、scene、node、component、入力、描画を組み合わせたゲーム構造を提供します。
- 学習例は公開 API の利用方法だけを示し、製品機能や代替実装を所有しません。
- 共有 owner、寿命、更新、終了処理を持つ service だけを subsystem とします。
- 局所状態、値、単純な計算は利用側の型または関数が所有します。
- 回帰契約は unit test が所有し、学習例の実行結果だけに依存しません。

## 学習トラック

| トラック | 学習対象 | 前提 |
|---|---|---|
| Foundation | 結果型、memory、container、delegate、event | なし |
| Application | application lifecycle、window、input、storage、asset | Foundation |
| Game 2D | scene、node、component、sprite、camera、collision、tilemap、UI | Application |
| Game 3D | mesh、camera、material、light、shadow、animation | Application |
| Rendering | RHI、shader、buffer、texture、render target、post process | Foundation |
| Services | audio、network、scripting、telemetry、package、optional backend | Foundation |
| Tools | editor extension、MVVM、asset tool、profiling | Application |
| Integration | 複数 module を組み合わせる小規模なゲーム | 各対象トラック |

各トラックでは画面を必要としない例を先に配置し、対話的な例を後に配置します。
一つの例は一つの中心概念を扱い、必要な module と前提を明示します。

## 各学習例の契約

各学習例は次を備えます。

- 学習目標、前提 API、実行方法、期待結果、失敗条件
- 一つの主要公開型につき一つの header と、必要な同名 cpp
- ACS の既存 module、memory、event、build 機構を使う実装
- Debug と Release の build 検証
- 画面を持たない例の終了値と出力検証
- 対話的な例の代表フレームと入力後状態の検証
- optional feature の有効時と無効時の明確な結果
- 関連する tutorial、quickstart、reference の同時更新

## 実装候補

優先順は、Foundation、Application、Game 2D、Game 3D、Rendering、Services、Tools、
Integration とします。各候補は公開 API の不足を発見した場合、学習例へ局所的な代替機能を
追加せず、責務を持つ Engine module または Framework component の改善として扱います。

候補は、責務の明確さ、回帰検証としての価値、画面確認の必要性、自動化の可否で評価します。
既存の unit test や文書で十分に説明・検証できる候補は追加しません。

## 受け入れ条件

- 学習例の責務と owner が明確である。
- 一つの学習例が一つの中心責務だけを扱う。
- subsystem だけで機能を構成せず、値、object、service の責務が分離されている。
- source、CMake、文書、配布設定の登録内容が一致する。
- 型名、コメント、source 配置が ACS の規約に一致する。
- Debug と Release、対象 backend、optional feature の検証結果が揃う。
- 見た目を扱う場合は代表フレームと主要操作後状態の visual gate が揃う。
- unit test と配布物検証が再現可能である。
- ACS 以外の資料や外部参照を説明の根拠にしない。
