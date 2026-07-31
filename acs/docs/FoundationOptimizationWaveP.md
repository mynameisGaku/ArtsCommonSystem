# Foundation Optimization Wave P

## 判断

Wave P は、並列waveで追加した高速化と安全性を公開可能な一つのrevisionへ閉じる統合waveである。
本書の各節は目標契約を表し、`EngineFoundationOptimization80.md` の状態が`Complete`になるまでは
統合レビュー中として扱う。

- 目的: 入力境界、再入、世代番号、backend失敗、公開ABI、生成物closureに残る低頻度欠陥を公開前に除去する。
- 期待効果: 高速経路を維持したまま、長時間運用と失敗注入でも古いhandleやclosureが別の登録へ化けず、配布物だけを使うconsumerも同じ契約を得る。
- 依存関係: 既存のMemory、Module、CTest、型役割監査、Raw/Diligent backend、single-header生成、配布consumer、end-to-end証跡を再利用する。
- 検証可能性: Debug/Release、失敗注入、負のcompile probe、公開layout/symbol、全backend、生成物hash、物理的に異なる二つの配布treeで判定する。

## T81-T82: OS境界

Networkはnull、0 byte、WinSockの`int`上限をOS呼び出し前に分類する。TCPの0 byteはOSを呼ばず、UDPの空datagramは非null dummyを使ってOSへ渡す。`RecvFrom`は成功時だけ送信元をcommitし、port 0の実endpointは`LocalAddress`で取得する。

FileSystemは通常pathを`CreateDirectoryW`へ渡し、`ERROR_ALREADY_EXISTS`の直後に実directoryであることを再確認する。特例はdrive root、構造的に正しいUNC share root、extended drive rootだけである。通常file、中間file、device namespace、壊れたUNCを成功扱いしない。

## T83-T84: Lua登録

`FLuaVm::RegisterNativeFunction`は、登録簿を変更する前から同一VMへの再入を拒否する。Lua stackと再入状態はRAIIで全return経路から戻す。公開失敗時は登録簿末尾と同名globalの復元を試みる。

closureは登録位置だけでなく登録識別番号を持つ。失敗中にLua側へ退避されたclosureは後続登録を呼べない。公開確認は`__index`を通さないraw global lookupで行う。識別番号を使い切った後はwrapさせず、新規登録を恒久拒否する。

## T85-T88: EventとTimer

BrokerとTimerのgenerationは最大値を一度使った後に飽和し、古いhandleへ再一致する新規登録を拒否する。Brokerは購読数と解除の定数時間経路を維持し、異なるchannelを跨ぐnested publishから`Clear`されても参照中の領域を解放しない。

Timerはactive bitsetと定数時間cancelを維持する。callbackが同じwordを変更した後はwordを再読込し、追加timerは次のtickから処理する。

TypedEventとdelegateはrvalue referenceを拒否し、値引数のcopy可能性と実際のstatic/raw呼び出しの`noexcept`をcompile時に検査する。負のcompile probeが期待した`static_assert`で失敗することもgateに含める。

## T89-T90: Audio

volumeは全公開経路で共通の正規化を通す。有限値は`[0,1]`へclampし、NaNと正負の無限大は0にする。

Play中のbackend volume設定が失敗した場合は、確保したvoiceとbufferを破棄して無効handleを返す。既存voiceまたはmasterの更新失敗は以前のbackend値を保ち、warningと診断値で観測可能にする。test hookはtest buildだけへ入り、production layoutとsymbolを変えない。

## T91-T94: 規約、Module、ABI

型役割監査は`F=value/handle/service`を一般則として扱い、純粋仮想構文や`Shutdown`の存在だけでserviceを`I`や`C`へ変えない。EventとScriptingを実sourceで走査し、fixtureだけの成功にしない。

公開headerが参照するmoduleは`PUBLIC_DEPS`へ含める。Lua C APIを含む内部headerは公開header一覧とsingle-headerから除外する。generator入力と生成`Module.cmake`は同じsource closureを表す必要がある。

公開型の正規名、decorated symbol、size、alignment、vtable順序を既存配布revisionと比較する。安全性に必要なlayout変更は隠さず、事前build済みconsumerの再build要件を文書化する。

## T95-T96: RenderとEditor

Raw DX12とDiligentのDebug/Releaseで、transient upload arenaのpage境界をGPU readbackまで検証する。draw sortはNaN、無限大、denormal、正負0、同値の提出順を含む全float領域で決定的にする。

Editorとpackage consumerは、正規SHA-256 keyだけをbounded LRUへ保持する。entry数とUTF-16 code unit数の二重上限、thread ownership、path containment、reparse拒否、allocation削減をDebug/Releaseで検証する。

## T97-T100: 公開

全sourceが固定した後に正規generatorを一度だけ実行し、single-header、Debug/Release library、任意backend依存を再生成する。source外consumerは両構成で構文、link、実行まで通す。

end-to-end証跡はfileをopenした対象そのものの前後metadataを比較し、reparseを辿らず二回同一scanを得る。SUBSTなど同じ物理directoryのaliasは二つの配布treeとして数えない。

公開はreview済みfileだけをcommitし、remote mainのSHAを確認してから最終配布rootへmirrorする。生成元と配置先はrelpath、size、SHA-256が完全一致しなければ成功にしない。

## 統合証跡

各taskの最終状態、構成別test数、公開ABI差分、配布manifest hashは、全並行差分を固定した後の最終集約で追記する。途中のpatch適用成功やdirty worktreeのbuildは公開証跡として数えない。
