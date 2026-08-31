# ArenaAllocator の利用

`FArenaAllocator` は、同じ寿命の小さな領域をページ単位でまとめて管理します。個別の
`Free` は行わず、`Reset` で一括して無効化します。

`AllocBatch` は、同じ大きさと整列幅の領域を一回のカーソル更新で予約します。成功時の
`BytesAllocated` と `AllocationCount` は、領域を個別に確保した場合と同じ値になります。
失敗時は出力配列をすべて `nullptr` に戻し、統計値を変更しません。

`Reset(false)` は世代番号を進め、先頭ページだけを直ちに初期化します。残りのページは
再利用する直前に初期化するため、通常のリセット時間は保持ページ数に比例しません。
`Reset(true)` は全ページを確保元へ返します。

`Diagnostics` は、保持ページ数、バッチ確保回数、直前のリセットで参照したページ数、
遅延初期化したページ数を返します。公開レイアウトは `FArenaAllocator` が 120 バイト、
`FArenaAllocatorDiagnostics` が 40 バイトで、どちらも整列幅は 8 バイトです。

専用テスト `ACS.ArenaAllocatorAbi` は Debug と Release の両方で、公開レイアウト、
バッチ確保の失敗時契約、世代リセット、遅延ページ再利用を確認します。
