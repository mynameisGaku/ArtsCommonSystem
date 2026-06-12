# acsbuild — ACS Build Tool

UE の `*.Build.cs` (ModuleRules) 風にモジュールを **C# で定義**し、そこから
`Module.cmake` を**生成**する薄いツール。CMake はそのままビルドバックエンドとして残る。

UE の UnrealBuildTool が `*.Build.cs` を集めて評価するのと同じ役割を、最小構成で果たす。

## なぜ

従来は各モジュールを `src/<mod>/Module.cmake` の `acs_module(...)` で手書きしていた。
これは UE 風のモジュール分割そのものだが、**ソース一覧を手で維持する**必要があった
(新しい `.cpp` を足すたびに `Module.cmake` の `SOURCES` に追記)。

acsbuild は UE と同様に **ソース/ヘッダをディレクトリ走査で自動収集**するので、
`.Build.cs` には依存と feature だけ書けばよい。新しいファイルは再生成で自動的に拾われる。

## モジュールの書き方

`src/<mod>/<Name>.Build.cs` (クラス名 = モジュール名、その小文字 = ディレクトリ名):

```csharp
using Acs.Build;
namespace Acs.Modules;

public sealed class Event : AcsModule
{
    public Event()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Container", "Threading" });
        // ソース/ヘッダは src/event を走査して自動収集される (手書き不要)。
    }
}
```

feature 付き・外部ライブラリ付きの例:

```csharp
public sealed class Math : AcsModule
{
    public Math()
    {
        PublicDeps.AddRange(new[] { "Foundation", "Threading" });
        Feature("AVX2", "MATH_AVX2", def: true, desc: "Compile AVX2 fast paths");
        // PublicLibs / PrivateLibs で LINK_PUBLIC / LINK_PRIVATE を宣言。
        // ExcludeFiles / ExtraFiles で自動収集の調整 (条件付きソース等)。
    }
}
```

`AcsModule` のプロパティ ↔ `acs_module()` 対応:

| C# (`AcsModule`)         | `Module.cmake`        | UE (`ModuleRules`)                 |
|--------------------------|-----------------------|------------------------------------|
| `Type`                   | `TYPE`                | `Type`                             |
| (自動収集)               | `SOURCES` / `HEADERS` | (UBT が自動収集)                   |
| `PublicDeps`             | `PUBLIC_DEPS`         | `PublicDependencyModuleNames`      |
| `PrivateDeps`            | `PRIVATE_DEPS`        | `PrivateDependencyModuleNames`     |
| `PublicLibs`             | `LINK_PUBLIC`         | `PublicAdditionalLibraries`        |
| `PrivateLibs`            | `LINK_PRIVATE`        | `PrivateAdditionalLibraries`       |
| `Feature(...)`           | `acs_module_feature`  | `bWithX` 等のビルドフラグ          |

## 使い方

```bash
# 全モジュールの Module.cmake を生成 (上書き)
dotnet run --project tools/acsbuild -- gen

# 1 モジュールだけ
dotnet run --project tools/acsbuild -- gen --module Event

# 生成結果と既存 Module.cmake を突き合わせ (上書きしない。CI 向け、差分があれば exit 1)
dotnet run --project tools/acsbuild -- --check

# リポジトリルートを明示する場合
dotnet run --project tools/acsbuild -- gen --root C:\path\to\acs
```

CMake からも検証だけ回せる (dotnet がある環境で `acs_buildcs_check` ターゲットが生える。
ALL_BUILD には含まれないので通常ビルドには影響しない):

```bash
cmake --build <build> --target acs_buildcs_check
```

ワークフロー: `.Build.cs` を編集 → `acsbuild gen` → `Module.cmake` 再生成 →
CMake 構成 → ビルド。`--check` を CI に置けば「`.Build.cs` と `Module.cmake` の
ズレ」を検出できる。

## 仕組み

`AcsBuild.csproj` が `src/**/*.Build.cs` を本ツールへコンパイル取り込みし、reflection で
`AcsModule` 派生をすべて発見する (UE の UBT が Build.cs を集めるのと同じ)。各モジュールの
ディレクトリを再帰走査して `*.cpp` / `*.h` を集め、`acs_module(...)` 形式の `Module.cmake`
を出力する。CMake 側 (`engine/cmake/ACSModuleSystem.cmake`) は一切変更不要。

## 条件付き・ゲート・preamble

UE と違い単純グロブで表現できないものは以下で対応する:

- **Guard** (`Guard = "ACS_BUILD_XXX"`) — 先頭に `if(NOT XXX) return() endif()` を出力
  (gated module: CrashWin / Scripting / Steamworks / OpenXr / MlOnnx / LocalMatch / TelemetryFile)。
- **Preamble** (`Preamble = @"..."`) — `acs_module()` の前に生 CMake を出力
  (third_party fetch: Asset / Render、外部 target 構築: Imgui)。
- **条件付きグループ** (`When("COND").SubdirSrc("Dx12").LinkPrivate(...)` 等) — CMake の
  `if(COND) list(APPEND ...) endif()` を生成し、`acs_module(SOURCES ${var} ...)` で組み立てる
  (バックエンド分岐: Render の Dx12/Diligent、Mvvm の ImguiBindings)。
- **ExcludeFiles / ExtraFiles** — 自動収集の調整。

## 現状

**全 28 モジュールが `.Build.cs` 化済み** (engine 全体)。`--check` で既存と突き合わせ、
ソース・依存・リンクは全モジュール一致 (Foundation の `Cast.h` と GameFramework の btedit
3 ヘッダはグロブが拾った「手書き一覧の漏れ」= ドリフト修正)。Diligent backend ON を含む
フルビルド + 150 テスト緑。以後モジュールを足す/ファイルを足すときは `.Build.cs` を編集して
`acsbuild gen` を回す (新規 `.cpp` は再生成で自動収集される)。
