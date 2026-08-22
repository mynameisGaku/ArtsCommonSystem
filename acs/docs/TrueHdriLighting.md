# TrueHDRI照明契約

## 目的

TrueHDRIはGIそのものではない。測定された環境放射輝度をIBLへ渡し、HDR画像から分離した太陽を
平行光として同じ方向・色・強度で再現するための入力である。IBLが拡散環境光と鏡面反射を作り、
SSGI、VXGI、ライトマップなどが場面内の間接反射を追加する。

公式資料:

- <https://www.bandainamcostudios.com/projects/truehdri>
- <https://www.bandainamcostudios.com/projects/truehdri/library>
- <https://www.fab.com/listings/b65cba4d-e038-4d8a-96fc-73fde41f039e?lang=ja>

## 現在の実装

`FTrueHdriLightData::ParseSrgbCsv()`は公式LightData CSVをNUL終端や動的確保へ依存せず読み取る。
次の値を相互検証してから一つの値型として返す。

- パノラマ上の太陽中心UV座標
- 太陽の高度と方位
- sRGB色域の線形RGB色照度
- 公式CSV内の正規化色と照度尺度

画像座標は、Vの50%を高度0度、Uの50%を方位0度（+Z）、Uの75%を方位90度（+X）として
角度行と照合する。経度の継ぎ目では359.88度と-0.12度を同じ方向として比較する。

`ResolveSunDirection()`はHDR画像へ加えるY軸回転と同じ角度を太陽へ適用する。
`ResolveDirectionalLightColor()`は測定照度を勝手にACSの光強度へ直結せず、利用側が指定した
「基準照度と、そのときのACS強度」を通して換算する。現在のACS平行光は物理単位ではないため、
約10万luxをそのまま`ALightComponent3D`へ渡してはいけない。

## 実時間描画で使うデータ

現在の`CImageAssetLoader`はRadiance RGBE（`.hdr`）を線形浮動小数へ展開できるが、OpenEXRは
まだ読み込めない。また、`CImageBasedLighting`の環境、拡散照度、鏡面事前積分は
`R11G11B10_Float`を使う。太陽を含むUnclipped画像はこの経路の表現範囲と有限標本積分に適さず、
太陽が二重照明や点状の異常値になる。

実時間経路では次を一組として使う。

1. sRGB / Radiance RGBE / ClippedのHDR画像を環境IBLへ渡す。
2. 対応するLightData CSVを`FTrueHdriLightData`で読む。
3. CSVから求めた太陽方向を平行光へ渡す。
4. HDR画像に回転を加える場合は、同じ角度を`ResolveSunDirection()`へ渡す。
5. 平行光に置換した太陽円盤をIBL積分から除外する。

Unclipped画像を保った経路、OpenEXR読込、HDR画像のGPU回転、Editor上の作成・取消・再取得は
後続実装とする。今回の値型はRenderモジュールだけに属し、Editorをリンクしない通常C++からも使える。
