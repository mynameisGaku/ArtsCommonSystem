# StudioWorkflow ローカルアセットロック安全契約

`FLocalFileAssetLocking` は `<asset_path>.lock` という sidecar file を使う、
単一ホスト上の協調ロックです。ネットワーク共有や敵対的な OS 管理者に対する
security boundary ではありませんが、通常権限の複数 process/thread 間では
「取得した世代だけを解除できる」ownership boundary を提供します。

## 検証付き API

- `TryLockAsset(path, owner)` は `CREATE_NEW` でのみ取得し、成功時に 128-bit token
  と取得時刻を返します。全 byte の書き込み、`FlushFileBuffers`、`CloseHandle`
  が完了するまで成功を公開しません。
- `TryQueryLock(path, out_info)` は size、完全読み取り、EOF、文法、overflow、
  close を検証してから `out_info` と内部 query buffer を一度に更新します。
  失敗時に呼び出し側の `out_info` は変わりません。
- `TryUnlockAsset(path, owner, token)` は owner と token が両方一致するときだけ
  解除します。検証用 handle は `FILE_SHARE_DELETE` なしで開き、同じ handle を
  `FileDispositionInfo` で delete-pending にします。検証後に path が別世代へ
  すり替わり、その新しい file を `DeleteFileW(path)` で消す競合はありません。

従来の `LockAsset`、`UnlockAsset`、`UnlockAssetAs`、`QueryLock` は残っています。
取得は checked API へ委譲します。token 引数を持たない解除 API は、同じ backend
instance が取得時から固定長 table に保持している token だけを使います。他 process
や backend 再生成前の lock を owner 名だけで解除することはできません。process を
またいで解除する利用側は、取得結果の token を安全に保持して checked API を使います。

## record 形式と上限

現在の canonical record は次の byte-exact な UTF-8 text です。

```text
ACSLOCK/1
OWNER:<owner>
TOKEN:<32 uppercase hex digits>
TIME:<unsigned decimal UNIX seconds>
```

最終改行は必須です。次を一つでも満たす record は
`CorruptRecord` または `RecordTooLarge` になります。

- magic/version、label、行数、最終改行が一致しない
- partial read、余分な byte、二つ目の record、embedded NUL がある
- owner が空、255 byte 超、invalid UTF-8、control character を含む
- token が 32 桁の大文字 hex でない、または all-zero
- time が空、10 進数以外、`u64` overflow
- record 全体が 512 byte を超える

API 入力は path が NUL を含め 1024 byte 未満、owner が NUL を含め 256 byte 未満
で、厳密な UTF-8 である必要があります。owner の CR/LF、NUL、control character
は禁止です。内部 table は 1 instance あたり 64 lock です。これらの上限超過は
切り詰めず、安定した `ELocalAssetLockError` 値で失敗します。

## fail-closed と stale policy

既存 sidecar は、内容が破損・旧形式・古い時刻であってもロック済みとして扱います。
この API は PID 生存確認や時刻だけを根拠に自動削除しません。そうした推測は、遅延中の
正当な owner や別 process が再取得した lock を消す危険があるためです。

stale/corrupt lock の復旧は、運用者が owner process の停止、対象 path、record 内容、
バックアップを確認した後に行う管理操作です。通常の解除 API に force-delete は
ありません。旧 2 行形式は自動 migration せず、管理確認後に削除して再取得します。

## error と回復

`ELocalAssetLockError` の数値 1500–1519 はログ・テレメトリ向けの安定値です。
代表的な分類は次のとおりです。

| 分類 | 意味 | 安全な対応 |
|---|---|---|
| `AlreadyLocked` | `CREATE_NEW` が既存 file を検出 | query して owner を表示し、待機する |
| `OwnerMismatch` / `TokenMismatch` | 別 owner または別取得世代 | file を削除せず、最新 token を再確認する |
| `CorruptRecord` / `RecordTooLarge` | 非 canonical、partial、trailing、overflow | fail-closed のまま管理者へ通知する |
| `NotOwned` / `CapacityExceeded` | instance の追跡境界 | checked token を使う、または取得数を減らす |
| `WriteFailed` / `FlushFailed` / `CloseFailed` | 永続化完了を証明できない | 成功扱いにせず、sidecar を調査する |
| `DeleteFailed` | 同一 file handle の delete-pending 化に失敗 | 所有権を保持したまま再試行する |

取得中の write/flush failure では、まだ開いている取得 handle 自身を
delete-pending にしてから close します。path 名による cleanup は行わないため、
cleanup と別 process の再取得が競合しても他世代を削除しません。

## テスト観点

`tests/studio_workflow_safety_tests.cpp` は checked round-trip、既存 file、
wrong owner/token、旧形式、partial、embedded NUL、trailing data、時刻 overflow、
record/path/owner 上限、失敗時 output 不変、二 thread の同時 `CREATE_NEW` で成功者が
一人だけになることを固定します。
