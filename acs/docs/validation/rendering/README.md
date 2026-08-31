# 描画品質の検証

ACS の描画品質は、処理量や数値の自動検査、対象バックエンドでの GPU 実行、取得画像の比較、目視確認を分けて判定します。自動検査の合格だけで、自然さ、物理的な正しさ、異なるバックエンドでの同等性を承認しません。

この配下の実行例にある相対パスは、ACS リポジトリールートを基準とします。

- [ボリューム雲の性能](cloud-performance.md)
- [ボリューム雲の視覚品質](cloud-visual-quality.md)
- [描画効果の品質](effects-quality.md)
- [空の視覚品質](sky-visual-quality.md)

## 基準画像の承認契約

ボリューム雲と空の基準マニフェストは、`schemaVersion: 1`、`captureGate.result: "PASS"` と、次の `referenceApproval` を持つ必要があります。各ページは、追加で必要なシナリオ結果と `scenarioRawSha256` の正確なキー集合を定義します。

```json
{
  "referenceApproval": {
    "status": "APPROVED",
    "criteriaVersion": 1,
    "reviewer": "<ACS画像品質確認者の識別子>",
    "approvedUtc": "<タイムゾーンを含むISO-8601日時>",
    "contractCanonicalSha256": "<contract.canonicalSha256>",
    "scenarioRawSha256": {
      "<シナリオ名>": "<captures[0].hashes.rawSha256>"
    }
  }
}
```

`reviewer` は空でない200文字以下、`approvedUtc` はタイムゾーンを含む ISO-8601 形式とします。`referenceApproval.contractCanonicalSha256` は、基準マニフェストの `contract.canonicalSha256` および現在のシナリオ設定を正規化したハッシュと一致しなければなりません。`scenarioRawSha256` は各ページが定める全シナリオだけを含み、各値は基準マニフェストの最初の取得 `captures[0].hashes.rawSha256` と一致する必要があります。

シナリオ、カメラ、設定、品質ゲート、比較しきい値、または最初の原画像が変わると承認は無効です。未承認または不整合な基準マニフェストでは比較を開始しません。

## 基準画像の承認状態

ボリューム雲と空の視覚品質取得は、次の共通状態を使用します。

- `captureGate` が不合格の場合、`acceptance` は `FAIL` です。
- 基準マニフェストを指定せず `captureGate` が合格した場合、`acceptance` は `HOLD_REFERENCE_NOT_REVIEWED` です。これは取得経路と自動ゲートの成立だけを示し、画像品質の承認ではありません。
- 現在の取得契約に一致する承認済み基準マニフェストを指定し、取得、基準、画像比較の全ゲートが合格した場合だけ、`acceptance` は `PASS` です。

各ページに記載する目視条件も満たした画像だけを、承認済み基準画像として使用します。
