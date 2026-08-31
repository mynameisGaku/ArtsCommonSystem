# StudioWorkflowの構成

スタジオ作業機能は `GameFramework` モジュールの `studio` 配下に置く。

| 分類 | 公開する型 |
|---|---|
| 共通値 | `FStudioWorkflowError`、`FAssetLockInfo` |
| 接続先 | `IAssetLockingBackend`、`IBuildFarmBackend` |
| 空実装 | `FAssetLockingStub`、`FBuildFarmStub` |
| ローカルロック | `ELocalAssetLockError`、`FLocalAssetLockToken`、`FLocalAssetLockResult`、`FLocalFileAssetLocking` |
| ローカルビルド | `FLocalBuildRunner` |
| 共有取得 | `GetAssetLockingStub`、`GetBuildFarmStub`、`GetLocalFileAssetLocking`、`GetLocalBuildRunner` |

既存コードは `gameframework/StudioWorkflow.h` をそのまま利用できる。
新規コードは必要な型と同名のヘッダーを直接インクルードする。
内部の文字変換と数値変換は `studio/detail` に置き、公開APIには含めない。
