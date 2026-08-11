# acsbuild — ACS Build Tool

acsbuild は `src/<mod>/<Name>.Build.cs` を読み取り、CMake が取り込む
`Module.cmake` を生成する ACS の build module 管理 tool です。

## 責務

- `AcsModule` 派生型を検出し、module ごとの依存、feature、guard を読み取ります。
- module directory を再帰走査し、source と header を収集します。
- 収集結果を `acs_module()` 形式の `Module.cmake` へ出力します。
- 生成予定内容と tracked `Module.cmake` の差を、file を変更せずに検査します。

source 一覧は生成時に収集されるため、`Build.cs` は module の依存関係と build 条件を
所有します。自動収集から除外する file と追加する file は module 定義が明示します。

## Module 定義契約

各 `src/<mod>/<Name>.Build.cs` は、module 名と一致する `AcsModule` 派生型を1つ定義します。
module directory 名は module 名の小文字表記です。入力 property と生成先の対応は次の通りです。

| `AcsModule` 入力 | `Module.cmake` 出力 | 責務 |
|-------------------|----------------------|------|
| `Type` | `TYPE` | module の runtime/developer 区分 |
| 自動収集結果 | `SOURCES` / `HEADERS` | module directory 内の build 対象 |
| `PublicDeps` | `PUBLIC_DEPS` | consumer へ公開する module 依存 |
| `PrivateDeps` | `PRIVATE_DEPS` | module 内部だけで使う依存 |
| `PublicLibs` | `LINK_PUBLIC` | consumer へ伝播する library |
| `PrivateLibs` | `LINK_PRIVATE` | module 内部だけで link する library |
| `Feature` | `acs_module_feature` | build option と compile definition の対応 |

## CLI 契約

- `gen` は検出した全 module の `Module.cmake` を更新します。
- `--module <name>` は処理対象を1 module に限定します。
- `--root <path>` は ACS root を明示し、省略時は `src` と `engine` を持つ親 directory を探索します。
- `--check` は file を更新せず生成予定内容と tracked file を比較し、差があれば exit code 1 を返します。
- root または module 定義を解決できない場合は診断を stderr へ出し、exit code 2 を返します。

`acs_buildcs_check` は `Build.cs` と `Module.cmake` の一致を検査する CMake target です。
dotnet が利用可能な場合だけ登録され、通常 build の既定 target には含まれません。

`acs_module_sources_check` は assembled module の条件付き source を含め、全 `.cpp` が
module manifest に登録されていることを検査します。source 監査の self-test は正常入力と
異常入力を source tree の変更なしで検証します。`editor_abi` は engine CMake と editor の
project generator が明示管理し、header の internal/public 区分は別の公開契約が所有します。

## 生成処理

`AcsBuild.csproj` は `src/**/*.Build.cs` を tool へ取り込み、C# runtime の型列挙機能で
全 `AcsModule` 派生型を検出します。各 module directory の `*.cpp` と `*.h` を収集し、`acs_module()` が
受け取る `Module.cmake` を出力します。CMake 側の module 解決は
`engine/cmake/ACSModuleSystem.cmake` が担当します。

単純な directory 走査では表せない build 条件は、次の入力が所有します。

- `Guard` は module 全体を有効化する CMake 条件を出力します。
- `Preamble` は `acs_module()` より前に必要な CMake 宣言を出力します。
- 条件付き group は条件ごとの subdirectory source と link 依存を出力します。
- `ExcludeFiles` と `ExtraFiles` は自動収集対象を補正します。

## 整合契約

tracked `Build.cs` が module 定義の唯一の定義元です。tracked `Module.cmake` はその生成結果と
一致する必要があり、`--check` と CMake の検査 target が不一致を失敗として報告します。
