# SPDX-License-Identifier: MIT

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class FSymbolRecord:
    """ACS の宣言一件と、その物理ページを表す。"""

    id: str
    module: str
    kind: str
    name: str
    qualified_name: str
    signature: str
    description: str
    access: str
    source_path: str
    source_line: int
    route: str = ""
    parent_id: str | None = None
    child_ids: list[str] = field(default_factory=list)
    declaration_paths: list[str] = field(default_factory=list)


@dataclass(slots=True)
class FFeatureMemberRecord:
    """機能ページに掲載するAPI項目一件を表す。"""

    id: str
    signature: str
    description: str
    returns: str
    usage: str
    sample: str
    note: str
    source_index: int
    target_ids: list[str] = field(default_factory=list)
    resolution: str = "unresolved"


@dataclass(slots=True)
class FFeatureRecord:
    """手書きの機能説明一件と、その物理ページを表す。"""

    id: str
    module: str
    title: str
    kind: str
    header: str
    summary: str
    usage: str
    sample: str
    source_file: str
    route: str
    symbol_id: str | None = None
    symbol_ids: list[str] = field(default_factory=list)
    members: list[FFeatureMemberRecord] = field(default_factory=list)
    note: str = ""


@dataclass(slots=True)
class FGlossaryRecord:
    """用語一件と、その説明を表す。"""

    id: str
    term: str
    definition: str
    source_file: str


@dataclass(slots=True)
class FGuideRecord:
    """導入ガイド一節を表す。"""

    id: str
    title: str
    blocks: list[dict[str, object]]
    source_file: str
    route: str


@dataclass(slots=True)
class FTroubleshootingRecord:
    """症状別の対処一件を表す。"""

    id: str
    title: str
    item: dict[str, object]
    source_file: str
    route: str


@dataclass(slots=True)
class FReferenceCatalog:
    """生成対象となる ACS リファレンス全体を保持する。"""

    symbols: list[FSymbolRecord]
    features: list[FFeatureRecord]
    glossary: list[FGlossaryRecord]
    guides: list[FGuideRecord]
    troubleshooting: list[FTroubleshootingRecord]
