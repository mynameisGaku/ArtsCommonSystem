// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — in-game ParticleEditor 実装 (Phase 19b)
//
// 仕様の意図は ParticleEditorPanel.h を参照。本ファイルでは:
//   ・emitter list の Add / Remove / Duplicate (= editor 側 index 管理)
//   ・ImGui を使った 2 カラムレイアウトの描画
//   ・Save / Load callback の forward
// を実装する。すべて noexcept、STL 不使用。
//
// ImGui 依存はこの .cpp に閉じる (ヘッダには imgui.h を含めない)。
#include "gameframework/tools/fxedit/ParticleEditorPanel.h"

#include <imgui.h>

#include <cstdio>  // snprintf (emitter ラベル整形)

namespace acs::game::fxedit {

// ---- ローカルヘルパ ------------------------------------------------------
// emitter 1 件の表示ラベル "Emitter NN" を整形する。ImGui::Selectable の
// 識別子兼表示文字列として使う。先頭 "##" で ID 衝突避けはせず、
// PushID(i) でユニーク化する戦略。
static void FormatEmitterLabel(char* buf, usize buf_size, i32 index) noexcept {
    if (buf == nullptr || buf_size == 0) return;
    std::snprintf(buf, buf_size, "Emitter %d", static_cast<int>(index));
}

// emitter index の境界チェック。負値 / 範囲外を弾く。
static bool IsValidIndex(i32 index, u32 count) noexcept {
    if (index < 0) return false;
    return static_cast<u32>(index) < count;
}

// =============================================================================
// Init / Shutdown
// =============================================================================
void ParticleEditorPanel::Init() noexcept {
    // 完全リセット: 既存登録があれば破棄して 0 件状態にする。
    // 多重 Init を許容するため、_emitters.Clear() で実体は解放せず
    // 容量だけ保持 (= 次回再構築のアロケーション節約)。
    _emitters.Clear();
    _extra_spread_radians.Clear();
    _selected  = -1;
    _dirty     = false;
    // callback はリセットしない (Init は emitter list の reset、callback の
    // クリアは別操作と扱う)。
}

void ParticleEditorPanel::Shutdown() noexcept {
    // TArray はデストラクタで解放されるが、明示的に Clear することで
    // 多重 Shutdown / 再 Init の確定状態を作る。
    _emitters.Clear();
    _extra_spread_radians.Clear();
    _selected = -1;
    _dirty    = false;
    // callback は外部所有 (関数ポインタ) なので破棄不要。明示的に解除する。
    _save_cb   = nullptr;
    _save_user = nullptr;
    _load_cb   = nullptr;
    _load_user = nullptr;
}

// =============================================================================
// AddEmitter
// =============================================================================
// default ParticleEmitterDef を 1 件追加し、新規 emitter を selection に。
// 上限 kMaxEmitters に達していれば no-op。
// =============================================================================
void ParticleEditorPanel::AddEmitter() noexcept {
    if (_emitters.Size() >= static_cast<usize>(kMaxEmitters)) {
        // 上限到達は silent no-op (UI からは Add ボタンが見えていても安全)。
        return;
    }
    ParticleEmitterDef def {};
    // default で全くパラメータが無いと放出されないため、初学者にも見やすい
    // 「火花っぽい」プリセットを入れる。手で調整する前提の出発点。
    def.lifetime_sec       = 0.8f;
    def.emit_rate_per_sec  = 20.0f;
    def.burst_count        = 8.0f;
    def.speed_min          = 30.0f;
    def.speed_max          = 80.0f;
    def.scale_start        = 6.0f;
    def.scale_end          = 0.0f;
    def.color_start        = {1.0f, 0.8f, 0.2f};
    def.color_end          = {0.8f, 0.1f, 0.0f};
    def.gravity            = {0.0f, 60.0f};

    _emitters.PushBack(def);
    _extra_spread_radians.PushBack(0.0f);  // spread = 0 = 円周一様
    _selected = static_cast<i32>(_emitters.Size()) - 1;
    _dirty    = true;
}

// =============================================================================
// RemoveSelectedEmitter
// =============================================================================
// 選択中 emitter を順序保存しつつ削除 (PushBack/PopBack ではなく
// 詰める方式)。emitter リストは「上から順に表示される」ので順序を保つ。
// =============================================================================
void ParticleEditorPanel::RemoveSelectedEmitter() noexcept {
    const u32 count = static_cast<u32>(_emitters.Size());
    if (!IsValidIndex(_selected, count)) return;

    const usize sel = static_cast<usize>(_selected);
    // 順序保存削除 (= 後ろの要素を 1 個ずつ前に詰める)。
    // TArray に Erase API が無いので手書き。
    for (usize i = sel + 1; i < _emitters.Size(); ++i) {
        _emitters[i - 1]               = _emitters[i];
        _extra_spread_radians[i - 1]   = _extra_spread_radians[i];
    }
    _emitters.PopBack();
    _extra_spread_radians.PopBack();

    // selection 更新: 削除後は同 index の要素 (= 元の次の emitter) を
    // 選択。最後尾を消した場合は前の要素を選択。空なら -1。
    const i32 new_count = static_cast<i32>(_emitters.Size());
    if (new_count == 0) {
        _selected = -1;
    } else if (_selected >= new_count) {
        _selected = new_count - 1;
    }
    // _selected < new_count の場合はそのまま (= 詰めた要素が選ばれる)。
    _dirty = true;
}

// =============================================================================
// DuplicateSelectedEmitter
// =============================================================================
// 選択中 emitter を直後に挿入。selection は複製先に移る。
// 上限到達 / 未選択 は no-op。
// =============================================================================
void ParticleEditorPanel::DuplicateSelectedEmitter() noexcept {
    const u32 count = static_cast<u32>(_emitters.Size());
    if (!IsValidIndex(_selected, count)) return;
    if (_emitters.Size() >= static_cast<usize>(kMaxEmitters)) return;

    const usize src = static_cast<usize>(_selected);
    // 末尾に PushBack してから 1 個ずつ後ろにシフトして挿入位置を空ける。
    // src+1 番目に挿入したい。
    ParticleEmitterDef copy   = _emitters[src];                // 値コピー
    f32                spread = _extra_spread_radians[src];
    _emitters.PushBack(copy);
    _extra_spread_radians.PushBack(spread);

    // src+1 .. (旧末尾) を 1 つ後ろにシフト。末尾には今 PushBack した
    // 値が居るので、その上書きは避けるため逆順に走査する。
    // 旧末尾 index = new_size - 2、shift 範囲 = src+1 .. old_last
    const usize new_size = _emitters.Size();
    if (new_size >= 2) {
        for (usize i = new_size - 1; i > src + 1; --i) {
            _emitters[i]             = _emitters[i - 1];
            _extra_spread_radians[i] = _extra_spread_radians[i - 1];
        }
    }
    // src + 1 に複製を書き込む (シフト後は src+1 の中身が src の元値で
    // 上書きされている可能性があるため、最後に明示代入する)。
    if (src + 1 < new_size) {
        _emitters[src + 1]             = copy;
        _extra_spread_radians[src + 1] = spread;
    }

    _selected = static_cast<i32>(src) + 1;
    _dirty    = true;
}

// =============================================================================
// SelectEmitter / EmitterCount / GetEmitterDef
// =============================================================================
void ParticleEditorPanel::SelectEmitter(i32 index) noexcept {
    const u32 count = static_cast<u32>(_emitters.Size());
    if (index < 0) {
        _selected = -1;
        return;
    }
    if (static_cast<u32>(index) >= count) {
        _selected = -1;
        return;
    }
    _selected = index;
}

u32 ParticleEditorPanel::EmitterCount() const noexcept {
    return static_cast<u32>(_emitters.Size());
}

const ParticleEmitterDef* ParticleEditorPanel::GetEmitterDef(i32 index) const noexcept {
    if (!IsValidIndex(index, static_cast<u32>(_emitters.Size()))) return nullptr;
    return &_emitters[static_cast<usize>(index)];
}

ParticleEmitterDef* ParticleEditorPanel::GetEmitterDefMutable(i32 index) noexcept {
    if (!IsValidIndex(index, static_cast<u32>(_emitters.Size()))) return nullptr;
    return &_emitters[static_cast<usize>(index)];
}

// =============================================================================
// SetSaveCallback / SetLoadCallback
// =============================================================================
void ParticleEditorPanel::SetSaveCallback(SaveCallback cb, void* user) noexcept {
    _save_cb   = cb;
    _save_user = user;
}

void ParticleEditorPanel::SetLoadCallback(LoadCallback cb, void* user) noexcept {
    _load_cb   = cb;
    _load_user = user;
}

// =============================================================================
// DrawUI — メイン ImGui window
// =============================================================================
// 2 カラムレイアウト:
//   left  : emitter list + Add / Dup / Remove / Save / Load
//   right : 選択中 emitter の properties (slider / colorEdit / inputFloat)
//
// ImGui 関数の戻り値 (= true on change) を捕まえて _dirty を立てる。
// =============================================================================
void ParticleEditorPanel::DrawUI() noexcept {
    // Phase 24: EditorPanel 継承で no-param DrawUI 化。target system は
    // SetTargetSystem() で事前に set 想定 (nullptr のときは live particle
    // 数を "(no system)" 表示)。
    if (!ImGui::Begin("Particle Editor")) {
        ImGui::End();
        return;
    }

    // ヘッダ行: 現在の active particle 数 (target_system 由来) と emitter 数。
    // editor 内の emitter 数と system 側の真の emitter 数は別物だが、
    // particle 数は system が真値なので分けて表示する。
    if (_target_system != nullptr) {
        ImGui::Text("Editor Emitters: %u / %u    Live Particles: %u",
                    static_cast<unsigned>(_emitters.Size()),
                    static_cast<unsigned>(kMaxEmitters),
                    static_cast<unsigned>(_target_system->ActiveParticleCount()));
    } else {
        ImGui::Text("Editor Emitters: %u / %u    Live Particles: (no system attached)",
                    static_cast<unsigned>(_emitters.Size()),
                    static_cast<unsigned>(kMaxEmitters));
    }
    ImGui::Separator();

    // ----- 2 カラムレイアウト -----
    // BeginColumns でも良いが、Phase 19b では Child window で左右を分ける方が
    // resize ハンドル不要で読みやすい。
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = (content_w > 480.0f) ? 200.0f : content_w * 0.35f;

    // ===== 左カラム: emitter list =====
    ImGui::BeginChild("##fxedit_left", ImVec2(left_w, 0), true);
    {
        ImGui::TextUnformatted("Emitters");
        ImGui::Separator();

        for (i32 i = 0; i < static_cast<i32>(_emitters.Size()); ++i) {
            char label[32];
            FormatEmitterLabel(label, sizeof(label), i);
            const bool is_selected = (_selected == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(label, is_selected)) {
                _selected = i;
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("+ Add")) {
            AddEmitter();
        }
        ImGui::SameLine();
        if (ImGui::Button("Dup")) {
            DuplicateSelectedEmitter();
        }
        ImGui::SameLine();
        if (ImGui::Button("- Remove")) {
            RemoveSelectedEmitter();
        }

        ImGui::Separator();
        // Save: callback 未登録時は disabled 風表示。
        const bool save_enabled = (_save_cb != nullptr);
        if (!save_enabled) ImGui::BeginDisabled();
        if (ImGui::Button("Save")) {
            if (_save_cb) {
                _save_cb(_save_user,
                         _emitters.IsEmpty() ? nullptr : _emitters.Data(),
                         static_cast<u32>(_emitters.Size()));
                // Save 後は dirty クリア (= 外部書き出し完了)。
                _dirty = false;
            }
        }
        if (!save_enabled) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool load_enabled = (_load_cb != nullptr);
        if (!load_enabled) ImGui::BeginDisabled();
        if (ImGui::Button("Load")) {
            if (_load_cb) {
                // Load は in-out で個数を受け渡し。callback 側で kMaxEmitters
                // 個まで埋めてもらう。内部 TArray を resize してから渡す。
                _emitters.Resize(static_cast<usize>(kMaxEmitters));
                _extra_spread_radians.Resize(static_cast<usize>(kMaxEmitters));
                for (usize i = 0; i < _extra_spread_radians.Size(); ++i) {
                    _extra_spread_radians[i] = 0.0f;
                }
                u32 actual = kMaxEmitters;
                _load_cb(_load_user, _emitters.Data(), actual);
                if (actual > kMaxEmitters) actual = kMaxEmitters;
                _emitters.Resize(static_cast<usize>(actual));
                _extra_spread_radians.Resize(static_cast<usize>(actual));
                _selected = (actual == 0) ? -1 : 0;
                _dirty    = true;
            }
        }
        if (!load_enabled) ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右カラム: 選択中 emitter properties =====
    ImGui::BeginChild("##fxedit_right", ImVec2(0, 0), true);
    {
        const u32 count = static_cast<u32>(_emitters.Size());
        if (!IsValidIndex(_selected, count)) {
            ImGui::TextDisabled("(No emitter selected)");
        } else {
            ParticleEmitterDef& def = _emitters[static_cast<usize>(_selected)];
            ImGui::Text("Selected: idx=%d", static_cast<int>(_selected));
            ImGui::Separator();

            // ---- timing ----
            if (ImGui::SliderFloat("lifetime_sec", &def.lifetime_sec,
                                   0.0f, 10.0f, "%.3f")) {
                _dirty = true;
            }
            if (ImGui::SliderFloat("emit_rate_per_sec", &def.emit_rate_per_sec,
                                   0.0f, 500.0f, "%.1f")) {
                _dirty = true;
            }
            if (ImGui::SliderFloat("burst_count", &def.burst_count,
                                   0.0f, 256.0f, "%.0f")) {
                _dirty = true;
            }

            ImGui::Separator();
            // ---- speed ----
            if (ImGui::SliderFloat("speed_min", &def.speed_min,
                                   0.0f, 500.0f, "%.2f")) {
                if (def.speed_min > def.speed_max) def.speed_max = def.speed_min;
                _dirty = true;
            }
            if (ImGui::SliderFloat("speed_max", &def.speed_max,
                                   0.0f, 500.0f, "%.2f")) {
                if (def.speed_max < def.speed_min) def.speed_min = def.speed_max;
                _dirty = true;
            }

            ImGui::Separator();
            // ---- scale ----
            if (ImGui::SliderFloat("scale_start", &def.scale_start,
                                   0.0f, 64.0f, "%.2f")) {
                _dirty = true;
            }
            if (ImGui::SliderFloat("scale_end", &def.scale_end,
                                   0.0f, 64.0f, "%.2f")) {
                _dirty = true;
            }

            ImGui::Separator();
            // ---- spread_radians (editor-side placeholder) ----
            f32& spread = _extra_spread_radians[static_cast<usize>(_selected)];
            if (ImGui::SliderFloat("spread_radians", &spread,
                                   0.0f, 6.28318f /* 2*pi */, "%.3f")) {
                _dirty = true;
            }
            ImGui::TextDisabled("(spread_radians: editor-side preview)");

            ImGui::Separator();
            // ---- gravity ----
            f32 gravity_xy[2] = { def.gravity.x, def.gravity.y };
            if (ImGui::InputFloat2("gravity", gravity_xy)) {
                def.gravity.x = gravity_xy[0];
                def.gravity.y = gravity_xy[1];
                _dirty = true;
            }

            ImGui::Separator();
            // ---- color ----
            f32 color_start_rgb[3] = { def.color_start.x, def.color_start.y, def.color_start.z };
            if (ImGui::ColorEdit3("color_start", color_start_rgb)) {
                def.color_start.x = color_start_rgb[0];
                def.color_start.y = color_start_rgb[1];
                def.color_start.z = color_start_rgb[2];
                _dirty = true;
            }
            f32 color_end_rgb[3] = { def.color_end.x, def.color_end.y, def.color_end.z };
            if (ImGui::ColorEdit3("color_end", color_end_rgb)) {
                def.color_end.x = color_end_rgb[0];
                def.color_end.y = color_end_rgb[1];
                def.color_end.z = color_end_rgb[2];
                _dirty = true;
            }

            ImGui::Separator();
            // ---- status footer ----
            if (_dirty) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "* modified");
            } else {
                ImGui::TextDisabled("(saved)");
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::fxedit
