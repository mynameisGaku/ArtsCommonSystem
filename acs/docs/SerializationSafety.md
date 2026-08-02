# シリアライズ・シーン入力の安全境界

2026-07-19 時点の、外部入力を扱う GameFramework API の共通契約をまとめる。
個別形式の詳細は
[`SaveArchive.md`](SaveArchive.md)、
[`SceneTextLoadingSafety.md`](SceneTextLoadingSafety.md)、
[`Scene3DSerialization.md`](Scene3DSerialization.md)、
[`SettingsPersistenceSafety.md`](SettingsPersistenceSafety.md)、
[`TextAssetLoadingSafety.md`](TextAssetLoadingSafety.md)、
[`JsonAssetSafety.md`](JsonAssetSafety.md)、
[`NetSnapshotSafety.md`](NetSnapshotSafety.md)、
[`ReplayDirectorSafety.md`](ReplayDirectorSafety.md)、
[`ScriptHostSafety.md`](ScriptHostSafety.md)、
[`FxeditSerializerSafety.md`](FxeditSerializerSafety.md)、
[`StudioWorkflowLockSafety.md`](StudioWorkflowLockSafety.md)、
[`EditorCorePersistenceSafety.md`](EditorCorePersistenceSafety.md)、
[`EasingSafety.md`](EasingSafety.md)、
[`AnimationCurveSafety.md`](AnimationCurveSafety.md)、
[`AnimationCurvePersistenceSafety.md`](AnimationCurvePersistenceSafety.md)、
[`HotReloadSafety.md`](HotReloadSafety.md)、
[`BeatGridHoldSafety.md`](BeatGridHoldSafety.md)、
[`UiTextInputSafety.md`](UiTextInputSafety.md)、
[`AssetPackManifestSafety.md`](AssetPackManifestSafety.md)、
[`SaveGamePersistenceSafetyV2.md`](SaveGamePersistenceSafetyV2.md)、
[`BehaviorTreePersistenceSafety.md`](BehaviorTreePersistenceSafety.md)、
[`NodeOwnershipSafety.md`](NodeOwnershipSafety.md)、
[`NodeUnification.md`](NodeUnification.md) を参照する。

## 共通原則

- 新規コードは詳細結果を返す `Try*` API を使い、簡易 API は互換用途に限定する。
- 個数、深度、文字列、payload、入力全体の固定上限を、確保や実体生成より先に検証する。
- 読み込みは全入力を検証してから commit し、失敗時に呼び出し側のツリー、オブジェクト、
  要求配列、出力バッファを部分変更しない。
- 保存は構造検証と必要サイズ計測を先に行い、容量不足時に出力を変更しない。
- 木構造の保存走査は明示スタックを使い、循環と共有子・重複参照を区別して拒否する。
- ノード深度は `kNodeMaxTreeDepth` と各形式の上限を一致させる。
- エラー enum と安定したエラー名/subcode をログ、UI、テレメトリで保持する。

## 主な境界

| 対象 | checked API | 主な上限・保証 |
| --- | --- | --- |
| 反射値 | `TrySerializeReflected` / `TryDeserializeReflected` | 1,024 fields、名前255 bytes、値16 bytes、範囲外メタデータ拒否、`FIELD_TRANSIENT`除外 |
| ノードバイナリ | `TrySaveNodeTree` / `TryLoadNodeTree` | 65,536 nodes、深度512、1,024 components/node、payload 4,096 bytes |
| `.acscene` テキスト | `TryLoadAcsceneText` / `TryLoadAcsceneFile` | 入力8 MiB、行2,047 bytes、4,096 nodes、親参照・循環・有限値検証 |
| `CSceneNodeGraph` テキスト | `TrySaveScene3DText` / `TryLoadScene3DText` | 入力4 MiB、65,536 nodes、深度512、連続ID・単一root・有限値検証 |
| セーブ包絡 | `FSaveArchive` | payload 256 MiB、実サイズ完全一致、CRC後commit、flush済みatomic replace |
| 設定 | `FSettings::TrySave` / `TryLoad` | 入力4 MiB、4,096 entries、厳密scalar、全件staging、一意tempのatomic replace |
| テキストアセット | `TryParseAcsmatText` / `FProjectSettings::TryLoadText` | 入力1 MiB、行・件数・schema上限、close成功後commit |
| JSONアセット | `FTilemap::TryLoadTiledJson` / `FSpritePack::TryLoadAtlasJson` | 入力4–8 MiB、深度64、積overflow・重複key・非有限値拒否 |
| ネットsnapshot | `FNetSnapshot::DecodeSnapshot` / `TryTick` | payload 4 MiB、UDP 65,507 bytes、CRC・正規header・transport契約 |
| replay | `FReplayDirector::TrySaveReplay` / `TryLoadReplay` | container 256 MiB、内包blob事前検証、sourceとmetadataの一括commit |
| script | `FScriptHost::LoadAndRunSource` / `CallGlobalFunction` | source 64 MiB、引数・名前上限、戻り値とnative cacheのrollback |
| particle editor | `FFxeditSerializer::TrySave` / `TryLoad` | 入力・行・emitter上限、厳密値検証、atomic replace |
| asset lock | `FLocalFileAssetLocking` checked API | path・owner・record上限、CREATE_NEW取得、所有token照合解除 |
| editor theme/layout | editor_core checked load/save | 行形式のbyte・entry上限、strict schema・全件staging、open-reader対応atomic replace |
| easing catalog | `Easing::TryEvaluate` / `TryGetName` / `TryParseNameChecked` / enum Tween overload | 33種類、有限入力、安定名、名前エラー分類、無効enum拒否、失敗時出力不変 |
| animation curve | `TrySetKeys` / `TryAddKey` / `TryEvaluate` / `FAnimationCurveArchive` | 65,536 keys、固定LE wire + CRC、完全size一致、有限値・enum検証、OOM/破損時状態不変 |
| hot reload | `TryWatchDirectory` / `TryEnqueueEvent` / `TryTick` | path 256、directory 64、event 1,024、strict UTF-8、通知欠落/再発行失敗を明示診断 |
| beat chart | `TryLoadChart` / `PressLane` / `ReleaseLane` | 65,536 notes、有限時刻/BPM/lane検証、hold exact-once、OOM時chart不変 |
| UI text input | `FTextInput` checked cursor/edit/cap API | canonical UTF-8、4,096 bytes既定/1 MiB hard cap、codepoint境界、OOM時text/cursor不変 |
| asset pack manifest | `FAcpakReader::Open` / `FAcpakWriter::Finalize` / `TryRegisterLoader` | path・entry・schema上限、重複・範囲検証、transactional mount、atomic publish |
| save envelope v2 | `FSaveArchive::ValidateFile` / `TSaveSlot::TryInit` | 256 MiB、完全CRC・実size一致、所有path、open-reader対応atomic replace |
| behavior tree editor | btedit checked parse/load/save | 256 KiB、128 nodes、深さ64、参照・cycle・有限値検証、transactional graph commit |

## 回帰防止

`scripts/audit_cpp_conventions.py` は C++ の実 token を解析し、型接頭辞違反と削除済み
Node API の再流入を検出する。コメント、文字列、raw string 内の shader は誤検出しない。
CTest の `ACS.CppConventionsAuditSelfTest` と `ACS.CppConventionsAudit`、または
`acs_conventions_check` target で実行する。

`scripts/audit_reference_type_names.py` は手書き API リファレンスの class/struct/enum 名を
現行 C++ 宣言と照合する。正規scalar alias 3件と`AObject` / C4は正規名・header各1件を
固定し、旧scalar 3名と旧`FObject` / C4旧`F`名5件の独立entryを拒否する。delegate、callback、関数、および他のalias分類は対象外で
ある。CTest の `ACS.ReferenceTypeNamesAuditSelfTest` と
`ACS.ReferenceTypeNamesAudit`、または `acs_reference_check` target で実行する。

変更後は Debug の `ALL_BUILD`、全 CTest、上記監査、`git diff --check` を通す。
