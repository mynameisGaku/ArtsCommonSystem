# FScriptHost 外部入力・実行安全性

`FScriptHost` はファイル、メモリ上のsource、関数引数、native function登録を
スクリプトVMへ渡す境界です。VMを呼ぶ前の検証と、VM失敗時の呼び出し側状態保持を
この層で共通化します。

## Source契約

- `LoadAndRunSource` は長さ付きsourceを受け取る。
- sourceは最大64 MiB。
- `source_len > 0` のnull pointerと埋込みNULを拒否する。
- chunk名は255 bytes以下のNUL終端文字列に限定する。
- 検証失敗時はVMを呼び出さない。
- backend失敗時だけerror callbackを発火する。

`LoadAndRun(file_path)` は同じchecked経路へ委譲します。ファイルは
`FILE_SHARE_DELETE`付きで開き、完全read後にサイズとEOFを再確認し、handleを正常に
閉じてからVMを呼びます。読込中の伸長・短縮、途中EOF、close失敗、確保失敗では
sourceを実行しません。パスは空を拒否し、最大1,023 UTF-16 code unitです。

## Call契約

`CallGlobalFunction` は次をVM呼出前に検証します。

- 関数名は1〜127 bytes
- 引数は最大1,024個
- 文字列引数の合計は最大1 MiB
- enum範囲外の値種別とnull文字列を拒否

戻り値は一時値へ受け、backend成功後にだけ`ret_out`へ反映します。backendが失敗前に
一時戻り値を書いても、呼び出し側の既存値は変化しません。

## Native registry契約

native function名は1〜127 bytes、1 hostの登録数は最大4,096件です。新規追加と既存の
同名上書きは、内部cacheをstagingした後にbackendへ登録します。backendが拒否した場合は
追加を削除、または既存entryを復元し、cacheを完全にロールバックします。

`TryGetRegisteredNative`でcacheの関数とcontextを診断・hot-swap準備に利用できます。
関数名はentry内へ複製するため、呼び出し側の一時バッファ寿命には依存しません。
backendには同期登録中だけ参照可能な名前を渡し、backend側が必要なら複製します。

## 安定した診断

| subcode | 意味 |
| --- | --- |
| `kSub_FileTooLarge` | source/fileが64 MiB超過 |
| `kSub_Io` | open/read/EOF確認/close失敗 |
| `kSub_AllocationFailed` | sourceまたはregistryの確保失敗 |
| `kSub_FileChanged` | read中にファイル長が変化 |
| `kSub_EmbeddedNul` | sourceに埋込みNUL |
| `kSub_InvalidPath` | file pathがnull、空、長すぎる、未終端 |
| `kSub_InvalidName` | 空、長すぎる、未終端の名前 |
| `kSub_ArgumentLimit` | 引数数または文字列合計上限超過 |
| `kSub_RegistryLimit` | native登録数上限超過 |

回帰テストは`tests/script_host_safety_tests.cpp`にあり、source事前拒否、callback、
戻り値transaction、native cache rollback、名前上限、ファイル入力を検証します。
