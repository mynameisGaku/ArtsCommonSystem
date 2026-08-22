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

## 配布物と外部C++利用の検証

`bbe85e11`のTrueHDRI実装を含むDebug・Release配布物を隔離生成した。生成時は44件の実体ファイルと
manifestの計45件について、生成元と配置先のファイル集合、サイズ、SHA-256が一致した。主要SHA-256は
次のとおりである。

- manifest: `18E2C85E279522CB44F2012AE1E82EDE6770B6D2C9378FC5C3FDF349C59F420D`
- 単一header: `188857534090187C9EB1DEB0F6CCD45F7E84E82704438B8D458D75135DACADD2`
- 正規consumer契約: `70384BCBB557434B3750046F3554D66FFFA8D49685CDC900A8CC05769FFAF3DE`

正規consumer契約は`#include <acs.h>`だけから公式LightData相当CSVを解析し、HDR画像へ加えた90度の
回転を太陽方向へ適用し、測定照度をACSの既定太陽強度へ換算する。配布headerと配布libraryを実際に
リンクしてDebug・Releaseとも実行し、両構成で`truehdri=1`を確認した。配布検証器自身の安全検査と
CMakeを使う独立consumer smokeも両構成で合格した。

外部ACS Framework `5ba0200`へ同じ隔離配布物を明示し、通常アプリ本体をDebug・Releaseともビルドした。
ヘッドレス単体試験は両構成でそれぞれ538件中0件失敗だった。Framework作業ツリーに変更はない。
既存の`AcsEnumReflection.h:90`にある`C4267`警告は両構成で残るが、今回の配布物による新規警告ではない。
TrueHDRIの本体コードと今回のconsumer追加部には、標準コンテナ、`std::`、`friend`を追加していない。
