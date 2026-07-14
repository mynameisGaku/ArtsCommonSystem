# メモリ・資源診断

ACS は単一の検査結果に依存せず、アロケータ統計、CRT debug heap、AddressSanitizer、
GPU live object、Win32 HANDLE / Winsock、Application Verifier を相互補完で使う。
機械可読ログの `leak_detected` は `true` / `false` / `inconclusive` の三値で扱い、
利用できない検査を成功扱いにしない。

## 通常実行

`FMemorySystem::Shutdown()` はセグメント別の未解放件数・要求バイトと mimalloc の独立走査を
照合する。Diligent memory adapter と D3D12 / DXGI live object 診断も、それぞれ
`outstanding_allocations`、`live_object_messages`、`leak_detected` を出力する。

`Network::CaptureDiagnostics()` と `FUdpTransport::CaptureDiagnostics()` は、Winsock の
現在参照数、cleanup debt、debt の所有者、破棄後も回収を待つ socket 数、仮想メモリ上の
overflow 回収表に移した socket 数、資源解放失敗の累積数を返す。正常終了時は
`active_winsock_reference_count`、`pending_cleanup_error`、`orphaned_socket_count`、
`overflow_orphaned_socket_count` がすべて 0 で、`cleanup_debt_orphaned` が false になる。

transport 破棄時にも `closesocket` / `WSACleanup` を完了できなかった資源は、固定回収表と
`VirtualAlloc` で確保する overflow 回収表へ所有権ごと移す。終了シーケンスでは
`FUdpTransport::DrainDeferredResources()` を呼ぶと再回収できる。残存中は
`deferred_cleanup_still_pending ... leak_detected=inconclusive`、全回収後は
`deferred_cleanup_resolved ... leak_detected=false` を出す。生存中 transport が所有する
cleanup debt は共有 drain の対象にしない。

同時失敗が内部の固定 report buffer を超えた場合も、資源解放失敗の累積数は全件分を保持し、
超過分を `diagnostic_report_buffer_overflow ... suppressed_report_count=N` と集約して記録する。

`closesocket` / `WSACleanup` が失敗した場合は、次の形式を標準エラーと debugger へ
即時出力する。破棄時に回収できない socket または WSA 参照は共有回収処理へ所有権を移し、
`transport_cleanup_deferred` と現在の `orphaned_socket_count` / `cleanup_debt_orphaned` を
出力する。次の接続は共有 debt の回収を先に再試行し、生存中 transport が所有する debt は
別 transport から回収しない。

```text
memory_diagnostic_method=win32_resource tracker=winsock ... resource_release_failed=true leak_detected=inconclusive
```

unit test の失敗注入は `ACS_GAMEFRAMEWORK_TEST_HOOKS` を付けた test 構成だけに組み込み、
製品 build には Winsock 呼び出しの差し替えを残さない。並行所有権テストは ACS 内部の参照・
debt が毎ラウンド 0 に戻ること、明示作成した HANDLE をすべて閉じること、プロセス HANDLE の
連続増加が上限内で停止することを別々に検証する。Winsock provider の遅延初期化 HANDLE は
総数の完全一致だけではリークと断定しない。

## AddressSanitizer

Developer PowerShell から次を実行する。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/invoke-address-sanitizer.ps1 `
  -Preset diligent-address-sanitizer -BuildParallelism 1
```

スクリプトは preset、生成物の鮮度、PE import、compiler instrumentation を検証してから
テストを実行する。Windows のランタイムが LeakSanitizer を提供しない場合は
`capability=leak_detection available=false reason=runtime_unsupported` と記録し、通常の
AddressSanitizer 検査は継続する。

## Application Verifier

Application Verifier はシステム設定を書き換えるため、管理者 PowerShell で通常 Debug の
テスト実行ファイルを対象にする。まず Networking を単独で実行する。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/invoke-application-verifier.ps1 `
  -ExecutablePath acs/Binaries/acs_unit_tests.exe `
  -Layers Networking -RequireAvailable
```

スクリプトは verifier 設定、対象実行、XML log export、設定削除を `finally` で一組にする。
管理者権限またはツールが無い場合は `capability=unavailable` と理由を記録する。
Heaps / Handles / Locks / Memory / Leak は Networking と別実行にする。mimalloc のような
カスタムヒープでは OS heap 検査の対象範囲が限定されるため、CRT debug heap、mimalloc
独立走査、AddressSanitizer の代替にはしない。

出力先は既定で次の通り。

- `acs/Saved/Diagnostics/AddressSanitizer`
- `acs/Saved/Diagnostics/ApplicationVerifier`
- `acs/Saved/Diagnostics/Memory`
