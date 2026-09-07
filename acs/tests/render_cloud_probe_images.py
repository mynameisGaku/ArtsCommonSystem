# SPDX-License-Identifier: Apache-2.0
"""雲のGPU読戻しを同じ変換で並べる。生成画像は画質の合格基準には使わない。"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def validate_run_log(text):
    """成功した全提出と実方策を検査し、旧誤設定や未完了の画像を比較から除く。"""
    expected_frames = [(str(view), mode, str(frame)) for view in range(4) for mode, count in (("native", 2), ("temporal", 32)) for frame in range(1, count + 1)]
    actual_frames = re.findall(r"cloud_probe view=(\d+) mode=(native|temporal) frame=(\d+) ", text)
    actual_work = re.findall(r"cloud_probe work trace=(\d+)x(\d+) temporal=(\d+)", text)
    if actual_frames != expected_frames or actual_work != [("320", "180", "0"), ("80", "45", "1")] * 4:
        raise ValueError("提出回数または描画方策が比較条件と一致しません")
    if re.findall(r"cloud_probe completed result=(\d+)", text) != ["0"]:
        raise ValueError("実行の正常完了が確認できません")


def read_pfm(path):
    """検証プログラムの固定PFM形式を読み、保存時の上下反転だけを戻す。"""
    with path.open("rb") as stream:
        signature = stream.readline().strip()
        if signature not in (b"PF", b"Pf"):
            raise ValueError(f"PFM形式ではありません: {path}")
        width, height = map(int, stream.readline().split())
        if (width, height) != (320, 180):
            raise ValueError(f"診断寸法と一致しません: {path}")
        if stream.readline().strip() != b"-1.0":
            raise ValueError(f"未対応の倍率またはバイト順です: {path}")
        channels = 3 if signature == b"PF" else 1
        values = np.frombuffer(stream.read(), dtype="<f4")
    if values.size != width * height * channels or not np.isfinite(values).all():
        raise ValueError(f"画素数または数値が不正です: {path}")
    return values.reshape(height, width, channels)[::-1]


def image_from_values(values):
    """表示だけを0～1へ制限する。元の浮動小数点画像は変更しない。"""
    rgb = np.repeat(values, 3, axis=2) if values.shape[2] == 1 else values
    return Image.fromarray(np.rint(np.clip(rgb, 0, 1) * 255).astype(np.uint8))


def radiance_display(values):
    """全視点・全モードに同じ圧縮と表示変換を使い、自動露出を混ぜない。"""
    linear = np.maximum(values, 0)
    mapped = linear / (1 + linear)
    return np.where(mapped <= 0.0031308, mapped * 12.92, 1.055 * mapped ** (1 / 2.4) - 0.055)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--font", default="C:/Windows/Fonts/meiryo.ttc")
    parser.add_argument("--validate-only", action="store_true", help="ログの条件だけを検査し、画像を生成しない")
    arguments = parser.parse_args()
    root = arguments.directory
    validate_run_log((root / "run.log").read_text(encoding="utf-8-sig"))
    if arguments.validate_only:
        print("GPU比較の提出回数・実方策・正常完了を確認しました")
        return
    font = ImageFont.truetype(arguments.font, 14)
    headings = ["等倍：不透明度", "32回後：不透明度", "不透明度差の絶対値×8", "等倍：黒背景の散乱光", "32回後：黒背景の散乱光", "等倍：距離（白＝30km）"]
    views = ["地上", "層の高さ（雲内とは未確定）", "上空", "雲内候補（被覆率は据え置き）"]
    canvas = None
    for view in range(4):
        native_alpha = read_pfm(root / f"view{view}_native_02_alpha.pfm")
        temporal_alpha = read_pfm(root / f"view{view}_temporal_32_alpha.pfm")
        native_rgb = read_pfm(root / f"view{view}_native_02_radiance.pfm")
        temporal_rgb = read_pfm(root / f"view{view}_temporal_32_radiance.pfm")
        native_depth = read_pfm(root / f"view{view}_native_02_distance.pfm")
        height, width, _ = native_alpha.shape
        if canvas is None:
            canvas = Image.new("RGB", (width * 6, (height + 30) * 4 + 54), "#151b24")
        draw = ImageDraw.Draw(canvas)
        draw.text((8, 2), "固定時刻・同一媒質のGPU比較／320×180の切り分け用。空・大気・自動露出なし。", font=font, fill="white")
        for column, heading in enumerate(headings):
            draw.text((column * width + 6, 27), heading, font=font, fill="white")
        row_y = 54 + view * (height + 30)
        draw.text((6, row_y), views[view], font=font, fill="white")
        panels = [native_alpha, temporal_alpha, np.abs(native_alpha - temporal_alpha) * 8, radiance_display(native_rgb), radiance_display(temporal_rgb), np.where(native_alpha > 0.003, native_depth / 30000, 0)]
        for column, panel in enumerate(panels):
            canvas.paste(image_from_values(panel), (column * width, row_y + 30))
    # 比較画像も新規作成だけを許し、既存の証拠を上書きしない。
    destination = root / "comparison.png"
    with destination.open("xb") as output:
        canvas.save(output, format="PNG")
    print(destination)


if __name__ == "__main__":
    main()
