// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — in-game ParticleEditor
//
// `CParticleEffectSystem` (gameframework/ParticleEffectSystem.h) の emitter
// パラメータを ImGui で実機編集するためのツール用パネル。Editor / DevTool
// ビルドからのみ使われる前提で、retail ビルドからは #ifdef で消す想定。
//
// 役割分担:
//   ・本パネルは `FParticleEmitterDef` の **編集** のみを担う。emitter handle
//     や particle pool への反映 (CreateEmitter / DestroyEmitter) は呼び出し
//     側 (例: dev tool 統合レイヤ) が行う。これは
//       (1) editor 側で in-place に EmitterDef を弄り続け、Apply 時にまとめて
//           CreateEmitter する運用と
//       (2) ライブ編集 (emitter は既に走っており、def を毎フレーム反映する) の
//           両運用を切り替えられるようにするため。
//   ・Save / Load は callback hook (`SaveCallback` / `LoadCallback`) で
//     外部に委譲。JSON / binary / pak 等のフォーマット選択を editor 自身が
//     知らなくて済む。
//
// 設計選択:
//   ・**非コピー / 非ムーブ**: 内部 `TArray<FParticleEmitterDef>` の所有を
//     曖昧にしない。ACS の他 system (CInspectorSeam, CParticleEffectSystem 等)
//     と同じ規約。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range 等を no-op /
//     null で表現。
//   ・**STL 不使用**: emitter list は `acs::TArray<FParticleEmitterDef>`。
//   ・**ImGui ヘッダは .cpp 側のみ include**: header からは imgui 依存を
//     漏らさず、ヘッダだけ見ても include order を意識せずに済むようにする。
//   ・**SaveCallback / LoadCallback は raw function pointer + void* user**:
//     STL の std::function を使えないため、C スタイルのコールバック規約に
//     揃える。InputManager / IAllocator など ACS 既存の callback パターン
//     (`acs/src/platform/Input.h` のキャプチャ関数等) と同形。
//   ・**SelectedIndex は i32**: -1 を「未選択」シグネルとして使うため、
//     u32 ではなく i32 を採用。`u32 EmitterCount()` とは型が違うが、
//     これは Selected 専用の API 規約 (= 「-1 で 'なし' を表す」) を貫くため。
//
// ImGui レイアウト (DrawUI):
//   ┌────────────── "Particle Editor" window ───────────────────┐
//   │ ┌─ left column ──┐  ┌─ right column ──────────────────┐   │
//   │ │ [Emitter list] │  │ Selected: idx=N                  │   │
//   │ │  - Emitter 0   │  │ lifetime_sec     [slider]        │   │
//   │ │  - Emitter 1   │  │ emit_rate_per_sec[slider]        │   │
//   │ │  ...           │  │ burst_count      [slider]        │   │
//   │ │ [+] [Dup] [-]  │  │ speed_min        [slider]        │   │
//   │ │ [Save] [Load]  │  │ speed_max        [slider]        │   │
//   │ └────────────────┘  │ scale_start      [slider]        │   │
//   │                     │ scale_end        [slider]        │   │
//   │                     │ spread_radians   [slider]        │   │
//   │                     │ gravity          [InputFloat2]   │   │
//   │                     │ color_start      [ColorEdit3]    │   │
//   │                     │ color_end        [ColorEdit3]    │   │
//   │                     └──────────────────────────────────┘   │
//   └─────────────────────────────────────────────────────────────┘
//
// 注意:
//   ・`CParticleEffectSystem::CreateEmitter()` 等の真の `FEmitterHandle` 管理
//     とは独立 (editor 内では index 管理)。これは
//       「編集中に handle を持ち続けると、emitter 削除のたびに gen が変わって
//        editor 側の参照が壊れる」
//     のを避けるため。Apply 時に呼び出し側が `CreateEmitter` で反映する。
//   ・ImGui 関数の戻り値 (true on change) を見て emitter def を書き換えた
//     直後に `m_Dirty = true` を立てる。外部は `IsDirty()` で確認後 `ClearDirty()`
//     で同期 (= Save 用のシグナル)。
//   ・`spread_radians` は `FParticleEmitterDef` には現状フィールドが無いが、
//     仕様で要求されているため editor 側で「将来追加予定」の
//     placeholder field を持つ。実 emitter 実行時には未使用。
//     FParticleEmitterDef に spread_radians フィールドが正式追加されたら、
//     `m_ExtraSpreadRadians` を削除して `def.spread_radians` 直結に切替える。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::fxedit {

/**
 * ImGui ベースの emitter property editor パネル。
 *
 * @details
 * `CParticleEffectSystem` の emitter パラメータ (FParticleEmitterDef) を実機編集する
 * editor_core::AEditorPanel 派生のツールパネル。emitter list の編集 (Add/Remove/
 * Duplicate) のみを担い、emitter handle や particle pool への反映と Save/Load の実体は
 * 呼び出し側 / callback に委譲する。非コピー・非ムーブ・全 noexcept・STL 不使用。
 */
class AParticleEditorPanel : public ::acs::game::editor_core::AEditorPanel {
public:
    /**
     * Save callback 型。
     *
     * @details user は SetSaveCallback の第二引数で渡したポインタがそのまま戻る (closure 代替)。
     */
    using SaveCallback = void (*)(void* user, const FParticleEmitterDef* defs, u32 count) noexcept;

    /**
     * Load callback 型。
     *
     * @details
     * user は SetLoadCallback の第二引数で渡したポインタがそのまま戻る。inout_count は
     * 入力で受け入れ可能な最大数、出力で実際にロードした数。
     */
    using LoadCallback = void (*)(void* user, FParticleEmitterDef* defs, u32& inout_count) noexcept;

    /** 空のパネルを構築する (emitter list は空、未選択)。 */
    AParticleEditorPanel() noexcept = default;

    /** 破棄する (内部 TArray が emitter list を解放)。 */
    ~AParticleEditorPanel() noexcept = default;

    /** コピー禁止 (内部 TArray の所有を曖昧にしないため)。 */
    AParticleEditorPanel(const AParticleEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AParticleEditorPanel& operator=(const AParticleEditorPanel&) = delete;

    /** ムーブ禁止。 */
    AParticleEditorPanel(AParticleEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AParticleEditorPanel& operator=(AParticleEditorPanel&&)      = delete;

    /**
     * emitter list を空に戻し selection を解除する (完全リセット)。
     *
     * @details 多重 Init 可能。callback はリセットしない。
     */
    void Init() noexcept;

    /**
     * emitter list と callback を解放する。
     *
     * @details TArray は実体破棄時にも解放するため明示 Shutdown は冪等。多重 Shutdown 可能。
     */
    void Shutdown() noexcept;

    /**
     * read-only の target system を設定する (active particle 数表示用)。
     *
     * @param system 紐付ける CParticleEffectSystem (nullptr で解除)。
     */
    void SetTargetSystem(CParticleEffectSystem* system) noexcept { m_TargetSystem = system; }

    /**
     * 現在の target system を返す。
     *
     * @return 設定済みの CParticleEffectSystem (未設定なら nullptr)。
     */
    CParticleEffectSystem* TargetSystem() const noexcept { return m_TargetSystem; }

    /**
     * パネルのタイトルを返す (AEditorPanel override)。
     *
     * @return "Particle Editor"。
     */
    const char* Title() const noexcept override { return "Particle Editor"; }

    /**
     * メイン ImGui window を描画する (AEditorPanel override)。
     *
     * @details
     * `Begin("Particle Editor")` から始まる単一 window で、左に emitter list、右に
     * 選択中 emitter の properties を 2 カラムで並べる。target system が set されていれば
     * active particle 数も表示する。
     */
    void DrawUI() noexcept override;

    /**
     * 新規 emitter を「火花っぽい」プリセットで末尾に追加し、selection を移す。
     *
     * @details 上限 kMaxEmitters に達していれば no-op。
     */
    void AddEmitter() noexcept;

    /**
     * 選択中 emitter を順序保存しつつ削除する。
     *
     * @details 削除後 selection は同 index (= 元の次の emitter)、末尾削除時は前の要素、
     * 空なら -1 になる。未選択 / 範囲外なら no-op。
     */
    void RemoveSelectedEmitter() noexcept;

    /**
     * 選択中 emitter を複製して直後に挿入し、selection を複製先に移す。
     *
     * @details 未選択 / 上限到達は no-op。
     */
    void DuplicateSelectedEmitter() noexcept;

    /**
     * 現在の選択 index を返す。
     *
     * @return 選択中の index、未選択なら -1。
     */
    i32 SelectedIndex() const noexcept { return m_Selected; }

    /**
     * 選択 index を設定する。
     *
     * @details 範囲外 (>= EmitterCount()) や負値は -1 (未選択) に正規化する。
     * @param index 選択する emitter index。
     */
    void SelectEmitter(i32 index) noexcept;

    /**
     * 現在の emitter 数を返す。
     *
     * @return emitter list の要素数。
     */
    u32 EmitterCount() const noexcept;

    /**
     * index 番目の emitter def を返す (read-only)。
     *
     * @param index 取得する emitter index。
     * @return emitter def への const ポインタ、範囲外なら nullptr。
     */
    const FParticleEmitterDef* GetEmitterDef(i32 index) const noexcept;

    /**
     * index 番目の emitter def を返す (mutable)。
     *
     * @details 書き換え後は dirty を立てたい場合 IsDirty/ClearDirty の運用に注意。
     * @param index 取得する emitter index。
     * @return emitter def へのポインタ、範囲外なら nullptr。
     */
    FParticleEmitterDef* GetEmitterDefMutable(i32 index) noexcept;

    /**
     * Save callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback 呼び出し時にそのまま渡すユーザポインタ。
     */
    void SetSaveCallback(SaveCallback cb, void* user) noexcept;

    /**
     * Load callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback 呼び出し時にそのまま渡すユーザポインタ。
     */
    void SetLoadCallback(LoadCallback cb, void* user) noexcept;

    /**
     * emitter list の上限 (UI と内部両方の安全弁)。
     *
     * @details これを超えると AddEmitter / DuplicateSelectedEmitter は no-op になる。
     */
    static constexpr u32 kMaxEmitters = 64u;

    /**
     * dirty フラグ (UI 編集 / Add / Remove / Dup で立つ) を返す。
     *
     * @return 何か変更されていれば true。外部の Save タイミング判定に使う。
     */
    bool IsDirty() const noexcept { return m_Dirty; }

    /** dirty フラグをクリアする (Save 完了後に外部が呼ぶ想定)。 */
    void ClearDirty() noexcept { m_Dirty = false; }

private:
    /** 編集中の emitter def 群 (所有)。 */
    TArray<FParticleEmitterDef> m_Emitters {};

    /**
     * editor 専用の spread_radians 値 (FParticleEmitterDef に未配備の placeholder)。
     *
     * @details 長さは m_Emitters と同期する。実 emitter 実行時には未使用。
     */
    TArray<f32>                m_ExtraSpreadRadians {};

    /** 現在の選択 index (未選択は -1)。 */
    i32           m_Selected     = -1;

    /** dirty フラグ (編集された)。 */
    bool          m_Dirty        = false;

    /** Save callback (未登録なら nullptr)。 */
    SaveCallback  m_SaveCb      = nullptr;

    /** Save callback に渡すユーザポインタ。 */
    void*         m_SaveUser    = nullptr;

    /** Load callback (未登録なら nullptr)。 */
    LoadCallback  m_LoadCb      = nullptr;

    /** Load callback に渡すユーザポインタ。 */
    void*         m_LoadUser    = nullptr;

    /** read-only target (active particle count 表示用、nullptr で "(no system attached)")。 */
    CParticleEffectSystem* m_TargetSystem = nullptr;
};

using FParticleEditorPanel = AParticleEditorPanel;

} // namespace acs::game::fxedit
