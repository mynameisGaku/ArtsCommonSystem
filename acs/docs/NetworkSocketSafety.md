<!-- SPDX-License-Identifier: Apache-2.0 -->
# ネットワーク送受信の入力条件

`FTcpConnection` と `FUdpSocket` は、WinSock へ渡す前にバッファの先頭と大きさを確認する。
これにより、ACS の `usize` から WinSock の `int` へ変換する際の値の欠落と、
`size` が 1 以上なのに先頭が `null` である呼び出しを同じ規則で拒否する。
無効な接続またはソケットでは、`size` に関係なく `-1` を返す。

## 共通条件

- `size` が 1 以上なら、`data` または `buf` は `null` 以外でなければならない。
- `size` は WinSock の長さ引数で表せる最大値以下でなければならない。
- 条件を満たさない場合は OS を呼び出さず `-1` を返す。テストは WinSock の
  スレッド固有エラーを番兵値にして、OS 呼出し前の拒否を直接確認する。
- 同じ接続またはソケットへの呼び出しは、利用側で順番に実行する。
- 送受信する領域を、同じ処理中に別スレッドから変更してはならない。
- 接続とソケットをすべて破棄してから、対応する `FNetwork::Shutdown` を呼ぶ。

## 0 バイトの扱い

TCP の `Send` と `Recv` は、`size == 0` なら領域を参照せず `0` を返す。
この場合の `Recv` の `0` は、相手による切断を示すものではない。

UDP では 0 バイトも有効な 1 通であるため、`SendTo` は空のデータグラムを実際に送る。
`RecvFrom` も空のデータグラムを受け取り、送信元を `from` へ書き込む。
入力条件の拒否または OS の受信失敗では、`from` を変更しない。

## 確認

`tests/network_socket_io_tests.cpp` は OS 割当ポートとノンブロッキングの期限付き
受信を使い、別 process のポート利用やデータ欠落で停止しない。次の契約を固定する。

- TCP の `null` 領域と WinSock 上限超過が接続を壊さず拒否される。
- 拒否後も TCP の 1 バイト送受信が成功する。
- UDP の入力拒否時と OS 受信失敗時に送信元の出力値が変わらない。
- UDP の空データグラムがループバックで送受信される。
- `LocalAddress` が port 0 で作成した TCP/UDP ソケットの実ポートを返す。

対象を含む確認コマンドは次のとおり。

```powershell
cmake --build Intermediate\vs --config Debug --target acs_network_socket_io_tests --parallel 1
ctest --test-dir Intermediate\vs -C Debug -R "^ACS.NetworkSocketIo$" --output-on-failure
python scripts\audit_changed_cpp_rules.py --base HEAD
```
