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

`FFxeditSerializeResult` は安定したエラー列挙、入力行、エミッター数、処理バイト数、
Win32 エラーを分離して返す。失敗時に `out_defs` と `out_name_buffer` は変化しない。

## 入力上限

| 対象 | 上限 |
|---|---:|
| ファイルまたはテキスト | 8 MiB |
| 1 行 | 512 バイト |
| 行数 | 65,536 |
| エミッター | 1,024 |
| エミッター名 | 31 バイト |
| 曲線予約レコード（エミッターごと） | 16 |
| キーフレーム予約レコード（曲線ごと） | 256 |
| パス | 1,023 ワイド文字 |

`EMITTER count` は必須かつ 1 回だけで、呼び出し側の `max_emitters` と名前バッファー容量を超えては
ならない。`E<n>` は宣言された範囲内だけを受理する。名前バッファーは 1 エミッターあたり
32 バイトの固定スロットとして扱うため、必要量を満たさない場合は空名へ縮退せず失敗する。

## スキーマと値の検証

- マジック値は `ACS_FXEDIT`、バージョンは現在 `1` のみ。
- 埋め込み NUL、長い行、切り詰め、余分な数値字句を拒否する。
- 既知のスカラーまたは名前キーはエミッター単位で 1 回だけ。重複は曖昧なので拒否する。
- 未知キーは無視する。
- 数値変換は `from_chars` / `to_chars` を使い、プロセスロケールに依存しない。
- `NaN`、`Inf`、オーバーフローを拒否する。寿命、放出数、速度、拡大率、重力、色、
  予約`spread_radians`には用途別の有限範囲を適用する。
- `speed_min <= speed_max` を全文解析後に検証する。
- 名前は表示可能な ASCII のみで、引用符と制御文字を拒否する。切り詰めや置換はしない。

v1 の `curve` と `keyframe` は予約レコードです。次のレコードを検証して読み捨てます。

```text
E0 curve 0
E0 keyframe 0 0.5 1.0
```

曲線番号、時間（0..1）、値の有限性、曲線とキーフレームの件数上限を検証する。

## 読み込みトランザクション

`TryLoad` は同じファイルハンドルから 64 ビットのサイズを取得し、上限内のバッファーを検査付きで確保
する。申告サイズを完全に読み取った後、現在位置から 1 バイトを試し読みして EOF を確認し、さらに
ハンドルの最終サイズを比較する。短縮、伸長、読み取り失敗、終了失敗では反映しない。

解析器は一時定義、名前スロット、重複マスク、曲線とキーフレームの件数へ全行を解析する。
全スキーマと範囲の検証が成功した後だけ呼び出し側の出力を初期化して一括複製する。

## 原子的な保存

`TrySave` は最初に全定義と全名前を検証し、出力全体をメモリ上に構築する。
保存処理は次の順序で行う。

1. 保存先と同じディレクトリに、プロセス、スレッド、カウンターを含む一意な一時ファイルを `CREATE_NEW` で作る。
2. 全バイトを分割して書き込む。
3. `FlushFileBuffers` で永続化する。
4. ハンドルを閉じる。
5. `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` で原子的に置換する。
6. 開いた読み取り側が `FILE_SHARE_DELETE` を持つ場合は、`FileRenameInfoEx` に実装内の
   `kRenameReplaceIfExists | kRenamePosixSemantics` を指定する方式へ
   切り替え、読み取り側の旧スナップショットを維持したまま置換する。

書き込み、永続化、終了、置換のいずれかが失敗した場合は一時ファイルを削除し、既存の保存先を
残す。保存先を先に切り詰めない。
