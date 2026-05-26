// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — FModRegistry (Mod 読み込み順管理)
//
// ユーザー Mod (= 追加コンテンツパック) の登録・有効化・並び順管理を行う薄い
// レジストリ。各 Mod は `.acpak` (Pillar G AssetPack 形式) を 1 つ伴うことが
// 期待され、ロード順に従って top-level の VirtualFileSystem に重ねていく
// (後勝ち = load_order 大きい方が前段の同名アセットを上書き) 想定。
//
// 使い方:
//   FModRegistry mr;
//   FModInfo a{};
//   a.id         = "core";
//   a.name       = "Core Pack";
//   a.version    = 0x00010000u;  // 1.0.0
//   a.load_order = 0;
//   a.enabled    = true;
//   a.pack_path  = "mods/core.acpak";
//   mr.Register(a);
//
//   FModInfo b{};
//   b.id         = "weapons-ex";
//   b.name       = "Weapons EX";
//   b.version    = 0x00000200u;  // 0.2.0
//   b.load_order = 10;
//   b.enabled    = false;
//   b.pack_path  = "mods/weapons_ex.acpak";
//   mr.Register(b);
//
//   mr.Enable("weapons-ex");
//   mr.SortByLoadOrder();
//   for (u32 i = 0; i < mr.Count(); ++i) {
//       const FModInfo& m = mr.All()[i];
//       if (m.enabled) MountPack(m.pack_path);  // TODO: AssetPack Phase 2
//   }
//
// 設計選択 (Pillar N Phase 1 = skeleton):
//   ・**所有しない const char***: id / name / pack_path は外部所有の文字列を
//     借りるだけ (`<string>` 禁止)。呼び出し側 (= Mod manifest loader) が
//     寿命を持ち、Registry の寿命を超えるまで生かす責務を負う。manifest を
//     パースしたバッファをそのまま指す前提。
//   ・**load_order = i32 昇順**: 数値が小さい方を先に load、大きい方が後で
//     上書きする (UE / Bethesda 系と同じ慣習)。負値は core 系 (= 必須前段)
//     のため確保。
//   ・**並び替えは明示**: `Register` 時には末尾追加するだけで、`SortByLoadOrder`
//     を呼ぶまでは登録順を保つ。Editor 側 UI で一覧表示 → ユーザーが順序を
//     変えてから sort という流れを想定。
//   ・**簡素 insertion sort**: N (= Mod 数) は実用上 < 64 と想定 (steam workshop の
//     1 ゲーム平均 ~30)。比較関数を渡す sort utility をまだ持っていないため、
//     in-place の insertion sort で十分。安定 sort なので同 load_order の
//     先着優先も保たれる。
//   ・**非コピー・非ムーブ**: Mod 管理はゲーム寿命に 1 インスタンスのみ。複製を
//     許すと "どの Registry が active か" が曖昧になるので禁止 (FInputMap と同じ)。
//
// 範囲外 (Phase 2+ で):
//   ・実際の `.acpak` mount (Pillar G AssetPack 統合) — Mount API 自体が
//     未確定のため、Registry は path を保持するだけ。
//   ・Mod 間依存解決 (dependency graph、循環検出、欠落警告)。
//   ・Mod manifest (.toml / .json) のパース — 外部 loader が FModInfo を組み立てる。
//   ・hook 適用 (Pillar N の核となる script / DLL ロード)。Lua 5.4 統合や
//     C++ plugin 動的 load は別レイヤで、Registry は「enabled かどうか」を
//     公開するだけ。
//   ・ホットリロード / 実行中の load_order 並び替え反映。
//   ・Workshop / CurseForge 等の外部配布チャネル統合 (= Pillar S Storefront 側)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// 1 Mod 分のメタデータ。
//
// 文字列フィールドは「呼び出し側が所有する」前提で nullptr 許容にしている:
//   ・`id`        : Mod 一意キー。空文字や nullptr は Register 時にスキップ。
//   ・`name`      : 表示用 (UI が pull する)。nullptr 可。
//   ・`pack_path` : `.acpak` のファイルパス。nullptr なら「path 未指定」(= manifest
//                   のみ登録で実体 mount は後で別途渡す) を意味する。
//
// `version` は (major << 24) | (minor << 16) | patch エンコーディングを想定する
// が、Registry 側は不透明に扱う (比較のみ)。
struct FModInfo {
    const char* id         = nullptr;
    const char* name       = nullptr;
    u32         version    = 0;
    i32         load_order = 0;
    bool        enabled    = false;
    const char* pack_path  = nullptr;
};

class FModRegistry {
public:
    FModRegistry() noexcept = default;
    ~FModRegistry() noexcept = default;

    FModRegistry(const FModRegistry&)            = delete;
    FModRegistry& operator=(const FModRegistry&) = delete;
    FModRegistry(FModRegistry&&)                 = delete;
    FModRegistry& operator=(FModRegistry&&)      = delete;

    // ----- 登録 -----
    // info を内部 TArray にコピーで追加する (FModInfo は POD なので浅いコピーで OK、
    // ただし指している文字列バッファの寿命は呼び出し側が保証する)。
    // id == nullptr のエントリは無視 (警告ログのみ)。
    void Register(const FModInfo& info) noexcept;

    // ----- 有効化 -----
    // id に一致する Mod の enabled フラグを書き換える。見つかれば true。
    bool Enable (const char* mod_id) noexcept;
    bool Disable(const char* mod_id) noexcept;

    // 指定 Mod の load_order を変更する (sort は別途呼び出し側で実行)。
    // 見つからなくても黙って noop (UI 側で都度同期する想定)。
    void SetLoadOrder(const char* mod_id, i32 order) noexcept;

    // ----- 検索・列挙 -----
    u32                Count() const noexcept;
    const FModInfo*     Find (const char* mod_id) const noexcept;  // nullptr if not found
    const FModInfo*     All  () const noexcept;  // 生バッファ、Count() で長さ確定

    // load_order 昇順に並べ替える (同値は登録順を保つ安定 sort)。
    void SortByLoadOrder() noexcept;

    // 全 Mod を削除。
    void Clear() noexcept;

private:
    // 内部の id 比較ヘルパ (両者 nullptr 安全)。
    static bool IdEquals(const char* a, const char* b) noexcept;

    TArray<FModInfo> _mods;
};

} // namespace acs::game
