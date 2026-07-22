# FNetSnapshot の安全境界

`FNetSnapshot` は信頼できない peer と、差し替え可能な `INetTransport` の両方を境界として扱う。
この文書では、wire codec、UDP transport、snapshot director の検証順序と失敗時保証を定義する。

## 固定上限

| 対象 | 上限 |
|---|---:|
| snapshot payload | 4 MiB |
| ring buffer | 4,096 snapshots |
| 1 snapshot の entity record | 65,536 |
| 1 Tick の Receive | 8,192 messages |
| IPv4 UDP datagram | 65,507 bytes |
| snapshot rate の設定値 | 1–1,000 Hz |
| interpolation delay | 0–60 seconds |

`TryInit` は範囲外設定を `kSub_InvalidConfig` で拒否し、以前の session state を維持する。
互換APIの `Init` は、古いcall siteを壊さないため範囲内へ正規化してから `TryInit` を呼ぶ。
特にring件数とpayload長を先に制限するため、設定値の乗算や巨大確保を受信処理まで持ち込まない。

## wire codec

frame は次の完全一致形式である。

```text
[ACSN:4][version:4][tick:4][sequence:4][timestamp_us:8]
[payload_size:4][reserved_crc32:4][payload:N][crc32_footer:4]
```

- 整数はlittle endian固定。
- `sequence == 0` は予約値であり拒否する。
- header内の `reserved_crc32` は常に0。CRC値はfooterだけに置く。
- `payload_size + 36` は入力buffer長と完全一致しなければならない。trailing byteも拒否する。
- encodeのpayloadと出力frame、decodeの入力frameと既存出力storageは重複不可。
  再確保で入力pointerを無効化するin-place decodeや、header書込みでpayloadを上書きするaliasを拒否する。
- payload上限の検証後にだけCRCを計算する。
- CRC対象はmagicを除くversionからpayload末尾まで。
- `EncodeSnapshot` は全引数・容量を確認してから書き始めるため、失敗時にframeを部分書込みしない。
- `DecodeSnapshot` はmagic、version、正規header、完全サイズ、CRC、allocationの順に検証する。
  `out_header` と `out_payload` は最後のallocationまで成功した場合だけ置換される。

## entity payload

directorがringへ入れるpayloadは、次のrecordを末尾までpreflightする。

```text
[entity_id:u32][component_mask:u32][data_size:u32][data:data_size]
```

`entity_id == 0`、record headerの切詰め、`data_size`の加算overflow、payload末尾を越えるdata、
record途中のtrailing byte、entity件数上限超過は `kSub_BadEntityPayload` 相当として棄却する。
preflight完了前にring slot、head、count、last tickを変更しない。
`TryGetInterpolatedSnapshot` も出力配列へviewを書く前に同じ完全検証を行う。

## Checked API と互換API

| Checked API | 成功時 | 失敗時 |
|---|---|---|
| `TryInit` | sessionを一括置換 | 既存session不変 |
| `TryAddEntitySnapshot` | pendingへ1record追加 | pendingと合計サイズ不変 |
| `TryCommitSnapshot` | Send、sequence/stat更新、pending消費 | pending、sequence、ring、stat不変 |
| `TryTick` | 検証済みmessageごとにringへcommit | 問題messageはring不変、詳細件数を返す |

既存の `AddEntitySnapshot`、`CommitSnapshot`、`Tick` はchecked APIを呼ぶ。
`CommitSnapshot` だけは従来のbest-effort契約を維持し、失敗したtickのpendingを破棄する。
再送が必要なcallerは `TryCommitSnapshot` を使う。

`TryTick` は複数messageを消費するため、途中停止より前にcommit済みのsnapshotは保持する。
戻り値の `received_messages`、`accepted_snapshots`、`rejected_messages`、`received_bytes` と
`stop_subcode`、`last_rejected_subcode` で部分進行を判定できる。`stop_subcode == 0` は正常に
受信なしへ到達した状態であり、棄却frameの最後の理由は処理継続時でも失われない。

## Transport 契約

`INetTransport::Receive(out, capacity)` は、成功時に `0..capacity` の値だけを返さなければならない。
`capacity` より大きい値を返す実装は `kSub_TransportContractViolation` として停止し、
返された値をbuffer長として使用しない。これにより、壊れたfake、plugin、stream adapterが
decoderへ範囲外viewを渡すことを防ぐ。

`FUdpTransport` は追加で次を保証する。

- `sendto` 前にIPv4 UDP payload上限を検証する。
- 戻り値が要求長と異なる場合は部分datagram成功として扱わない。
- OSへ渡す受信長を65,507 bytes以下へ制限してからWinsockの`int`へnarrowingする。
- `WSAEMSGSIZE` は一般受信失敗と区別し、`kSub_DatagramTruncated`で報告する。
- 切り詰められたdatagramはsnapshot decoderへ渡さない。

## 診断counter

packet数・byte数・棄却数・transport契約違反数はwrapせず `u32` 最大値で飽和する。
長寿命serverでcounter wrapが正常値に見えることを防ぐ。
`RejectedPackets()` と `TransportContractViolations()` は運用監視および壊れたtransportの特定に使う。

## テスト方針

`tests/net_snapshot_safety_tests.cpp` は次を固定する。

- truncation、trailing byte、CRC改竄、非正規headerの拒否と出力不変
- payload製品上限とencode先の非部分書込み
- allocation failure時のheader/payload不変
- 入出力storage aliasの拒否
- 不正configで既存sessionを置換しないこと
- Send失敗後にchecked commitを同じsequence/pendingで再試行できること
- 正常frame、破損frame、capacity超を返す悪性transportを同一Tickで処理してもringが汚染されないこと
