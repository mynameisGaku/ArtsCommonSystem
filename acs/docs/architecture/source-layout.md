# ソース配置

ACSは責務ごとにモジュール、エディター、ツール、テスト、文書を分離します。

```text
acs/
├─ engine/                 CMake の入口とモジュール有効化
├─ src/                    C++ モジュール
│  ├─ foundation/
│  ├─ render/
│  ├─ gameframework/
│  └─ <module>/
├─ editor/AcsEditor/       WPF Editor
├─ tools/                  ACS の CLI と生成ツール
├─ tests/                  C++ とスクリプトの検証
├─ scripts/                監査、生成、保守スクリプト
└─ docs/                   ACS ドキュメントとリファレンス
```

## C++ モジュール

各モジュールは `src/<module>/` に置き、次のファイルでビルド境界を定義します。

| ファイル | 役割 |
|---|---|
| `<Name>.Build.cs` | モジュール定義の編集元 |
| `Module.cmake` | CMakeが読み込むソース、ヘッダー、依存関係 |
| `<Type>.h` | 公開または内部の型宣言 |
| `<Type>.cpp` | 非テンプレート実装 |
| `<Type>.inl` | ヘッダーから分けるテンプレート実装 |

`Module.cmake` は `acsbuild` で生成します。依存関係を変える場合は `.Build.cs` を更新し、生成結果との整合を `acs_buildcs_check` ターゲットで確認します。

## 型とファイル

公開 `class`、`struct`、`enum` は原則として1主要型1ヘッダーにします。実装を持つ主要型は同名の `.cpp` へ分けます。小さな結果型や、その主要型だけが利用する列挙は同じヘッダーに置けます。

公開ヘッダーは次の順で開始します。

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
```

ヘッダー冒頭には長い説明や使用例を置かず、型と関数の直前に日本語で役割、入力、失敗条件を記述します。

## 責務の境界

- 複数モジュールで共有する基本型だけを `foundation/` に置きます。
- 所有権と割り当ては `memory/`、汎用データ構造は `container/` に置きます。
- ゲーム固有のシーンや進行機能は `gameframework/` に置きます。
- OS APIは `platform/`、GPU APIは `render/` のバックエンド境界へ閉じ込めます。
- 外部ランタイムの実装は個別モジュールへ置き、Game Frameworkにはインターフェイスまたはスタブを置きます。
- Editor 固有の C# 型は `editor/AcsEditor/`、C ABI だけを `src/editor_abi/` に置きます。

## ドキュメント

目的別の説明文書は `docs/` の対応するサブディレクトリへ置きます。機能・API リファレンスは `docs/reference/source/` を正本として生成し、公開される HTML と内容を一致させます。
