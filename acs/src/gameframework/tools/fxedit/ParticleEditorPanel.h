// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — in-game ParticleEditor (Phase 19b)
//
// `FParticleEffectSystem` (gameframework/FParticleEffectSystem.h) の emitter
// パラメータを ImGui で実機編集するためのツール用パネル。Editor / DevTool
// ビルドからのみ使われる前提で、retail ビルドからは #ifdef で消す想定。
//
// 役割分担:
//   ・本パネルは `ParticleEmitterDef` の **編集** のみを担う。emitter handle
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
//   ・**非コピー / 非ムーブ**: 内部 `TArray<ParticleEmitterDef>` の所有を
//     曖昧にしない。ACS の他 system (FInspectorSeam, FParticleEffectSystem 等)
//     と同じ規約。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range 等を no-op /
//     null で表現。
//   ・**STL 不使用**: emitter list は `acs::TArray<ParticleEmitterDef>`。
//   ・**ImGui ヘッダは .cpp 側のみ include**: header からは imgui 依存を
//     漏らさず、ヘッダだけ見ても include order を意識せずに済むようにする。
//   ・**SaveCallback / LoadCallback は raw function pointer + void* user**:
//     STL の std::function を使えないため、C スタイルのコールバック規約に
//     揃える。InputManager / FAllocator など ACS 既存の callback パターン
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
//   ・`FParticleEffectSystem::CreateEmitter()` 等の真の `FEmitterHandle` 管理
//     とは独立 (editor 内では index 管理)。これは
//       「編集中に handle を持ち続けると、emitter 削除のたびに gen が変わって
//        editor 側の参照が壊れる」
//     のを避けるため。Apply 時に呼び出し側が `CreateEmitter` で反映する。
//   ・ImGui 関数の戻り値 (true on change) を見て emitter def を書き換えた
//     直後に `_dirty = true` を立てる。外部は `IsDirty()` で確認後 `ClearDirty()`
//     で同期 (= Save 用のシグナル)。
//   ・`spread_radians` は `ParticleEmitterDef` には現状フィールドが無いが、
//     仕様 (Phase 19b) で要求されているため editor 側で「将来追加予定」の
//     placeholder field を持つ。実 emitter 実行時には未使用。
//     ParticleEmitterDef に spread_radians フィールドが正式追加されたら、
//     `_extra_spread_radians` を削除して `def.spread_radians` 直結に切替える。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::fxedit {

// ---------------------------------------------------------------------------
// FParticleEditorPanel — ImGui ベースの emitter property editor
// (Phase 24: editor_core::FEditorPanel 継承)
// ---------------------------------------------------------------------------
class FParticleEditorPanel : public ::acs::game::editor_core::FEditorPanel {
public:
    // Save / Load callback 型。`user` は SetSaveCallback / SetLoadCallback の
    // 第二引数で渡したポインタがそのまま戻る (closure 代替)。
    using SaveCallback = void (*)(void* user, const ParticleEmitterDef* defs, u32 count) noexcept;
    using LoadCallback = void (*)(void* user, ParticleEmitterDef* defs, u32& inout_count) noexcept;

    FParticleEditorPanel() noexcept = default;
    ~FParticleEditorPanel() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray<ParticleEmitterDef> の所有を曖昧にしない。
    FParticleEditorPanel(const FParticleEditorPanel&)            = delete;
    FParticleEditorPanel& operator=(const FParticleEditorPanel&) = delete;
    FParticleEditorPanel(FParticleEditorPanel&&)                 = delete;
    FParticleEditorPanel& operator=(FParticleEditorPanel&&)      = delete;

    // 初期化: emitter list を空に戻し selection を解除する。
    // 多重 Init 可能 (= 完全リセット)。
    void Init() noexcept;

    // 後片付け: emitter list を解放 (TArray は実体破棄時にも解放するため
    // 明示 Shutdown は冪等)。多重 Shutdown 可能。
    void Shutdown() noexcept;

    // Phase 24: FEditorPanel 基底に乗せるため、target system は SetTargetSystem
    // で先に set してから DrawUI() を呼ぶパターンに変更。
    void SetTargetSystem(class FParticleEffectSystem* system) noexcept { _target_system = system; }
    class FParticleEffectSystem* TargetSystem() const noexcept { return _target_system; }

    // FEditorPanel override: タイトル。
    const char* Title() const noexcept override { return "Particle Editor"; }

    // FEditorPanel override: メイン ImGui window 描画。target system が set されて
    // いれば active particle 数を表示。`Begin("Particle Editor")` から始まる
    // 単一 window レイアウト。
    void DrawUI() noexcept override;

    // 新規 emitter を default param で末尾に追加し、selection を新規 emitter
    // に移す。上限 (kMaxEmitters) に達したら no-op。
    void AddEmitter() noexcept;

    // 選択中 emitter を削除。selection は前の index に詰める。
    // 未選択 / 範囲外なら no-op。
    void RemoveSelectedEmitter() noexcept;

    // 選択中 emitter を複製して直後に挿入、selection を複製先に移す。
    // 未選択 / 上限到達は no-op。
    void DuplicateSelectedEmitter() noexcept;

    // 現在の選択 index。未選択は -1。
    i32 SelectedIndex() const noexcept { return _selected; }

    // 選択 index を設定。範囲外 (>= EmitterCount()) は -1 (未選択) に
    // 正規化する。負値も -1 にクランプ。
    void SelectEmitter(i32 index) noexcept;

    // 現在の emitter 数。
    u32 EmitterCount() const noexcept;

    // index 番目の emitter def (read-only)。範囲外は nullptr。
    const ParticleEmitterDef* GetEmitterDef(i32 index) const noexcept;

    // index 番目の emitter def (mutable)。範囲外は nullptr。
    // 書き換え後は呼び出し側で `MarkDirty()` する想定。
    ParticleEmitterDef* GetEmitterDefMutable(i32 index) noexcept;

    // Save callback を登録。nullptr 渡しで解除可能。
    void SetSaveCallback(SaveCallback cb, void* user) noexcept;

    // Load callback を登録。nullptr 渡しで解除可能。
    void SetLoadCallback(LoadCallback cb, void* user) noexcept;

    // emitter list 上限 (UI と内部両方の安全弁)。これを超えると AddEmitter /
    // DuplicateSelectedEmitter は no-op。
    static constexpr u32 kMaxEmitters = 64u;

    // 現在 dirty (= UI で何か書き換えた / Add / Remove / Dup された) か。
    // 外部の Save タイミング判定に使う。
    bool IsDirty() const noexcept { return _dirty; }

    // dirty フラグをクリア。Save 完了後に外部が呼ぶ想定。
    void ClearDirty() noexcept { _dirty = false; }

private:
    TArray<ParticleEmitterDef> _emitters {};
    // editor 専用の「将来 ParticleEmitterDef に追加される予定」の値。
    // 現状 ParticleEmitterDef に spread_radians フィールドが無いので、
    // editor 側のみで持つ。長さは _emitters と同期する。
    TArray<f32>                _extra_spread_radians {};

    i32           _selected     = -1;
    bool          _dirty        = false;

    SaveCallback  _save_cb      = nullptr;
    void*         _save_user    = nullptr;
    LoadCallback  _load_cb      = nullptr;
    void*         _load_user    = nullptr;

    // Phase 24: read-only target (active particle count 表示用)。
    // nullptr のときは "(no system attached)" を表示。
    class FParticleEffectSystem* _target_system = nullptr;
};

} // namespace acs::game::fxedit
