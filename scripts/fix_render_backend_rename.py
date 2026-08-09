#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# レンダーバックエンドのメンバー参照を更新する。
# enum class の変換で誤って E 接頭辞が付いた参照を元へ戻す。
#
# 対象パターン: `<dot|arrow>E(Format|CullMode|PrimitiveTopology|BufferUsage|BlendMode|SampleFormat|PixelFormat|ShaderStage)` を element-access として認識し、E を剥がす。
#   `desc.EFormat`        -> `desc.Format`        (Diligent::TextureDesc.Format)
#   `desc->EPrimitiveTopology` -> `desc->PrimitiveTopology`
#   `gp.RasterizerDesc.ECullMode` -> `gp.RasterizerDesc.CullMode`
# 等。
#
# 影響を限定するため src/render/Diligent/ と src/render/Dx12/ にのみ適用。
# ACS の enum 値リテラル (`EFormat::R8G8B8A8`) は `.` の左に identifier が無いか
# `::` が来るので影響しない。

import os
import re
from pathlib import Path

ROOT_DIRS = [
    r"C:\Users\g0190\OneDrive\Desktop\acs_github\acs\src\render\Diligent",
    r"C:\Users\g0190\OneDrive\Desktop\acs_github\acs\src\render\Dx12",
]

# member-access 後の E-prefix を持つ field 名。Diligent/D3D12/DXGI 由来の field 名は
# 全部 PascalCase なので、ACS が誤 rename したものをここで列挙する。
NAMES = [
    "Format", "CullMode", "PrimitiveTopology", "BufferUsage",
    "BlendMode", "SampleFormat", "PixelFormat", "ShaderStage",
]

# pattern: `.` or `->` の直後の `E<Name>` を `<Name>` に戻す。
# `\bE` は前置 word-boundary 無しでも OK だが、安全のため `(?<=[.>])` の lookbehind を使う。
PATTERN = re.compile(r"(?<=[.>])E(" + "|".join(NAMES) + r")\b")

def main():
    total_replaces = 0
    files_changed = 0
    for root in ROOT_DIRS:
        for dirpath, _, files in os.walk(root):
            for fname in files:
                if not (fname.endswith(".cpp") or fname.endswith(".h")):
                    continue
                path = Path(dirpath) / fname
                try:
                    content = path.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    continue
                new_content, n = PATTERN.subn(r"\1", content)
                if n > 0:
                    files_changed += 1
                    total_replaces += n
                    path.write_text(new_content, encoding="utf-8")
                    repo_root = Path(r"C:\Users\g0190\OneDrive\Desktop\acs_github")
                    rel = path.relative_to(repo_root)
                    print(f"  {rel}: {n} fixes")
    print(f"=== Summary ===")
    print(f"Files changed:    {files_changed}")
    print(f"Total replaces:   {total_replaces}")

if __name__ == "__main__":
    main()
