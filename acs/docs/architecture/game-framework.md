# Game Framework の構成

`ACS::GameFramework` は、ACS のゲーム進行、シーン、ノード、コンポーネント、固定更新、保存、入力、ゲーム向け機能をまとめるモジュールです。公開ヘッダーは `src/gameframework/` にあり、機能ごとに独立した型として提供されます。

## 中心となる型

| 型 | 役割 |
|---|---|
| `FGame` | アプリケーション上のゲーム実行、時間倍率、固定更新、シーン管理 |
| `FSceneManager` | シーンスタックと遅延遷移 |
| `FScene` | 1つの画面または状態の基底型 |
| `FScene2D` | `FScene` を拡張した2Dシーン実装 |
| `FScene3D` | GPUに依存しない3Dシーングラフ。`FScene` へ接続する場合は `FLegacyScene3DAdapter` を使う |
| `ANode` | 親子ツリーを構成する所有対象 |
| `AComponent` | `ANode` が所有する振る舞い |
| `FSceneServices` | カメラ、入力、描画などのシーン向けサービス |
| `FSubsystemCollection` | Engine、GameInstance、World のスコープ別サービス管理 |

## シーンのライフサイクル

`FScene` は次のフックを持ちます。

```text
OnEnter
  ├─ OnUpdate
  ├─ OnFixedUpdate
  ├─ OnRender
  └─ OnEvent
OnExit
```

別のシーンが上へ積まれる場合は `OnPause`、再び最上段へ戻る場合は `OnResume` が呼ばれます。`ChangeScene`、`PushScene`、`PopScene` の要求は走査中に構造を壊さないよう遅延して適用されます。

## ノードとコンポーネント

`ANode` は `FTransform3D`、子ノード、コンポーネントを所有します。`SetPosition2D` は位置のZ成分、`SetScale2D` は拡大率のZ成分を保持します。`SetRotation2D` は回転全体をZ軸回転へ置き換えるため、X/Y軸回転は保持しません。

ノードの主なフックは `OnSpawn`、`OnUpdate`、`OnFixedUpdate`、`OnDraw`、`OnDespawn` です。子の追加は `TryAddChild` の結果で失敗理由を確認できます。null、自己追加、循環、既に親を持つ子、親または子の破棄予定、追加後のツリー深度が `kNodeMaxTreeDepth` の512を超える場合は拒否されます。

`AComponent` は所有ノードへ追加し、所有ノードより長く生存しません。外部からノードを保持するときは、所有権を増やさない `TWeakObjectPtr<ANode>` を使用します。

## 時間と固定更新

可変更新はフレームごとの経過時間を受け取ります。固定更新は `FFixedStepClock` が蓄積時間から0回以上のステップを生成し、物理や決定論ロジックを一定間隔で進めます。

- `fixed_dt <= 0` は固定更新を無効にします。
- 1フレームの最大ステップ数で過大な追いつきを制限します。
- `FFixedStepInputBuffer` は表示フレームの入力を固定ステップへ配送します。
- スナップショット型は時計、入力、乱数などの再現に使用します。

詳細は[固定更新クロック](../guides/game-framework/fixed-step-clock.md)と[固定更新入力バッファ](../guides/game-framework/fixed-step-input-buffer.md)を参照してください。

## サービスとサブシステム

`FScene::WantedServices` はシーンが必要とするサービスを `ESvc` で宣言します。宣言されていないサービスへ `Services()` でアクセスすると、アサーションにより契約違反を検出します。

サブシステムは次の順で検索されます。

```text
World → GameInstance → Engine
```

複数の利用者が共有し、明確な所有者、寿命、更新または終了処理を持つ機能だけをサブシステムとして登録します。局所状態や単純な計算は、利用する型のメンバまたは値型として保持します。

## 機能群

Game Framework には次の機能群があります。

- シーン、ノード、コンポーネント、プレハブ、リフレクションの基盤
- 入力マップ、入力記録、固定更新入力
- 衝突、トリガー、剛体、タイルマップの処理
- アニメーション、トゥイーン、シーケンス、カメラ、エフェクト、水面の表現
- 保存、設定、シーンのシリアライズ、リプレイ、ロールバックの永続化
- インベントリ、戦闘、進行、会話などのゲーム進行機能
- 音声、ローカライズ、アクセシビリティ、プライバシーの支援機能
- Steamworks、スクリプティング、OpenXR、ML、バックエンドの差し替え境界
- Studio WorkflowとEditor向けツール型

個別の型、関数、メンバ、定数は[機能・API リファレンス](../reference/index.html)から参照できます。

## 失敗の扱い

読込、保存、ネットワーク、バックエンドの検査済みAPIは、原則として `TResult<T>` または機能固有の結果型で失敗理由を返します。戻り値、出力引数、失敗時の変更範囲はAPIごとの契約を正本とします。詳細な入力制限は[安全契約](../safety/README.md)に分離されています。
