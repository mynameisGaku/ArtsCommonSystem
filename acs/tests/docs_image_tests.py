# SPDX-License-Identifier: MIT

from __future__ import annotations

import binascii
import re
import struct
import tempfile
import unittest
import zlib
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


ACS_ROOT = Path(__file__).resolve().parents[1]
DOCS_ROOT = ACS_ROOT / "docs"
EDITED_ROOT = DOCS_ROOT / "media" / "captures" / "edited"
JAPANESE_PATTERN = re.compile(r"[ぁ-んァ-ヶ一-龠々〆ヵヶ]")
IMAGE_TAG_PATTERN = re.compile(r"<\s*img\b", flags=re.I)
FENCE_PATTERN = re.compile(r"^ {0,3}(`{3,}|~{3,})")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_COLOR_NAMES = {
    0: "Grayscale",
    2: "RGB",
    3: "Indexed",
    4: "GrayscaleAlpha",
    6: "RGBA",
}
VOID_ELEMENTS = {
    "area",
    "base",
    "br",
    "col",
    "embed",
    "hr",
    "img",
    "input",
    "link",
    "meta",
    "param",
    "source",
    "track",
    "wbr",
}


@dataclass(frozen=True)
class FImageReference:
    document: Path
    line: int
    src: str | None
    alt: str | None
    loading: str | None
    width: str | None
    height: str | None
    anchor_href: str | None
    in_figure: bool


@dataclass(frozen=True)
class FResponsiveSourceReference:
    document: Path
    line: int
    srcset: str | None
    width: str | None
    height: str | None
    in_figure: bool
    in_picture: bool


@dataclass(frozen=True)
class FPngInfo:
    width: int
    height: int
    bit_depth: int
    color_type: int
    transparent_pixels: int

    @property
    def color_name(self) -> str:
        return PNG_COLOR_NAMES.get(self.color_type, f"Unknown({self.color_type})")


class FImageReferenceParser(HTMLParser):
    def __init__(self, document: Path) -> None:
        super().__init__(convert_charrefs=True)
        self.document = document
        self.references: list[FImageReference] = []
        self.responsive_sources: list[FResponsiveSourceReference] = []
        self._open_elements: list[tuple[str, dict[str, str | None]]] = []

    def handle_starttag(
        self,
        tag: str,
        attrs: list[tuple[str, str | None]],
    ) -> None:
        self._handle_start(tag, attrs, is_self_closing=False)

    def handle_startendtag(
        self,
        tag: str,
        attrs: list[tuple[str, str | None]],
    ) -> None:
        self._handle_start(tag, attrs, is_self_closing=True)

    def handle_endtag(self, tag: str) -> None:
        normalized_tag = tag.casefold()
        for index in range(len(self._open_elements) - 1, -1, -1):
            if self._open_elements[index][0] == normalized_tag:
                del self._open_elements[index:]
                return

    def _handle_start(
        self,
        tag: str,
        attrs: list[tuple[str, str | None]],
        *,
        is_self_closing: bool,
    ) -> None:
        normalized_tag = tag.casefold()
        attributes = {name.casefold(): value for name, value in attrs}
        if normalized_tag == "img":
            anchor = next(
                (
                    open_attrs
                    for open_tag, open_attrs in reversed(self._open_elements)
                    if open_tag == "a"
                ),
                None,
            )
            self.references.append(
                FImageReference(
                    document=self.document,
                    line=self.getpos()[0],
                    src=attributes.get("src"),
                    alt=attributes.get("alt"),
                    loading=attributes.get("loading"),
                    width=attributes.get("width"),
                    height=attributes.get("height"),
                    anchor_href=anchor.get("href") if anchor is not None else None,
                    in_figure=any(
                        open_tag == "figure" for open_tag, _ in self._open_elements
                    ),
                )
            )
        elif normalized_tag == "source" and any(
            open_tag == "picture" for open_tag, _ in self._open_elements
        ):
            self.responsive_sources.append(
                FResponsiveSourceReference(
                    document=self.document,
                    line=self.getpos()[0],
                    srcset=attributes.get("srcset"),
                    width=attributes.get("width"),
                    height=attributes.get("height"),
                    in_figure=any(
                        open_tag == "figure" for open_tag, _ in self._open_elements
                    ),
                    in_picture=True,
                )
            )

        if not is_self_closing and normalized_tag not in VOID_ELEMENTS:
            self._open_elements.append((normalized_tag, attributes))


def _without_markdown_code_blocks(text: str) -> str:
    """Markdownのコードブロックを行番号を保ったまま監査対象から除外します。"""
    output: list[str] = []
    fence_character: str | None = None
    fence_length = 0
    for line in text.splitlines(keepends=True):
        match = FENCE_PATTERN.match(line)
        if fence_character is None and match:
            marker = match.group(1)
            fence_character = marker[0]
            fence_length = len(marker)
            output.append("\n" if line.endswith(("\n", "\r")) else "")
            continue
        if fence_character is not None:
            if match:
                marker = match.group(1)
                if marker[0] == fence_character and len(marker) >= fence_length:
                    fence_character = None
                    fence_length = 0
            output.append("\n" if line.endswith(("\n", "\r")) else "")
            continue
        output.append(line)
    return "".join(output)


def _collect_image_references(
    docs_root: Path,
) -> tuple[list[FImageReference], list[FResponsiveSourceReference]]:
    references: list[FImageReference] = []
    responsive_sources: list[FResponsiveSourceReference] = []
    documents = sorted(docs_root.rglob("*.md")) + sorted(docs_root.rglob("*.html"))
    for document in documents:
        text = document.read_text(encoding="utf-8")
        if not IMAGE_TAG_PATTERN.search(text):
            continue
        if document.suffix.casefold() == ".md":
            text = _without_markdown_code_blocks(text)
        parser = FImageReferenceParser(document)
        parser.feed(text)
        parser.close()
        references.extend(parser.references)
        responsive_sources.extend(parser.responsive_sources)
    return references, responsive_sources


def _display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(ACS_ROOT.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def _reference_location(reference: FImageReference) -> str:
    return f"{_display_path(reference.document)}:{reference.line}"


def _responsive_source_location(reference: FResponsiveSourceReference) -> str:
    return f"{_display_path(reference.document)}:{reference.line}"


def _resolve_local_target(document: Path, raw_target: str, docs_root: Path) -> Path:
    target = raw_target.strip()
    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc or target.startswith(("/", "\\")):
        raise ValueError(f"ローカル相対参照ではありません: {raw_target}")
    path_text = unquote(parsed.path).replace("\\", "/")
    if not path_text:
        raise ValueError("参照先のパスが空です")
    resolved = (document.parent / path_text).resolve()
    try:
        resolved.relative_to(docs_root.resolve())
    except ValueError as error:
        raise ValueError(f"acs/docs の外を参照しています: {raw_target}") from error
    return resolved


def _positive_dimension(value: str | None) -> int | None:
    if value is None:
        return None
    normalized = value.strip()
    if not re.fullmatch(r"[1-9][0-9]*", normalized):
        return None
    return int(normalized)


def _paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def _unfilter_scanlines(
    compressed: bytes,
    *,
    width: int,
    height: int,
    channels: int,
    bit_depth: int,
) -> list[bytes]:
    bits_per_pixel = channels * bit_depth
    row_size = (width * bits_per_pixel + 7) // 8
    bytes_per_pixel = max(1, (bits_per_pixel + 7) // 8)
    try:
        filtered = zlib.decompress(compressed)
    except zlib.error as error:
        raise ValueError(f"IDATを展開できません: {error}") from error
    expected_size = height * (row_size + 1)
    if len(filtered) != expected_size:
        raise ValueError(
            f"IDAT展開サイズがIHDRと一致しません: {len(filtered)} != {expected_size}"
        )

    rows: list[bytes] = []
    previous = bytearray(row_size)
    offset = 0
    for row_index in range(height):
        filter_type = filtered[offset]
        offset += 1
        source = filtered[offset : offset + row_size]
        offset += row_size
        reconstructed = bytearray(row_size)
        for byte_index, value in enumerate(source):
            left = (
                reconstructed[byte_index - bytes_per_pixel]
                if byte_index >= bytes_per_pixel
                else 0
            )
            above = previous[byte_index]
            upper_left = (
                previous[byte_index - bytes_per_pixel]
                if byte_index >= bytes_per_pixel
                else 0
            )
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                predictor = _paeth_predictor(left, above, upper_left)
            else:
                raise ValueError(
                    f"未対応のPNGフィルターです: row={row_index}, filter={filter_type}"
                )
            reconstructed[byte_index] = (value + predictor) & 0xFF
        rows.append(bytes(reconstructed))
        previous = reconstructed
    return rows


def _palette_indices(row: bytes, width: int, bit_depth: int) -> list[int]:
    if bit_depth == 8:
        return list(row[:width])
    if bit_depth not in {1, 2, 4}:
        raise ValueError(f"Indexed PNGのbit depthが不正です: {bit_depth}")
    mask = (1 << bit_depth) - 1
    indices: list[int] = []
    for byte in row:
        for shift in range(8 - bit_depth, -1, -bit_depth):
            indices.append((byte >> shift) & mask)
            if len(indices) == width:
                return indices
    raise ValueError("Indexed PNGのpixel数がIHDRと一致しません")


def _count_transparent_pixels(
    rows: list[bytes],
    *,
    width: int,
    bit_depth: int,
    color_type: int,
    transparency: bytes | None,
) -> int:
    sample_size = bit_depth // 8 if bit_depth in {8, 16} else 0
    transparent_pixels = 0
    if color_type == 6:
        if sample_size == 0:
            raise ValueError(f"RGBA PNGのbit depthが不正です: {bit_depth}")
        pixel_size = 4 * sample_size
        opaque_alpha = b"\xff" * sample_size
        for row in rows:
            transparent_pixels += sum(
                row[offset + 3 * sample_size : offset + 4 * sample_size]
                != opaque_alpha
                for offset in range(0, width * pixel_size, pixel_size)
            )
        return transparent_pixels

    if color_type == 4:
        if sample_size == 0:
            raise ValueError(f"GrayscaleAlpha PNGのbit depthが不正です: {bit_depth}")
        pixel_size = 2 * sample_size
        opaque_alpha = b"\xff" * sample_size
        for row in rows:
            transparent_pixels += sum(
                row[offset + sample_size : offset + 2 * sample_size]
                != opaque_alpha
                for offset in range(0, width * pixel_size, pixel_size)
            )
        return transparent_pixels

    if transparency is None:
        return 0

    if color_type == 3:
        for row in rows:
            for palette_index in _palette_indices(row, width, bit_depth):
                alpha = transparency[palette_index] if palette_index < len(transparency) else 255
                transparent_pixels += alpha != 255
        return transparent_pixels

    if color_type == 0:
        if len(transparency) != 2 or sample_size == 0:
            raise ValueError("Grayscale PNGのtRNSが不正です")
        transparent_sample = int.from_bytes(transparency, "big")
        for row in rows:
            for offset in range(0, width * sample_size, sample_size):
                sample = int.from_bytes(row[offset : offset + sample_size], "big")
                transparent_pixels += sample == transparent_sample
        return transparent_pixels

    if color_type == 2:
        if len(transparency) != 6 or sample_size == 0:
            raise ValueError("RGB PNGのtRNSが不正です")
        transparent_sample = tuple(
            int.from_bytes(transparency[offset : offset + 2], "big")
            for offset in range(0, 6, 2)
        )
        pixel_size = 3 * sample_size
        for row in rows:
            for offset in range(0, width * pixel_size, pixel_size):
                sample = tuple(
                    int.from_bytes(
                        row[offset + channel * sample_size : offset + (channel + 1) * sample_size],
                        "big",
                    )
                    for channel in range(3)
                )
                transparent_pixels += sample == transparent_sample
        return transparent_pixels

    raise ValueError(f"color type {color_type}ではtRNSを使用できません")


def _read_png(path: Path) -> FPngInfo:
    content = path.read_bytes()
    if not content.startswith(PNG_SIGNATURE):
        raise ValueError("PNG signatureがありません")

    offset = len(PNG_SIGNATURE)
    chunks: list[tuple[bytes, bytes]] = []
    found_iend = False
    while offset < len(content):
        if len(content) - offset < 12:
            raise ValueError("PNG chunk headerが途中で切れています")
        chunk_length = struct.unpack_from(">I", content, offset)[0]
        chunk_end = offset + 12 + chunk_length
        if chunk_end > len(content):
            raise ValueError("PNG chunk dataが途中で切れています")
        chunk_type = content[offset + 4 : offset + 8]
        chunk_data = content[offset + 8 : offset + 8 + chunk_length]
        stored_crc = struct.unpack_from(">I", content, offset + 8 + chunk_length)[0]
        actual_crc = binascii.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if stored_crc != actual_crc:
            raise ValueError(f"{chunk_type.decode('ascii', errors='replace')} chunkのCRCが不正です")
        chunks.append((chunk_type, chunk_data))
        offset = chunk_end
        if chunk_type == b"IEND":
            found_iend = True
            break

    if not found_iend:
        raise ValueError("IEND chunkがありません")
    if offset != len(content):
        raise ValueError("IEND chunkの後ろに不要なdataがあります")
    if not chunks or chunks[0][0] != b"IHDR":
        raise ValueError("先頭chunkがIHDRではありません")

    ihdr_chunks = [data for chunk_type, data in chunks if chunk_type == b"IHDR"]
    if len(ihdr_chunks) != 1 or len(ihdr_chunks[0]) != 13:
        raise ValueError("IHDR chunkが不正です")
    width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", ihdr_chunks[0]
    )
    if width == 0 or height == 0:
        raise ValueError("IHDRのwidthまたはheightが0です")
    if compression != 0 or filtering != 0:
        raise ValueError("未対応のPNG圧縮またはfilter方式です")
    if interlace != 0:
        raise ValueError("透明度を検査できないinterlace PNGです")

    channels_by_color_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    channels = channels_by_color_type.get(color_type)
    if channels is None:
        raise ValueError(f"PNG color typeが不正です: {color_type}")
    valid_bit_depths = {
        0: {1, 2, 4, 8, 16},
        2: {8, 16},
        3: {1, 2, 4, 8},
        4: {8, 16},
        6: {8, 16},
    }
    if bit_depth not in valid_bit_depths[color_type]:
        raise ValueError(f"color type {color_type}に対するbit depthが不正です: {bit_depth}")

    idat = b"".join(data for chunk_type, data in chunks if chunk_type == b"IDAT")
    if not idat:
        raise ValueError("IDAT chunkがありません")
    transparency_chunks = [data for chunk_type, data in chunks if chunk_type == b"tRNS"]
    if len(transparency_chunks) > 1:
        raise ValueError("tRNS chunkが重複しています")
    transparency = transparency_chunks[0] if transparency_chunks else None
    if transparency is not None and color_type in {4, 6}:
        raise ValueError(f"color type {color_type}ではtRNSを使用できません")

    rows = _unfilter_scanlines(
        idat,
        width=width,
        height=height,
        channels=channels,
        bit_depth=bit_depth,
    )
    transparent_pixels = _count_transparent_pixels(
        rows,
        width=width,
        bit_depth=bit_depth,
        color_type=color_type,
        transparency=transparency,
    )
    return FPngInfo(
        width=width,
        height=height,
        bit_depth=bit_depth,
        color_type=color_type,
        transparent_pixels=transparent_pixels,
    )


def _audit_reference(
    reference: FImageReference,
    docs_root: Path,
    png_cache: dict[Path, FPngInfo | ValueError],
) -> tuple[list[str], Path | None]:
    location = _reference_location(reference)
    failures: list[str] = []
    if not reference.in_figure:
        failures.append(f"{location}: imgがfigure内にありません")
    if reference.anchor_href is None:
        failures.append(f"{location}: 原寸表示用のa[href]がありません")
    if reference.src is None or not reference.src.strip():
        failures.append(f"{location}: img[src]がありません")
        return failures, None
    if reference.alt is None or not JAPANESE_PATTERN.search(reference.alt):
        failures.append(f"{location}: altに日本語の代替説明がありません")
    if reference.loading is None or reference.loading.casefold() != "lazy":
        failures.append(f"{location}: loading=\"lazy\"ではありません")

    declared_width = _positive_dimension(reference.width)
    declared_height = _positive_dimension(reference.height)
    if declared_width is None:
        failures.append(f"{location}: widthが正の整数ではありません: {reference.width!r}")
    if declared_height is None:
        failures.append(f"{location}: heightが正の整数ではありません: {reference.height!r}")

    if reference.anchor_href is not None:
        if reference.anchor_href.strip() != reference.src.strip():
            failures.append(
                f"{location}: 原寸リンクhrefとimg srcが一致しません: "
                f"{reference.anchor_href!r} != {reference.src!r}"
            )
        try:
            anchor_target = _resolve_local_target(
                reference.document,
                reference.anchor_href,
                docs_root,
            )
            if not anchor_target.is_file():
                failures.append(
                    f"{location}: 原寸リンク先が存在しません: {reference.anchor_href}"
                )
        except ValueError as error:
            failures.append(f"{location}: 原寸リンクが不正です: {error}")

    try:
        resolved = _resolve_local_target(reference.document, reference.src, docs_root)
    except ValueError as error:
        failures.append(f"{location}: img srcが不正です: {error}")
        return failures, None
    if not resolved.is_file():
        failures.append(f"{location}: 画像参照先が存在しません: {reference.src}")
        return failures, None
    if resolved.suffix.casefold() != ".png":
        failures.append(f"{location}: PNG以外の画像です: {reference.src}")
        return failures, resolved

    if resolved not in png_cache:
        try:
            png_cache[resolved] = _read_png(resolved)
        except (OSError, ValueError) as error:
            png_cache[resolved] = ValueError(str(error))
    png_result = png_cache[resolved]
    if isinstance(png_result, ValueError):
        failures.append(f"{location}: PNGを検証できません: {png_result}")
        return failures, resolved
    if declared_width is not None and declared_width != png_result.width:
        failures.append(
            f"{location}: width={declared_width}とPNG IHDR実幅={png_result.width}が一致しません"
        )
    if declared_height is not None and declared_height != png_result.height:
        failures.append(
            f"{location}: height={declared_height}とPNG IHDR実高={png_result.height}が一致しません"
        )
    return failures, resolved


def _srcset_targets(raw_srcset: str) -> list[str]:
    targets: list[str] = []
    for candidate in raw_srcset.split(","):
        fields = candidate.strip().split()
        if fields:
            targets.append(fields[0])
    return targets


def _audit_responsive_source(
    reference: FResponsiveSourceReference,
    docs_root: Path,
    png_cache: dict[Path, FPngInfo | ValueError],
) -> tuple[list[str], set[Path]]:
    location = _responsive_source_location(reference)
    failures: list[str] = []
    resolved_targets: set[Path] = set()
    if not reference.in_figure or not reference.in_picture:
        failures.append(f"{location}: sourceがfigure内のpictureにありません")
    if reference.srcset is None or not reference.srcset.strip():
        failures.append(f"{location}: source[srcset]がありません")
        return failures, resolved_targets

    targets = _srcset_targets(reference.srcset)
    if len(targets) != 1:
        failures.append(
            f"{location}: 寸法を一意に検証できるsrcsetではありません: {reference.srcset!r}"
        )
        return failures, resolved_targets

    declared_width = _positive_dimension(reference.width)
    declared_height = _positive_dimension(reference.height)
    if declared_width is None:
        failures.append(f"{location}: source widthが正の整数ではありません: {reference.width!r}")
    if declared_height is None:
        failures.append(f"{location}: source heightが正の整数ではありません: {reference.height!r}")

    raw_target = targets[0]
    try:
        resolved = _resolve_local_target(reference.document, raw_target, docs_root)
    except ValueError as error:
        failures.append(f"{location}: source srcsetが不正です: {error}")
        return failures, resolved_targets
    if not resolved.is_file():
        failures.append(f"{location}: source参照先が存在しません: {raw_target}")
        return failures, resolved_targets
    resolved_targets.add(resolved.resolve())
    if resolved.suffix.casefold() != ".png":
        failures.append(f"{location}: sourceがPNGではありません: {raw_target}")
        return failures, resolved_targets

    if resolved not in png_cache:
        try:
            png_cache[resolved] = _read_png(resolved)
        except (OSError, ValueError) as error:
            png_cache[resolved] = ValueError(str(error))
    png_result = png_cache[resolved]
    if isinstance(png_result, ValueError):
        failures.append(f"{location}: source PNGを検証できません: {png_result}")
        return failures, resolved_targets
    if declared_width is not None and declared_width != png_result.width:
        failures.append(
            f"{location}: source width={declared_width}とPNG IHDR実幅="
            f"{png_result.width}が一致しません"
        )
    if declared_height is not None and declared_height != png_result.height:
        failures.append(
            f"{location}: source height={declared_height}とPNG IHDR実高="
            f"{png_result.height}が一致しません"
        )
    return failures, resolved_targets


def _png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    checksum = binascii.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", checksum)


def _write_regression_png(
    path: Path,
    *,
    width: int,
    height: int,
    color_type: int,
    rows: list[bytes],
) -> None:
    channels = {2: 3, 6: 4}[color_type]
    if len(rows) != height or any(len(row) != width * channels for row in rows):
        raise ValueError("回帰用PNGのpixel数が寸法と一致しません")
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    filtered = b"".join(b"\x00" + row for row in rows)
    path.write_bytes(
        PNG_SIGNATURE
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(filtered))
        + _png_chunk(b"IEND", b"")
    )


class DocsImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.references, cls.responsive_sources = _collect_image_references(DOCS_ROOT)
        cls.png_cache: dict[Path, FPngInfo | ValueError] = {}

    def test_document_image_references_follow_contract(self) -> None:
        self.assertTrue(self.references, "acs/docsに画像参照がありません")
        failures: list[str] = []
        for reference in self.references:
            reference_failures, _ = _audit_reference(
                reference,
                DOCS_ROOT,
                self.png_cache,
            )
            failures.extend(reference_failures)
        for source in self.responsive_sources:
            source_failures, _ = _audit_responsive_source(
                source,
                DOCS_ROOT,
                self.png_cache,
            )
            failures.extend(source_failures)
        self.assertFalse(failures, "画像文書の契約違反:\n" + "\n".join(failures))

    def test_edited_published_pngs_are_rgb_and_fully_opaque(self) -> None:
        edited_pngs = sorted(EDITED_ROOT.rglob("*.png"))
        self.assertTrue(edited_pngs, "edited掲載PNGがありません")
        failures: list[str] = []
        for image in edited_pngs:
            try:
                png = _read_png(image)
            except (OSError, ValueError) as error:
                failures.append(f"{_display_path(image)}: PNGを検証できません: {error}")
                continue
            if png.color_type != 2:
                failures.append(
                    f"{_display_path(image)}: RGBではありません: {png.color_name}"
                )
            if png.transparent_pixels:
                failures.append(
                    f"{_display_path(image)}: 透明または半透明pixelが"
                    f"{png.transparent_pixels}個あります"
                )
        self.assertFalse(
            failures,
            "edited掲載PNGの色・透明度違反:\n" + "\n".join(failures),
        )

    def test_meaningful_pngs_are_referenced(self) -> None:
        referenced: set[Path] = set()
        for reference in self.references:
            _, resolved = _audit_reference(reference, DOCS_ROOT, self.png_cache)
            if resolved is not None:
                referenced.add(resolved.resolve())
        for source in self.responsive_sources:
            _, source_targets = _audit_responsive_source(
                source,
                DOCS_ROOT,
                self.png_cache,
            )
            referenced.update(source_targets)

        meaningful = {
            path.resolve()
            for path in DOCS_ROOT.rglob("*.png")
            if "raw" not in path.relative_to(DOCS_ROOT).parts
        }
        self.assertTrue(meaningful, "raw以外の掲載対象PNGがありません")
        unreferenced = sorted(meaningful - referenced)
        self.assertFalse(
            unreferenced,
            "文書から参照されていない掲載対象PNG:\n"
            + "\n".join(_display_path(path) for path in unreferenced),
        )

    def test_png_decoder_detects_a_transparent_pixel(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "transparent.png"
            _write_regression_png(
                image,
                width=2,
                height=1,
                color_type=6,
                rows=[bytes((10, 20, 30, 255, 40, 50, 60, 0))],
            )
            png = _read_png(image)
            self.assertEqual(png.color_type, 6)
            self.assertEqual(png.transparent_pixels, 1)

    def test_reference_audit_detects_broken_dimensions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            docs_root = Path(temporary)
            image = docs_root / "image.png"
            document = docs_root / "sample.md"
            _write_regression_png(
                image,
                width=2,
                height=1,
                color_type=2,
                rows=[bytes((10, 20, 30, 40, 50, 60))],
            )
            document.write_text(
                '<figure><a href="image.png"><img src="image.png" '
                'alt="寸法検査用の画像" width="1" height="2" '
                'loading="lazy"></a></figure>\n',
                encoding="utf-8",
            )
            parser = FImageReferenceParser(document)
            parser.feed(document.read_text(encoding="utf-8"))
            failures, _ = _audit_reference(parser.references[0], docs_root, {})
            self.assertTrue(
                any("width=1とPNG IHDR実幅=2が一致しません" in failure for failure in failures)
            )
            self.assertTrue(
                any("height=2とPNG IHDR実高=1が一致しません" in failure for failure in failures)
            )


if __name__ == "__main__":
    unittest.main()
