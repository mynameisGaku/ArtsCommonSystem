# Windowsプロジェクト操作

Windowsではshellに対応するlauncherから、ACSのconfigure、build、test、配布生成、
cleanを同じ方法で実行します。呼び出した場所には依存せず、repository rootを基準に
既存scriptとCMakeを実行します。この文書を日常操作の正本とします。

| shell | 正規launcher |
|---|---|
| コマンドプロンプト、`.cmd`、IDE外部ツール | `acs.cmd` |
| PowerShell | `.\acs.ps1` |

二つのlauncherはargument境界だけをshellに合わせて保持し、操作の解析と実行は同じ
`acs/scripts/project_operations.ps1`へ委譲します。build規則やclean規則は複製しません。

## 基本操作

コマンドプロンプトでは次のまま実行できます。

```bat
acs.cmd configure --tests --scripting
acs.cmd build --config Debug --target acs_unit_tests
acs.cmd test --config Debug --filter "^ACS.UnitTests$"
acs.cmd all --config Release --target acs_unit_tests --filter "^ACS.UnitTests$"
acs.cmd dist --deploy "C:\ACS SDK"
acs.cmd clean
acs.cmd help
```

PowerShellではlauncherだけを置き換えます。

```powershell
.\acs.ps1 configure --tests --scripting
.\acs.ps1 build --config Debug --target acs_unit_tests
.\acs.ps1 test --config Debug --filter '^ACS.UnitTests$'
.\acs.ps1 help
```

`Debug` と `Release`、およびcommand名は大小文字を区別しません。未知のcommand、
構成、optionは処理を開始せず、終了code `2` で拒否します。

### shellの特殊文字

PowerShellでは通常のargumentとして`.\acs.ps1`へ渡します。追加argumentのseparatorは、
PowerShell自身に制御tokenとして取り除かれないよう`'--'`と引用します。コマンドプロンプト
ではseparatorを`--`のまま指定し、空白、`^`、`|`、`&`、`%`、`!`を含む値を二重引用符で
囲み、cmd.exeの通常規則でescapeします。対話中に環境変数と同じ形の値`%NAME%`を文字
として渡す場合だけは、二重引用符へ入れず`^%NAME^%`と記述します。`"^%NAME^%"`では
caretも値へ残るため使用できません。`.cmd`ファイル内のliteral `%`は`%%`と記述します。

```powershell
.\acs.ps1 test --config Debug --filter 'A^B|C&D%Z!E'
```

```bat
acs.cmd test --config Debug --filter "A^B|C&D%Z!E"
```

PowerShellからは`acs.cmd`を経由せず`.\acs.ps1`を使います。これによりPowerShellと
cmd.exeの二重解釈を避け、`--`以後を含む論理argumentをそのまま保持します。

| command | 処理 |
|---|---|
| `configure` | 既存の `generate.ps1`、または指定したCMake configure presetを実行する |
| `build` | configure済みtreeを `cmake --build` でbuildする |
| `test` | configure済みtreeを `ctest` でtestする |
| `all` | `configure`、`build`、`test`を順番に実行し、最初の失敗で停止する |
| `dist` | 既存の `build_single_header.ps1` で単一header配布物を生成する |
| `clean` | repository rootの既存 `clean-up.ps1` を実行する |
| `help` | command、option、貼り付け可能な例を表示する |

## IDE生成との互換入口

従来のIDE生成操作は次のaliasから同じconfigure処理へ入ります。

```bat
acs.cmd generate --scripting
acs.cmd open --scripting
acs.cmd ide generate --tests
acs.cmd ide open --tests
```

`open` はVisual Studio solutionの生成後にIDEを開きます。CMake presetはsolutionを
生成しないため、`--preset` と `open` は併用できません。

## 構成、target、test filter

`build`、`test`、`all`では`--config Debug`または`--config Release`を指定できます。
`dist`で構成を省略するとDebugとReleaseを生成し、named manifestを公開できます。
`--config`を指定した単一構成生成はlocal staging専用で、`--deploy`とは併用できません。
両構成を生成した配布物には、`acs.h`と全libraryのSHA-256を固定する
`acs-distribution.sha256`が含まれます。`--deploy`はsource配布物を変更する前に配置先を
検査し、payload検証後にだけmanifestを原子的に公開します。既存配置先はrootからvolume root
まで各ancestorのvolume serial・file IDを固定し、未作成配置先は最深既存ancestorの同じ
identityと正規化した残りcomponentで表します。この物理descriptorをrepository、`acs`、
build、`dist`と比較するため、同一pathだけでなくancestor・descendantも拒否します。
SUBST、localhost UNC、8.3短縮名から同じtreeを指した場合も拒否し、reparse検査または
identity取得に失敗した場合は配布物を変更せずfail-closedにします。
同じsource `dist`の生成と同じ配置先へのdeployは、親directoryのvolume serial・file IDと
実体名、およびroot自体のvolume serial・file IDから作るglobal named mutexで排他します。
SUBST、localhost UNC、利用可能な8.3短縮名など、同じ物理directoryを指すpath aliasも
同じ排他へ合流します。未作成の配置先は既存parentと残りpathで先に排他し、作成後の
root identity排他を重ねてから処理を続けます。別writerが所有中なら待機せず、payloadと
manifestを変更する前に失敗します。異常終了したownerのmutexはWindowsのabandoned通知を
確認して回収するため、stale lock fileは残りません。処理中は配布rootのdirectory handleも
保持し、配布元と配置先の物理identityが同じ場合はmirror前に拒否します。
lock取得後に外部processが未作成rootを先に作っても、ensure後は必ずroot identityを
固定します。identity排他へ移行できなければ既存payloadとmanifestを変更せず、
このprocessがcreate-onlyで作った空の通常directory chainだけをrollbackします。
未作成部分を持つ場合は既存ancestorごとのidentityと残りpathをすべて排他するため、
複数階層を一度に作成しても作成前後のlockに隙間はありません。異なる物理rootの
生成・検証・mirrorは並行できます。
単一構成だけを再生成した場合は既存manifestを失効させるため、Debug/Releaseを
同じ実行で再生成するまで公開可能なSDKとして扱われません。
drive、UNC、extended drive、extended UNC、volume GUIDのrootは末尾separatorを保持して
ancestor walkを停止します。`-SelfTest`はactual volume GUID root直下のdescriptor、
mutex、作成可能な環境でのensure・identity移行・cleanupも確認します。repositoryが
drive rootまたはその直下でも利用可能なparent chainの範囲だけalias変換範囲を広げ、
parentのないrootでは存在しないancestorの代わりにcanonical rootとalias rootの拒否を
確認します。repository自体がSUBST drive rootにある場合はpinした物理final pathの
volumeへ解決し、actual volume GUID rootのfull testも省略せず実行します。

この排他は同じscriptを使う協調writerの契約です。ancestor名前空間排他からroot identity排他
への切替中も両方を重ねて保持しますが、このmutexに参加しない外部processは権限が許す範囲で
親directoryの名前空間を変更できます。このTOCTOU（検査と使用の間の変更）は、
path APIと協調mutexだけでは完全に防げません。
配布元と配置先は信頼できるlocal filesystem上に置き、他processによるdirectory操作を
同時に行わないでください。reparse pointの事前・事後検査とroot identity照合は、この
制約を検出可能な範囲でfail-closedにします。

```bat
acs.cmd build -c release -t acs_unit_tests
acs.cmd test -c debug -r "^ACS.ProjectOperationsSelfTest$"
```

空白を含むtarget、filter、pathは二重引用符で囲みます。`--build-dir`は、既存の
`CMakeCache.txt`がある任意treeを`build`または`test`で使うoptionです。
configureと一体で別treeを使う場合は、既存の`--preset`を使用してください。

```bat
acs.cmd build --build-dir "C:\work trees\acs-debug" --config Debug
acs.cmd configure --preset dx12-debug
acs.cmd build --preset dx12-debug
acs.cmd test --preset dx12-debug
```

buildまたはtest対象がconfigureされていない場合は、使用中のshellに対応する
configure commandを表示し、終了code `3`で停止します。

## 追加argument

PowerShellでは引用した`'--'`、コマンドプロンプトでは`--`より後ろのargumentを、境界を
保ったまま実体へ渡します。

```powershell
.\acs.ps1 configure '--' '-DACS_BUILD_SCRIPTING=ON' '-DUSER_PATH=C:\Path With Spaces'
.\acs.ps1 build --config Release '--' '--parallel' '8'
.\acs.ps1 test --config Debug '--' '--repeat' 'until-pass:2'
.\acs.ps1 dist '--' -SelfTest
```

```bat
acs.cmd configure -- "-DACS_BUILD_SCRIPTING=ON" "-DUSER_PATH=C:\Path With Spaces"
acs.cmd build --config Release -- "--parallel" "8"
acs.cmd test --config Debug -- "--repeat" "until-pass:2"
acs.cmd dist -- -SelfTest
```

`configure`では`generate.ps1`の`CMakeArguments`、preset使用時はCMake configureへ
渡します。`build`は`cmake --build`、`test`は`ctest`、`dist`と`clean`はそれぞれの
既存PowerShell scriptへ渡します。`all`ではconfigure段階だけへ渡します。

## 基盤E2E

基盤全体の実測には、既存の`run_foundation_end_to_end.py`を呼ぶ専用optionを使います。

```bat
acs.cmd test --foundation --config Debug
acs.cmd test --foundation --config Release -- "--skip-performance"
```

`--filter`はCTest専用のため`--foundation`とは併用できません。基盤E2E固有のoptionは
`--`より後ろへ指定します。

## dry-runとcleanの安全性

`--dry-run`または`-n`は、実行予定のcommandを表示するだけです。configure tree、
生成物、配布先、clean対象を変更せず、子processも起動しません。

```bat
acs.cmd all --config Release --dry-run
acs.cmd dist --deploy "C:\ACS SDK" --dry-run
acs.cmd clean --dry-run
```

`clean`の対象判定、確認、削除規則は新しく複製せず、既存`clean-up.ps1`へ委譲します。
対話確認を省略する自動化では、対象を確認した上で`acs.cmd clean --yes`を使用します。

## 終了codeと環境診断

| code | 意味 |
|---:|---|
| `0` | 成功、または成功したdry-run |
| `1` | 操作中の予期しない失敗 |
| `2` | command、構成、optionの指定誤り |
| `3` | PowerShell、CMake、CTest、Python、configure tree、既存scriptの不足 |
| その他 | 実行した子processの終了code |

子processが非zeroで終了した場合は、そのcodeを呼び出し元へ返します。`all`はその時点で
後続処理を開始しません。`Ctrl+C`も子processとPowerShellを経由して呼び出し元へ戻ります。

自己テストは、空白を含む隔離directoryへ入口一式を複製し、cwd非依存、引用、
PowerShellとcmd.exeの特殊文字、大小文字正規化、dry-run、終了code、環境診断、
既存scriptへの委譲を確認します。配布manifestとclean対象には両launcherを含めます。

```bat
acs.cmd test --config Debug --filter "^ACS.ProjectOperationsSelfTest$"
```

```powershell
.\acs.ps1 test --config Debug --filter '^ACS.ProjectOperationsSelfTest$'
```
