# 複数ログ通知先の契約

## 目的

`FLogger::SetSink` はエディタ連携との互換性を保つ単一通知先である。
追加の診断利用者は `SubscribeSink` または `SubscribeSinkOwned` を使い、
同じ `FLogger` の writer スレッドからログ本文を受け取る。
履歴保存、category、統計、一括通知はこの契約に含めない。

## 寿命と容量

- 購読表は `FLogger::Init` で4096枠を一括確保し、`Shutdown` で解放する。
- 購読表の確保に失敗しても従来の Logger は起動し、複数通知先の登録だけが失敗する。
- 登録、解除、照会、通知では追加の動的確保を行わない。
- `user` は非所有であり、成功した外部 `UnsubscribeSink` または外部 `Shutdown` の
  復帰まで登録側が有効に保つ。
- `FLogSinkSubscription` は移動専用で、破棄または `Reset` により購読を解除する。

callback の `severity` は `Write` 時にリングのレコードへ保存した値である。
`message` は writer が所有するリング内の null終端コピーで、callback の実行中だけ有効である。
履歴や別スレッドで保持する利用者は callback 中に本文を自分の領域へコピーする。

## 順序と変更

各レコードでは従来の `SetSink` を先に呼び、続けて複数通知先を登録順に呼ぶ。
callback 中に追加した通知先は次のレコードから呼ぶ。
callback 中に自身または後続を解除した場合、解除対象は現在の巡回から除外する。

callback 外からの解除は、その購読の実行中 callback が完了してから戻る。
別スレッドからの `Shutdown` も writer の巡回完了を待ってから購読表を解放する。
callback 内からの `Init`、`Shutdown` は writer の自己待機を避けるため無視される。

## ハンドル

`FLogSinkHandle` は4byteの枠番号と4byteの世代番号からなる。
解除後、または `Shutdown` と再初期化をまたいだ古いハンドルは拒否する。
最大世代を発行した枠は、その購読解除後または active のまま `Shutdown` された後に恒久退役し、
世代を0へ循環させない。

`TryCopySinkHandles` は登録順の全ハンドルをコピーする。
購読数が0でも `output` は非 null が必要である。容量不足、整列違反、容量の byte 換算または
終端アドレスの桁あふれ、`output` の宣言範囲と `output_count` の4 byte領域の1 byte以上の重複を
検出した場合、出力配列と要素数はどちらも変更しない。成功時は registry の共有 lock 内で
登録順 snapshot を書き終え、最後に件数を確定する。
