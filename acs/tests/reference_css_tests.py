# SPDX-License-Identifier: MIT

from __future__ import annotations

import re
import unittest
from pathlib import Path


ACS_ROOT = Path(__file__).resolve().parents[1]
CSS_PATH = ACS_ROOT / "scripts" / "reference_site" / "assets" / "reference.css"


def _stylesheet() -> str:
    return CSS_PATH.read_text(encoding="utf-8")


def _rule_declarations(css: str, selector: str) -> list[dict[str, str]]:
    rules: list[dict[str, str]] = []
    for match in re.finditer(r"([^{}]+)\{([^{}]*)\}", css):
        selectors = [value.strip() for value in match.group(1).split(",")]
        if selector not in selectors:
            continue

        declarations: dict[str, str] = {}
        for declaration in match.group(2).split(";"):
            if ":" not in declaration:
                continue
            name, value = declaration.split(":", 1)
            declarations[name.strip()] = value.strip()
        rules.append(declarations)
    return rules


def _at_rule_block(css: str, prelude: str) -> str:
    prelude_start = css.index(prelude)
    block_start = css.index("{", prelude_start)
    depth = 0
    for index in range(block_start, len(css)):
        if css[index] == "{":
            depth += 1
        elif css[index] == "}":
            depth -= 1
            if depth == 0:
                return css[block_start + 1 : index]
    raise AssertionError(f"閉じ波括弧がありません: {prelude}")


def _hex_to_rgb(value: str) -> tuple[float, float, float]:
    match = re.fullmatch(r"#([0-9a-fA-F]{6})", value)
    if match is None:
        raise AssertionError(f"6桁の色指定ではありません: {value}")
    digits = match.group(1)
    return tuple(int(digits[index : index + 2], 16) / 255 for index in (0, 2, 4))


def _relative_luminance(value: str) -> float:
    channels = []
    for channel in _hex_to_rgb(value):
        channels.append(channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4)
    return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]


def _contrast(first: str, second: str) -> float:
    bright, dark = sorted((_relative_luminance(first), _relative_luminance(second)), reverse=True)
    return (bright + 0.05) / (dark + 0.05)


class ReferenceCssTests(unittest.TestCase):
    def assert_rule_has(self, css: str, selector: str, **expected: str) -> None:
        rules = _rule_declarations(css, selector)
        self.assertTrue(rules, f"CSS ruleがありません: {selector}")
        self.assertTrue(
            any(all(rule.get(name) == value for name, value in expected.items()) for rule in rules),
            f"{selector} に必要な宣言がありません: {expected}",
        )

    def test_custom_properties_are_defined_and_focus_contrast_is_visible(self) -> None:
        css = _stylesheet()
        definitions = set(re.findall(r"(--[a-z0-9-]+)\s*:", css, re.IGNORECASE))
        references = set(re.findall(r"var\(\s*(--[a-z0-9-]+)", css, re.IGNORECASE))
        self.assertEqual(set(), references - definitions)

        dark = next(rule for rule in _rule_declarations(css, ":root") if rule.get("--canvas", "").startswith("#"))
        automatic_light = next(
            rule
            for rule in _rule_declarations(css, ':root:not([data-theme="dark"])')
            if rule.get("--canvas", "").startswith("#")
        )
        explicit_light = next(
            rule
            for rule in _rule_declarations(css, ':root[data-theme="light"]')
            if rule.get("--canvas", "").startswith("#")
        )
        for palette in (dark, automatic_light, explicit_light):
            self.assertIn("--focus", palette)
            self.assertGreaterEqual(_contrast(palette["--focus"], palette["--canvas"]), 3.0)

    def test_requested_viewport_widths_activate_the_expected_layout_rules(self) -> None:
        css = _stylesheet()
        conditions = {
            (direction, int(width))
            for direction, width in re.findall(
                r"@media\s*\(\s*(min|max)-width\s*:\s*(\d+)px\s*\)",
                css,
            )
        }
        expected = {
            320: {("max", 1399), ("max", 1199), ("max", 700), ("max", 380)},
            360: {("max", 1399), ("max", 1199), ("max", 700), ("max", 380)},
            768: {("max", 1399), ("max", 1199)},
            1099: {("max", 1399), ("max", 1199)},
            1100: {("max", 1399), ("max", 1199)},
            1199: {("max", 1399), ("max", 1199)},
            1200: {("max", 1399)},
            1400: {("min", 1400)},
        }
        self.assertTrue(set().union(*expected.values()).issubset(conditions))
        for width, expected_active in expected.items():
            active = {
                condition
                for condition in conditions
                if (condition[0] == "min" and width >= condition[1])
                or (condition[0] == "max" and width <= condition[1])
            }
            self.assertEqual(expected_active, active, f"{width}px のmedia queryが想定外です")

    def test_focus_visible_and_accessibility_preferences_are_explicit(self) -> None:
        css = _stylesheet()
        self.assert_rule_has(
            css,
            ":focus-visible",
            outline="3px solid var(--focus)",
            **{"outline-offset": "3px"},
        )
        self.assert_rule_has(
            css,
            ".reference-image-link:focus-visible",
            **{"border-color": "var(--focus)"},
        )
        self.assert_rule_has(css, ".skip-link:focus-visible", transform="translateY(0)")
        self.assert_rule_has(css, "[data-theme-toggle]", display="none")
        self.assert_rule_has(css, ".js [data-theme-toggle]", display="inline-grid")

        reduced_motion = _at_rule_block(css, "@media (prefers-reduced-motion: reduce)")
        self.assertIn("scroll-behavior: auto !important", reduced_motion)
        self.assertIn("animation-duration: .01ms !important", reduced_motion)
        self.assertIn("animation-iteration-count: 1 !important", reduced_motion)
        self.assertIn("transition-duration: .01ms !important", reduced_motion)
        self.assertIn("transition-delay: 0ms !important", reduced_motion)

        forced_colors = _at_rule_block(css, "@media (forced-colors: active)")
        for system_color in ("Canvas", "CanvasText", "LinkText", "GrayText", "Highlight"):
            self.assertIn(system_color, forced_colors)
        self.assertIn("backdrop-filter: none", forced_colors)
        self.assertIn("box-shadow: none", forced_colors)

    def test_long_cpp_content_and_images_remain_inside_the_viewport(self) -> None:
        css = _stylesheet()
        self.assert_rule_has(css, ".content", **{"max-width": "100%"})
        self.assert_rule_has(
            css,
            ".signature",
            **{
                "max-width": "100%",
                "overflow-x": "auto",
                "overscroll-behavior-inline": "contain",
            },
        )
        self.assert_rule_has(css, ".doc-blocks pre", **{"max-width": "100%", "overflow-x": "auto"})
        self.assert_rule_has(
            css,
            ".doc-blocks pre code",
            **{"overflow-wrap": "normal", "word-break": "normal"},
        )
        for selector in (
            "code",
            ".breadcrumbs a",
            ".member-card .identifier",
            ".result-card .identifier",
            ".signature-summary",
            ".path",
        ):
            self.assert_rule_has(css, selector, **{"overflow-wrap": "anywhere"})
        for selector in (".member-list", ".result-list", ".plain-list"):
            self.assert_rule_has(css, selector, **{"min-width": "0", "max-width": "100%"})
        self.assert_rule_has(
            css,
            ".reference-figure",
            **{"min-width": "0", "max-width": "100%"},
        )
        self.assert_rule_has(
            css,
            ".reference-image-link",
            **{"max-width": "100%", "overflow": "hidden"},
        )
        self.assert_rule_has(
            css,
            ".reference-figure img",
            **{"width": "100%", "max-width": "100%", "height": "auto", "object-fit": "contain"},
        )

    def test_search_more_button_is_centered_focusable_and_mobile_width(self) -> None:
        css = _stylesheet()
        self.assert_rule_has(
            css,
            ".search-more",
            display="block",
            width="min(280px, 100%)",
            **{"min-height": "44px", "margin": "18px auto 0"},
        )
        self.assert_rule_has(
            css,
            ".search-more:focus-visible",
            outline="3px solid var(--focus)",
            **{"outline-offset": "3px"},
        )
        mobile = _at_rule_block(css, "@media (max-width: 700px)")
        self.assertRegex(mobile, r"\.search-more\s*\{[^{}]*width:\s*100%")
        forced_colors = _at_rule_block(css, "@media (forced-colors: active)")
        self.assertIn(".search-more", forced_colors)

    def test_tooltip_visibility_supports_hover_focus_tap_and_escape_suppression(self) -> None:
        css = _stylesheet()
        self.assert_rule_has(css, ".term-tooltip", display="none")
        self.assert_rule_has(css, ".term-wrap.is-open .term-tooltip", display="block")

        hover = _at_rule_block(css, "@media (hover: hover)")
        self.assertRegex(
            hover,
            r"\.term-wrap:hover\s+\.term-tooltip\s*\{[^{}]*display:\s*block",
        )
        self.assertRegex(
            hover,
            r"\.term-wrap\.is-suppressed:hover\s+\.term-tooltip\s*\{[^{}]*display:\s*none",
        )

    def test_primary_pointer_and_touch_targets_are_at_least_44_pixels(self) -> None:
        css = _stylesheet()
        for selector in (
            ".skip-link",
            ".brand",
            ".site-search input",
            ".page-search input",
            ".icon-button",
            ".nav-toggle",
            ".drawer-nav a",
            ".noscript-nav a",
            ".sidebar a",
            ".right-rail a",
            ".page-search button",
            ".search-more",
        ):
            rules = _rule_declarations(css, selector)
            target_sizes = [
                int(match.group(1))
                for rule in rules
                for value in (rule.get("min-height", ""),)
                if (match := re.fullmatch(r"(\d+)px", value)) is not None
            ]
            self.assertTrue(target_sizes, f"操作領域の高さがありません: {selector}")
            self.assertGreaterEqual(max(target_sizes), 44, selector)

        coarse_pointer = _at_rule_block(css, "@media (pointer: coarse)")
        self.assertRegex(coarse_pointer, r"width:\s*max\(100%,\s*44px\)")
        self.assertRegex(coarse_pointer, r"height:\s*44px")


if __name__ == "__main__":
    unittest.main()
