# シーンテキスト読み込みの安全性

SceneTextLoader API は `.acscene` と `.acsprefab` のテキストを非信頼入力として扱います。
新規コードは `TryLoadAcsceneText` または `TryLoadAcsceneFile` を呼び、
`FSceneTextLoadResult` を確認してください。従来の `LoadAcsceneText` と
`LoadAcsceneFile` は互換 wrapper として残します。

## transactional 保証

検証付き loader は object を構築する前に文書全体を検証します。次に、取り込む tree を
隔離した staging node 配下へ構築し、完成した最上位 nodes を 1 回の構造 commit で
出力先へ移します。完全な置換 request arrays も commit 前に呼び出し側 allocator で
準備するため、allocation failure では呼び出し側の capacity、element address、内容を
維持し、公開だけが途中で失敗することもありません。読み込み失敗時は次を保証します。

- 出力先 node の children は呼び出し前と同じ。
- sprite、rigid-body、material-texture request arrays へ追加しない。
- 検証付き API に渡した `out_root` を変更しない。

互換 wrapper は load を試みる前に `out_root` を clear する従来挙動を維持します。

## 強制する境界

`SceneTextLoader.h` の公開定数は、テキスト/ファイル読み込みの両方へ次の上限を適用します。

- NUL 終端入力テキスト 8 MiB。
- 1 行 2,047 bytes。
- 4,096 nodes。
- 262,144 directive records。
- 1 node あたり 1,024 components。
- sprite/material path 259 bytes。
- 共通の `kNodeMaxTreeDepth` 階層上限。

node transform と directive value は有限値でなければなりません。node ID は一意かつ
非負で、親 ID は解決可能である必要があります。自己 parent と長い cycle を拒否します。
既知 directive は文法を検証し、取り込み対象 node を参照しなければなりません。
未知 directive は無視し、新しい writer が record を追加しても旧 runtime を壊しません。

## 診断

`FSceneTextLoadResult` は安定した `ESceneTextLoadError`、テキストエラーの 1 始まりの
line number、検証済み node/directive 数、成功時の loaded bounds を返します。
`SceneTextLoadErrorName` はログや editor message に使える安定した文字列へ変換します。

ファイル読み込みは open、seek、size limit、short read、埋め込み NUL を区別します。
内容をテキスト parser へ渡す前にファイル全体を読み、途中の NUL byte は文書終端として
扱わず拒否します。
