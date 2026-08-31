# リファレンスの生成と復旧

ACS リファレンスの正本は `docs/reference/source/`、閲覧用の生成物は `docs/reference/` にあります。生成された HTML、CSS、JavaScript、`manifest.json` は直接編集せず、正本、C++ 宣言、描画生成処理、または共通資源を修正して再生成します。

正本では一つの機能を一つの JSON ファイルで管理します。生成時にはクラス、構造体、列挙型、関数、メンバー関数、メンバー変数、定数、列挙値、型の別名、マクロごとに個別の HTML を作り、機能ページ、所有型、モジュール、検索結果から相互に移動できるようにします。

## 生成前の検査

ACS リポジトリルートで、正本の登録内容と型の解決を確認します。

```pwsh
python -X utf8 -B .\scripts\rebuild_reference_source_manifest.py --check
python -X utf8 -B .\scripts\audit_reference_type_names.py --root . --source .\docs\reference\source
```

正本のファイルを追加、変更、削除した場合は、内容を確認してからマニフェストを再構築します。

```pwsh
python -X utf8 -B .\scripts\rebuild_reference_source_manifest.py
```

## 生成

ローカルHTTPサーバーで `docs/` を表示している場合は、生成を始める前に停止します。Windowsでは既存の非空ディレクトリを新しい非空ディレクトリへ一回の操作で置換できないため、切替中の配信要求を受け付けません。

```pwsh
python -X utf8 -B .\scripts\generate_reference_site.py `
  --acs-root . `
  --source .\docs\reference\source
```

生成処理は次の入力を同じ不変スナップショットへ複製します。

- `src/` の公開宣言
- `docs/reference/source/` の正本
- リファレンスに表示する編集済み画像
- リファレンス共通のCSSとJavaScript

完全なサイトを同一ボリュームの準備ディレクトリへ書き、全ファイル、相対リンク、画像寸法、正本の同一性を検査してから切り替えます。切替直前には排他ロックを持つジャーナルを作成し、二つの生成処理が同時に公開へ進まないようにします。旧出力の退避と新出力の公開は、全量検査を挟まず連続して行います。

## 中断からの復旧

切替中に処理が終了した場合、次回の生成または差分検査はジャーナルと各ツリーの同一性を確認します。旧出力が退避済みで正規位置が空なら旧出力を戻し、新出力が完全に公開済みならその出力を維持します。内容を一意に判定できないツリーは削除せず、次の接頭辞を持つ復旧用ディレクトリへ保持します。

- `.acs-ref-old-`: 切替前の完全な出力
- `.acs-ref-stage-`: 公開前または切替失敗時の生成物
- `.acs-ref-recovery-`: 競合または公開後検査で退避したツリー

これらは `manifest.json`、`.acs-reference-site.json`、`source/` の同一性を確認するまで削除しません。生成入力に使う `.acs-ref-input-` は処理終了時に実体の同一性を確認して削除します。通常の生成成功時も旧出力を一世代保持します。

## 差分と全検査

生成後は差分がないことを確認します。

```pwsh
python -X utf8 -B .\scripts\generate_reference_site.py `
  --acs-root . `
  --source .\docs\reference\source `
  --check
```

CMakeを構成済みの場合は、正本、構文解析、型解決、生成差分、CSS、画像、Markdownをまとめて検査できます。

```pwsh
cmake --build <build-directory> --target acs_reference_check
```

検査完了後にローカルHTTPサーバーを再開し、検索、ドロワー、ツールチップ、キーボード操作、スマートフォン幅、編集済み画像を確認します。
