# Foundation Optimization Wave O

## 判断

Wave O は最適化済み基盤を公開可能な状態へ固定する検証waveである。実行判断は次の材料で整理する。

- 目的: 起動時の同期stall、公開型の無制限な肥大化、並行所有権の低頻度race、構成ごとの生成物driftを公開前に検出する。
- 期待効果: 既存の描画品質や永続formatを変更せず、起動作業をUI応答可能な境界へ分割し、ABI・stress・全構成を機械判定できる。
- 依存関係: 既存のEditor ABI段階起動、CTest、単一header生成、規約・参照・Module監査、foundation性能JSON集約器を再利用する。
- 検証可能性: 段階数と所要時間log、公開型のcompile-time上限、別process反復、clean Debug/Releaseと有効backend、生成物hashで判定する。

## T77: 起動経路

Editor rendererは既存の28段階startup state machineを維持する。各pumpで完了済みの1段階だけを進め、shader compileはbackendが対応する非同期compileまたはCPU workerへ委譲し、RHI公開はowner threadで行う。各段階と合計時間は既存logへ記録される。

追加の無条件遅延初期化は採用しない。水面はsceneにwater componentが存在した後だけshaderとconstant-buffer ringを初期化済みである。volumetric cloudもPerspectiveかつcoverageが有効な場合だけcompileし、不要になった未公開candidateを破棄する。stack traceのsymbol resolverも最初のcaptureまで未初期化であり、通常起動から除外済みである。これ以上のRHI作業を任意workerへ移すにはrender-thread所有権protocolが必要なため、本waveで推測に基づく移動は行わない。

## T78: 公開型レイアウト予算

`foundation_public_layout_tests.cpp` は最適化で扱うcontainer、task、job、subscription、timer、diagnosticsのWin64サイズとalignmentに上限を置く。上限は機能を追加できない厳密一致ではなく、hot descriptorのword数とinline storageの設計容量を守る予算である。ReleaseとDebugの両方でcompileされるため、構成依存のfield増加も検出する。

## T79: 所有権stress

`run_foundation_stress.py` はWave B専用実行ファイルを既定32回、毎回新しいprocessで起動する。これによりglobal thread poolの初期化・終了、owned callableの一度だけの破棄、MPMC/SPSC順序、nested publish/cancel、timer generation、atomic file置換をprocess世代ごとに反復する。runner自体にも成功・失敗・出力上限のself-testを置く。

Publish中に追加された購読者が、未走査の空きslotへ再利用され同じ配送へ混入する契約違反も統合レビューで修正した。Publish中は新規購読を末尾へ追加し、発行開始時点の集合を固定する回帰テストをWave Bへ追加している。

## T80: 公開前ゲート

最終公開は新規build directoryで行い、次をすべて満たすまで実施しない。

1. DebugとReleaseの全target buildおよび全CTest。
2. raw DX12とDiligentの構成・compile・該当CPU test。
3. 規約、変更C++、参照型名、Module source、単一header、amalgamation drift、配布header構文。
4. foundation end-to-end JSON、stress、公開型layout、package生成。
5. `origin/main` drift確認、非force統合、remote SHA確認。
6. 公式single-header scriptによる `C:\acs` 配布とconsumer構文確認。

## 現在のfocused検証

- Release: layout、runner self-test、32 process ownership stressが3/3成功。
- Debug: layout、runner self-test、32 process ownership stressが3/3成功。
- Win64実測サイズ: `TArray<u32>` 32、`FString` 32、`THashMap<u32,u32>` 64、`FTask` 24、`FJobHandle` 16、`FJobGraph` 4032、`FSubscriptionHandle` 12、`FTimerHandle` 8 byte。
- 診断型の実測サイズ: thread pool 96、job graph 40、timer 24、file 24 byte。

最終結果は全wave統合後のclean buildで更新する。Temp配下のfocused buildが出す`MSB8029`は中間directory位置の警告であり、最終build・実行・artifact検証を省略する根拠にはしない。
