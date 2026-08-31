# 旧 Kit 境界監査

## 目的

`audit_kit_absorption.py` は、ACS 内へ独立した Kit のディレクトリ、名前空間、
include、C++ module 境界が再導入されることを防ぎます。該当機能は責務に対応する
ACS の module または機能フォルダーへ配置し、別の Kit 境界を作りません。

## 監査範囲

指定した ACS root が `engine/CMakeLists.txt` と
`scripts/audit_cpp_conventions.py` を持ち、`src` と `tests` を含むことを最初に確認します。
既定では `src`、`tests`、`tools`、`editor` を走査します。

対象は C++ の header、source、inline、module interface です。通常の C++ 拡張子に加えて
`.ccm`、`.cppm`、`.cxxm`、`.mpp`、`.mxx` を監査します。独立した `Kit`
ディレクトリは、実ディレクトリ、symbolic link、Windows junction のいずれも検出します。
reparse directory は名前を検査してから走査対象外とし、リンク先を ACS の一部として
再帰しません。

`.git`、`.vs`、`Binaries`、`Intermediate`、`Saved`、`ThirdParty`、`third_party`、
`cmake-build-*` は生成物または外部依存として除外します。C# など C++ 以外のファイル内容は
解析対象外ですが、監査範囲内にある独立した `Kit` ディレクトリは言語に関係なく検出します。

C++ のコメント、文字列、文字 literal、raw string にある一致は除外します。前処理 directive、
行継続、global module fragment を考慮し、通常の式にある `module.kit` や `import.kit` を
module 宣言または import として扱いません。

## 診断規則

| 規則 | 検出内容 |
|---|---|
| `ACS-KIT000` | ACS marker、必須 scope、または走査可能な root がない |
| `ACS-KIT001` | 独立した `Kit` ディレクトリ、symbolic link、または Windows junction がある |
| `ACS-KIT002` | `kit::` または `using namespace` で Kit を参照している |
| `ACS-KIT003` | `namespace kit`、`namespace acs::kit`、または Kit を指す namespace alias がある |
| `ACS-KIT004` | Kit header の include/header unit、または Kit の module 宣言・import がある |
| `ACS-KIT005` | C++ ファイルを UTF-8 として読めない、またはディレクトリ走査に失敗した |

root 検査、読み込み、走査の失敗は成功扱いにせず、診断を返して終了値を 1 にします。
JSON は各診断の `rule`、`path`、`line`、`column`、`message` を保持します。
JSON の生成または原子的な置換に失敗した場合は一時ファイルを削除し、既存の出力先を
変更しません。

## CLI と自己検査

```powershell
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs --format json
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs --json-output $env:TEMP\acs-kit-absorption.json
python -X utf8 .\acs\scripts\audit_kit_absorption.py --self-test
```

自己検査は、Kit の名前空間、include、header unit、module 宣言、行継続、除外ディレクトリ、
symbolic link、Windows junction、非 UTF-8 入力、JSON 保存失敗、不正な root を一時 fixture で
確認します。

## CMake と CTest

生成済み build directory では、監査 target と CTest を個別に実行できます。

```powershell
cmake --build <build-directory> --config Debug --target acs_kit_absorption_check
ctest --test-dir <build-directory> -C Debug --output-on-failure -R "^ACS\.KitAbsorptionAudit"
ctest --test-dir <build-directory> -C Release --output-on-failure -R "^ACS\.KitAbsorptionAudit"
```

`acs_kit_absorption_check` と `ACS.KitAbsorptionAudit` は ACS tree を監査します。
`ACS.KitAbsorptionAuditSelfTest` は監査器の検出規則と失敗時の扱いを確認します。
