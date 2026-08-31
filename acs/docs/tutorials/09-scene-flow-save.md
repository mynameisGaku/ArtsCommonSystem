# 09 シーン遷移と保存

`FSceneManager` はシーンスタックを管理し、`ChangeScene`、`PushScene`、`PopScene` の要求を走査後の安全な境界で適用します。`FGame::TransitionTo` はフェードの中点で次のシーンへ切り替えます。

`FGameFlow` はゲーム全体の状態遷移を管理します。

```cpp
acs::game::FGameFlow flow;
flow.Init(acs::game::EFlowState::Loading);
if (flow.CanTransitionTo(acs::game::EFlowState::Gameplay))
{
    flow.RequestTransition(acs::game::EFlowState::Gameplay, 0.0f);
}
flow.Tick(0.0f);
flow.Tick(0.0f);
```

不正遷移と遷移中の追加要求は無視されます。`RequestTransition` の結果は、長寿命の所有者が毎フレーム `Tick` を呼んで進行させます。フェードが 0 の場合も、最初の `Tick` で状態を切り替え、次の `Tick` でフェードイン段階を完了します。

小さく、`IsTriviallyCopyableV<T>` を満たすデータには `TSaveSlot<T>`、構造化データには `FSaveArchive` を使います。`TSaveSlot<T>` の条件は POD ではなく `IsTriviallyCopyableV<T>` です。

保存では一時ファイルへ完全な内容を書き、検証後に正本へ公開します。読み込み失敗時は既存のゲーム状態を部分的に置き換えません。`FSettings::TryLoad` はファイル形式、型タグ、件数、文字列長、重複キーなどを検証してから現在値を置き換えます。ゲーム固有のキー形式や数値範囲は呼び出し側が検証します。`FSettings` の各 `Set` API はキー文字列を複製せず保持し、`FSettings::SetString` は文字列値も複製せず保持します。キーと文字列値には、登録項目を削除するか `FSettings` を破棄するまで有効な文字列リテラルまたは長寿命のバッファを渡します。読み出しや保存の完了前に、その記憶領域を破棄してはいけません。

[次章: エフェクト、光源、ステンシル、文字](10-effects-light-stencil-text.md)
