# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import shutil
import sys
import tempfile
import unicodedata
from collections import Counter
from pathlib import Path

from reference_site.catalog import (
    BR_PATTERN,
    CITATION_PATTERN,
    EXTERNAL_ENTITY_PATTERN,
    JAPANESE_PATTERN,
    MARKUP_PATTERN,
    PAIRED_MARKUP_PATTERN,
    PHASE_TOKEN_PATTERN,
    PROCESS_PATTERN,
    normalize_space,
    sanitize_acs_prose,
    strip_rich_text,
)


SCRIPT_PATH = Path(__file__).resolve()
ACS_ROOT_DEFAULT = SCRIPT_PATH.parents[1]
KNOWN_MARKUP_PATTERN = re.compile(
    r"</?(?:b|code|small)\s*>|<(?:t)\s*>[^<]*</t\s*>|<br\s*/?>",
    flags=re.S,
)
PLACEHOLDER_ENDPOINT_PATTERN = re.compile(r"https?://api\.example\.com(?:/[^\s\"']*)?", flags=re.I)
ENGLISH_WORD_PATTERN = re.compile(r"[A-Za-z]{3,}")
FORBIDDEN_PROSE_PATTERN = re.compile(
    r"ユーザーの指示|作業エージェント|別エージェント|\bagent\b|\bCodex\b|"
    r"\bTODO\b|\bFIXME\b|\bTBD\b",
    flags=re.I,
)
GENERIC_FALLBACK_PATTERN = re.compile(
    r"ACS の項目です。|ACS の動作を確認します。|入力値とACSの状態を確認してください。|"
    r"ACS で使用する「[^」]+」を表す用語です。|"
    r"[^。]+ の役割を表します。|[^。]+ を利用する処理で使用します。"
)
RET_TOKEN_PATTERN = re.compile(
    r"^(?:const\s+)?(?:[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*|nullptr|true|false|void|—|[-+]?\d+(?:\.\d+)?)"
    r"(?:\s*<[^\n]+>)?(?:\s*[*&]+)?"
    r"(?:\s+または\s+(?:[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*|nullptr|true|false|[-+]?\d+(?:\.\d+)?))?$"
)
SUPPORTED_CATEGORIES = {"features", "guides", "troubleshooting", "glossary"}
PROSE_KEYS = {
    "title", "description", "summary", "when", "note", "desc",
    "definition", "h2", "h3", "p", "q", "a", "ul",
}
RETURN_TRANSLATIONS = {
    "voice handle": "voice handle を返します。",
    "clip index": "clip index を返します。",
    "cooldown handle": "cooldown handle を返します。",
    "scaled dt": "scale 適用後の dt を返します。",
    "world transform": "world transform を返します。",
    "tile id": "tile ID を返します。",
}
CANONICAL_GLOSSARY = {
    "gameframework": (
        "GameFramework",
        "シーン、コンポーネント、各ジャンル用機能、アセット読み書きの抽象 API など、ゲーム構築用の部品を提供する acs::game モジュールです。",
    ),
    "pimpl": (
        "Pimpl",
        "実装詳細を前方宣言した型のポインタ先に隠し、公開 header の依存を減らす構成です。FLuaVm は lua_State をこの構成で隠します。",
    ),
}

PROSE_REPLACEMENTS = (
    (
        re.compile(r"DXLib 等で慣れた 0〜255 表記で色を作りたい時。"),
        "0〜255の整数で色を作るときに使用します。",
    ),
    (
        re.compile(r"UE5 級の見た目を狙う 3D 描画。"),
        "ACSの3D描画で高品質な見た目を構成します。",
    ),
    (
        re.compile(r"DXLib のような感覚で、設計を気にせずまず動くゲームを書き始めたい時。 本格化したくなったら acs::CApplication へ進めます。"),
        "OpenWindow と NextFrame を使い、クラスを用意せずに2Dゲームを開始するときに使用します。構成を明示して管理する場合は acs::CApplication を使用します。",
    ),
    (
        re.compile(r"DXLib 等で慣れた 0〜255 表記で色を作りたい時。 フェードイン/アウトや半透明描画に Fade。"),
        "0〜255の整数で色を作るときに使用します。Fadeはフェードイン、フェードアウト、半透明描画の不透明度設定に使用します。",
    ),
    (re.compile(r"VS Code Dark\+ 風\(青みのある dark\)"), "青みを加えた暗色配色"),
    (
        re.compile(r"UE5 級の見た目を狙う 3D 描画。 リアルな金属・プラスチック・肌・布などを描き分けたいとき。"),
        "金属度、粗さ、法線、環境光、影などを組み合わせて3D材質を描き分けるときに使用します。",
    ),
    (
        re.compile(r"Slay the Spire / Hearthstone / MtG のようなデッキ構築系を作る時。 対戦相手がいれば instance を人数分持つ。"),
        "デッキ、手札、捨札、除外の4領域を使うカード処理に使用します。参加者ごとに FDeckSystem を1つ保持します。",
    ),
    (
        re.compile(r"DayZ / The Long Dark / Don't Starve のような「食べないと死ぬ」ゲームを作る時。 HP 自体は持たず、危険/ダメージはコールバックで外部 \(FHealthSystem\) に橋渡しする。"),
        "生存統計値を時間経過で減少させ、危険状態とダメージを通知するときに使用します。HPの更新はコールバック先が担当します。",
    ),
    (
        re.compile(r"Vampire Survivors や Hades のような「近づくと吸い寄せられて自動で拾う」アイテムドロップを作りたい時。 拾うと callback で通知が来るので、インベントリ追加や HP 回復は呼び出し側で行う。"),
        "プレイヤーとの距離に応じて pickup を引き寄せ、自動取得と寿命切れを処理するときに使用します。取得後の状態変更はコールバック先が担当します。",
    ),
    (
        re.compile(r"Bejeweled / Candy Crush 系 match-3 パズルのコアロジックを担う 2D グリッド。"),
        "match-3パズルの2Dグリッドを管理します。隣接 swap、3個以上の連続検出、消去、special 効果、重力落下、補充、連鎖を処理します。",
    ),
    (
        re.compile(r"同じ色を 3 個以上並べて消すパズルの定番ジャンル \(Bejeweled / Candy Crush 系\)"),
        "FMatchGrid が扱う、同じ種類の要素を縦または横に3個以上並べて消去するパズル規則",
    ),
    (
        re.compile(r"UGC \(ユーザー生成コンテンツ = チャット文 / 画像 / ユーザー名\) を「公開可 / 警告付き / ブロック」の 3 値で判定するseam。 実 API \(Azure / OpenAI Moderation 等\) は別モジュールで差し込む前提で、ここは I/F と Stub だけ。"),
        "IContentModerator はテキスト、画像、利用者名を EModerationVerdict と EContentRating で判定するインターフェースです。FContentModeratorStub はテキスト辞書と画像の肌色比率でローカル判定します。",
    ),
    (
        re.compile(r"出荷ビルドでプロセスが落ちた時に外部のクラッシュ集約サービス \(Sentry / Crashpad / BugSnag 等\) へ context を吐き出すためのseam。 本体は具象 HTTP/IPC を抱えず、I/F と Stub だけを持つ。 失敗は黙って握り潰す \(二次クラッシュ防止\)。"),
        "ICrashReporterBackend はクラッシュ、非致命的エラー、breadcrumb の報告境界を定義します。FCrashReporterStub は報告処理で NotImplemented を返し、FCrashHandler はバックエンド未設定時に何も行いません。",
    ),
    (
        re.compile(r"リポジトリは OneDrive 外の作業コピーで扱う運用です。"),
        "既定の出力先は acs/Binaries です。複数構成 generator では acs/Binaries/Debug または acs/Binaries/Release を確認してください。ACS_LAYOUT_ROOT を指定した場合は、その配下の Binaries が基準になります。",
    ),
)

FEATURE_WHEN_OVERRIDES = {
    "EDamageType": "ダメージ処理や表示効果を EDamageType で分岐するときに使用します。",
}

MEMBER_DESCRIPTION_OVERRIDES = {
    "Texture = 1": "FAssetBrowser::ClassifyByExtensionは.png、.jpg、.jpeg、.tga、.bmp、.dds、.ktx、.hdrをTextureに分類します。",
    "Mesh = 2": ".mdl、.fbx、.gltf、.glb、.objをMeshに分類します。",
    "Font = 3": ".ttf、.otfをFontに分類します。",
    "Audio = 4": ".wav、.ogg、.mp3、.flacをAudioに分類します。",
    "Material = 5": ".mat、.materialをMaterialに分類します。",
    "Particle = 6": ".fx、.particleをParticleに分類します。",
    "Animation = 7": ".animをAnimationに分類します。",
    "BehaviorTree = 8": ".btをBehaviorTreeに分類します。",
    "Tilemap = 9": ".tilemap、.tmxをTilemapに分類します。",
    "Prefab = 10": ".prefabをPrefabに分類します。",
    "Cinematic = 11": ".cineをCinematicに分類します。",
    "Scene = 12": ".sceneをSceneに分類します。",
    "Center = 0": "Centerはpivotを(0.5, 0.5)に設定します。",
    "TopLeft = 1": "TopLeftはpivotを(0.0, 0.0)に設定します。",
    "RemoveRefT<T>&& Move(T&& v)": "vをRemoveRefT<T>&&へcastし、所有内容を移動できる形で返します。",
    "T&& Forward<T>(...)": "受け取った値のlvalue/rvalue区分を保ったままT&&として転送します。",
    "static constexpr f32 kMinMetallic=0 / kMaxMetallic=1": "Metallicの編集値を0.0f以上1.0f以下に制限します。",
    "MetaQuest = 1": "Init(EXrPlatform)でMetaQuest backendを明示指定する列挙値です。",
    "ValveIndex = 2": "Init(EXrPlatform)でValveIndex backendを明示指定する列挙値です。",
    "PicoNeo = 4": "Init(EXrPlatform)でPicoNeo backendを明示指定する列挙値です。",
    "AppleVisionPro = 5": "Init(EXrPlatform)でAppleVisionPro backendを明示指定する列挙値です。",
    "PsVr2 = 6": "Init(EXrPlatform)でPsVr2 backendを明示指定する列挙値です。",
    "WindowsMR = 7": "Init(EXrPlatform)でWindowsMR backendを明示指定する列挙値です。",
    "FRelocHandle|bool IsValid() const": "generationが0以外ならtrueを返します。",
    "bool IsPow2(usize v) noexcept": "v != 0 && (v & (v - 1)) == 0のときtrueを返します。",
    "EVoiceProvider::{None, SteamVoice, EosVoice, Vivox, Discord, OpusSelf}": "None、SteamVoice、EosVoice、Vivox、Discord、OpusSelf は、ACSが選択するボイス実装種別です。",
    "FAnimationCurve|f32 Duration() const": "キーがない場合は0、それ以外は最後のキーの time を返します。最初のキーとの時間差ではありません。",
    "FDialogueScript|EDialogueScriptState State()": "Idle、Playing、AwaitingInput、AwaitingChoice、Finishedのいずれかを返します。",
    "CDialogueScript|EDialogueScriptState State()": "Idle、Playing、AwaitingInput、AwaitingChoice、Finishedのいずれかを返します。",
    "FDynamicDifficulty|f32 EnemyHealthMultiplier()": "Easy=0.5、Normal=1.0、Hard=1.5、VeryHard=2.0です。Adaptiveは4値を補間します。",
    "CDynamicDifficulty|f32 EnemyHealthMultiplier()": "Easy=0.5、Normal=1.0、Hard=1.5、VeryHard=2.0です。Adaptiveは4値を補間します。",
    "FDynamicDifficulty|f32 EnemyDamageMultiplier()": "Easy=0.6、Normal=1.0、Hard=1.4、VeryHard=1.8です。Adaptiveは4値を補間します。",
    "CDynamicDifficulty|f32 EnemyDamageMultiplier()": "Easy=0.6、Normal=1.0、Hard=1.4、VeryHard=1.8です。Adaptiveは4値を補間します。",
    "FDynamicDifficulty|f32 EnemySpeedMultiplier()": "Easy=0.85、Normal=1.0、Hard=1.15、VeryHard=1.3です。Adaptiveは4値を補間します。",
    "CDynamicDifficulty|f32 EnemySpeedMultiplier()": "Easy=0.85、Normal=1.0、Hard=1.15、VeryHard=1.3です。Adaptiveは4値を補間します。",
    "FScriptHost|void RegisterStandardBindings()": "VM未設定時は警告して終了します。VM設定済みでも現在は native function を登録しません。",
    "CScriptHost|void RegisterStandardBindings()": "VM未設定時は警告して終了します。VM設定済みでも現在は native function を登録しません。",
    "FTutorialFlow|void Tick(f32 dt)": "dtを受け取りますが、現在は状態を変更しません。",
    "CTutorialFlow|void Tick(f32 dt)": "dtを受け取りますが、現在は状態を変更しません。",
    "FTypeInfoBase|usize size / usize alignment": "sizeはsizeof(T)、alignmentはalignof(T)を保持します。",
    "FVoiceFrameHeader|u32 sample_count / u32 reserved": "sample_count は payload の int16 PCM サンプル数です。reserved は EncodeFrame が0に設定します。",
}

MEMBER_WHEN_OVERRIDES: dict[str, str] = {}

MEMBER_SIGNATURE_REPLACEMENTS = {
    ("FRelocHandle", "u32 index / u32 generation"): "u32 index / u64 generation",
}

CODE_COMMENT_TRANSLATIONS = {
    "root": "ルートボーン",
    "do": "実行処理",
    "undo": "取り消し処理",
    "onexit:": "終了時:",
    "ui:": "順位表示:",
    "1 = leader": "1 = 先頭",
    "world": "ワールド描画",
    "hud": "HUD描画",
    "ok": "妥当",
    "or": "または",
    "draw": "キャスターを描画",
}

GLOSSARY_DEFINITION_OVERRIDES = {
    "fmutex": "一度に1つのスレッドだけが保護区間へ入れる、ACSの排他ロックです。",
    "frwlock": "複数の読み取りを同時に許可し、書き込みは1つのスレッドだけに限定する読み書きロックです。",
    "orbit": "ACSの3Dカメラが注視点を中心に yaw、pitch、distance で周回する操作です。",
    "stl 不使用": "ACSの基盤コードと公開APIでは、可変長配列、文字列、所有権管理に TArray、FString、TUniquePtr などのACS型を使用する方針です。",
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ACSリファレンスの分割正本を日本語へ正規化します。")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def stable_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + "\n").encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FSourceSanitizer:
    def __init__(self) -> None:
        self.changes: Counter[str] = Counter()

    def identifier(self, value: object) -> str:
        return strip_rich_text(value)

    def prose(self, value: object, fallback: str) -> str:
        before = strip_rich_text(value)
        normalized = before
        for pattern, replacement in PROSE_REPLACEMENTS:
            normalized, count = pattern.subn(replacement, normalized)
            self.changes["外部比較のACS表現化"] += count
        text = sanitize_acs_prose(normalized)
        if text != before:
            self.changes["本文から比較・工程を除去"] += 1
        if not JAPANESE_PATTERN.search(text):
            self.changes["日本語fallback"] += 1
            return fallback
        return text

    def code(self, value: object) -> str:
        text = str(value or "")
        text = BR_PATTERN.sub("\n", text)
        while PAIRED_MARKUP_PATTERN.search(text):
            text = PAIRED_MARKUP_PATTERN.sub(lambda match: match.group(2), text)
        text = MARKUP_PATTERN.sub("", text)
        text = html.unescape(text)
        text, endpoint_count = PLACEHOLDER_ENDPOINT_PATTERN.subn("acs-backend-endpoint", text)
        self.changes["placeholder endpoint"] += endpoint_count

        text = "\n".join(line.rstrip() for line in text.splitlines())

        def replace_block_comment(match: re.Match[str]) -> str:
            body = match.group(0)[2:-2]
            translated = self._translated_code_comment(body)
            if translated is not None:
                self.changes["英語code commentの日本語化"] += 1
                return f"/* {translated} */"
            words = re.findall(r"[A-Za-z]{2,}", body)
            if words and not self._preserve_code_comment(body) and not JAPANESE_PATTERN.search(body):
                self.changes["英語code comment"] += 1
                return ""
            return match.group(0)

        text = re.sub(r"/\*.*?\*/", replace_block_comment, text, flags=re.S)
        lines: list[str] = []
        for line in text.splitlines():
            comment_index = self._line_comment_index(line)
            if comment_index >= 0:
                body = line[comment_index + 2:]
                translated = self._translated_code_comment(body)
                if translated is not None:
                    prefix = line[:comment_index]
                    line = f"{prefix}// {translated}" if not prefix.strip() else f"{prefix.rstrip()} // {translated}"
                    self.changes["英語code commentの日本語化"] += 1
                    lines.append(line.rstrip())
                    continue
                words = re.findall(r"[A-Za-z]{2,}", body)
                if words and not self._preserve_code_comment(body) and not JAPANESE_PATTERN.search(body):
                    line = line[:comment_index].rstrip()
                    self.changes["英語code comment"] += 1
            lines.append(line.rstrip())
        return "\n".join(lines).strip("\n")

    @staticmethod
    def _translated_code_comment(body: str) -> str | None:
        key = normalize_space(body).casefold()
        return CODE_COMMENT_TRANSLATIONS.get(key)

    @staticmethod
    def _preserve_code_comment(body: str) -> bool:
        stripped = body.strip()
        if re.fullmatch(r"[A-Za-z_]\w*\s*=", stripped):
            return True
        if re.fullmatch(r"(?:\d+(?:\.\d+)?\s*)?(?:fps|ms|hz|kb|mb|gb)", stripped, flags=re.I):
            return True
        if re.fullmatch(r"[A-Z][A-Z0-9_]*", stripped):
            return True
        if re.fullmatch(r"[A-Z][A-Za-z0-9_]*[A-Z][A-Za-z0-9_]*", stripped):
            return True
        if re.fullmatch(r"[\"'][^\"']+[\"']", stripped):
            return True
        if re.fullmatch(r"(?:→|->)\s*[A-Za-z_]\w*", stripped):
            return True
        return False

    @staticmethod
    def _line_comment_index(line: str) -> int:
        quote = ""
        escaped = False
        for index, character in enumerate(line[:-1]):
            if quote:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = ""
                continue
            if character in {'"', "'"}:
                quote = character
                continue
            if character == "/" and line[index + 1] == "/":
                return index
        return -1

    def ret(self, value: object) -> str:
        text = self.identifier(value)
        text = re.sub(r"\bor\b", "または", text)
        if not text or text == "—" or JAPANESE_PATTERN.search(text) or RET_TOKEN_PATTERN.fullmatch(text):
            return text
        if re.fullmatch(r'"[^"\n]*"|\'[^\'\n]*\'', text):
            return text
        if re.search(r"[\[\]{}=/.,]", text):
            return text
        if len(re.findall(r"[A-Za-z_]\w*", text)) <= 1:
            return text
        translated = RETURN_TRANSLATIONS.get(text.casefold())
        if translated:
            self.changes["戻り値説明の日本語化"] += 1
            return translated
        self.changes["戻り値説明fallback"] += 1
        return f"{text} を返します。"

    def feature(self, data: dict[str, object]) -> dict[str, object]:
        module = dict(data.get("module", {})) if isinstance(data.get("module"), dict) else {}
        feature = dict(data.get("feature", {})) if isinstance(data.get("feature"), dict) else {}
        module_id = self.identifier(module.get("id", "module")) or "module"
        name = self.identifier(feature.get("name", "機能")) or "機能"
        module_title = self.prose(module.get("title", ""), f"{module_id} — ACSモジュール")
        result_feature: dict[str, object] = {
            "name": name,
            "kind": self.prose(feature.get("kind", ""), "機能"),
            "header": self.identifier(feature.get("header", "")),
            "summary": self.prose(feature.get("summary", ""), f"ACS の {name} 機能です。"),
        }
        raw_feature_when = self.identifier(feature.get("when", ""))
        if raw_feature_when and raw_feature_when != "—":
            result_feature["when"] = self.prose(
                raw_feature_when,
                FEATURE_WHEN_OVERRIDES.get(name, f"{name} を利用する処理で使用します。"),
            )
        if "note" in feature:
            result_feature["note"] = self.prose(feature.get("note", ""), f"{name} の補足事項です。")
        if "sample" in feature:
            result_feature["sample"] = self.code(feature.get("sample", ""))
        members: list[dict[str, object]] = []
        for raw_member in feature.get("members", []) if isinstance(feature.get("members"), list) else []:
            if not isinstance(raw_member, dict):
                continue
            signature = self.identifier(raw_member.get("sig", ""))
            signature = MEMBER_SIGNATURE_REPLACEMENTS.get((name, signature), signature)
            description_fallback = MEMBER_DESCRIPTION_OVERRIDES.get(
                f"{name}|{signature}",
                MEMBER_DESCRIPTION_OVERRIDES.get(
                    signature,
                    f"{signature or name} の役割を表します。",
                ),
            )
            member: dict[str, object] = {"sig": signature}
            if "ret" in raw_member:
                member["ret"] = self.ret(raw_member.get("ret", ""))
            if "desc" in raw_member:
                member["desc"] = self.prose(
                    raw_member.get("desc", ""),
                    description_fallback,
                )
            raw_member_when = self.identifier(raw_member.get("when", ""))
            if raw_member_when and raw_member_when != "—":
                member["when"] = self.prose(
                    raw_member_when,
                    MEMBER_WHEN_OVERRIDES.get(
                        f"{name}|{signature}",
                        MEMBER_WHEN_OVERRIDES.get(
                            signature,
                            f"{signature or name} を利用する処理で使用します。",
                        ),
                    ),
                )
            if "sample" in raw_member:
                member["sample"] = self.code(raw_member.get("sample", ""))
            members.append(member)
        if members:
            result_feature["members"] = members
        return {
            "schema": 2,
            "module": {
                "id": module_id,
                "title": module_title,
                "description": self.prose(
                    module.get("description", ""),
                    f"ACS の {module_title} に含まれる機能です。",
                ),
            },
            "feature": result_feature,
        }

    def guide(self, data: dict[str, object]) -> dict[str, object]:
        raw_title = strip_rich_text(data.get("title", ""))
        if raw_title == "STL を使わない理由と代わり":
            self.changes["ACS型guideの再構成"] += 1
            return {
                "schema": 2,
                "title": "ACSのコンテナと所有権管理",
                "blocks": [
                    {"h2": "ACSの標準型"},
                    {"p": "ACSの公開APIでは、用途に応じて次のコンテナ、文字列、所有権管理型を使用します。"},
                    {"ul": [
                        "可変長配列には TArray を使用します。",
                        "キーと値の対応表には THashMap を使用します。",
                        "文字列には FString を使用します。",
                        "単一所有には TUniquePtr を使用します。",
                        "共有所有には TSharedPtr を使用します。",
                        "弱参照には TWeakPtr を使用します。",
                    ]},
                ],
            }
        title = self.prose(data.get("title", "ガイド"), "ACS ガイド") or "ACS ガイド"
        blocks: list[dict[str, object]] = []
        for raw_block in data.get("blocks", []) if isinstance(data.get("blocks"), list) else []:
            if not isinstance(raw_block, dict):
                continue
            block: dict[str, object] = {}
            for key, value in raw_block.items():
                if key == "code":
                    block[key] = self.code(value)
                elif key == "ul" and isinstance(value, list):
                    block[key] = [self.prose(item, "ACS の項目です。") for item in value]
                elif key == "kind":
                    block[key] = self.identifier(value)
                elif key in {"h2", "h3", "p", "note"}:
                    block[key] = self.prose(value, f"ACS の {title} を説明します。")
            if block:
                blocks.append(block)
        return {"schema": 2, "title": title, "blocks": blocks}

    def troubleshooting(self, data: dict[str, object]) -> dict[str, object]:
        item = dict(data.get("item", {})) if isinstance(data.get("item"), dict) else {}
        raw_question = strip_rich_text(item.get("q", ""))
        if "std::" in raw_question:
            question = "ACSで可変長配列、文字列、キー付き連想配列を使うには、どの型を選びますか？"
            answer = "可変長配列には TArray、文字列には FString、キー付き連想配列には THashMap を使用してください。"
            self.changes["ACS型troubleshootingの再構成"] += 1
        else:
            question = self.prose(item.get("q", ""), "ACS の動作を確認します。")
            answer = self.prose(item.get("a", ""), "入力値とACSの状態を確認してください。")
        title = self.identifier(data.get("title", ""))
        if "std::" in raw_question:
            title = "ACSコンテナ型を選ぶ"
        if not title or not JAPANESE_PATTERN.search(title) or re.fullmatch(r"項目\d+", title):
            title = question.rstrip("。？！")[:72] or "確認項目"
        tags = [self.identifier(tag) for tag in item.get("tags", []) if self.identifier(tag)] if isinstance(item.get("tags"), list) else []
        return {
            "schema": 2,
            "title": title,
            "item": {"q": question, "tags": tags, "a": answer},
        }

    def glossary(self, data: dict[str, object]) -> dict[str, object]:
        term = self.identifier(data.get("term", "用語")) or "用語"
        fallback = GLOSSARY_DEFINITION_OVERRIDES.get(
            normalized_term(term),
            f"ACS で使用する「{term}」を表す用語です。",
        )
        return {
            "schema": 2,
            "term": term,
            "definition": self.prose(
                data.get("definition", ""),
                fallback,
            ),
        }


def normalized_term(value: str) -> str:
    return normalize_space(unicodedata.normalize("NFKC", value)).casefold()


def _require_keys(
    value: dict[str, object],
    required: set[str],
    optional: set[str],
    context: str,
) -> None:
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        raise ValueError(f"必須fieldがありません: {context}: {sorted(missing)}")
    if unknown:
        raise ValueError(f"未知fieldがあります: {context}: {sorted(unknown)}")


def _require_string(value: dict[str, object], key: str, context: str) -> None:
    if not isinstance(value.get(key), str):
        raise ValueError(f"文字列fieldが不正です: {context}/{key}")


def validate_input_document(relative: Path, data: object) -> dict[str, object]:
    context = relative.as_posix()
    if not isinstance(data, dict):
        raise ValueError(f"JSON rootがobjectではありません: {context}")
    if data.get("schema") != 1:
        raise ValueError(f"入力schemaが1ではありません: {context}")
    category = relative.parts[0] if relative.parts else ""
    if category == "features":
        _require_keys(data, {"schema", "module", "feature"}, set(), context)
        module = data.get("module")
        feature = data.get("feature")
        if not isinstance(module, dict) or not isinstance(feature, dict):
            raise ValueError(f"moduleまたはfeatureがobjectではありません: {context}")
        _require_keys(module, {"id", "title", "description"}, set(), f"{context}/module")
        for key in ("id", "title", "description"):
            _require_string(module, key, f"{context}/module")
        _require_keys(
            feature,
            {"name", "kind", "header", "summary"},
            {"when", "note", "sample", "members"},
            f"{context}/feature",
        )
        for key in ("name", "kind", "header", "summary"):
            _require_string(feature, key, f"{context}/feature")
        for key in ("when", "note", "sample"):
            if key in feature:
                _require_string(feature, key, f"{context}/feature")
        members = feature.get("members", [])
        if not isinstance(members, list):
            raise ValueError(f"membersが配列ではありません: {context}")
        for index, member in enumerate(members):
            member_context = f"{context}/feature/members[{index}]"
            if not isinstance(member, dict):
                raise ValueError(f"memberがobjectではありません: {member_context}")
            _require_keys(member, {"sig"}, {"ret", "desc", "when", "sample"}, member_context)
            for key in member:
                _require_string(member, key, member_context)
    elif category == "guides":
        _require_keys(data, {"schema", "title", "blocks"}, set(), context)
        _require_string(data, "title", context)
        blocks = data.get("blocks")
        if not isinstance(blocks, list):
            raise ValueError(f"blocksが配列ではありません: {context}")
        for index, block in enumerate(blocks):
            block_context = f"{context}/blocks[{index}]"
            if not isinstance(block, dict):
                raise ValueError(f"blockがobjectではありません: {block_context}")
            _require_keys(block, set(), {"code", "h2", "kind", "note", "p", "ul"}, block_context)
            for key, value in block.items():
                if key == "ul":
                    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
                        raise ValueError(f"ulが文字列配列ではありません: {block_context}")
                elif not isinstance(value, str):
                    raise ValueError(f"block fieldが文字列ではありません: {block_context}/{key}")
    elif category == "troubleshooting":
        _require_keys(data, {"schema", "title", "item"}, set(), context)
        _require_string(data, "title", context)
        item = data.get("item")
        if not isinstance(item, dict):
            raise ValueError(f"itemがobjectではありません: {context}")
        _require_keys(item, {"q", "tags", "a"}, set(), f"{context}/item")
        _require_string(item, "q", f"{context}/item")
        _require_string(item, "a", f"{context}/item")
        tags = item.get("tags")
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            raise ValueError(f"tagsが文字列配列ではありません: {context}")
    elif category == "glossary":
        _require_keys(data, {"schema", "term", "definition"}, set(), context)
        _require_string(data, "term", context)
        _require_string(data, "definition", context)
    else:
        raise ValueError(f"未知categoryです: {context}")
    return data


def load_input_documents(source: Path) -> list[tuple[Path, dict[str, object]]]:
    manifest_path = source / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError("入力manifest.jsonがありません。")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        raise ValueError("入力manifestのschemaが不正です。")
    files = manifest.get("files")
    if not isinstance(files, list) or not all(isinstance(item, str) for item in files):
        raise ValueError("入力manifestのfilesが文字列配列ではありません。")
    if len(files) != len(set(files)):
        raise ValueError("入力manifestのfilesに重複があります。")
    listed: set[str] = set()
    documents: list[tuple[Path, dict[str, object]]] = []
    counts: Counter[str] = Counter()
    for raw_relative in files:
        relative = Path(raw_relative)
        target = (source / relative).resolve()
        target.relative_to(source)
        if relative.is_absolute() or relative.suffix.lower() != ".json":
            raise ValueError(f"入力manifestのpathが不正です: {raw_relative}")
        if not target.is_file():
            raise ValueError(f"入力manifestのfileがありません: {raw_relative}")
        category = relative.parts[0] if relative.parts else ""
        if category not in SUPPORTED_CATEGORIES:
            raise ValueError(f"入力manifestに未知categoryがあります: {raw_relative}")
        data = validate_input_document(
            relative,
            json.loads(target.read_text(encoding="utf-8")),
        )
        listed.add(relative.as_posix())
        counts[category] += 1
        documents.append((relative, data))
    actual = {
        path.relative_to(source).as_posix()
        for path in source.rglob("*.json")
        if path.name != "manifest.json"
    }
    if actual != listed:
        missing = sorted(listed - actual)
        extra = sorted(actual - listed)
        raise ValueError(f"入力manifestとfile集合が一致しません: missing={missing[:5]} extra={extra[:5]}")
    expected_counts = {
        "features": manifest.get("featureCount"),
        "guides": manifest.get("guideSectionCount"),
        "troubleshooting": manifest.get("troubleshootingCount"),
        "glossary": manifest.get("glossaryCount"),
    }
    for category, expected in expected_counts.items():
        if expected != counts[category]:
            raise ValueError(
                f"入力manifestの件数が一致しません: {category}: {expected} != {counts[category]}"
            )
    return documents


def validate_document(relative: str, data: dict[str, object]) -> list[str]:
    errors: list[str] = []

    try:
        validate_output_document(Path(relative), data)
    except ValueError as error:
        errors.append(str(error))

    def visit(value: object, path: str, prose: bool = False) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                visit(child, f"{path}/{key}", key in PROSE_KEYS)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, f"{path}[{index}]", prose)
        elif isinstance(value, str):
            if KNOWN_MARKUP_PATTERN.search(value):
                errors.append(f"既知markupが残っています: {relative}:{path}")
            if PLACEHOLDER_ENDPOINT_PATTERN.search(value):
                errors.append(f"placeholder endpointが残っています: {relative}:{path}")
            if prose:
                if not JAPANESE_PATTERN.search(value):
                    errors.append(f"日本語本文がありません: {relative}:{path}")
                if CITATION_PATTERN.search(value) or PROCESS_PATTERN.search(value) or PHASE_TOKEN_PATTERN.search(value):
                    errors.append(f"引用・工程文が残っています: {relative}:{path}")
                if EXTERNAL_ENTITY_PATTERN.search(value):
                    errors.append(f"外部比較が残っています: {relative}:{path}")
                if FORBIDDEN_PROSE_PATTERN.search(value):
                    errors.append(f"ACS外の工程文が残っています: {relative}:{path}")
                if GENERIC_FALLBACK_PATTERN.fullmatch(value.strip()):
                    errors.append(f"意味を特定しないfallbackが残っています: {relative}:{path}")

    visit(data, "")
    return errors


def validate_output_document(relative: Path, data: object) -> dict[str, object]:
    """schema 2の出力文書がcategory別契約を満たすことを確認する。"""
    context = relative.as_posix()
    if not isinstance(data, dict):
        raise ValueError(f"出力JSON rootがobjectではありません: {context}")
    if data.get("schema") != 2:
        raise ValueError(f"出力schemaが2ではありません: {context}")
    category = relative.parts[0] if relative.parts else ""
    if category == "features":
        _require_keys(data, {"schema", "module", "feature"}, set(), context)
        module = data.get("module")
        feature = data.get("feature")
        if not isinstance(module, dict) or not isinstance(feature, dict):
            raise ValueError(f"出力moduleまたはfeatureがobjectではありません: {context}")
        _require_keys(module, {"id", "title", "description"}, set(), f"{context}/module")
        for key in ("id", "title", "description"):
            _require_string(module, key, f"{context}/module")
        _require_keys(
            feature,
            {"name", "kind", "header", "summary"},
            {"when", "note", "sample", "members"},
            f"{context}/feature",
        )
        for key in ("name", "kind", "header", "summary"):
            _require_string(feature, key, f"{context}/feature")
        for key in ("when", "note", "sample"):
            if key in feature:
                _require_string(feature, key, f"{context}/feature")
        members = feature.get("members", [])
        if not isinstance(members, list):
            raise ValueError(f"出力membersが配列ではありません: {context}")
        for index, member in enumerate(members):
            member_context = f"{context}/feature/members[{index}]"
            if not isinstance(member, dict):
                raise ValueError(f"出力memberがobjectではありません: {member_context}")
            _require_keys(member, {"sig"}, {"ret", "desc", "when", "sample"}, member_context)
            for key in member:
                _require_string(member, key, member_context)
    elif category == "guides":
        _require_keys(data, {"schema", "title", "blocks"}, set(), context)
        _require_string(data, "title", context)
        blocks = data.get("blocks")
        if not isinstance(blocks, list):
            raise ValueError(f"出力blocksが配列ではありません: {context}")
        for index, block in enumerate(blocks):
            block_context = f"{context}/blocks[{index}]"
            if not isinstance(block, dict):
                raise ValueError(f"出力blockがobjectではありません: {block_context}")
            _require_keys(block, set(), {"code", "h2", "h3", "kind", "note", "p", "ul"}, block_context)
            for key, value in block.items():
                if key == "ul":
                    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
                        raise ValueError(f"出力ulが文字列配列ではありません: {block_context}")
                elif not isinstance(value, str):
                    raise ValueError(f"出力block fieldが文字列ではありません: {block_context}/{key}")
    elif category == "troubleshooting":
        _require_keys(data, {"schema", "title", "item"}, set(), context)
        _require_string(data, "title", context)
        item = data.get("item")
        if not isinstance(item, dict):
            raise ValueError(f"出力itemがobjectではありません: {context}")
        _require_keys(item, {"q", "tags", "a"}, set(), f"{context}/item")
        _require_string(item, "q", f"{context}/item")
        _require_string(item, "a", f"{context}/item")
        tags = item.get("tags")
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            raise ValueError(f"出力tagsが文字列配列ではありません: {context}")
    elif category == "glossary":
        _require_keys(data, {"schema", "term", "definition"}, set(), context)
        _require_string(data, "term", context)
        _require_string(data, "definition", context)
    else:
        raise ValueError(f"出力categoryが不正です: {context}")
    return data


def ensure_empty_output(output: Path) -> None:
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"出力先が空ではありません: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)


def write_document(output: Path, relative: Path, data: dict[str, object]) -> tuple[str, bytes]:
    target = (output / relative).resolve()
    target.relative_to(output.resolve())
    target.parent.mkdir(parents=True, exist_ok=True)
    content = stable_json(data)
    target.write_bytes(content)
    return relative.as_posix(), content


def main() -> int:
    arguments = parse_arguments()
    source = arguments.source.resolve()
    output = arguments.output.resolve()
    if not source.is_dir():
        print(f"入力sourceがありません: {source}", file=sys.stderr)
        return 2
    if source == output or source in output.parents:
        print("出力先は入力sourceの外側にしてください。", file=sys.stderr)
        return 2
    try:
        ensure_empty_output(output)
        source_documents = load_input_documents(source)
    except (RuntimeError, ValueError, OSError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 2

    sanitizer = FSourceSanitizer()
    pending: list[tuple[Path, dict[str, object]]] = []
    glossary_by_term: dict[str, list[tuple[Path, dict[str, object]]]] = {}
    for relative, data in source_documents:
        category = relative.parts[0] if relative.parts else ""
        if category == "features":
            sanitized = sanitizer.feature(data)
        elif category == "guides":
            sanitized = sanitizer.guide(data)
        elif category == "troubleshooting":
            sanitized = sanitizer.troubleshooting(data)
        elif category == "glossary":
            sanitized = sanitizer.glossary(data)
            key = normalized_term(str(sanitized["term"]))
            glossary_by_term.setdefault(key, []).append((relative, sanitized))
            continue
        else:
            raise ValueError(f"未知categoryです: {relative.as_posix()}")
        pending.append((relative, sanitized))

    for key, candidates in sorted(glossary_by_term.items()):
        if len(candidates) == 1:
            pending.append(candidates[0])
            continue
        canonical = CANONICAL_GLOSSARY.get(key)
        if canonical is None:
            sources = [relative.as_posix() for relative, _ in candidates]
            raise ValueError(f"glossaryの正規化衝突に定義がありません: {key}: {sources}")
        selected_relative = min(relative for relative, _ in candidates)
        selected = {
            "schema": 2,
            "term": canonical[0],
            "definition": canonical[1],
        }
        pending.append((selected_relative, selected))
        sanitizer.changes["用語の正規化統合"] += len(candidates) - 1

    pending.sort(key=lambda item: item[0].as_posix())
    errors: list[str] = []
    written: dict[str, dict[str, object]] = {}
    counts: Counter[str] = Counter()
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{output.name}.staging-",
            dir=output.parent,
        )
    ).resolve()
    try:
        for relative, data in pending:
            relative_text = relative.as_posix()
            errors.extend(validate_document(relative_text, data))
            route, content = write_document(staging, relative, data)
            written[route] = {"sha256": sha256(content), "bytes": len(content)}
            counts[relative.parts[0]] += 1
        if errors:
            raise ValueError("\n".join(errors[:100]))
        manifest = {
            "schema": 2,
            "counts": dict(sorted(counts.items())),
            "files": written,
        }
        write_document(staging, Path("manifest.json"), manifest)
        if output.exists():
            output.rmdir()
        staging.replace(output)
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        raise

    changes = " / ".join(f"{name} {count}件" for name, count in sorted(sanitizer.changes.items()) if count)
    print(
        f"ACSリファレンス正本を正規化しました: {len(written)}ファイル / "
        f"機能 {counts['features']}件 / 用語 {counts['glossary']}件"
    )
    if changes:
        print(f"正規化: {changes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
