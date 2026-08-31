# メモリ・資源診断

ACS は単一の検査結果に依存せず、アロケータ統計、CRT デバッグヒープ、AddressSanitizer、
GPU 生存オブジェクト、Win32 HANDLE / Winsock、Application Verifier を相互補完で使う。
機械可読ログの `leak_detected` は `true` / `false` / `inconclusive` の三値で扱い、
利用できない検査を成功扱いにしない。

本書はリークの**検出**を扱う。アロケータの**スレッド安全設計**（ライフサイクルゲート、
mimalloc ヒープロック、猶予回収、ロック順序、各アロケータの並行契約）は
[並行実行の安全契約](../../safety/memory/concurrency.md)を参照。

## 通常実行

`FMemorySystem::Shutdown()` はセグメント別の未解放件数・要求バイトと mimalloc の独立走査を
照合する。Diligent メモリアダプターと D3D12 / DXGI 生存オブジェクト診断も、それぞれ
`outstanding_allocations`、`live_object_messages`、`leak_detected` を出力する。

`CNetwork::CaptureDiagnostics()` と `FUdpTransport::CaptureDiagnostics()` は、Winsock の
現在参照数、未完了の後片付け、その所有者、破棄後も回収を待つソケット数、仮想メモリ上の
あふれ回収表に移したソケット数、資源解放失敗の累積数を返す。正常終了時は
`active_winsock_reference_count`、`pending_cleanup_error`、`orphaned_socket_count`、
`overflow_orphaned_socket_count` がすべて 0 で、`cleanup_debt_orphaned` が false になる。

トランスポート破棄時にも `closesocket` / `WSACleanup` を完了できなかった資源は、固定回収表と
`VirtualAlloc` で確保するあふれ回収表へ所有権ごと移す。終了手順では
`FUdpTransport::DrainDeferredResources()` を呼ぶと再回収できる。残存中は
`deferred_cleanup_still_pending ... leak_detected=inconclusive`、全回収後は
`deferred_cleanup_resolved ... leak_detected=false` を出す。生存中トランスポートが所有する
未完了の後片付けは共有回収の対象にしない。

同時失敗が内部の固定レポート領域を超えた場合も、資源解放失敗の累積数は全件分を保持し、
超過分を `diagnostic_report_buffer_overflow ... suppressed_report_count=N` と集約して記録する。

`closesocket` / `WSACleanup` が失敗した場合は、次の形式を標準エラーとデバッガーへ
即時出力する。破棄時に回収できないソケットまたは WSA 参照は共有回収処理へ所有権を移し、
`transport_cleanup_deferred` と現在の `orphaned_socket_count` / `cleanup_debt_orphaned` を
出力する。次の接続は共有の未完了資源を先に再回収し、生存中トランスポートが所有する資源は
別のトランスポートから回収しない。

```text
memory_diagnostic_method=win32_resource tracker=winsock ... resource_release_failed=true leak_detected=inconclusive
```

単体テストの失敗注入は `ACS_GAMEFRAMEWORK_TEST_HOOKS` を付けたテスト構成だけに組み込み、
製品ビルドには Winsock 呼び出しの差し替えを残さない。並行所有権テストは ACS 内部の参照・
未完了資源が毎回 0 に戻ること、明示作成した HANDLE をすべて閉じること、プロセス HANDLE の
連続増加が上限内で停止することを別々に検証する。Winsock 提供側の遅延初期化 HANDLE は
総数の完全一致だけではリークと断定しない。

## MSVC Debug CRT による検査

MSVC Debug かつ AddressSanitizer 無効の構成では、`CApplication` の基盤寿命を
`FCrtDebugHeapScope` で囲み、開始時と終了時の正味増加、ヒープ整合性、CRT 設定の安定性を
1 行の機械可読ログへ出す。`FCrtDebugHeapDiagnostics::DumpProcessMemoryLeaks()` は実際に
`_CrtDumpMemoryLeaks` を呼び、`supported`、`inspection_succeeded`、`leak_detected` を
分離して返す。プロセス全体の検査は、意図的に生存させている静的・大域確保も
対象になり得るため、サブシステムを停止した静止点または隔離したプロセスで使う。

Debug の CTest には正常終了と意図的リークの両経路があり、`_CrtDumpMemoryLeaks` の戻り値を
直接検証する。

ACS リポジトリルートで、Debug 構成をビルドした後に実行する。

```powershell
ctest --test-dir .\Intermediate\vs -C Debug `
  -R "ACS.CrtDumpMemoryLeaks(Clean|Positive)" --output-on-failure
```

MSVC Debug CRT 構成では `_CrtDumpMemoryLeaks` の戻り値とレポートを ACS の診断結果へ反映します。Release、非 MSVC、AddressSanitizer 構成では `unsupported` を記録し、成功とは扱いません。

## AddressSanitizer による検査

ACS リポジトリルートの PowerShell で、`diligent-address-sanitizer` 構成を生成し、
`acs_unit_tests` だけをビルドする。

```powershell
cmake --preset diligent-address-sanitizer -S .\engine
cmake --build .\Intermediate\address-sanitizer-diligent `
  --target acs_unit_tests --parallel 1

.\Intermediate\address-sanitizer-diligent\layout\Binaries\acs_unit_tests.exe `
  --address-sanitizer-capability-probe

ctest --test-dir .\Intermediate\address-sanitizer-diligent `
  -R "^ACS.UnitTests$" --output-on-failure
```

機能確認の出力が `binary_instrumented=true` であることを確認してから CTest を実行する。
この確認は `ACS_ADDRESS_SANITIZER` とコンパイラーの `__SANITIZE_ADDRESS__` を照合する。
ACS にはこの手順を包む別の PowerShell スクリプトはない。

## Application Verifier による検査

Application Verifier はシステム設定を書き換えるため、通常の Debug 構成でビルドした
`acs_unit_tests.exe` を管理者権限で検査する。ACS 内に設定用スクリプトはないため、
Application Verifier の画面で次の順に設定する。

1. `appverif.exe` を管理者権限で起動する。
2. 検査対象へ `.\Binaries\Debug\acs_unit_tests.exe` を追加する。`dx12-debug` 構成の場合は
   `.\Intermediate\layout\dx12-debug\Binaries\acs_unit_tests.exe` を追加する。
3. 最初は `Networking` だけを有効にして設定を保存する。
4. ACS リポジトリルートから選択した実行ファイルを起動する。
5. Application Verifier のログと実行ファイルの終了コードを確認する。
6. 検査後は対象の全設定を削除し、次の検査へ設定を残さない。

管理者権限または `appverif.exe` がない環境では、この検査を利用できないものとして扱う。
`Heaps` / `Handles` / `Locks` / `Memory` / `Leak` は `Networking` と別々に実行する。mimalloc のような
カスタムヒープでは OS ヒープ検査の対象範囲が限定されるため、CRT デバッグヒープ、mimalloc
独立走査、AddressSanitizer の代替にはしない。
