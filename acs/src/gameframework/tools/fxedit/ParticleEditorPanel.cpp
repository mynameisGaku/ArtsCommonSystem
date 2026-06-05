// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — in-game ParticleEditor 実装
//
// 仕様の意図は FParticleEditorPanel.h を参照。本ファイルでは:
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

/**
 * emitter 1 件の表示ラベル "Emitter NN" を整形する。
 *
 * @details ImGui::Selectable の表示文字列として使う (ID 衝突避けは呼出側の PushID(i))。
 * @param buf 整形結果の書き込み先。
 * @param buf_size buf のバイト数。
 * @param index emitter index。
 */
static void FormatEmitterLabel(char* buf, usize buf_size, i32 index) noexcept {
    if (buf == nullptr || buf_size == 0) return;
    std::snprintf(buf, buf_size, "Emitter %d", static_cast<int>(index));
}

/**
 * emitter index の境界チェックを行う (負値 / 範囲外を弾く)。
 *
 * @param index 検査する index。
 * @param count 有効な emitter 数。
 * @return 0 <= index < count なら true。
 */
static bool IsValidIndex(i32 index, u32 count) noexcept {
    if (index < 0) return false;
    return static_cast<u32>(index) < count;
}

/** emitter list を空に戻し selection と dirty を解除する (callback は保持)。 */
void FParticleEditorPanel::Init() noexcept {
    // 完全リセット: 既存登録があれば破棄して 0 件状態にする。
    // 多重 Init を許容するため、m_Emitters.Clear() で実体は解放せず
    // 容量だけ保持 (= 次回再構築のアロケーション節約)。
    m_Emitters.Clear();
    m_ExtraSpreadRadians.Clear();
    m_Selected  = -1;
    m_Dirty     = false;
    // callback はリセットしない (Init は emitter list の reset、callback の
    // クリアは別操作と扱う)。
}

/** emitter list を解放し selection / dirty / callback をすべてクリアする。 */
void FParticleEditorPanel::Shutdown() noexcept {
    // TArray はデストラクタで解放されるが、明示的に Clear することで
    // 多重 Shutdown / 再 Init の確定状態を作る。
    m_Emitters.Clear();
    m_ExtraSpreadRadians.Clear();
    m_Selected = -1;
    m_Dirty    = false;
    // callback は外部所有 (関数ポインタ) なので破棄不要。明示的に解除する。
    m_SaveCb   = nullptr;
    m_SaveUser = nullptr;
    m_LoadCb   = nullptr;
    m_LoadUser = nullptr;
}

/** 「火花っぽい」プリセットの emitter を 1 件追加し、新規 emitter を選択する。 */
void FParticleEditorPanel::AddEmitter() noexcept {
    if (m_Emitters.Size() >= static_cast<usize>(kMaxEmitters)) {
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

    m_Emitters.PushBack(def);
    m_ExtraSpreadRadians.PushBack(0.0f);  // spread = 0 = 円周一様
    m_Selected = static_cast<i32>(m_Emitters.Size()) - 1;
    m_Dirty    = true;
}

/** 選択中 emitter を順序保存しつつ削除し、selection を更新する。 */
void FParticleEditorPanel::RemoveSelectedEmitter() noexcept {
    const u32 count = static_cast<u32>(m_Emitters.Size());
    if (!IsValidIndex(m_Selected, count)) return;

    const usize sel = static_cast<usize>(m_Selected);
    // 順序保存削除 (= 後ろの要素を 1 個ずつ前に詰める)。
    // TArray に Erase API が無いので手書き。
    for (usize i = sel + 1; i < m_Emitters.Size(); ++i) {
        m_Emitters[i - 1]               = m_Emitters[i];
        m_ExtraSpreadRadians[i - 1]   = m_ExtraSpreadRadians[i];
    }
    m_Emitters.PopBack();
    m_ExtraSpreadRadians.PopBack();

    // selection 更新: 削除後は同 index の要素 (= 元の次の emitter) を
    // 選択。最後尾を消した場合は前の要素を選択。空なら -1。
    const i32 new_count = static_cast<i32>(m_Emitters.Size());
    if (new_count == 0) {
        m_Selected = -1;
    } else if (m_Selected >= new_count) {
        m_Selected = new_count - 1;
    }
    // m_Selected < new_count の場合はそのまま (= 詰めた要素が選ばれる)。
    m_Dirty = true;
}

/** 選択中 emitter を直後に複製挿入し、selection を複製先へ移す。 */
void FParticleEditorPanel::DuplicateSelectedEmitter() noexcept {
    const u32 count = static_cast<u32>(m_Emitters.Size());
    if (!IsValidIndex(m_Selected, count)) return;
    if (m_Emitters.Size() >= static_cast<usize>(kMaxEmitters)) return;

    const usize src = static_cast<usize>(m_Selected);
    // 末尾に PushBack してから 1 個ずつ後ろにシフトして挿入位置を空ける。
    // src+1 番目に挿入したい。
    const ParticleEmitterDef copy   = m_Emitters[src];          // 値コピー
    const f32                spread = m_ExtraSpreadRadians[src];
    m_Emitters.PushBack(copy);
    m_ExtraSpreadRadians.PushBack(spread);

    // src+1 .. (旧末尾) を 1 つ後ろにシフト。末尾には今 PushBack した
    // 値が居るので、その上書きは避けるため逆順に走査する。
    // 旧末尾 index = new_size - 2、shift 範囲 = src+1 .. old_last
    const usize new_size = m_Emitters.Size();
    if (new_size >= 2) {
        for (usize i = new_size - 1; i > src + 1; --i) {
            m_Emitters[i]             = m_Emitters[i - 1];
            m_ExtraSpreadRadians[i] = m_ExtraSpreadRadians[i - 1];
        }
    }
    // src + 1 に複製を書き込む (シフト後は src+1 の中身が src の元値で
    // 上書きされている可能性があるため、最後に明示代入する)。
    if (src + 1 < new_size) {
        m_Emitters[src + 1]             = copy;
        m_ExtraSpreadRadians[src + 1] = spread;
    }

    m_Selected = static_cast<i32>(src) + 1;
    m_Dirty    = true;
}

/** 選択 index を設定する (範囲外 / 負値は -1 に正規化)。 */
void FParticleEditorPanel::SelectEmitter(i32 index) noexcept {
    const u32 count = static_cast<u32>(m_Emitters.Size());
    if (index < 0) {
        m_Selected = -1;
        return;
    }
    if (static_cast<u32>(index) >= count) {
        m_Selected = -1;
        return;
    }
    m_Selected = index;
}

/** 現在の emitter 数を返す。 */
u32 FParticleEditorPanel::EmitterCount() const noexcept {
    return static_cast<u32>(m_Emitters.Size());
}

/** index 番目の emitter def を返す (read-only、範囲外は nullptr)。 */
const ParticleEmitterDef* FParticleEditorPanel::GetEmitterDef(i32 index) const noexcept {
    if (!IsValidIndex(index, static_cast<u32>(m_Emitters.Size()))) return nullptr;
    return &m_Emitters[static_cast<usize>(index)];
}

/** index 番目の emitter def を返す (mutable、範囲外は nullptr)。 */
ParticleEmitterDef* FParticleEditorPanel::GetEmitterDefMutable(i32 index) noexcept {
    if (!IsValidIndex(index, static_cast<u32>(m_Emitters.Size()))) return nullptr;
    return &m_Emitters[static_cast<usize>(index)];
}

/** Save callback とユーザポインタを登録する (nullptr で解除)。 */
void FParticleEditorPanel::SetSaveCallback(SaveCallback cb, void* user) noexcept {
    m_SaveCb   = cb;
    m_SaveUser = user;
}

/** Load callback とユーザポインタを登録する (nullptr で解除)。 */
void FParticleEditorPanel::SetLoadCallback(LoadCallback cb, void* user) noexcept {
    m_LoadCb   = cb;
    m_LoadUser = user;
}

/** メイン ImGui window を 2 カラム (emitter list + properties) で描画する。 */
void FParticleEditorPanel::DrawUI() noexcept {
    // FEditorPanel 継承で no-param DrawUI 化。target system は
    // SetTargetSystem() で事前に set 想定 (nullptr のときは live particle
    // 数を "(no system)" 表示)。
    if (!ImGui::Begin("Particle Editor")) {
        ImGui::End();
        return;
    }

    // ヘッダ行: 現在の active particle 数 (target_system 由来) と emitter 数。
    // editor 内の emitter 数と system 側の真の emitter 数は別物だが、
    // particle 数は system が真値なので分けて表示する。
    if (m_TargetSystem != nullptr) {
        ImGui::Text("Editor Emitters: %u / %u    Live FParticles: %u",
                    static_cast<unsigned>(m_Emitters.Size()),
                    static_cast<unsigned>(kMaxEmitters),
                    static_cast<unsigned>(m_TargetSystem->ActiveParticleCount()));
    } else {
        ImGui::Text("Editor Emitters: %u / %u    Live FParticles: (no system attached)",
                    static_cast<unsigned>(m_Emitters.Size()),
                    static_cast<unsigned>(kMaxEmitters));
    }
    ImGui::Separator();

    // 2 カラムレイアウト: BeginColumns でも良いが、Child window で左右を分ける方が
    // resize ハンドル不要で読みやすい。
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = (content_w > 480.0f) ? 200.0f : content_w * 0.35f;

    // 左カラム: emitter list。
    ImGui::BeginChild("##fxedit_left", ImVec2(left_w, 0), true);
    {
        ImGui::TextUnformatted("Emitters");
        ImGui::Separator();

        for (i32 i = 0; i < static_cast<i32>(m_Emitters.Size()); ++i) {
            char label[32];
            FormatEmitterLabel(label, sizeof(label), i);
            const bool is_selected = (m_Selected == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(label, is_selected)) {
                m_Selected = i;
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
        const bool save_enabled = (m_SaveCb != nullptr);
        if (!save_enabled) ImGui::BeginDisabled();
        if (ImGui::Button("Save")) {
            if (m_SaveCb) {
                m_SaveCb(m_SaveUser,
                         m_Emitters.IsEmpty() ? nullptr : m_Emitters.Data(),
                         static_cast<u32>(m_Emitters.Size()));
                // Save 後は dirty クリア (= 外部書き出し完了)。
                m_Dirty = false;
            }
        }
        if (!save_enabled) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool load_enabled = (m_LoadCb != nullptr);
        if (!load_enabled) ImGui::BeginDisabled();
        if (ImGui::Button("Load")) {
            if (m_LoadCb) {
                // Load は in-out で個数を受け渡し。callback 側で kMaxEmitters
                // 個まで埋めてもらう。内部 TArray を resize してから渡す。
                m_Emitters.Resize(static_cast<usize>(kMaxEmitters));
                m_ExtraSpreadRadians.Resize(static_cast<usize>(kMaxEmitters));
                for (usize i = 0; i < m_ExtraSpreadRadians.Size(); ++i) {
                    m_ExtraSpreadRadians[i] = 0.0f;
                }
                u32 actual = kMaxEmitters;
                m_LoadCb(m_LoadUser, m_Emitters.Data(), actual);
                if (actual > kMaxEmitters) actual = kMaxEmitters;
                m_Emitters.Resize(static_cast<usize>(actual));
                m_ExtraSpreadRadians.Resize(static_cast<usize>(actual));
                m_Selected = (actual == 0) ? -1 : 0;
                m_Dirty    = true;
            }
        }
        if (!load_enabled) ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右カラム: 選択中 emitter properties。
    ImGui::BeginChild("##fxedit_right", ImVec2(0, 0), true);
    {
        const u32 count = static_cast<u32>(m_Emitters.Size());
        if (!IsValidIndex(m_Selected, count)) {
            ImGui::TextDisabled("(No emitter selected)");
        } else {
            ParticleEmitterDef& def = m_Emitters[static_cast<usize>(m_Selected)];
            ImGui::Text("Selected: idx=%d", static_cast<int>(m_Selected));
            ImGui::Separator();

            // timing
            if (ImGui::SliderFloat("lifetime_sec", &def.lifetime_sec,
                                   0.0f, 10.0f, "%.3f")) {
                m_Dirty = true;
            }
            if (ImGui::SliderFloat("emit_rate_per_sec", &def.emit_rate_per_sec,
                                   0.0f, 500.0f, "%.1f")) {
                m_Dirty = true;
            }
            if (ImGui::SliderFloat("burst_count", &def.burst_count,
                                   0.0f, 256.0f, "%.0f")) {
                m_Dirty = true;
            }

            ImGui::Separator();
            // speed
            if (ImGui::SliderFloat("speed_min", &def.speed_min,
                                   0.0f, 500.0f, "%.2f")) {
                if (def.speed_min > def.speed_max) def.speed_max = def.speed_min;
                m_Dirty = true;
            }
            if (ImGui::SliderFloat("speed_max", &def.speed_max,
                                   0.0f, 500.0f, "%.2f")) {
                if (def.speed_max < def.speed_min) def.speed_min = def.speed_max;
                m_Dirty = true;
            }

            ImGui::Separator();
            // scale
            if (ImGui::SliderFloat("scale_start", &def.scale_start,
                                   0.0f, 64.0f, "%.2f")) {
                m_Dirty = true;
            }
            if (ImGui::SliderFloat("scale_end", &def.scale_end,
                                   0.0f, 64.0f, "%.2f")) {
                m_Dirty = true;
            }

            ImGui::Separator();
            // spread_radians (editor-side placeholder)
            f32& spread = m_ExtraSpreadRadians[static_cast<usize>(m_Selected)];
            if (ImGui::SliderFloat("spread_radians", &spread,
                                   0.0f, 6.28318f /* 2*pi */, "%.3f")) {
                m_Dirty = true;
            }
            ImGui::TextDisabled("(spread_radians: editor-side preview)");

            ImGui::Separator();
            // gravity
            f32 gravity_xy[2] = { def.gravity.x, def.gravity.y };
            if (ImGui::InputFloat2("gravity", gravity_xy)) {
                def.gravity.x = gravity_xy[0];
                def.gravity.y = gravity_xy[1];
                m_Dirty = true;
            }

            ImGui::Separator();
            // color
            f32 color_start_rgb[3] = { def.color_start.x, def.color_start.y, def.color_start.z };
            if (ImGui::ColorEdit3("color_start", color_start_rgb)) {
                def.color_start.x = color_start_rgb[0];
                def.color_start.y = color_start_rgb[1];
                def.color_start.z = color_start_rgb[2];
                m_Dirty = true;
            }
            f32 color_end_rgb[3] = { def.color_end.x, def.color_end.y, def.color_end.z };
            if (ImGui::ColorEdit3("color_end", color_end_rgb)) {
                def.color_end.x = color_end_rgb[0];
                def.color_end.y = color_end_rgb[1];
                def.color_end.z = color_end_rgb[2];
                m_Dirty = true;
            }

            ImGui::Separator();
            // status footer
            if (m_Dirty) {
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
