# 基盤最適化の公開契約と生成物同期

## 目的

基盤最適化で追加した型やヘッダーを、実装だけ存在して利用方法が見つからない状態にしない。
変更した C++ の日本語コメント規約、公開 API リファレンス、生成済み module manifest を同じ
検証 wave で同期する。

## 判断と期待効果

| 判断 | 期待効果 | 依存関係 | 検証方法 |
|---|---|---|---|
| `Module.cmake` を手編集しない | 手書き一覧と実ファイルの差異を残さない | `acsbuild` の module 配下再帰収集 | `dotnet run --project acs/tools/acsbuild -- gen` を二度実行し、二度目が無差分 |
| 公開型の一部を必須リファレンスとして固定する | 新しい基盤 API の掲載忘れを即時検出する | `docs/reference/data/*.js` と C++ 宣言 | `audit_reference_type_names.py` の通常実行と自己テスト |
| 変更行のコード末尾コメントを拒否する | コメントの対象を明確にし、共通 C++ 規約を機械検証する | Git の比較基点 | `audit_changed_cpp_rules.py` の通常実行と自己テスト |
| 性能契約テストへ宣言コメントを付ける | 診断値が何を証明するかをコードから追跡できる | Wave A/B の計測 API | focused test と Debug / Release の全 test |

## Module 生成

`acs/tools/acsbuild` の `Collect()` は各 module のディレクトリ以下にある `*.h` と `*.cpp`
を再帰収集する。`AcsModule` に公開ヘッダーを個別登録する API はないため、ヘッダー追加だけを
理由に `Build.cs` へ重複一覧を作らない。

今回の再生成対象は、追加ヘッダーを持つ `container`、`gameframework`、`platform`、
`render` の `Module.cmake` である。生成器を再実行して無差分になることを、生成結果が安定した
証拠とする。

## 公開 API リファレンス

型名監査は従来の「掲載名が宣言と一致する」検査に加え、基盤最適化で追加した公開型のうち
32 型が少なくとも一つのリファレンス項目を持つことを確認する。内部実装である
`acs::detail::TGamepadPollScheduler` は公開 API の完全性対象へ含めない。

必須型を改名または廃止する場合は、C++ 宣言、参照項目、監査内の限定一覧を同じ変更で更新する。
一覧を全宣言へ拡大しないのは、内部型や意図的に未掲載の実装詳細まで公開契約へ固定しないためである。

## 検証

```powershell
python acs/scripts/audit_changed_cpp_rules.py --self-test
python acs/scripts/audit_reference_type_names.py --self-test
python acs/scripts/audit_reference_type_names.py
python acs/scripts/audit_cpp_conventions.py
dotnet run --project acs/tools/acsbuild -- gen
dotnet run --project acs/tools/acsbuild -- gen
```

製品側の挙動と ABI は Debug / Release build、全 CTest、配布物生成で最終確認する。変更行監査が
他担当中の未統合ファイルだけを報告する場合、例外で隠さず、その担当修正を統合してから通過させる。
