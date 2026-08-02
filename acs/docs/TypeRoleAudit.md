<!-- SPDX-License-Identifier: Apache-2.0 -->
# C++型役割監査

## 役割

| 接頭辞 | 対象 |
|---|---|
| `A` | owner / registryに所有され、多態的に扱われるobject |
| `C` | 状態や寿命を動かす操作を持ち、機能・処理を担う具象class |
| `F` | データ中心のstruct、値、設定、結果、ID、handle |
| `I` | データを持たず、仮想操作を公開するinterface |
| `T` | templateのclass、struct、union |
| `E` | enum |

`class`か`struct`かという構文だけではなく責務で決める。`A`は名前だけで推定しない。
監査は登録済みmigration registry 335件と`acs::AObject`の推移実継承、scope解決した
登録macro実呼び出しを照合する。未登録のobject候補はR020fで拒否し、現行debtは0件である。
macro定義中と`#if 0`中の
`ACS_OBJECT(Type)`は実登録ではない。delegate、callback、関数pointer aliasには接頭辞を
強制しない。template aliasもこのwaveのrole監査対象外である。
監査はpreprocess前のtoken列を読み、確実に無効な`#if 0`以外の条件式やbranchを評価しない。
このためhard canonical定義と一時compatibility aliasは、`#if SOME_FLAG` / `#else`へ分けず
unconditionalに一件だけ宣言する。条件branchごとの重複宣言もraw scanでは契約違反となる。

## hard canonical

次は第一waveで固定した代表例である。全335件の正本はmigration registryとする。

| 正規名 | 一時的な旧名 | header |
|---|---|---|
| `AObject` | `FObject` | `memory/AObject.h` |
| `CAudioEngine` | `FAudioEngine` | `audio/AudioEngine.h` |
| `CMessageBroker` | `FMessageBroker` | `event/MessageBroker.h` |
| `CTimerManager` | `FTimerManager` | `event/TimerManager.h` |
| `scripting::CLuaVm` | `scripting::FLuaVm` | `scripting/LuaVm.h` |

名前だけでなく、各正規型の公開責務も次の根拠で固定する。

| 正規名 | 責務 | `A` / `C`の根拠 |
|---|---|---|
| `AObject` | `NewObject<T>()`の制御blockと強参照・弱参照の管理境界を提供する | owner側に所有され、仮想destructorを通して多態的に扱われるobject基底 |
| `CAudioEngine` | 音声backend、発音枠、再生状態の初期化・停止・終了を管理する | `Init` / `Play` / `Stop` / `Shutdown`で外部状態と寿命を動かす機能class |
| `CMessageBroker` | 型別の購読、解除、配信、全解除を管理する | `Subscribe` / `Unsubscribe` / `Publish` / `Clear`で複数の購読状態を協調する機能class |
| `CTimerManager` | 一回・周期timerの登録、取消、更新を管理する | `SetTimeout` / `SetInterval` / `Cancel` / `Tick`でtimer状態と寿命を動かす機能class |
| `scripting::CLuaVm` | Lua実行状態、script読込、native関数登録を管理する | `Init` / `LoadScript` / `CallFunction` / `Shutdown`で実行状態と寿命を動かす機能class |

旧名は正規型を指すexact `using`だけを許可する。通常の製品sourceで旧名を再利用したり、
旧名を独立したclassとして再定義したりできない。`using`は再コンパイルするsourceの互換を
助けるだけで、旧object fileの装飾名、仮想関数表、RTTIを提供しない。symbol shimは設けず、
この変更を取り込むconsumerはDebug/Releaseとも全量再buildする。

例外は、登録済み旧名を公開namespaceへ再公開するqualified `using`と、既存の保存IDを
変えないための`ACS_RTTI*`、`ACS_REGISTER_*`、`ACS_*SUBSYSTEM_KIND`引数だけである。
監査はmacro名と登録済み旧名を組で照合し、通常式、未登録macro、forward classへの再流入は
R020eとして拒否する。これらの旧綴りはC++型の正規名ではなく、互換入口または実行時identityである。

公開scalar aliasは次の正規3件を固定する。

| 正規名 | 旧名 | header | 実体 |
|---|---|---|---|
| `FEventTypeId` | `EventTypeId` | `event/EventTypeId.h` | `u32` |
| `FComponentTypeId` | `ComponentTypeId` | `ecs/ComponentId.h` | `u32` |
| `FComponentSignatureId` | `ComponentSignatureId` | `ecs/ComponentId.h` | `u64` |

## exact migration debt

現行の正本は`scripts/data/cpp_type_role_migrations.json`である。schema version 2の335件に、
正規型、定義header、旧名、旧名を公開するheader、宣言種別、接頭辞を固定する。旧名がない
review済み`F`型も登録し、正規型の定義と一時互換aliasを別々に検査する。監査器はsourceから
独立した件数335とentries semantic SHA-256
`44C90E6BF0F40A8BC460DAFD0C87F991F88BE0C4C57291FE4B23B6B5DEAC16D5`を持つため、
sourceとregistryを同時に削除しても通らない。

未解決型だけを置く`scripts/data/cpp_type_role_migration_debt.json`は現在0件である。
default baselineは件数0とsemantic SHA-256
`4F53CDA18C2BAA0C0354BB5F9A3ECBE5ED12AB4D8E11BA873C2F11161202B945`に固定する。
公開collectorは毎回candidate/manual集合を再構成し、新しい未登録候補、stale entry、移動、
重複、順序違反、接頭辞・status・候補・理由・waveのdriftをfail-closedにする。将来debtを
追加または支払う場合は、source、registryまたはdebt、件数、baseline hashを同じreview済み
commitで更新する。

valueのconstructor・accessor・operatorは`F`を保つ。member initializerの関数呼び出しと
function-pointer fieldはmethodと数えない。操作のないデータ中心classは`F`とし、
`Draw` / `Render`など明白な処理を持つstructは`C`候補とする。

`AAsset`派生は単純名、修飾名、公開型alias chainを解決して推移閉包を作る。
structや`FScoped*`を含む候補も名前だけで除外しない。
基底参照が曖昧または解決不能なら監査自体を失敗させる。JSONは全階層の重複keyと
C0制御文字・DELを拒否する。`schema_version`はboolや浮動小数ではないexact integer `1`とする。
台帳pathの親componentはsymlink、junction、reparse pointではない通常directoryで、末尾は通常file
でなければならない。bytesはBOMなしUTF-8、CR 0件、LF改行、最終LFちょうど1件に固定し、
この物理契約を検証してから厳密decodeとJSON parseを行う。

### historical provenance（active contractではない）

初回分類の正本artifactは
`public-type-inventory-924-debt326-v1.json`（243,808 bytes、SHA-256
`624BD17B433EBBB0FA9E018DDB4F475262B8C94FA934F94148D1D059570AB45C`）から機械変換した。
先に生成された非atomic初版はbyte identityが変化したため破棄し、正本には使用していない。
機械変換直後の正規JSON semantic hashは
`286EA95DC7FF3653BC7C75B4166AD2568D316E09797543D55908925BE0A1D13A`であった。実methodの
`Draw` / `Render`をreviewし、`FDiligentCommandList`、`FDx12CommandList`、`FWidget`、
`FLabel`、`FButton`、`FSlider`、`FCheckbox`、`FTextInput`の8件をmanualから
candidate `C`へ更新した。これはcandidate 200件、manual 126件、合計326件だった移行前の
provenanceであり、現行のregistry件数やdebt件数ではない。

default台帳はrepo schemaの全9 fieldを固定順array rowとし、そのrow arrayを
`ensure_ascii=false`、空白なしの正規JSON UTF-8 bytesへ直列化したsemantic SHA-256
`7F3260326DFC5C77494DC5DF3B43F8D479B9F73B42AB4496EEDCC259EA404841`でもfreezeする。
これはinventory artifactのunresolved tuple hash `F32F5C0068ACFFCE101C3642345210E40C2D695B67DAC713240AB753C9AF8F6D`
とは直列化と用途が異なる。この旧hashは現行gateでは使用しない。

今回のsample reviewでは、旧`F`定義128件を`A` 25件、`C` 77件、値・データとして維持する
`F` 26件へ確定した。移行対象102件の旧tokenは0件で、型名に合わせたfile renameは行わない。

`violations=0`はhard canonicalとregistry 335件が成立し、新しい未登録debtがなく、default
debt 0件が一致したことを示す。build、ABI、実行時の正しさまで示すものではない。

## 規則と実行

- R020c: 確定roleと接頭辞の不一致、または未登録の役割違反
- R020d: hard canonical、scalar alias、互換aliasの契約違反
- R020e: 一時互換名の通常sourceへの再流入
- R020f: migration debtの追加、消失、移動、分類drift

```powershell
python -B scripts\audit_cpp_type_roles.py --self-test
python -B scripts\audit_cpp_type_roles.py --root src --migration-debt scripts\data\cpp_type_role_migration_debt.json
python -B scripts\audit_cpp_type_roles.py --root src\event --migration-debt scripts\data\cpp_type_role_migration_debt.json
python -B scripts\audit_cpp_type_roles.py --root samples
python -B scripts\audit_cpp_type_roles.py --root src --migration-debt scripts\data\cpp_type_role_migration_debt.json --format json
cmake --build Intermediate\vs --config Debug --target acs_type_roles_check
ctest --test-dir Intermediate\vs -C Debug -R "ACS.(CppTypeRoleAudit|CppTypeRoleAuditSelfTest|EventTypeRoleAudit|EcsTypeRoleAudit|ScriptingTypeRoleAudit|SampleTypeRoleAudit)"
```

終了値は適合が`0`、違反ありが`1`、入力・schema・解決・出力の失敗が`2`である。JSON schema 3は
走査数、役割数、alias数、matched debt、status/wave別debt、規則別違反と判定根拠を保存する。
