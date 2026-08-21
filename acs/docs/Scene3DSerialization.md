# CSceneNodeGraph テキストシリアライズ

`Scene3DSerialize` は `CSceneNodeGraph` の `ANode` 階層、`FTransform3D`、
`AMeshComponent3D`、`ASprite3DComponent`、マテリアル、反射コンポーネント、および明示的なゲームカメラを
行ベースのテキストから復元する。保存APIが出力する従来の `N3D` / `MSH3D` 形式に加え、
エディタの `ACS3D v2` を互換アダプタとして直接読み込める。

パッケージ内の正準起動パスは2D/3Dとも `main.acscene` である。拡張子でシーン種別を
決めず、内容の厳密なヘッダー (`ACSCENE v1` / `ACS3D v2`) で対応アダプタを選ぶ。
元アセットの永続IDはパッケージmanifestに保持される。

## API

- `TrySaveScene3DText` はシーン全体を検証・計測してから出力する。結果の
  `FScene3DSaveResult` にはエラー、終端 NUL を含む必要容量、書込文字数、ノード数、
  メッシュパス数、カメラ数、3Dスプライト数が入る。`out=nullptr, cap=0` はサイズ照会になる。
- `TryLoadScene3DText` は明示された入力サイズ内を完全に解析し、全行の検証と作業領域の
  確保が成功した後だけ既存シーンを置き換える。結果の `FScene3DLoadResult` にはエラー、
  消費 bytes、エラー行、宣言ノード数、メッシュパス数、ロード済み依存数、カメラ数、
  active指定数、手続き生成polygon数、3Dスプライト数、および決定的に選択されたカメラ状態が入る。
- `TryLoadScene3DFile` はloose sceneを読み、相対メッシュ/マテリアル/画像参照をシーンの
  親ディレクトリ基準で解決・検証してから適用する。
- `TryLoadScene3DAssetPack` は既定でpack内の `main.acscene` を読み、同じpackだけから
  依存を解決する。mount後の欠損、CRC/解凍、デコード失敗ではloose fileへfallbackしない。
  mesh/material/image 依存は kind と完全pathで重複排除し、初出順の最大8 entry、合計
  32 MiBを目安とするbatchで読む。32 MiBを超える単一依存は進行保証のため一件だけで
  読む。現在の外部依存は相互参照を持たないleafなので初出順が安定したtopological
  orderになる。同じ依存を複数nodeが参照してもread/decodeは一度だけ行い、
  `DependenciesLoaded`は従来どおり参照nodeごとに数える。
- `SaveScene3DText` / `LoadScene3DText` は既存呼び出し向けの簡易 API として残る。
  新規コード、ファイル入力、ネットワーク入力ではサイズ付き `TryLoadScene3DText` を使う。
- `Scene3DSerializeErrorName` はログやテレメトリ向けの安定した ASCII 名を返す。

## 対応フォーマット

### 従来互換の保存形式

- ヘッダー無しの `N3D` / `MSH3D` / `SPR3D` / `PFAB3D` / `PINS3D` / `PSID3D` / `POVR3D` / `PNOVR3D` / `CAM3D`。カメラ、
  3Dスプライト、Prefabリンクを持つgraphを往復しても参照を失わない。
- rootは `id=0, parent=-1` の1件だけ。
- idは0から連続し、子のparentは必ず先に宣言されたidを参照する。

### エディタ互換 `ACS3D v2`

- 先頭行は大文字小文字も含めて正確に `ACS3D v2`。
- node idは一意な非負整数であれば疎でもよい。parentは先に宣言されたnodeを参照する。
- `parent=-1` のtop-level nodeを複数保持でき、runtimeでは1つの合成root配下へ接続する。
- `N3D`、`MSH3D`、`FLG3D`、`EMPTY3D`、`MAT3D`、`CMP3D`、
  `CPROP3D`、`PLY3D`、`SPR3D`、`PFAB3D`、`PINS3D`、`PSID3D`、`POVR3D`、`PNOVR3D`、`PCOVR3D`、`CAM3D`、`SEL3D` を扱う。
- `MAT3D` は従来のmetallic/roughness値または `.acsmat` パスを扱う。
- `CMP3D` は反射factoryで事前生成し、`CPROP3D` を適用してからnodeへattachする。
- `PLY3D <nodeId> <pointCount> <x0> <y0> ...` は既出の `Mesh` nodeへXY平面上の
  polygon点列を1件attachする。点数は3から4,096、座標は有限値だけとし、先頭点を共有する
  決定的なtriangle fanへ変換する。頂点のZは0、normalは+Zになる。
- `SPR3D <nodeId> <imagePath>` は既出nodeへ画像参照を1件attachする。file/pack経路は
  画像をCPUへデコードしてコンポーネントが共有所有し、描画アダプターがGPU画像へ変換する。
  表示形状はnodeのworld transformを受けるローカルXY単位板で、billboard回転は行わない。
  UV、alpha cutoff 0.02、alpha blend、depth test有効、depth write無効はEditorと共通である。
- `PFAB3D <nodeId> <sourcePath>` は、Editorが既に実体化して保存した3Dサブツリーと
  `.acsprefab` または `.acsbp` 原本を結ぶ非実行リンクである。runtimeは原本を再展開せず、
  保存済みnodeを実行状態として使うため、子を二重生成しない。
- `PINS3D <nodeId> <instanceId>` は直前までに宣言された同じnodeの `PFAB3D` へ、
  32桁小文字hexのscene内stable instance IDを付ける。旧sceneの `PFAB3D` 単独は引き続き
  読み込める。新規配置はmanaged側でGUIDを明示入力し、Apply/Revertは値を維持、duplicateと
  subtree pasteは新しいnode IDを入力とした別IDへ移る。
- `PSID3D <nodeId> <sourceNodeId>` はPrefab原本内の対応nodeを表す32桁小文字hex IDである。
  Applyで数値node IDが再採番されても値を維持し、同じ原本から作られた各instanceの対応nodeは
  同じ値を共有する。scene内の対象はinstance rootの `PINS3D` と組み合わせて一意に解決する。
- `POVR3D <nodeId> <mask>` は同じnodeの `PINS3D` より後に、instance rootで原本より優先する
  propertyを記録する。maskはVisible=`1`、Enabled=`2`、Color=`4` の和であり、transformは
  instance配置として従来どおり常に保持する。値自体は同じnodeの `N3D` / `FLG3D` に保存する。
- `PNOVR3D <nodeId> <mask>` は `PSID3D` を持つinstance childで原本より優先するVisible=`1`、
  Enabled=`2`、Color=`4`、Position=`8`、Rotation=`16`、Scale=`32`、Material=`64`、Name=`128` の和を記録する。
  値は対象childの `N3D` / `FLG3D` / `MAT3D` に保存し、source更新後は一時的な数値node IDではなく
  source node IDでreplacement childを再解決して適用する。空のMaterial overrideは割り当て解除を表す。
  Nameは`N3D`末尾の127-byte以下の値として保持する。root transform、root Material、root Nameはinstance child overrideの対象外とする。
- `PCOVR3D <nodeId> <componentSlot> <propertyIndex>` はstable instance rootの反射component
  propertyを1件overrideとして記録する。値は同じslot/propertyの `CPROP3D` に保存する。
  source再生成時は現在slotのcomponent型IDを取得し、replacement内の同型componentへ値を
  戻すため、source側でcomponentの宣言順が変わってもslotずれを起こさない。
- `CAM3D <nodeId> <stableId> <projection> <priority> <active> <fovYDeg>
  <orthoHeight> <near> <far>` は既存nodeへ1件のカメラをattachする。`projection` は
  Perspective=`0`、Orthographic=`1`、`active` は `0` または `1`。
- 未知命令は、黙って欠落させず明示エラーでfail closedする。
- `TrySaveScene3DText` は従来互換graphを保存し、runtime meshから `PLY3D` の元点列を
  再構築しない。authoring sourceの保持とpackageへの可逆な転記はEditor adapterが担う。

## 共通の検証規則

- 深度上限は `ANode` と同じ512、ノード上限は65,536、入力上限は4 MiB。
- 1行は4,095 bytes、名前は127 bytes、メッシュ/マテリアル/画像/Prefabパスは299 bytesまで。
- primitive は `-1` または `EMeshPrimitive3D` の `0..3` のみ。
- transform と色は有限の `f32` だけを受理する。整数範囲外、`NaN`、`Inf`、途中で
  切れた数値、未知行、埋め込み NUL はエラーになる。
- `MSH3D` は既出の `Mesh` ノードに1件だけ指定できる。
- `PLY3D` は既出の `Mesh` ノードに1件だけ指定でき、同じnodeの `MSH3D` と併用できない。
- `SPR3D` は既出nodeに1件だけ指定できる。スプライトは同じnodeの下地メッシュより表示を
  優先し、不透明、影、SSAO、水面の各メッシュpassへ下地を重複投入しない。raycastと
  半径付きcamera probeも下地メッシュではなく同じローカルXY単位板を判定する。
- `PFAB3D` は既出nodeに1件だけ指定できる。text-only経路はリンクだけを復元し、file/pack経路は
  `.acsprefab` の `ACS3D v2` または `.acsbp` の `ACSBP 1` ヘッダーと参照資産の存在をcommit前に
  検証する。リンクを実行しないためruntime内でPrefab循環は発生せず、Cook closureが循環を拒否する。
- `PINS3D` は同じnodeの `PFAB3D` より後に1件だけ指定できる。instance IDはscene全体で一意とし、
  大文字hex、長さ違反、nodeまたはIDの重複、孤立したIDをcommit前に拒否する。
- `PSID3D` は既出nodeに1件だけ指定できる。値は32桁小文字hexに限定し、同じnodeへの重複を
  commit前に拒否する。同じPrefabの別instanceでは対応nodeの値を共有するため、scene全体の
  値重複は許容する。Prefab原本内の重複はmanaged migrationがfail closedで拒否する。
- `POVR3D` は同じnodeの `PINS3D` より後に1件だけ指定できる。maskは `1..7` とし、未知bit、
  重複、stable instance IDを持たないnodeへの指定をcommit前に拒否する。
- `PNOVR3D` は同じnodeの `PSID3D` より後に1件だけ指定できる。対象はstable `PINS3D` root自身では
  なく、そのscope内のchildに限定する。maskは `1..255` とし、未知bit、重複、孤立child、nested root
  自身への指定をcommit前に拒否する。
- `PCOVR3D` は同じnodeの `PINS3D` と対象 `CMP3D` より後に指定する。component slotは既存、
  property indexは `0..23` かつ現在の反射schema内でなければならず、同じnode/slot/propertyの
  重複とstable instance IDを持たないnodeへの指定をcommit前に拒否する。
- カメラは最大256件、stable IDは64 bytesまでの正準ASCIIで、nodeとstable IDの重複を
  拒否する。投影値、priority、FOV/orthographic height、near/farは有限かつ範囲内でなければ
  ならない。複数のactive指定は入力として保持するが、選択はactive指定、priority降順、
  stable ID昇順、node ID昇順で決定する。
- 保存時は明示スタックと visiting/complete 訪問表を使い、C++ 呼び出しスタックを
  消費せずに循環と共有子・重複参照を区別して拒否する。

## トランザクション性

容量不足を含む保存前検証エラーでは出力バッファを変更しない。読み込みは全入力を固定上限内で
解析し、親関係・深度・値・文字列・命令・反射型を検証する。file/pack APIはさらに全メッシュ/
マテリアル/画像依存をデコードし、Prefab/Blueprint原本を検証してからcommitする。したがって入力破損、未知命令、
欠損依存、CRC/解凍/デコード失敗では読み込み先の既存シーンを変更しない。

Editor ABIの3D subtree貼り付け、stable ID付きinstance生成、Prefab/Blueprint instance再生成も、入力検証後に2D+3Dの
変更前snapshotを確保してから処理する。失敗時はscene、選択、Undo履歴を変更せず、成功時だけ
1件のUndoを公開する。instance再生成は旧subtreeをsnapshot内で退役させてから新subtreeを
読み込むため、旧instance自身との一時的なcamera stable ID衝突を発生させない。Editor内の
`PFAB3D` source linkは終端を除く255 UTF-8 bytesまでとし、超過を切り詰めず拒否する。
instance生成はsource、32桁ID、subtreeを1つのnative adapterへ渡し、途中失敗で未リンクnodeを残さない。
`PSID3D` を持たない旧原本と旧sceneは引き続き読み込める。Editorは3D Prefab/Blueprintの
Save、Place、Revert、Apply入口で既存IDを保持したまま不足分だけを決定論的に計算し、原本を
atomic更新する。native ABIを直接使うlegacy callerにはsource pathと原本node IDから同じ値を補う。
root override付き再生成は明示maskのVisible/Enabled/Colorだけを再適用する。Color保持先が描画対象を
持たない場合を含め、再適用できなければ旧subtreeとoverride maskを復元する。既存の全Revert入口は
maskを保持せず原本値へ戻す。selective Revertは指定bitを現在maskから除いた残りだけを再適用し、
未知bitまたは既にoverrideでないpropertyを指定した場合はsceneを変更しない。override保持付き
再生成は `PCOVR3D` も型IDで解決して再適用し、componentまたはpropertyがsourceから消えた場合は
sceneとUndo履歴をrollbackする。component selective Revertは現在slotから型IDを解決し、指定propertyだけを
保持snapshotから除いてsource値へ戻す。他のroot/component overrideは維持し、未override property、
不正slot/property、source不一致ではsceneを変更しない。component selective Applyは現在slotの型名で
原本componentを解決し、選択`CPROP3D`だけをatomic更新して選択instanceのmarkerだけを外す。他instanceの
明示overrideは型IDで維持する。child node overrideも同じrefresh transactionでcaptureし、`PSID3D`で
replacementを再解決して値、ローカルtransform、Material path、`PNOVR3D` maskを戻す。対象childの削除、identity重複、
ColorまたはMaterial適用先のmesh欠落はsceneとUndo履歴をrollbackする。child selective Revertは選択bitだけを保持snapshotから
除き、selective Applyはsource node IDに対応する原本`N3D`/`FLG3D`/`MAT3D`だけを純粋計算とatomic writerで更新する。他instanceのchild overrideは
維持され、全Applyと全Revertは選択instanceのchild markerを破棄する。全Revertはcomponent overrideも破棄する。

pack batch は後続entryの失敗時に先行entryの完了数を返す。loaderはprivate parsed
document上で完了済み依存を初出順にdecodeし、旧逐次経路と同じ先行decode errorを優先する。
batch bufferやparsed nodeが更新されても、全依存が成功するまでは公開Sceneへcommitしない。

呼び出し中に別スレッドからシーンを変更することはサポートしない。保存の計測後に内容が変化した
場合は `scene_changed_during_save` を返す。

## 生成とノード登録

`CSceneNodeGraph::TrySpawn` は parent が同じシーンのpoolとrootツリーの両方に属することを確認する。
新規ノードを `FNodePool` へ仮登録してから `ANode::TryAddChild` を呼び、破棄予定parentや
深度上限でattachが拒否された場合は登録をrollbackする。失敗時はツリー、active slot数、
既存ノードのIDを変更しない。互換用 `Spawn` は成功時に従来どおりノード参照を返し、失敗時は
安全なparentまたはroot sentinelを返す。

`FNodePool::TryRegisterExistingNode` は、同じpoolへの二重登録を既存ID付きで報告し、別poolの
有効IDを持つノードは拒否する。互換用 `RegisterExistingNode` は同一poolへの重複呼び出しを
冪等に扱う。親をDestroyした場合は子孫自身のpending flagが立たなくても所有関係とともに
破棄されるため、`PurgePendingDestroy` は祖先chainも確認し、親と全子孫のhandleをreap前に
stale化する。

## スタンドアロン3Dランタイム

`ALegacyScene3DAdapter` は互換入力を正準 `ANode` graphへ復元し、プリミティブまたは
デコード済みmeshを `CPbrShader`、3Dスプライトを `CSprite3DRenderer` で描画する。visible/enabled、階層transform、
PBR/Substrate material、fogを反映し、loose/packの双方を同じ読み込み契約で扱う。

各 `CAM3D` はgraph所有の `ACameraComponent3D` になり、姿勢は所有nodeの現在のworld
transformから更新される。runtimeはstable IDまたはnode IDでカメラを安全に切り替えられる。
投影方式はscene全体の固定属性ではなく、各cameraがPerspective/Orthographicを選択する。
パッケージmanifestの `sceneBootstrap.adapterProjectionHint` は旧形式を取り込むための
参考値にすぎず、runtime cameraを上書きするauthoritativeなscene propertyではない。
