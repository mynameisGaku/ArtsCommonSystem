# 型付きイベント

`acs::TEvent<Args...>` は、既存のウィンドウ入力値 `acs::FEvent` と名前・責務を
分離した同期イベントです。コールバック型は
`void(void* user, Args... values) noexcept` で、動的型消去を必要としません。

## ヘッダー

- `event/TypedEvent.h`: `TEvent`
- `event/TypedEventCallback.h`: `TEventCallback`
- `event/TypedEventHandle.h`: `FTypedEventHandle`
- `event/TypedEventSubscription.h`: `TEventSubscription`

`TypedEvent.h`は利用時に必要な補助型もincludeするため、従来どおり一つのincludeでも
使えます。型の宣言位置を参照する場合は、上記の正規ヘッダーを使います。

## 配信中の変更

- 配信開始後に追加した購読は、次回の `Publish` から有効になります。
- まだ呼ばれていない購読をコールバックから解除すると、その回にも呼ばれません。
- `SubscribeOnce` はコールバックを呼ぶ前に解除されるため、再入配信でも一度だけです。
- priority は大きい値を先に、同値ならslot順に配信します。
- 同じ値を複数の購読へ渡すため、右辺値参照は指定できず、値引数はコピー構築可能である必要があります。

`Publish` は内部状態の強参照を保持します。コールバックがイベント所有者を破棄しても、
進行中の配信が参照する格納領域はコールバック復帰まで生存します。

## ハンドルと所有購読

`FTypedEventHandle` はイベント識別子、slot、世代を保持します。解除後に同じslotが
再利用されても、古い世代のハンドルでは新しい購読を解除できません。

`SubscribeOwned` はムーブ専用の `TEventSubscription<Args...>` を返します。
所有購読の破棄または `Reset` で購読を解除します。イベントが先に破棄された場合は
弱参照が失効するため、所有購読の後始末は何も参照せず安全に終了します。

## 使い分け

- 型が実行時まで不明な疎結合通知には `CMessageBroker`
- ウィンドウ・入力値には `FEvent`
- コンパイル時に引数型が決まる局所通知には `TEvent`

## MessageBrokerの配信中操作

`CMessageBroker`も配信開始時点の購読を一回分の対象にします。コールバックから追加した
購読は、配信前から空き枠があった場合も次回の`Publish`から呼ばれます。まだ呼ばれて
いない購読を解除した場合は、その回から呼ばれません。

コールバックから`Clear`を呼ぶと全購読を直ちに無効化し、残りの処理を止めます。
配信が戻るまでは新しい購読を受け付けず、通路の保持領域は最外側の配信終了時に
解放します。配信終了後は同じ仲介器へ再び購読できます。

`CMessageBroker`とコールバック対象の寿命は、`Publish`が戻るまで呼出し側が保ちます。
別スレッドへ値を渡す用途には`TMessagePipe`を使います。

## 公開名と互換性

`CMessageBroker`と`CTimerManager`が正規の公開型です。旧`FMessageBroker`と
`FTimerManager`は同じ型を指す一時的な`using` source互換名です。型名を含む装飾symbolは
変わるため旧object fileとのABI互換はなく、symbol shimも提供しません。consumerは全量再buildします。

64-bit環境の`CMessageBroker`は既存の40 byte配置を維持します。配信中の`Clear`に
必要な全通路状態は非公開の動的管理領域へ置き、公開オブジェクトへfieldを追加しません。

旧名は、互換性試験と監査規則を除くリポジトリ内の実利用が0件になり、外部consumerの
移行も確認してリリースノートで告知した後の次回major releaseでのみ削除します。
