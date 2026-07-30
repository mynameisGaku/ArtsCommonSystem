# Editor Core 永続化の安全境界

`FEditorTheme` の `.acstheme` と `FEditorWorkspace` の `.acslayout` は、
明示長を受け取る checked API、全件検証後の一括反映、原子的な保存を共通契約とする。
従来の `SaveTheme` / `LoadTheme` / `SaveLayout` / `LoadLayout` は互換ラッパーとして残し、
詳細な失敗理由が必要な呼び出し元は checked API を使う。

## 検証付き API

- `FEditorTheme::TryParseThemeText`
- `FEditorTheme::TryLoadTheme`
- `FEditorTheme::TrySaveTheme`
- `FEditorWorkspace::TryParseLayoutText`
- `FEditorWorkspace::TryLoadLayout`
- `FEditorWorkspace::TrySaveLayout`

結果型は安定したエラー分類、入力行、処理済みバイト数、OS エラーを返す。
workspace の結果は検証した `PANEL` エントリ数も返す。

## 形式

両形式とも JSON ではなく、magic と version を持つ行指向テキストである。
したがって共通 JSON DOM の深さ制限や DOM allocator は適用対象外であり、
各 parser が明示長の範囲だけを直接走査する。

`.acstheme` v1 は `ACS_THEME 1` に続き、preset、3 個の metric、13 個の RGBA
を記述する。v1 の 17 key はすべて必須で、未知 key、重複 key、欠落 key、
余分な token を拒否する。数値変換は C locale に依存せず、非有限値を許可しない。
RGBA は `[0, 1]`、font scale は `[0.25, 4]`、corner radius は `[0, 32]`、
item spacing は `[0, 64]` に限定する。

`.acslayout` v1 は次の順序で構成する。

1. `ACS_EDLAYOUT 1`
2. `IMGUI_INI <byte-size>`
3. 指定バイト数の ImGui ini
4. `PANEL <title> <visible> <dock-target>` 行

ImGui ini は ImGui が所有する不透明データとして扱うが、明示バイト長、全体上限、
embedded NUL を検証してから ImGui に渡す。byte-size が 0 の場合だけ、ImGui 内部の
`strlen` 経路へ静的な NUL 終端空文字列を渡す。非空 ini は元の明示長を維持し、
入力バッファに終端 NUL を要求しない。panel title は内部 ASCII space を許可するが、
空、先頭・末尾 space、制御文字、非 ASCII は拒否する。重複 title、重複 section、
未知 trailing data も拒否する。
未登録 panel の行は互換性のため検証後に無視する。

## 入力上限

| 対象 | 上限 |
| --- | ---: |
| theme 全体 | 64 KiB |
| theme 1 行 | 255 bytes |
| theme 行数 | 64 |
| layout 全体 | 4 MiB |
| ImGui ini | 2 MiB |
| layout 1 行 | 255 bytes |
| layout 行数 | 4096 |
| panel 数 | 32 |
| panel title | 127 bytes |
| persistence path | 1023 wchar |

上限超過は走査や allocation より先に拒否する。ファイル読み込みでは開始時サイズを
記録し、全バイトを読み、EOF probe と最終サイズ確認を行う。途中で伸縮したファイルは
`FileChanged` として失敗させる。

## transactional load

theme は一時 palette と metric に、workspace は固定長の panel change 配列に staging
する。構文、重複、範囲、上限、ImGui context の全検証が成功するまで、既存 theme、
panel visibility、dock target、登録 panel 配列、selection service pointer を変更しない。
workspace はその後に ImGui ini を読み、panel 状態を反映する。

## 原子的 save

保存前に全状態を検証・serialize し、同一 directory の衝突しにくい一時ファイルへ
全量書き込み、flush、close してから置換する。Windows の通常置換が共有 reader に
阻まれた場合は POSIX rename semantics を試す。いずれも失敗した場合は一時ファイルを
削除し、既存の保存先を保持する。

## テスト観点

`editor_core_persistence_safety_tests.cpp` は canonical v1、重複、非有限値、embedded NUL、
切断 ini、NUL 終端を持たない空 ini、入力上限、失敗時の状態不変、空白入り panel title、
開いた旧 reader を維持した原子的 theme 置換と load round-trip を検証する。
