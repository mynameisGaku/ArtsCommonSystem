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
//       if (m.enabled) MountPack(m.pack_path);  // AssetPack 統合は別途
//   }
//
// 設計選択:
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
// 範囲外:
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

/**
 * 1 Mod 分のメタデータ。
 *
 * @details
 * 文字列フィールドは「呼び出し側が所有する」前提で nullptr 許容にしている
 * (id は Mod 一意キーで空文字や nullptr は Register 時にスキップ、name は表示用、
 * pack_path は .acpak のパスで nullptr なら path 未指定)。version は
 * (major << 24) | (minor << 16) | patch エンコーディングを想定するが、Registry
 * 側は不透明に扱う (比較のみ)。
 */
struct FModInfo {
    /** Mod 一意キー (外部所有。空文字や nullptr は Register 時にスキップ)。 */
    const char* id         = nullptr;

    /** 表示用の名前 (外部所有。UI が pull する。nullptr 可)。 */
    const char* name       = nullptr;

    /** バージョン ((major << 24) | (minor << 16) | patch、Registry は不透明扱い)。 */
    u32         version    = 0;

    /** load 順 (昇順。小さい方を先に load、大きい方が後で上書き)。 */
    i32         load_order = 0;

    /** 有効フラグ (true で mount 対象)。 */
    bool        enabled    = false;

    /** .acpak のファイルパス (外部所有。nullptr なら path 未指定)。 */
    const char* pack_path  = nullptr;
};

/**
 * ユーザー Mod の登録・有効化・並び順を管理する薄いレジストリ。
 *
 * @details
 * 各 Mod を FModInfo の内部 TArray に保持し、load_order 昇順に並べる。文字列は
 * 所有せず呼び出し側の寿命に依存する。1 ゲーム寿命に 1 インスタンスのみを想定し
 * non-copy / non-move とする。
 */
class FModRegistry {
public:
    /** 空のレジストリを構築する。 */
    FModRegistry() noexcept = default;

    /** 破棄する (内部 TArray が解放される)。 */
    ~FModRegistry() noexcept = default;

    /** コピー禁止 (active な Registry を一意にするため)。 */
    FModRegistry(const FModRegistry&)            = delete;

    /** コピー代入も禁止。 */
    FModRegistry& operator=(const FModRegistry&) = delete;

    /** ムーブ禁止。 */
    FModRegistry(FModRegistry&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FModRegistry& operator=(FModRegistry&&)      = delete;

    /**
     * Mod を内部リストの末尾に登録する。
     *
     * @details
     * info を内部 TArray に浅くコピーして追加する (指す文字列バッファの寿命は
     * 呼び出し側が保証する)。id == nullptr のエントリは無視する (警告ログのみ)。
     * @param info 登録する Mod のメタデータ。
     */
    void Register(const FModInfo& info) noexcept;

    /**
     * id に一致する Mod を有効化する。
     *
     * @param mod_id 有効化する Mod の一意キー。
     * @return 見つかって enabled を立てたら true。
     */
    bool Enable (const char* mod_id) noexcept;

    /**
     * id に一致する Mod を無効化する。
     *
     * @param mod_id 無効化する Mod の一意キー。
     * @return 見つかって enabled を落としたら true。
     */
    bool Disable(const char* mod_id) noexcept;

    /**
     * 指定 Mod の load_order を変更する (sort は別途呼び出し側で実行)。
     *
     * @details 見つからなくても黙って no-op (UI 側で都度同期する想定)。
     * @param mod_id 対象 Mod の一意キー。
     * @param order 新しい load 順。
     */
    void SetLoadOrder(const char* mod_id, i32 order) noexcept;

    /**
     * 登録済み Mod の件数を返す。
     *
     * @return Mod の件数。
     */
    u32                Count() const noexcept;

    /**
     * id に一致する Mod を検索する。
     *
     * @param mod_id 探す Mod の一意キー。
     * @return 見つかった FModInfo へのポインタ (無ければ nullptr)。
     */
    const FModInfo*     Find (const char* mod_id) const noexcept;

    /**
     * 登録済み Mod の生バッファ先頭を返す。
     *
     * @return FModInfo 配列の先頭 (長さは Count() で確定)。
     */
    const FModInfo*     All  () const noexcept;

    /**
     * load_order 昇順に並べ替える (同値は登録順を保つ安定 sort)。
     */
    void SortByLoadOrder() noexcept;

    /**
     * 登録済み Mod を全て削除する。
     */
    void Clear() noexcept;

private:
    /**
     * 2 つの id 文字列が等しいかを返す (両者 nullptr 安全)。
     *
     * @param a 比較する id (nullptr 可)。
     * @param b 比較する id (nullptr 可)。
     * @return 文字列として一致すれば true。
     */
    static bool IdEquals(const char* a, const char* b) noexcept;

    /** 登録済み Mod の配列 (登録順、SortByLoadOrder で並べ替え)。 */
    TArray<FModInfo> m_Mods;
};

} // namespace acs::game
