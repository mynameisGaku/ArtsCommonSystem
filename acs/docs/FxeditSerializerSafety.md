# FFxeditSerializer 永続化の安全境界

`FFxeditSerializer` は `.fxedit` を外部入力として扱い、解析途中の値や書き込み途中の
ファイルを公開しない。従来の `Load`／`Save` は互換 API として残るが、処理の正本は
`TryParseText`／`TryLoad`／`TrySave` である。

## 検証付き API

```cpp
FParticleEmitterDef definitions[16]{};
char names[16 * 32]{};

FFxeditSerializeResult result = FFxeditSerializer::TryLoad(
    path, definitions, names, sizeof(names), 16);
if (!result.Succeeded()) {
    // ErrorName(result.error), result.line, result.os_error を UI に表示する。
}
```

`FFxeditSerializeResult` は安定した error enum、入力行、emitter 数、処理 byte 数、
Win32 error を分離して返す。失敗時に `out_defs` と `out_name_buffer` は変化しない。

## 入力上限

| 対象 | 上限 |
|---|---:|
| ファイル／text | 8 MiB |
| 1 行 | 512 byte |
| 行数 | 65,536 |
| emitter | 1,024 |
| emitter name | 31 byte |
| curve（将来予約、emitter ごと） | 16 |
| keyframe（将来予約、curve ごと） | 256 |
| path | 1,023 wide character |

`EMITTER count` は必須かつ1回だけで、callerの`max_emitters`と名前buffer容量を超えては
ならない。`E<n>` は宣言された範囲内だけを受理する。name buffer は1 emitterあたり
32 byteの固定slotとして扱うため、必要量を満たさない場合は空名への縮退をせず失敗する。

## schemaと値検証

- magicは`ACS_FXEDIT`、versionは現在`1`のみ。
- embedded NUL、長い行、truncation、余分な数値tokenを拒否する。
- 既知のscalar／name keyはemitter単位で1回だけ。重複は曖昧なので拒否する。
- 未知keyは将来versionとの前方互換のため無視する。
- 数値変換は`from_chars`／`to_chars`を使い、process localeに依存しない。
- `NaN`、`Inf`、overflowを拒否する。寿命、放出数、速度、scale、gravity、色、
  予約`spread_radians`には用途別の有限範囲を適用する。
- `speed_min <= speed_max`を全文解析後に検証する。
- nameはprintable ASCIIのみで、引用符と制御文字を拒否する。切り詰めや置換はしない。

v1の`FParticleEmitterDef`にはcurve格納先がまだない。将来予約として次のレコードを
検証して読み捨てる。

```text
E0 curve 0
E0 keyframe 0 0.5 1.0
```

curve index、time（0..1）、valueの有限性、curve/keyframe件数上限を検証する。

## load transaction

`TryLoad`は同じfile handleから64 bit sizeを取得し、上限内のbufferをchecked allocation
する。宣言sizeを完全readした後、現在位置から1 byte probeしてEOFを確認し、さらに
handleのfinal sizeを比較する。短縮・伸長・read／close failureはcommitしない。

parserは一時definitions、名前slot、duplicate mask、curve/keyframe countへ全行を解析する。
全schema・範囲検証が成功した後だけcallerの出力を初期化して一括copyする。

## 原子的 save

`TrySave`は最初に全definitionと全nameを検証し、出力全体をmemory上に構築する。
保存処理は次の順序で行う。

1. 保存先と同じdirectoryに、process／thread／counterを含む一意tempを`CREATE_NEW`で作る。
2. 全byteをchunk writeする。
3. `FlushFileBuffers`でdurable化する。
4. handleをcloseする。
5. `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`でatomic replaceする。
6. open readerが`FILE_SHARE_DELETE`を持つ場合は`FileRenameInfoEx`のPOSIX semanticsへ
   fallbackし、readerの旧snapshotを維持したまま置換する。

write、flush、close、replaceのいずれかが失敗した場合はtempを削除し、既存の保存先を
残す。保存先を先にtruncateしない。
