# Kit 吸収境界の監査と公開

## 目的

Kit の機能は別の基盤として残さず、責務が同じものを ACS の既存モジュールへ統合する。
名前だけが似ていて責務、所有者、寿命、保存形式が異なるものは、役割が分かる別名へ分離する。
ゲーム固有実装は統合対象に含めない。

この文書の監査は、ACS 内へ旧 Kit の C++ 境界や独立ディレクトリが再流入することを防ぐ。
監査結果が 0 件でも、外部 Kit の全機能、挙動、保存値、ABI が移植済みであることは意味しない。
機能ごとの実装、独立レビュー、Debug/Release、外部利用者、配布物の検証を別途完了させる。

## 現在の統合判定

| 領域 | 判定 | 完了条件 |
|---|---|---|
| TypedEvent | 統合中 | clean 統合作業中の typed-event 差分を独立レビューし、Debug/Release と配布物で閉じる |
| Scene timer と Event timer | 責務分離済み | シーン寿命の `FSceneTimerHandle` は4byte、event寿命の `FTimerHandle` は8byteとして、所有者、保存値、0秒登録方針を別契約で固定する |
| Ease の旧数値 ID | 統合中 | `FLegacyKitEaseIdCodec` で固定33値を正規 enum と相互変換し、独立レビュー、Debug/Release、直接・単一ヘッダ利用、配布物の検証後に完了へ移す |
| Random snapshot | `GameFramework`へ責務統合 | 既存 `FRandom` の16byte配置、乱数列、消費順を保ち、定数時間snapshotと検査済みAPIを同じ型へ吸収する |
| Fixed-step | `Timing`へ分離統合 | `FFixedStepClock`の値所有、48byte配置、固定境界、一括不変、Debug/Release、単一header、外部利用を同じclean treeで確認する |
| Input options、Diagnostics | 未統合 | 既存モジュールとの責務比較後、機能単位で実装と試験を移す |

TypedEvent を含む各行は、作業ツリーに関連ファイルが存在するだけでは「吸収済み」へ変更しない。
正式な完了判定は、対象差分と依存差分が同じ clean tree で検証された後に更新する。

## Random の責務統合

Kit のgame randomとACSの`acs::game::FRandom`は、seedから決定論的な値列を生成する同じ責務を持つ。
別の`FRandomStream`、subsystem、mutex、thread-local値は追加せず、既存`FRandom`一つへ機能を吸収する。
既存利用者が依存するxoshiro128**の4状態、`NextU32`列、16byte layout、
`RangeInt`の成功値と1回消費は変更しない。

Kit由来の偏り除去や入力検査は、既存の偏りを許容するAPIを置き換えず、
`TryWeightedIndex`、`TryFillRangeIntUnbiased`、`TryShuffleIndicesUnbiased`という別名で公開する。
検査済み配列APIは最大4,096要素で、pointer、alignment、address加算、乱数器自身との重なりを
全て乱数消費前に確認し、失敗時は状態と出力を維持する。
共有`Global()`は呼び出し側が単一threadへ閉じて使う。

`FRandomSnapshot`は4状態を直接保持し、消費回数に依存せずO(1)で復元する。
版、予約値、全0状態、標準FNV-1a 64bit検査値を復元前に確認する。
この型のnative byte列は永続形式にせず、各fieldを明示したbyte順で符号化する。
外部24byte保存値の利用実績がないため、旧Kit snapshot型や互換aliasは追加しない。

完了判定には、Debug/Releaseの専用test、既存objectと新headerの直接利用、
単一header利用、Kit random回帰、referenceと配布物の同一tree検証を含める。

## 監査対象

`audit_kit_absorption.py` は ACS root 配下の `src`、`samples`、`tests`、`tools`、`editor` を走査する。
次の二種類だけを監査する。

- C++ の header、source、inline、`.ixx`、`.cppm` などの module interface にある Kit namespace、`kit::` 参照、include、module 宣言・import
- 走査 scope 内に作られた独立 `Kit` ディレクトリ。実ファイルを持たないディレクトリ、symlink、Windows junction も含む

`.cs` の内容や managed API 全般は字句解析しない。
ただし `editor/Kit` や `tools/Kit` のような独立ディレクトリは、内部の言語に関係なく検出する。
C# や外部プロジェクトを含む機能移植の完全性は、この監査の主張に含めない。

`ThirdParty`、`Intermediate`、`Binaries`、`Saved`、`.git`、`.vs`、`x64`、`third_party`、
および `cmake-build-*` は生成物または外部依存として降りない。
symlink と Windows の reparse directory は名前が `Kit` なら字句上の位置を記録し、名前に関係なく全て走査対象から外す。
これにより、junction の外部 target やその配下を ACS の一部として再帰走査しない。
コメント、通常文字列、raw string 内の疑似コードは C++ 字句解析で除外する。
backslash-newline は C++ translation phase 2 と同じ前処理で除去し、診断位置は元の物理行・列へ対応させる。
`module.kit` や `import.kit` という通常式は module 宣言ではないため検出しない。
preprocessor directive は長さと改行を維持した空白へ置き換え、global module fragment 後の宣言開始位置を壊さない。

## 診断規則

| 規則 | 検出内容 |
|---|---|
| `ACS-KIT000` | 指定 root が存在しない、ACS marker・必須 scope がない、または走査できない |
| `ACS-KIT001` | 独立した `Kit` ディレクトリ、symlink、または Windows junction |
| `ACS-KIT002` | `kit::` 参照または `using namespace` による Kit 参照 |
| `ACS-KIT003` | `namespace kit`、`namespace acs::kit`、または Kit を指す namespace alias |
| `ACS-KIT004` | `Kit/`、`Kit\`、`Kit.h` の include/header unit、または任意 component が `kit` の module 宣言・import |
| `ACS-KIT005` | C++ 境界ファイルを UTF-8 として読み取れないか、ディレクトリ走査に失敗した |

読み取り失敗は traceback で監査を中断せず、`ACS-KIT005` として fail-closed に扱う。
JSON にも同じ規則、path、line、column、message を保存する。
JSON の生成または原子的な置換に失敗した場合は一時ファイルを削除し、既存 destination を維持する。
存在しない root、空 root、ACS 以外の root は成功扱いにせず、`ACS-KIT000` と終了値 1 を返す。

## ローカル検証

```powershell
python -X utf8 .\acs\scripts\audit_kit_absorption.py --self-test
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs --format json
python -X utf8 .\acs\scripts\audit_kit_absorption.py --root .\acs --json-output $env:TEMP\acs-kit-absorption.json
```

self-test は、次の回帰を一時 tree で確認する。

- `Kit.h`、slash と backslash の include
- `kit::`、nested namespace、namespace alias、`using namespace`
- `.ixx` と `.cppm` の Kit module 宣言、`import kit;`、`import acs.kit;`
- `import "Kit.h";` と `import <Kit/Timer.h>;` の header unit
- keyword と header 名を分断する line splice、header 名内部の line splice、継続した line comment
- 通常式 `module.kit` と `import.kit` を module 宣言として扱わないこと
- `module;` 後に `#define` と `#include` がある場合も後続 `export module kit` を検出すること
- `tools/Kit`、`editor/Kit`、symlink または Windows junction の字句 path
- Kit 以外の junction 配下にある外部 tree を走査しないこと
- `ThirdParty`、`Intermediate`、`cmake-build-*` の除外
- 非 UTF-8 ファイルの `ACS-KIT005` と JSON 保存
- JSON dump/replace 失敗時の一時ファイル削除と既存 destination 維持
- 存在しない root、空 root、誤 root の `ACS-KIT000`

CMake を生成した tree では、次の target と CTest を利用できる。
登録コマンドは `PYTHONUTF8=1` と `PYTHONIOENCODING=utf-8` を明示し、成功 summary は MSBuild でも崩れない英数字で出力する。

```powershell
cmake --build .\acs\Intermediate\vs --config Debug --target acs_kit_absorption_check
ctest --test-dir .\acs\Intermediate\vs -C Debug --output-on-failure -R "^ACS\.KitAbsorptionAudit"
ctest --test-dir .\acs\Intermediate\vs -C Release --output-on-failure -R "^ACS\.KitAbsorptionAudit"
```

## 統合と公開

機能単位の差分は、実装担当とは別の担当が source review する。
同じ責務は既存 ACS API へ統合し、単なる別名実装を増やさない。
責務が異なる型は、layout、保存値、所有者、寿命を試験で固定してから別名へ分離する。
公開 API、source 構成、module 依存を変えた場合は reference と関連文書も同じ差分で更新する。

公開前に、少なくとも次を同じ clean tree で確認する。

1. 静的監査と Kit 吸収監査
2. Debug と Release の build、CTest
3. optional backend と sample の該当構成
4. 単一 header、外部 consumer、配布 manifest
5. `dist` と最終配布先の tree 同一性

作業中の配布再現先には専用の一時ディレクトリを使う。
`C:\acs` は main の全変更と配布検証が完了した最後の公開工程で一度だけ更新する。
