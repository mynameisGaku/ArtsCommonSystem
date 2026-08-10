# Cinematic asset persistence

## Format

`.cine` は magic、version、header size、flags、イベント数、明示 duration、イベント領域サイズを持つ little-endian バイナリです。イベント数はACSの上限4096件、Dialogue/MusicのUTF-8 textは65535 bytesです。version 1 以外、未定義 flags、予約値、truncated、trailing、サイズ上限超過、NaN/Inf、並び順違反、未知種別、種別ごとの payload 違反、無効 UTF-8、埋め込み NUL を拒否します。

同時刻イベントと同一内容イベントは入力順を保持します。duration は有限で 0 以上、空イベントでも正数を保存でき、最後のイベント時刻未満は拒否します。

## Ownership and loading

`ACinematicAsset` は検証済みイベント列と duration を所有する不変アセットです。`FCinematicCodec` はメモリ上の bytes を canonical に encode/decode し、`CCinematicAssetLoader` と `CAssetRegistry` は既存のファイル読み取り経路を .cine へ接続します。失敗時はアセットを公開せず、成功時だけ id と Ready 状態を設定します。filesystem path への書き込み、atomic publication、destination の reparse policy はこのAPIの責務外です。


## Legacy bridge

`CCinematicPlayer` はアセットを strong owner として保持し、全イベントを一時配列へ検証・構築してから Director を一括置換します。Dialogue と Music の文字列はアセット所有領域を参照し、player と同じ寿命を保ちます。有限値、順序、duration をそのまま表現できない入力は置換せず、既存の再生状態を変更しません。
再生中はcallback列の処理が終わるまでアセットの強参照を保持し、処理中のClear後も同じ列のpayloadを有効にします。
