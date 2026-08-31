# ECS ソース構成

ECS の公開型は役割ごとのヘッダーに分ける。状態と寿命を持ってワールドへ遅延操作を適用する `CEntityCommandBuffer` と `CParallelEntityCommandBuffer` は `C` を正本名とし、旧 `F` 名は互換別名として残す。

`FComponentOps` はコンポーネント操作を運ぶ値なので `ComponentOps.h` に置く。`FSparseSetBase` は型を隠した集合の共通処理なので `SparseSetBase.h/.cpp` に置き、`SparseSet.h` は `TSparseSet<T>` だけを持つ。テンプレートに依存する実装はヘッダーに残す。

通常の反復中は `CEntityCommandBuffer`、並列反復中は `CParallelEntityCommandBuffer` に構造変更を記録し、反復が終わってから `Flush()` を呼ぶ。
