<!-- SPDX-License-Identifier: Apache-2.0 -->
# C++型役割監査

## 役割

| 接頭辞 | 対象 |
|---|---|
| `A` | `FObject`継承または`ACS_OBJECT`登録によりACSが所有する多態的object |
| `F` | 値、設定、結果、handle、および共有して利用するservice |
| `I` | データを持たず、仮想操作または仮想破棄を公開するinterface |
| `T` | templateのclass、struct、union |
| `E` | enum |

delegate、callback、関数型のaliasには接頭辞を強制しない。`class`と`struct`の構文だけでは
役割を決めず、継承、登録、仮想操作、状態、寿命操作、型名を組み合わせて分類する。
従来の`C` serviceは`F`へ移行し、必要なsource互換名だけを`using` aliasとして一時保持する。

## 監査内容

`scripts/audit_cpp_type_roles.py`は`audit_cpp_conventions.py`のC++ lexerを再利用する。
コメント、通常文字列、文字リテラル、raw文字列を型定義として扱わず、次を検証する。

- `FObject`からの推移的な継承と`ACS_OBJECT` / `ACS_REGISTER_OBJECT`は`A`
- データを持たない純粋仮想型、または仮想破棄を公開する`I`型は`I`
- template型は`T`、enumは`E`
- それ以外の具象的な値、handle、serviceは`F`
- 値・serviceを示す型名、状態・寿命操作は、純粋仮想という構文だけで`I`へ変えず`F`
- `CMessageBroker`や`CTimerManager`のような旧`C` serviceは`F`期待のR020c違反
- 無接頭辞、未知接頭辞、小文字接頭辞も、意味から求めた接頭辞と一致しなければR020c違反

判定はallowlistに依存しない。型名や操作を分類器へ追加する場合は、正常例と違反例を
自己試験へ同時に追加する。aliasとネスト型を誤って状態とみなさないこと、全役割の
無接頭辞、`FMessageBroker`、`FTimerManager`、`FLuaVm`の正常例に対する旧`C`名を固定する。

## 実行

```powershell
python scripts\audit_cpp_type_roles.py --self-test
python scripts\audit_cpp_type_roles.py --root src\event
python scripts\audit_cpp_type_roles.py --root src\scripting
python scripts\audit_cpp_type_roles.py --root src\event --format json
python scripts\audit_cpp_type_roles.py --root src\event --json-output Intermediate\event-type-roles.json
cmake --build Intermediate\vs --config Debug --target acs_type_roles_check
ctest --test-dir Intermediate\vs -C Debug -R "ACS.(CppTypeRoleAuditSelfTest|EventTypeRoleAudit|ScriptingTypeRoleAudit)"
```

終了値は適合が`0`、違反ありが`1`、入力または出力の失敗が`2`である。JSONには走査数、
期待接頭辞ごとの型数、規則ごとの違反数、各判定の根拠を保存する。
JSON保存は既存C++ conventions監査の原子的保存処理を再利用し、既存のsymlinkまたは
Windows reparse pointを出力先として指定した場合は拒否する。
