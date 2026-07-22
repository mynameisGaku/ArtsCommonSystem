# UI テキスト入力の編集機能と安全性

`FTextInput` は UTF-8 の1行エディタである。`FString` がバイト指向のため、
cursor と selection anchor はバイトオフセットで保持するが、公開操作の前後では必ず
デコード済みコードポイントの境界へ正規化する。

## 編集 API

- `Left` と `Right` は1バイトではなく1コードポイントずつ移動する。
- `Home` と `End` は先頭と末尾へ移動する。
- `Shift+Left` / `Shift+Right` は selection anchor を固定し、cursor 側を1コードポイントずつ
  移動して選択を拡張または縮小する。anchor を越えた逆方向の選択も保持できる。
- `Shift+Home` / `Shift+End` は anchor を固定したまま cursor を先頭または末尾へ移動する。
- `Ctrl+A` は文字列全体を選択する。左右どちらの Ctrl も同じ扱いになる。Ctrl+Alt として
  通知される AltGr は文字入力を優先し、全選択と誤認しない。
- `Backspace` は cursor の直前、`Delete` は cursor の位置にあるコードポイントを削除する。
- 選択中の文字入力は選択範囲を置換し、`Backspace` と `Delete` はどちらも選択範囲全体を削除する。
- 選択中の `Left` と `Right` は、それぞれ選択範囲の先頭と末尾へ cursor を折り畳む。
- `TrySetCursorByteOffset` は範囲内かつ UTF-8 境界上のオフセットだけを受理し、成功時に選択を解除する。
- `CursorByteOffset` は正規化済みの cursor を返す。

プログラムからの選択操作には次の API を使う。

- `HasSelection`
- `SelectionStart` / `SelectionEnd`
- `TrySetSelection`
- `ClearSelection`
- `SelectAll`

`TrySetSelection(start, end)` は `start <= end` かつ両端が UTF-8 境界の場合だけ成功し、
cursor を `end` へ置く。失敗時は cursor、anchor、end-follow 状態を変更しない。
`SelectionStart` と `SelectionEnd` は anchor と cursor の向きに依存せず、小さい側と
大きい側のオフセットを返す。

`FUiInput` は `FInput::IsKeyDown` から左右の Shift / Ctrl / Alt / Super を読み取り、
`FUiKeyModifiers` の1フレーム分のスナップショットとして3引数版
`FWidget::OnKey(key, pressed, modifiers)` へ渡す。基底の3引数版は従来の2引数版へ転送するため、
既存 widget が2引数版だけを override していてもキー配信は継続する。2引数版を直接呼ぶ
既存コードも修飾キーなしとして従来どおり動作する。`FTextInput` 派生型についても3引数版が
修飾キー snapshot を一時保持して仮想2引数版へ転送するため、2引数版だけを override した
既存派生へ実 `Dispatch` の押下と解放が届く。組み込み編集も残す override は
`FTextInput::OnKey(key, pressed)` を呼ぶ。編集キーは押下で `pressed=true`、
解放で `pressed=false` を配信する。`Ctrl+A` は A の押下を受けた widget の実体 ID を記録し、
Ctrl を先に離した場合でも同じ生存 widget へ対応する A の解放を送る。

新規入力、end-follow 中の外部テキスト更新、pointer focus では従来どおり cursor が末尾に
置かれる。pointer API から glyph hit 情報を得られないため、クリック位置への cursor 配置は
まだ行わない。

## `FUiInput` の寿命保証

`FUiInput` は hover、pointer-down、focus の対象を非所有の生ポインタでは保持しない。
各 `FWidget` の生存中アドレスを整数化した比較専用 address token、構築 module が使った
generation counter のアドレスを整数化した module token、0 を除外した module 内 generation
の3要素 identity だけを保存し、`Dispatch` のたびに現在生存する root subtree を走査して
対象を解決する。address/module token はポインタへ戻さず、決して参照しない。module token は
`FWidget` constructor が一度だけ member へ保存し、`InputIdentity` を呼ぶ側の module では
再計算しない。

- root の複合 identity が変わった場合は、以前の root や widget を参照する前に全追跡を破棄する。
- allocator が以前と同じアドレスへ新しい root/widget を構築しても、実体 ID が異なるため
  古い hover/focus を継承しない。
- inline counter が DLL ごとに独立し、同じ widget address と generation を同時または逐次
  使用しても、両 DLL が load 中なら保存済み module token が区別する。`u64` 周回時も予約値0を
  返さず、3要素の比較を維持する。
- 同じ root 内で child が除去された場合も、現在の所有 subtree に ID が見つからなければ
  追跡を破棄し、解放済み領域を参照しない。
- 非表示になった widget とその subtree は入力対象から外し、対応する一時状態も解除する。
- `Reset()` は widget を参照せず追跡 ID だけを破棄するため、root の破棄直前にも呼べる。
  同じ生存 root を再び `Dispatch` すると、新規採用時に subtree の一時入力フラグを初期化する。
  `Reset(live_root)` は、生存が保証された root の hover/focused/pressed も再帰的に解除する。

DLL を unload して同じ base address へ reload すると、module token の元になった counter
address と widget address に加えて generation まで再利用され得る。この境界は identity だけで
旧実体と新実体を区別できないため、host 側の `FUiInput` は module 所有 root がまだ生存中なら
`Reset(live_root)` を呼んでから root を破棄・unload する。root が既に破棄済みの場合も、
reload または新 root の `Dispatch` より前に必ず引数なし `Reset()` を呼ぶ。root destruction /
module unload 境界での Reset は必須契約である。

`Dispatch(root)` の呼び出し中だけは、呼び出し側が `root` 自身の寿命を保証しなければならない。
callback から current child または別 child を除去してよい。pointer move/down/up、text、
key の各配信後は保存ポインタを使わず、必要な対象を複合 identity から解決し直す。ただし
実行中の `Dispatch` が戻る前に root 自身を破棄してはならない。

複合 identity と callback lifetime guard は内部実装であり、保存・通信に使う永続 handle でも
安定 plugin ABI でもない。`FWidget` と `FUiInput` のレイアウトも変更されているため、
旧 DLL / plugin と新 DLL の混在は未サポートである。この変更を取り込むときは engine、
game module、plugin を同じ revision からフル rebuild する。

## 通知中の child 破棄

`TObservable::Notify` は allocation-free の stack guard を登録する。listener が `TObservable` を
所有 object ごと同期破棄した場合、destructor が実行中 guard を失効させ、復帰した `Notify` は
破棄済み member に触れず直ちに終了する。残りの listener は呼ばれない。
ただし MVVM callback 側は owner の破棄を最後の操作にし、破棄後は `new_value`、`user`、
owner を再参照せず直ちに return しなければならない。

`FButton` の `clicked` pulse と `FTextInput` の編集 commit / 互換 `OnKey` bridge も widget の
stack lifetime guard を使う。subscriber または派生 callback が current child を親ツリーから
除去した場合、破棄後に `clicked=false`、selection 正規化、modifier snapshot 復元のため
`this` へ再接触しない。これらは UI thread 上の同期 callback を対象とする。

## 非表示 root の描画

`FUiRenderer::Render` は `root.visible == false` のとき、root の `Layout`、
`FSpriteBatch::Begin/End`、root/child の `Render` をすべて省略する。可視性 gate は
GPU-free test と共通化され、hidden root の layout callback と render callback がどちらも
呼ばれないことを検証する。

## 入力検証と上限

入力経路は C0/C1 制御文字、`DEL`、UTF-16 surrogate、`U+10FFFF` を超える値、
Unicode の line separator と paragraph separator を拒否する。プラットフォーム由来の
UTF-8 も共通の正規デコーダを通し、overlong encoding、surrogate、範囲外 scalar、
不正な lead/continuation byte を widget へ渡さない。正規に符号化された `U+FFFD` は
通常文字として受理する。

dispatcher は `TryDecodeUtf8` の成否で、正規の `U+FFFD` と不正入力の診断値を区別する。
互換 API の `DecodeUtf8` も残している。どちらも不正入力では少なくとも lead byte を消費し、
NUL 終端を越えて読まない。

既定上限 `kDefaultMaxTextBytes` は 4096 bytes、設定可能な hard limit
`kHardMaxTextBytes` は 1 MiB である。`TrySetMaxTextBytes` で上限を変更できる。
選択置換では「元の長さ - 選択長 + 挿入文字長」が上限以内の場合だけ成功し、
コードポイントを途中まで受け入れることはない。上限を既存値より小さくしても文字列を
暗黙に切り詰めず、削除は引き続き利用できる。

## 外部 binding と正規化

MVVM binding は editor API を経由せず `text` を置き換えられる。編集、cursor/selection
照会、描画の前に次を行う。

- end-follow 中の端点は新しい末尾へ追従する。
- それ以外の端点は、現在長以下で最も近い直前のコードポイント境界へ丸める。
- cursor と anchor が同じ位置へ折り畳まれた場合は end-follow 状態も同期し、後続の文字列
  伸長で古い選択が復活しないようにする。
- 外部から不正 UTF-8 が渡された場合は1バイトずつ安全に進み、範囲外読みや停止を避ける。

## 確保失敗時の契約

挿入、選択置換、削除は、現在の文字列と同じ allocator を使う一時 `FString` 上で結果を
完成させる。reserve と append がすべて成功した後だけ observable へ move する。
無効 scalar、境界違反、上限超過、確保失敗では、次の状態をすべて維持する。

- 以前の文字列 bytes
- cursor offset
- selection anchor offset
- 各端点の end-follow 状態
- observable 通知回数（失敗時は通知しない）

外部 binding 後の端点正規化も編集成功まではローカルに staged されるため、編集失敗を
内部状態へ部分的に commit しない。

## 描画と現在の範囲

caret と selection highlight は `FUiRenderer::MeasureTextBytes` で UTF-8 prefix だけを
測り、NUL 終端の一時文字列を確保せずに位置を決める。highlight は文字より先に
`input_selection` 色で描画し、入力欄の左右内側へ clamp する。

クリップボード操作、IME composition range、pointer glyph hit-test、文字と caret の
横スクロールおよび clipping は今後の拡張対象である。

テストではマルチバイト選択・置換・削除、Shift による順方向・逆方向の選択と折り畳み、
`Ctrl+A`、従来 `OnKey` override への互換転送、cursor 移動、外部 binding 短縮、境界違反、
上限超過、強制 OOM、失敗時の通知抑止を検証する。選択ハイライトの純粋な矩形計算も、
通常範囲、左右のはみ出し、長い選択、幅・高さ不足について GPU なしで検証する。
さらに実 `FInput` フィードから押下/解放と左右修飾キーを Dispatch し、root の同一アドレス
再構築および同一 root 内の focused child 除去後にも古い入力状態を再利用しないことを検証する。
全入力 callback 中の current/other child 除去、`TObservable` 通知中の current widget 破棄、
挿入と erase commit 中の current/other child 除去、nested 3→2 `OnKey` 後の外側 modifier
snapshot 復元、引数なし `Reset` 後の再初期化、hidden root の layout/render 抑止も
GPU なしで検証する。
